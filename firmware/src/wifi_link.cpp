#include "config.h"
#ifdef TRANSPORT_WIFI

#include "wifi_link.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "config.h"
#include "proto_frame.h"
#include "log_util.h"

#include "dsp_pipeline.h"
#include "adpcm.h"

static WiFiUDP s_udp;
static QueueHandle_t s_rawQ = nullptr, s_beamQ = nullptr;
static volatile bool s_beamMode = false;   // false=3ch 48k raw(도구용), true=빔포밍 16k mono
static bool s_running = false;

static IPAddress s_subIp;
static uint16_t  s_subPort = 0;
static uint32_t  s_subLastMs = 0;

static volatile uint32_t s_txFrames = 0;
static volatile uint32_t s_txBytes = 0;

static const uint32_t SUB_TIMEOUT_MS = 5000;  // SUB keepalive 없으면 송신 중단

// EVENT blind 2회 전송 상태 (§6.1)
static uint8_t  s_evtFrame[64];
static size_t   s_evtLen = 0;
static uint32_t s_evtSentAt = 0;
static volatile uint8_t s_evtSendsLeft = 0;
static uint16_t s_evtSeq = 0;

bool wifi_link_haveCreds() {
  Preferences p;
  p.begin("net", true);
  const bool ok = p.isKey("ssid");
  p.end();
  return ok;
}

void wifi_link_setCreds(const String &ssid, const String &pass) {
  Preferences p;
  p.begin("net", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  Log.printf("[wifi] 자격증명 저장됨 (ssid=%s)\n", ssid.c_str());
}

void wifi_link_forget() {
  Preferences p;
  p.begin("net", false);
  p.clear();
  p.end();
  Log.println("[wifi] 자격증명 삭제됨");
}

bool wifi_link_connect(uint32_t timeoutMs) {
  Preferences p;
  p.begin("net", true);
  const String ssid = p.getString("ssid", "");
  const String pass = p.getString("pass", "");
  p.end();
  if (ssid.isEmpty()) return false;

  Log.printf("[wifi] '%s' 접속 중...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) return true;  // 이전 시도가 백그라운드에서 이미 성공
  WiFi.disconnect();  // 진행 중이던 접속 중단 — 없으면 재시도 begin()이 ESP_ERR_WIFI_STATE(0x3006)
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > timeoutMs) {
      Log.println("[wifi] 접속 실패 (타임아웃). 'wifi <ssid>;<pass>'로 재설정 가능. 2.4GHz 네트워크인지 확인!");
      return false;
    }
    delay(200);
  }
  esp_wifi_set_ps(WIFI_PS_NONE);  // 저지연 필수 (CLAUDE.md §3)
  Log.printf("[wifi] 접속됨. IP=%s RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Log.printf("[wifi] PC에서: python tools/receiver.py %s\n", WiFi.localIP().toString().c_str());
  return true;
}

static void senderTask(void *) {
  static audio_block_t blk;
  static uint8_t pkt[2 + 6 + 16 + AUDIO_BLOCK_SAMPLES * 2 + 2];
  uint16_t seq = 0;

  for (;;) {
    // 수신 처리: SUB 구독 / PING 에코
    int ps;
    while ((ps = s_udp.parsePacket()) > 0) {
      uint8_t rb[64];
      const int n = s_udp.read(rb, sizeof(rb));
      if (n >= 3 && rb[0] == 'S' && rb[1] == 'U' && rb[2] == 'B') {
        const bool isNew = (s_subPort == 0);
        s_subIp = s_udp.remoteIP();
        s_subPort = s_udp.remotePort();
        s_subLastMs = millis();
        if (isNew) Log.printf("[udp] 구독자 등록: %s:%u\n", s_subIp.toString().c_str(), s_subPort);
      } else if (n >= 8 && rb[0] == 0xA5 && rb[1] == 0x5A && rb[2] == PROTO_TYPE_PING) {
        rb[2] = PROTO_TYPE_PONG;  // t1 페이로드 그대로 에코 (RTT 측정용)
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.write(rb, n);
        s_udp.endPacket();
      }
    }

    // EVENT 재전송 슬롯 (blind 2회, 50ms 간격)
    if (s_evtSendsLeft > 0 && s_subPort != 0 &&
        (s_evtSentAt == 0 || millis() - s_evtSentAt >= 50)) {
      s_udp.beginPacket(s_subIp, s_subPort);
      s_udp.write(s_evtFrame, s_evtLen);
      s_udp.endPacket();
      s_evtSendsLeft = s_evtSendsLeft - 1;
      s_evtSentAt = millis();
    }

    const bool hasSub = (s_subPort != 0) && (millis() - s_subLastMs < SUB_TIMEOUT_MS);
    size_t len = 0;
    if (s_beamMode) {
      static dsp_frame_t f;
      if (xQueueReceive(s_beamQ, &f, pdMS_TO_TICKS(20)) == pdTRUE && hasSub) {
        // flags: bit0=0(16kHz), bits4-5=0(1ch). fmt은 payload 첫 바이트(0=PCM16, 1=ADPCM)
        if (adpcm_enabled()) {
          static uint8_t nib[DSP_OUT_SAMPLES / 2];
          adpcm_state_t st;
          adpcm_encode(f.samples, f.nsamples, nib, &st);
          len = proto_build_audio_adpcm(pkt, sizeof(pkt), seq++, f.t_ms, f.azimuth01, f.conf,
                                        f.hist, st.predictor, st.stepIdx, nib, f.nsamples / 2, 0x00);
        } else {
          len = proto_build_audio_meta(pkt, sizeof(pkt), seq++, f.t_ms,
                                       f.azimuth01, f.conf, f.hist, f.samples, f.nsamples, 0x00);
        }
      }
    } else {
      if (xQueueReceive(s_rawQ, &blk, pdMS_TO_TICKS(20)) == pdTRUE && hasSub) {
        const uint8_t flags = FRAME_FLAG_48K | (uint8_t)((blk.channels - 1) << 4);
        len = proto_build_audio(pkt, sizeof(pkt), seq++, blk, flags);
      }
    }
    if (len > 0) {
      s_udp.beginPacket(s_subIp, s_subPort);
      s_udp.write(pkt, len);
      s_udp.endPacket();
      s_txFrames = s_txFrames + 1;
      s_txBytes = s_txBytes + len;
    }
  }
}

void wifi_link_sendEvent(uint8_t subtype, const uint8_t *data, size_t len) {
  if (len > 32) return;
  uint8_t pl[33];
  pl[0] = subtype;
  if (len > 0) memcpy(pl + 1, data, len);
  const size_t n = proto_build(s_evtFrame, sizeof(s_evtFrame), PROTO_TYPE_EVENT, s_evtSeq++, pl, 1 + len);
  if (n > 0) {
    s_evtLen = n;
    s_evtSentAt = 0;
    s_evtSendsLeft = 2;  // senderTask가 즉시 1회 + 50ms 후 1회 전송
  }
}

void wifi_link_start(QueueHandle_t rawQueue, QueueHandle_t beamQueue) {
  if (s_running) return;
  s_rawQ = rawQueue;
  s_beamQ = beamQueue;
  {  // beam 모드는 NVS에 유지 — COM3(HWCDC)는 시리얼 닫을 때 보드가 리셋되므로 RAM만으론 증발 (2026-09-10)
    Preferences p;
    p.begin("cfg", true);
    s_beamMode = p.getBool("beam", false);
    p.end();
  }
  s_udp.begin(UDP_AUDIO_PORT);
  // 무선 송수신은 코어0 (CLAUDE.md §5.1)
  xTaskCreatePinnedToCore(senderTask, "udp_tx", 8192, nullptr, 8, nullptr, 0);
  s_running = true;
  Log.printf("[udp] 스트리머 시작 (포트 %d, 구독 대기, 모드=%s)\n",
             UDP_AUDIO_PORT, s_beamMode ? "beam16k" : "raw3ch");
}

void wifi_link_setBeamMode(bool beam) {
  s_beamMode = beam;
  Preferences p;
  p.begin("cfg", false);
  p.putBool("beam", beam);
  p.end();
  Log.printf("[udp] 오디오 스트림 = %s (재부팅에도 유지)\n", beam ? "빔포밍 16kHz mono (beam)" : "3ch 48kHz raw");
}

bool wifi_link_beamMode() { return s_beamMode; }

bool wifi_link_running() { return s_running; }

void wifi_link_stats(uint32_t &txFrames, uint32_t &txBytes, bool &hasSubscriber) {
  txFrames = s_txFrames;
  txBytes = s_txBytes;
  hasSubscriber = (s_subPort != 0) && (millis() - s_subLastMs < SUB_TIMEOUT_MS);
}

#endif  // TRANSPORT_WIFI
