#include "config.h"
#ifdef TRANSPORT_WIFI

#include "img_link.h"
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_rom_crc.h"
#include "camera_test.h"
#include "proto_frame.h"
#include "ui_oled.h"
#include "dsp_pipeline.h"
#include "log_util.h"

#define IMG_CHUNK_BYTES 1024

static WiFiServer s_server(TCP_CTRL_PORT);
static bool s_started = false;
static volatile bool s_conn = false;
static volatile bool s_capNow = false;
static volatile uint32_t s_imgs = 0, s_kb = 0;
static volatile uint32_t s_periodMs = CAM_PERIOD_MS;  // SET_CONFIG(key=0x01)로 런타임 변경 가능

static size_t buildFrame(uint8_t *out, uint8_t type, uint16_t seq, const uint8_t *payload, size_t plen) {
  out[0] = 0xA5; out[1] = 0x5A;
  out[2] = type; out[3] = 0;
  out[4] = seq & 0xFF; out[5] = seq >> 8;
  out[6] = plen & 0xFF; out[7] = plen >> 8;
  memcpy(out + 8, payload, plen);
  const uint16_t crc = crc16_ccitt(out + 2, 6 + plen);
  out[8 + plen] = crc & 0xFF;
  out[9 + plen] = crc >> 8;
  return 10 + plen;
}

static bool sendImage(WiFiClient &c, uint16_t imgId) {
  if (!camera_ensure_init()) return false;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Log.println("[img] 캡처 실패 (fb=null)");
    return false;
  }
  const uint32_t t0 = millis();
  static uint8_t pkt[10 + 4 + IMG_CHUNK_BYTES + 2];
  static uint8_t cp[4 + IMG_CHUNK_BYTES];
  uint8_t pl[11];
  uint16_t seq = 0;
  bool ok = true;

  // IMG_START: img_id u16 · total_len u32 · width u16 · height u16 · fmt u8
  pl[0] = imgId & 0xFF; pl[1] = imgId >> 8;
  pl[2] = fb->len & 0xFF; pl[3] = (fb->len >> 8) & 0xFF; pl[4] = (fb->len >> 16) & 0xFF; pl[5] = (fb->len >> 24) & 0xFF;
  pl[6] = fb->width & 0xFF; pl[7] = fb->width >> 8;
  pl[8] = fb->height & 0xFF; pl[9] = fb->height >> 8;
  pl[10] = 0;  // fmt=JPEG
  size_t n = buildFrame(pkt, PROTO_TYPE_IMG_START, seq++, pl, 11);
  ok = c.write(pkt, n) == n;

  // IMG_CHUNK: img_id u16 · chunk_idx u16 · data[]
  uint16_t idx = 0;
  for (size_t off = 0; ok && off < fb->len; off += IMG_CHUNK_BYTES, idx++) {
    const size_t take = (fb->len - off < IMG_CHUNK_BYTES) ? fb->len - off : IMG_CHUNK_BYTES;
    cp[0] = imgId & 0xFF; cp[1] = imgId >> 8;
    cp[2] = idx & 0xFF; cp[3] = idx >> 8;
    memcpy(cp + 4, fb->buf + off, take);
    n = buildFrame(pkt, PROTO_TYPE_IMG_CHUNK, seq++, cp, 4 + take);
    ok = c.write(pkt, n) == n;
  }

  // IMG_END: img_id u16 · crc32 u32
  if (ok) {
    const uint32_t crc = esp_rom_crc32_le(0, fb->buf, fb->len);
    pl[0] = imgId & 0xFF; pl[1] = imgId >> 8;
    pl[2] = crc & 0xFF; pl[3] = (crc >> 8) & 0xFF; pl[4] = (crc >> 16) & 0xFF; pl[5] = (crc >> 24) & 0xFF;
    n = buildFrame(pkt, PROTO_TYPE_IMG_END, seq++, pl, 6);
    ok = c.write(pkt, n) == n;
  }

  if (ok) {
    s_imgs = s_imgs + 1;
    s_kb = s_kb + fb->len / 1024;
    Log.printf("[img] #%u %ux%u %uKB 전송 %lums (청크 %u개)\n",
               imgId, fb->width, fb->height, (unsigned)(fb->len / 1024),
               (unsigned long)(millis() - t0), idx);
  } else {
    Log.println("[img] TCP 전송 실패");
  }
  esp_camera_fb_return(fb);
  return ok;
}

// 수신 스트림에서 CMD/PING 프레임 파싱 (CAPTURE_NOW, PONG 에코)
static void parseIncoming(WiFiClient &c, uint8_t *buf, size_t &blen, size_t bcap) {
  while (c.available() && blen < bcap) buf[blen++] = (uint8_t)c.read();
  size_t pos = 0;
  while (blen - pos >= 10) {
    if (buf[pos] != 0xA5 || buf[pos + 1] != 0x5A) { pos++; continue; }
    const uint16_t plen = buf[pos + 6] | (buf[pos + 7] << 8);
    if (plen > 512) { pos++; continue; }
    if (blen - pos < (size_t)(10 + plen)) break;  // 프레임 미완성 — 다음 호출에서 계속
    const uint8_t type = buf[pos + 2];
    const uint8_t *pl = buf + pos + 8;
    if (type == PROTO_TYPE_CMD && plen >= 1 && pl[0] == 0x05) {
      s_capNow = true;
      Log.println("[img] CMD CAPTURE_NOW 수신");
    } else if (type == PROTO_TYPE_CMD && plen >= 2 && pl[0] == 0x01) {
      ui_set_mode(pl[1]);            // SET_MODE {mode u8: 0~4}
      dsp_pipeline_setMode(pl[1]);   // §5.3 프리셋: 빔 가중/스티어링/HPF (2026-09-10)
    } else if (type == PROTO_TYPE_CMD && plen >= 2 && pl[0] == 0x02) {
      ui_set_volume((int8_t)pl[1]);  // SET_VOLUME {gain i8}
      dsp_pipeline_addVolume((int8_t)pl[1]);  // 스트림 디지털 게인
    } else if (type == PROTO_TYPE_CMD && plen >= 6 && pl[0] == 0x03) {
      // DISPLAY_ALERT {azimuth i16, icon u8, dur_ms u16}
      const int16_t az = (int16_t)(pl[1] | (pl[2] << 8));
      const uint16_t dur = (uint16_t)(pl[4] | (pl[5] << 8));
      ui_alert(az, pl[3], dur);
    } else if (type == PROTO_TYPE_CMD && plen >= 6 && pl[0] == 0x04) {
      // SET_CONFIG {key u8, val i32 LE} — key 0x01 = 캡처 주기(ms)
      const uint8_t key = pl[1];
      int32_t val;
      memcpy(&val, pl + 2, 4);
      if (key == 0x01) {
        if (val < 500) val = 500;
        if (val > 60000) val = 60000;
        s_periodMs = (uint32_t)val;
        Log.printf("[img] 캡처 주기 변경: %ldms\n", (long)val);
      }
    } else if (type == PROTO_TYPE_PING) {
      static uint8_t pong[10 + 64];
      if (plen <= 64) {
        const size_t n = buildFrame(pong, PROTO_TYPE_PONG, 0, pl, plen);
        c.write(pong, n);
      }
    }
    pos += 10 + plen;
  }
  if (pos > 0) {
    memmove(buf, buf + pos, blen - pos);
    blen -= pos;
  }
}

static void imgTask(void *) {
  s_server.begin();
  Log.printf("[img] TCP 이미지 서버 시작 (포트 %d, 주기 %dms) — tools/image_receiver.py\n",
             TCP_CTRL_PORT, CAM_PERIOD_MS);
  uint16_t imgId = 0;
  static uint8_t rxbuf[1024];

  for (;;) {
    WiFiClient c = s_server.accept();
    if (!c) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    c.setNoDelay(true);
    s_conn = true;
    Log.printf("[img] 클라이언트 접속: %s\n", c.remoteIP().toString().c_str());
    size_t blen = 0;
    uint32_t lastCap = 0;  // 접속 직후 1장 즉시

    while (c.connected()) {
      parseIncoming(c, rxbuf, blen, sizeof(rxbuf));
      if (s_capNow || millis() - lastCap >= s_periodMs) {
        s_capNow = false;
        lastCap = millis();
        sendImage(c, imgId++);
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    c.stop();
    s_conn = false;
    Log.println("[img] 클라이언트 연결 종료");
  }
}

void img_link_start() {
  if (s_started) return;
  s_started = true;
  // 카메라/전송은 코어0 (오디오 실시간 경로와 분리 — CLAUDE.md §5.1)
  xTaskCreatePinnedToCore(imgTask, "img_tx", 8192, nullptr, 5, nullptr, 0);
}

void img_link_stats(uint32_t &imgsSent, uint32_t &kbSent, bool &clientConnected) {
  imgsSent = s_imgs;
  kbSent = s_kb;
  clientConnected = s_conn;
}

#endif  // TRANSPORT_WIFI
