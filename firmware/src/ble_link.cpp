#include "config.h"
#ifdef TRANSPORT_BLE

#include "ble_link.h"
#include <NimBLEDevice.h>
#include "dsp_pipeline.h"
#include "adpcm.h"
#include "camera_test.h"
#include "esp_camera.h"
#include "esp_rom_crc.h"
#include "proto_frame.h"
#include "ui_oled.h"
#include "log_util.h"

#ifndef BLE_GAP_LE_PHY_2M_MASK
#define BLE_GAP_LE_PHY_2M_MASK 0x02
#endif

static QueueHandle_t s_q = nullptr;
static NimBLECharacteristic *s_audioChar = nullptr;
static NimBLECharacteristic *s_imgChar = nullptr;
static NimBLECharacteristic *s_evtChar = nullptr;
static volatile bool s_connected = false;
static volatile bool s_subscribed = false;   // AUDIO 구독
static volatile bool s_subImg = false;
static volatile bool s_subEvt = false;
static volatile bool s_capNow = false;
static volatile uint32_t s_imgPeriodMs = CAM_PERIOD_MS;
static uint16_t s_evtSeq = 0;

static volatile uint32_t s_txFrames = 0, s_txBytes = 0, s_txFail = 0;

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &ci) override {
    s_connected = true;
    // conn interval 7.5~15ms(6~12×1.25ms) 요청 — BLE 최소 간격으로 처리량 최대화 (2026-08-25)
    // 최종 수락값은 상대(폰/PC)가 결정 — 실측 시 협상 결과 확인
    srv->updateConnParams(ci.getConnHandle(), 6, 12, 0, 400);
    Log.printf("[ble] 연결됨: %s\n", ci.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *srv, NimBLEConnInfo &ci, int reason) override {
    s_connected = false;
    s_subscribed = false;
    Log.printf("[ble] 연결 해제(reason=%d) → 광고 재시작\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

// 구독 상태 추적 (characteristic별 플래그)
class SubCB : public NimBLECharacteristicCallbacks {
 public:
  SubCB(volatile bool *flag, const char *name) : m_flag(flag), m_name(name) {}
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &, uint16_t subValue) override {
    *m_flag = (subValue & 0x1) != 0;
    Log.printf("[ble] %s 구독=%u\n", m_name, subValue);
  }
 private:
  volatile bool *m_flag;
  const char *m_name;
};

class CmdCB : public NimBLECharacteristicCallbacks {
  // BLE CMD는 GATT가 프레이밍하므로 payload만 수신: [subtype][args...] (§6.2)
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    const uint8_t *pl = v.data();
    const size_t n = v.length();
    if (n < 1) return;
    if (pl[0] == 0x01 && n >= 2) {
      ui_set_mode(pl[1]);
      dsp_pipeline_setMode(pl[1]);          // §5.3 프리셋: 빔 가중/스티어링/HPF (2026-09-10)
    } else if (pl[0] == 0x02 && n >= 2) {
      ui_set_volume((int8_t)pl[1]);
      dsp_pipeline_addVolume((int8_t)pl[1]);  // 스트림 디지털 게인 (통화 VOL- 실효화)
    } else if (pl[0] == 0x03 && n >= 6) {
      const int16_t az = (int16_t)(pl[1] | (pl[2] << 8));
      const uint16_t dur = (uint16_t)(pl[4] | (pl[5] << 8));
      ui_alert(az, pl[3], dur);
    } else if (pl[0] == 0x05) {
      s_capNow = true;
      Log.println("[ble] CMD CAPTURE_NOW 수신");
    } else if (pl[0] == 0x04 && n >= 6) {
      // SET_CONFIG {key u8, val i32 LE} — key 0x01 = 캡처 주기(ms)
      int32_t val;
      memcpy(&val, pl + 2, 4);
      if (pl[1] == 0x01) {
        if (val < 500) val = 500;
        if (val > 60000) val = 60000;
        s_imgPeriodMs = (uint32_t)val;
        Log.printf("[ble] 캡처 주기 변경: %ldms\n", (long)val);
      }
    } else {
      Log.printf("[ble] CMD subtype=0x%02x %u바이트 (미지원)\n", pl[0], (unsigned)n);
    }
  }
};

// notify가 스택 혼잡으로 거부되면 잠시 대기 후 재시도 (이미지 청크는 전부 도착해야 재조립 가능)
static bool notifyRetry(NimBLECharacteristic *c, const uint8_t *d, size_t n) {
  for (int i = 0; i < 400; i++) {
    if (c->notify(d, n)) return true;
    if (!s_connected) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

// BLE 이미지 태스크: IMAGE 구독 중일 때 주기/즉시 캡처 → IMG_START/CHUNK/END notify
// 오디오(32KB/s)와 대역 공유 → 24KB 이미지에 1~3초 소요 예상 (§9 비교실험의 관찰 포인트)
static void bleImgTask(void *) {
  uint16_t imgId = 0;
  uint32_t lastCap = 0;
  static uint8_t pkt[10 + 4 + BLE_IMG_CHUNK_BYTES + 2];
  static uint8_t cp[4 + BLE_IMG_CHUNK_BYTES];
  uint8_t pl[11];

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!(s_connected && s_subImg)) continue;
    if (!(s_capNow || millis() - lastCap >= s_imgPeriodMs)) continue;
    s_capNow = false;
    lastCap = millis();
    if (!camera_ensure_init()) continue;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;

    const uint32_t t0 = millis();
    uint16_t seq = 0;
    bool ok = true;

    pl[0] = imgId & 0xFF; pl[1] = imgId >> 8;
    pl[2] = fb->len & 0xFF; pl[3] = (fb->len >> 8) & 0xFF;
    pl[4] = (fb->len >> 16) & 0xFF; pl[5] = (fb->len >> 24) & 0xFF;
    pl[6] = fb->width & 0xFF; pl[7] = fb->width >> 8;
    pl[8] = fb->height & 0xFF; pl[9] = fb->height >> 8;
    pl[10] = 0;
    size_t n = proto_build(pkt, sizeof(pkt), PROTO_TYPE_IMG_START, seq++, pl, 11);
    ok = notifyRetry(s_imgChar, pkt, n);

    uint16_t idx = 0;
    for (size_t off = 0; ok && off < fb->len; off += BLE_IMG_CHUNK_BYTES, idx++) {
      const size_t take = (fb->len - off < BLE_IMG_CHUNK_BYTES) ? fb->len - off : BLE_IMG_CHUNK_BYTES;
      cp[0] = imgId & 0xFF; cp[1] = imgId >> 8;
      cp[2] = idx & 0xFF; cp[3] = idx >> 8;
      memcpy(cp + 4, fb->buf + off, take);
      n = proto_build(pkt, sizeof(pkt), PROTO_TYPE_IMG_CHUNK, seq++, cp, 4 + take);
      ok = notifyRetry(s_imgChar, pkt, n);
      // 오디오 우선(§5.4): 청크 사이 2ms 양보 → 오디오 notify 슬롯 확보 (이미지당 +약 0.1초)
      if (s_subscribed) vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (ok) {
      const uint32_t crc = esp_rom_crc32_le(0, fb->buf, fb->len);
      pl[0] = imgId & 0xFF; pl[1] = imgId >> 8;
      pl[2] = crc & 0xFF; pl[3] = (crc >> 8) & 0xFF;
      pl[4] = (crc >> 16) & 0xFF; pl[5] = (crc >> 24) & 0xFF;
      n = proto_build(pkt, sizeof(pkt), PROTO_TYPE_IMG_END, seq++, pl, 6);
      ok = notifyRetry(s_imgChar, pkt, n);
    }
    Log.printf("[ble][img] #%u %uKB %s %lums (청크 %u개)\n",
               imgId, (unsigned)(fb->len / 1024), ok ? "전송" : "실패",
               (unsigned long)(millis() - t0), idx);
    esp_camera_fb_return(fb);
    imgId++;
  }
}

// dsp_pipeline이 만든 16kHz mono 빔포밍 프레임(+DOA 메타)을 notify 1개=프레임 1개로 전송
static void senderTask(void *) {
  static dsp_frame_t f;
  static uint8_t pkt[2 + 6 + 16 + DSP_OUT_SAMPLES * 2 + 2];
  uint16_t seq = 0;

  for (;;) {
    if (xQueueReceive(s_q, &f, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (!(s_connected && s_subscribed)) continue;
    // flags: bit0=0(16kHz), bits4-5=0(1ch). fmt은 payload 첫 바이트(0=PCM16, 1=ADPCM)
    size_t len;
    if (adpcm_enabled()) {
      static uint8_t nib[DSP_OUT_SAMPLES / 2];
      adpcm_state_t st;
      adpcm_encode(f.samples, f.nsamples, nib, &st);
      len = proto_build_audio_adpcm(pkt, sizeof(pkt), seq++, f.t_ms, f.azimuth01, f.conf,
                                    f.hist, st.predictor, st.stepIdx, nib, f.nsamples / 2, 0x00);
    } else {
      len = proto_build_audio_meta(pkt, sizeof(pkt), seq++, f.t_ms, f.azimuth01, f.conf,
                                   f.hist, f.samples, f.nsamples, 0x00);
    }
    if (len == 0) continue;
    // 오디오 우선(§5.4): 이미지 청크가 notify 슬롯을 점유해도 최대 24ms 재시도 — 프레임 주기
    // 15ms를 넘지만 beamQ 16개(240ms)가 흡수. (재시도 없인 이미지 순간 드랍 집중 — 실측 9.2%)
    bool ok = false;
    for (int r = 0; r < 12 && !ok && s_connected; r++) {
      ok = s_audioChar->notify(pkt, len);
      if (!ok) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (ok) {
      s_txFrames = s_txFrames + 1;
      s_txBytes = s_txBytes + len;
    } else {
      s_txFail = s_txFail + 1;  // 그래도 실패 → 프레임 버림 (seq로 앱이 감지)
    }
  }
}

void ble_link_start(QueueHandle_t audioQueue) {
  s_q = audioQueue;

  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(9);  // +9dBm
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);

  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new SrvCB());

  NimBLEService *svc = srv->createService(BLE_UUID_SERVICE);
  s_audioChar = svc->createCharacteristic(BLE_UUID_AUDIO, NIMBLE_PROPERTY::NOTIFY);
  s_audioChar->setCallbacks(new SubCB(&s_subscribed, "AUDIO"));
  s_imgChar = svc->createCharacteristic(BLE_UUID_IMAGE, NIMBLE_PROPERTY::NOTIFY);
  s_imgChar->setCallbacks(new SubCB(&s_subImg, "IMAGE"));
  s_evtChar = svc->createCharacteristic(BLE_UUID_EVENT, NIMBLE_PROPERTY::NOTIFY);
  s_evtChar->setCallbacks(new SubCB(&s_subEvt, "EVENT"));
  NimBLECharacteristic *cmd = svc->createCharacteristic(
      BLE_UUID_CMD, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(new CmdCB());
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_DEVICE_NAME);
  adv->addServiceUUID(svc->getUUID());
  adv->start();
  Log.printf("[ble] 광고 시작: %s (MTU 517, 2M PHY 선호)\n", BLE_DEVICE_NAME);
  Log.println("[ble] 검증: nRF Connect 앱 또는 python tools/ble_receiver.py");

  // 무선 송수신은 코어0 (CLAUDE.md §5.1). 이미지는 별도 태스크(낮은 우선순위 — 오디오 우선)
  xTaskCreatePinnedToCore(senderTask, "ble_tx", 8192, nullptr, 8, nullptr, 0);
  xTaskCreatePinnedToCore(bleImgTask, "ble_img", 8192, nullptr, 4, nullptr, 0);
}

void ble_link_sendEvent(uint8_t subtype, const uint8_t *data, size_t len) {
  if (len > 32 || s_evtChar == nullptr) return;
  if (!(s_connected && s_subEvt)) return;
  uint8_t pl[33];
  pl[0] = subtype;
  if (len > 0) memcpy(pl + 1, data, len);
  uint8_t frame[64];
  const size_t n = proto_build(frame, sizeof(frame), PROTO_TYPE_EVENT, s_evtSeq++, pl, 1 + len);
  if (n > 0) s_evtChar->notify(frame, n);
}

bool ble_link_connected() { return s_connected; }
bool ble_link_subscribed() { return s_subscribed; }

void ble_link_stats(uint32_t &txFrames, uint32_t &txBytes, uint32_t &txFail) {
  txFrames = s_txFrames;
  txBytes = s_txBytes;
  txFail = s_txFail;
}

#endif  // TRANSPORT_BLE
