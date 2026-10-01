// AI 스마트 안경형 보청기 펌웨어 — proto M2/M4: 3ch 캡처 + 전송로 (env:wifi=UDP / env:ble=NimBLE)
// 시리얼 명령(양쪽 USB 포트 모두 동작): help | info | dump | (wifi 빌드) wifi <ssid>;<pass> | forget
#include <Arduino.h>
#include "config.h"
#include "audio_capture.h"
#include "dsp_pipeline.h"
#include "adpcm.h"
#include "camera_test.h"
#include "ui_oled.h"
#include "log_util.h"

#if defined(TRANSPORT_WIFI)
#include <WiFi.h>
#include "wifi_link.h"
#include "img_link.h"
static bool s_wifiUp = false;
#elif defined(TRANSPORT_BLE)
#include "ble_link.h"
#endif

static QueueHandle_t s_audioQ = nullptr;   // 캡처 → DSP
static QueueHandle_t s_beamQ = nullptr;    // DSP → 전송로 (16k 빔포밍 + DOA 메타)
#if defined(TRANSPORT_WIFI)
static QueueHandle_t s_rawQ = nullptr;     // DSP → WiFi raw 스트림 (PC 도구용)
#endif

static void printHelp() {
#if defined(TRANSPORT_WIFI)
  Log.println("명령: help | wifi <ssid>;<pass> | forget | info | dump | beam | raw | adpcm | pcm");
  Log.println("      img vga|qvga | imgq <10-40> | doaoff <n>");
  Log.println("  예) wifi MyHotspot;mypassword   (2.4GHz만 지원!)");
  Log.println("  beam=빔포밍 16k, raw=3ch 48k | adpcm/pcm=압축 | img/imgq=이미지 속도 A/B(§7.5)");
#else
  Log.println("명령: help | info | dump | adpcm | pcm | img vga|qvga | imgq <10-40> | doaoff <n>");
#endif
}

static void printInfo() {
  Log.printf("[info] transport=%s", TRANSPORT_NAME);
#if defined(TRANSPORT_WIFI)
  Log.printf(" wifi=%s", s_wifiUp ? "UP" : "DOWN");
  if (s_wifiUp) Log.printf(" ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Log.printf(" creds=%s", wifi_link_haveCreds() ? "saved" : "none");
#elif defined(TRANSPORT_BLE)
  Log.printf(" conn=%s sub=%s", ble_link_connected() ? "yes" : "no", ble_link_subscribed() ? "yes" : "no");
#endif
  Log.printf(" heap=%uKB psram=%uKB\n",
             (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
}

static void handleLine(String line) {
  line.trim();
  if (line.isEmpty()) return;
  if (line == "help") {
    printHelp();
  } else if (line == "info") {
    printInfo();
  } else if (line == "dump") {
    audio_capture_dump(64);
  } else if (line == "cam") {
    camera_test_capture();  // M3 스모크: init(최초 1회) + 1프레임 캡처
  } else if (line == "img vga") {
    camera_set_qvga(false);   // 속도 A/B (proto.md §7.5) — NVS 유지
  } else if (line == "img qvga") {
    camera_set_qvga(true);
  } else if (line == "img gray") {
    camera_set_gray(true);    // 모노크롬 전송 (모델이 흑백 학습 — 추가 절감)
  } else if (line == "img color") {
    camera_set_gray(false);
  } else if (line.startsWith("imgq ")) {
    camera_set_quality(line.substring(5).toInt());  // OV2640 스케일 10~40 (기본 12, 속도용 18)
  } else if (line.startsWith("doaoff ")) {
    dsp_pipeline_setBusOffset(line.substring(7).toInt());  // 부팅 후 sync_check 실측값 입력
  } else if (line == "adpcm") {
    adpcm_setEnabled(true);   // 16k 스트림 4:1 압축 (beam/BLE 오디오에 적용, raw 3ch는 항상 PCM)
  } else if (line == "pcm") {
    adpcm_setEnabled(false);
#if defined(TRANSPORT_WIFI)
  } else if (line == "beam") {
    wifi_link_setBeamMode(true);
  } else if (line == "raw") {
    wifi_link_setBeamMode(false);
  } else if (line.startsWith("wifi ")) {
    const String rest = line.substring(5);
    const int sep = rest.indexOf(';');
    if (sep <= 0) {
      Log.println("[err] 형식: wifi <ssid>;<pass>");
      return;
    }
    wifi_link_setCreds(rest.substring(0, sep), rest.substring(sep + 1));
    s_wifiUp = wifi_link_connect();
    if (s_wifiUp) { wifi_link_start(s_rawQ, s_beamQ); img_link_start(); }
  } else if (line == "forget") {
    wifi_link_forget();
#endif
  } else {
    Log.printf("[err] 알 수 없는 명령: %s\n", line.c_str());
    printHelp();
  }
}

static uint32_t s_lastInputMs = 0;  // 입력 중에는 stat 출력 일시정지 (타이핑 방해 방지)

static void pollSerial() {
  static String buf;
  int ci;
  while ((ci = log_read()) >= 0) {
    const char c = (char)ci;
    s_lastInputMs = millis();
    if (c == '\n' || c == '\r') {
      Log.println();  // 입력 줄 마무리 에코
      if (!buf.isEmpty()) { handleLine(buf); buf = ""; }
    } else if (c == 8 || c == 127) {  // Backspace / DEL
      if (!buf.isEmpty()) {
        buf.remove(buf.length() - 1);
        Log.print("\b \b");
      }
    } else if (buf.length() < 160) {
      buf += c;
      Log.print(c);  // 에코 (pio monitor는 로컬 에코가 없음)
    }
  }
}

void setup() {
  log_begin();
  delay(2000);  // USB-CDC 연결 대기

  Log.println();
  Log.printf("=== SmartGlass Hearing-Aid FW | %s | %dch %dHz ===\n",
             TRANSPORT_NAME, AUDIO_CHANNELS, AUDIO_CAPTURE_HZ);
  Log.printf("Chip: %s rev%d | Flash %uMB | PSRAM %uMB\n",
             ESP.getChipModel(), ESP.getChipRevision(),
             (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
             (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
  if (ESP.getPsramSize() < 7 * 1024 * 1024)
    Log.println("[WARN] PSRAM 8MB 미인식 — memory_type=qio_opi / 보드 각인(N16R8) 확인!");

  pinMode(BTN_PIN, INPUT_PULLUP);      // PTT: 온보드 BOOT
  pinMode(BTN_PIN_EXT, INPUT_PULLUP);  // PTT: 외부 버튼 (GPIO46↔GND, 극성 없음)
  ui_init();                       // SSD1306 OLED (SDA=47, SCL=14)
  adpcm_init();                    // NVS에서 압축 설정 복원 (adpcm/pcm 명령)

  s_audioQ = xQueueCreate(8, sizeof(audio_block_t));
  s_beamQ = xQueueCreate(16, sizeof(dsp_frame_t));  // 240ms — BLE 이미지 버스트 동안 오디오 흡수
#if defined(TRANSPORT_WIFI)
  s_rawQ = xQueueCreate(8, sizeof(audio_block_t));
#endif

  if (!audio_capture_start(s_audioQ)) {
    Log.println("[FATAL] I2S 초기화 실패 — 배선/핀맵 확인");
  } else {
    Log.printf("[i2s] %dch 캡처 시작 (I2S0 master + I2S1 slave 클럭공유)\n", AUDIO_CHANNELS);
  }

  // DSP: DAS 빔포밍 + 3:1 decimation + SRP-PHAT DOA (코어1, M2 C 포팅 2026-09-10)
#if defined(TRANSPORT_WIFI)
  dsp_pipeline_start(s_audioQ, s_rawQ, s_beamQ);
#else
  dsp_pipeline_start(s_audioQ, nullptr, s_beamQ);
#endif

#if defined(TRANSPORT_WIFI)
  if (wifi_link_haveCreds()) {
    s_wifiUp = wifi_link_connect();
    if (s_wifiUp) { wifi_link_start(s_rawQ, s_beamQ); img_link_start(); }
  } else {
    Log.println("[wifi] 저장된 자격증명 없음 — 'wifi <ssid>;<pass>' 입력 (2.4GHz)");
  }
#elif defined(TRANSPORT_BLE)
  ble_link_start(s_beamQ);  // BLE: 빔포밍 16kHz mono + DOA 메타 (dsp_pipeline 출력)
#endif
  printHelp();
}

void loop() {
  pollSerial();
  ui_tick();

  // PTT 버튼: 디바운스 50ms → press/release 이벤트 (구 스펙: press=STT 시작, release=종료)
  {
    static bool btnLast = false;
    static uint32_t btnChangeMs = 0;
    const bool pressed = (digitalRead(BTN_PIN) == LOW) || (digitalRead(BTN_PIN_EXT) == LOW);
    if (pressed != btnLast && millis() - btnChangeMs > 50) {
      btnLast = pressed;
      btnChangeMs = millis();
      ui_btn(pressed);
      Log.printf("[btn] %s\n", pressed ? "PRESS" : "RELEASE");
      const uint8_t st = pressed ? 1 : 0;
#if defined(TRANSPORT_WIFI)
      wifi_link_sendEvent(0x01, &st, 1);  // EVENT BUTTON {state} — UDP blind 2회
#elif defined(TRANSPORT_BLE)
      ble_link_sendEvent(0x01, &st, 1);   // EVENT BUTTON {state} — notify
#endif
    }
  }

#if defined(TRANSPORT_WIFI)
  // 핫스팟이 부팅 후에 켜지는 경우 대비: 20초마다 자동 재접속 시도 (2026-08-11)
  static uint32_t lastRetry = 0;
  if (!s_wifiUp && wifi_link_haveCreds() && millis() - lastRetry > 20000) {
    lastRetry = millis();
    s_wifiUp = wifi_link_connect(8000);
    if (s_wifiUp) { wifi_link_start(s_rawQ, s_beamQ); img_link_start(); }
  }
#endif

  // DOA_ALERT: 게이트 통과한 큰소리 → EVENT 0x02 {azimuth i16, energy_db u8, t_ms u32} (§6.1)
  {
    int16_t az01;
    uint8_t edb;
    uint32_t tms;
    if (dsp_pipeline_pollAlert(az01, edb, tms)) {
      uint8_t d[7];
      d[0] = az01 & 0xFF; d[1] = (az01 >> 8) & 0xFF;
      d[2] = edb;
      d[3] = tms & 0xFF; d[4] = (tms >> 8) & 0xFF; d[5] = (tms >> 16) & 0xFF; d[6] = (tms >> 24) & 0xFF;
      Log.printf("[doa] ALERT az=%.1f° %udB\n", az01 / 10.0f, edb);
#if defined(TRANSPORT_WIFI)
      wifi_link_sendEvent(0x02, d, 7);
#elif defined(TRANSPORT_BLE)
      ble_link_sendEvent(0x02, d, 7);
#endif
    }
  }

  static uint32_t lastStat = 0;
  const bool typing = (s_lastInputMs != 0) && (millis() - s_lastInputMs < 10000);
  if (!typing && millis() - lastStat >= 2000) {
    lastStat = millis();
    float rms[AUDIO_MAX_CH]; int16_t mn[AUDIO_MAX_CH], mx[AUDIO_MAX_CH]; uint32_t ovr;
    audio_capture_stats(rms, mn, mx, ovr);
    static const char *CHN[AUDIO_MAX_CH] = {"1", "2", "3"};
    Log.print("[stat]");
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++)
      Log.printf(" mic%s=%.0f(%d..%d)", CHN[ch], rms[ch], mn[ch], mx[ch]);
    if (audio_capture_i2s1_stalled()) Log.print(" [i2s1=STALL!]");

    {  // DOA: 최근 3초 유효 추정 + DSP 부하 (창당 µs / 21333µs = 코어 점유율)
      int16_t az01; uint8_t conf; uint32_t runs, us, valid;
      dsp_pipeline_getDoa(az01, conf);
      dsp_pipeline_stats(runs, us, valid);
      if (conf > 0) Log.printf(" doa=%.0f°(c%u)", az01 / 10.0f, conf);
      Log.printf(" dsp=%uus/%uv", (unsigned)us, (unsigned)valid);
    }

#if defined(TRANSPORT_WIFI)
    uint32_t txf = 0, txb = 0; bool sub = false;
    if (wifi_link_running()) wifi_link_stats(txf, txb, sub);
    Log.printf(" ovr=%u | wifi=%s sub=%s tx=%uf/%uKB",
               (unsigned)ovr, s_wifiUp ? "UP" : "DOWN", sub ? "yes" : "no",
               (unsigned)txf, (unsigned)(txb / 1024));
#elif defined(TRANSPORT_BLE)
    uint32_t txf = 0, txb = 0, txfail = 0;
    ble_link_stats(txf, txb, txfail);
    Log.printf(" ovr=%u | ble=%s sub=%s tx=%uf/%uKB fail=%u",
               (unsigned)ovr, ble_link_connected() ? "CONN" : "ADV",
               ble_link_subscribed() ? "yes" : "no",
               (unsigned)txf, (unsigned)(txb / 1024), (unsigned)txfail);
#endif
    Log.printf(" | heap=%uKB\n", (unsigned)(ESP.getFreeHeap() / 1024));

#ifdef RGB_BUILTIN
    static bool on = false;
    on = !on;
    rgbLedWrite(RGB_BUILTIN, 0, on ? 12 : 0, 0);
#endif
  }
  delay(10);
}
