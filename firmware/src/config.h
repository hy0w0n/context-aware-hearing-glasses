// config.h — 핀맵 및 전역 설정 (기준: CLAUDE.md §4.2 핀맵 v0.1)
// 변경 시 CLAUDE.md 먼저 갱신할 것
#pragma once

// ── 전송 계층 (빌드 env로 택1) ─────────────────────────────
#if defined(TRANSPORT_WIFI)
  #define TRANSPORT_NAME "WiFi"
#elif defined(TRANSPORT_BLE)
  #define TRANSPORT_NAME "BLE"
#else
  #error "TRANSPORT_WIFI 또는 TRANSPORT_BLE 빌드 플래그가 필요합니다 (env:wifi / env:ble)"
#endif

// ── 카메라 DVP (ESP32-S3-EYE 검증 핀맵) ──────────────────
#define CAM_PIN_XCLK    15   // LEDC 20MHz 공급 (모듈 온보드 크리스탈 확인 시 -1, Q1)
#define CAM_PIN_SIOD     4   // SCCB SDA (주소 0x30)
#define CAM_PIN_SIOC     5   // SCCB SCL
#define CAM_PIN_D0      11   // Y2
#define CAM_PIN_D1       9   // Y3
#define CAM_PIN_D2       8   // Y4
#define CAM_PIN_D3      10   // Y5
#define CAM_PIN_D4      12   // Y6
#define CAM_PIN_D5      18   // Y7
#define CAM_PIN_D6      17   // Y8
#define CAM_PIN_D7      16   // Y9
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13
#define CAM_PIN_RESET   -1   // 모듈 온보드 RC 리셋
#define CAM_PIN_PWDN    -1   // 절전 실험 시 GPIO3 (strapping — Q1에서 풀저항 확인 후)

// ── I2S0 RX: mic1(L) + mic2(R), SD 공유 + 풀다운 ─────────
#define I2S0_PIN_BCLK   41   // 3개 마이크 + I2S1에 공통 배선
#define I2S0_PIN_WS     42
#define I2S0_PIN_SD      2

// ── I2S1 RX: mic3 (BCLK/WS는 41/42 병렬, slave) ──────────
#define I2S1_PIN_SD      1

// ── I2S TX: 이어폰 (Q3 보류 중) — ⚠ CAM 보드 확정(2026-08-11)으로 재배정 필요:
//    구 계획 47/21/14 중 21=온보드 백라이트, 38(MCLK 후보)=SD_CMD. Q3 재개 시 결정
#define I2ST_PIN_BCLK   -1
#define I2ST_PIN_LRC    -1
#define I2ST_PIN_DOUT   -1

// ── I2C: OLED(0x3C) + ES8311(0x18) ───────────────────────
// ⚠ 구 계획 39/40은 GOOUUU S3-CAM 보드 SD슬롯(38=CMD, 39=CLK, 40=DATA)과 충돌 → 47/14로 변경(2026-08-11)
#define I2C_PIN_SDA     47
#define I2C_PIN_SCL     14

// ── 버튼(PTT) ────────────────────────────────────────────
// GPIO0은 CAM 보드 헤더에 미노출(온보드 BOOT 전용) → 외부 버튼은 GPIO46 (2026-08-25)
// 46은 strapping이지만 버튼→GND 구성은 안전: 눌린 채 부팅해도 기본 strap 값(LOW=정상 부팅)
#define BTN_PIN          0    // 온보드 BOOT (계속 PTT로 동작)
#define BTN_PIN_EXT     46    // 외부 버튼: 한쪽→GPIO46, 다른쪽→GND. 실크에 46 없으면 45로 변경

// ── 마이크 기하 (DOA 계산용 — 실측값, 배치 바꾸면 갱신) ──
// 안경 임시 테이프 부착 (2026-09-10): mic1=좌, mic2=우, mic3=후방 중앙 — 정삼각형 14/14/14cm
// 좌표(cm): mic1=(-7,0), mic2=(+7,0), mic3=(0,-12.1)=√(14²-7²). 최종 고정(납땜) 후 재실측할 것
// (구 케이블 배치 25/18/18cm — 2026-08-11~09-09 녹음 분석 시엔 구 값 사용)
#define MIC12_BASELINE_M  0.14f
#define MIC13_BASELINE_M  0.14f
#define MIC23_BASELINE_M  0.14f

// ── 오디오 파라미터 ──────────────────────────────────────
#define AUDIO_CAPTURE_HZ   48000   // 캡처 (INMP441 fWS 상한 50kHz 내)
#define AUDIO_STREAM_HZ    16000   // 전송 (48k → 3:1 decimation, YAMNet 입력 규격) — M2에서 FIR decimation 도입
#define AUDIO_FRAME_SAMPLES  256   // 캡처 프레임 (5.3ms @48kHz)
#define AUDIO_DOA_BLOCK     1024   // GCC-PHAT 블록 (FIR 상태는 블록 간 이월)

// ── M1/M2 임시 설정: 48kHz PCM16 raw 스트림 (decimation 전) ──
#define AUDIO_CHANNELS         3   // M1=1(mic1), M2a=2(mic1 L + mic2 R), M2b=3(+mic3, I2S1 slave)
#define AUDIO_BLOCK_SAMPLES  480   // UDP 패킷당 총 int16 수 (1ch=10ms, 2ch=5ms 프레임, 페이로드 976B < MTU)
#define AUDIO_MAX_CH           3
#define FRAME_FLAG_48K      0x01   // AUDIO flags bit0: 48kHz raw. bits4-5: (채널수-1)
// 24-bit → 16-bit 변환 시 디지털 게인: raw >> AUDIO_GAIN_SHIFT (16=x1, 13=x8) + 포화
// 상위 16-bit만 쓰면 일상 음량(~60dBSPL)이 RMS ~30 수준으로 유효비트 부족 (2026-07-28 실측)
#define AUDIO_GAIN_SHIFT      13

// ── DSP 파이프라인 (M2 C 포팅, 2026-09-10) ───────────────
// 좌표계 = tools/doa_prototype.py와 동일: 착용자 정면=+y=0°, 시계방향+, mic3=후방
#define MIC_POS_X          { -0.07f, +0.07f, 0.0f }
#define MIC_POS_Y          {  0.0f,   0.0f, -0.1212f }
#define SOUND_SPEED        343.0f
#define DSP_OUT_SAMPLES    240     // 16kHz 출력 프레임(15ms) — BLE notify 1개=프레임 1개 유지
#define DOA_WIN            1024    // GCC-PHAT 창 (21.3ms @48k), hop=창 크기(비중첩)
#define DOA_MAX_LAG        26      // 물리한계 ±19.6샘플(14cm) + 여유
#define DOA_GATE_MULT      2.5f    // 에너지 게이트: ambient EMA 대비 배수 (2026-09-10 실측 확립)
#define DOA_SUPPORT_TOL    3.0f    // 기하 지지 검사 허용오차 (샘플)
#define DOA_ALERT_MULT     8.0f    // 큰소리 이벤트(DOA_ALERT) 임계 — ambient 대비
#define DOA_ALERT_THROTTLE_MS 2000
// DAS 빔 가중/스티어링/HPF는 **모드 프리셋 5종**이 런타임 결정 (§5.3 — dsp_pipeline.cpp PRESETS 참조):
// 0 실내=광폭·HPF120 / 1 야외=광폭·HPF300(소음컷) / 2 TV=전방페어 0.45/0.45/0.10 / 3 일대일=0.4/0.4/0.2
// 4 다인=DOA 추적 스티어링. SET_VOLUME은 스트림 디지털 게인(2dB/스텝) — 2026-09-10 확장
// (HPF 근거: 마이크 럼블/DC 드리프트 0~100Hz가 상위 대역보다 +18~30dB 실측 — raw 경로·DOA는 미적용)

// ── 네트워크 ─────────────────────────────────────────────
#define UDP_AUDIO_PORT      5005   // 오디오+이벤트 (CLAUDE.md §6.2)
#define TCP_CTRL_PORT       5006   // 이미지+CMD+PING (M3부터)

// ── BLE (env:ble) ────────────────────────────────────────
// BLE는 3ch 48k raw(288KB/s)를 못 실으므로 ch1을 FIR+3:1 decimation한 16kHz mono(32KB/s) 전송
// 프레임 1개 = notify 1개 (MTU 517 협상 시 분할 없음 — CLAUDE.md §6.1 프레이밍 규칙)
#define BLE_AUDIO_SAMPLES    240   // 16kHz 15ms → 프레임 506B < 514B(MTU 517 유효)
#define BLE_IMG_CHUNK_BYTES  480   // IMG_CHUNK 프레임 494B < 514B — notify 1개=프레임 1개 유지
#define BLE_DEVICE_NAME     "SmartGlass-HA"

// ── 카메라 파라미터 ──────────────────────────────────────
#define CAM_PERIOD_MS     3000     // 주기 캡처 (WiFi=VGA JPEG, BLE=240x240 JPEG)
