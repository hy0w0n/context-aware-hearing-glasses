#include "audio_capture.h"
#include <math.h>
#include "log_util.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_periph.h"  // GPIO_PIN_MUX_REG + PIN_INPUT_ENABLE

// ── I2S 구성 ─────────────────────────────────────────────────────────────
// I2S0 = master RX: mic1(L) + mic2(R), BCLK/WS를 핀 41/42로 출력
// I2S1 = slave  RX: mic3(L) — 클럭을 별도로 만들지 않고 같은 핀 41/42를 입력으로 공유
//   → 두 컨트롤러가 동일 BCLK/WS로 샘플링하므로 채널 간 drift가 원천적으로 없음 (CLAUDE.md §3)
//   → 같은 패드를 "I2S0 출력 + I2S1 입력"으로 동시에 쓰기 위해 GPIO를 INPUT_OUTPUT 모드로 강제
// 시작 순서: slave enable(클럭 대기) → master enable(클럭 시작) → 두 쪽이 같은 첫 WS에서 출발
// 잔여 시작 오프셋(±수 샘플)은 클랩 테스트로 실측 후 보정 (tools/sync_check.py)

static i2s_chan_handle_t s_rx0 = nullptr;  // I2S0 master (mic1+mic2)
#if AUDIO_CHANNELS == 3
static i2s_chan_handle_t s_rx1 = nullptr;  // I2S1 slave (mic3)
static volatile bool s_i2s1Stall = false;
#endif
static QueueHandle_t s_out = nullptr;

static volatile float    s_rms[AUDIO_MAX_CH] = {0};
static volatile int16_t  s_min[AUDIO_MAX_CH] = {0};
static volatile int16_t  s_max[AUDIO_MAX_CH] = {0};
static volatile uint32_t s_overruns = 0;

// dump용 최근 블록 사본 (캡처 태스크만 씀, 읽기는 근사치라도 무방)
static int16_t s_lastBlock[AUDIO_BLOCK_SAMPLES];
static volatile uint16_t s_lastN = 0;

static void captureTask(void *) {
  constexpr int FRAMES = AUDIO_BLOCK_SAMPLES / AUDIO_CHANNELS;  // 3ch=160(3.3ms), 2ch=240, 1ch=480
  static int32_t raw0[FRAMES * 2];
#if AUDIO_CHANNELS == 3
  static int32_t raw1[FRAMES * 2];
#endif
  static audio_block_t blk;
  int warmup = 8;  // 시작 직후 블록은 정렬 전 쓰레기일 수 있어 버림

  for (;;) {
    size_t br0 = 0;
    if (i2s_channel_read(s_rx0, raw0, sizeof(raw0), &br0, portMAX_DELAY) != ESP_OK) continue;
    const int frames = br0 / 8;  // I2S 프레임 수 (L 4B + R 4B)

#if AUDIO_CHANNELS == 3
    // 공유 클럭이므로 I2S1에도 같은 수의 프레임이 이미 도착해 있어야 함 — 타임아웃이면 클럭 공유 실패
    size_t br1 = 0;
    if (i2s_channel_read(s_rx1, raw1, sizeof(raw1), &br1, pdMS_TO_TICKS(200)) != ESP_OK || br1 < br0) {
      s_i2s1Stall = true;
      memset(raw1, 0, sizeof(raw1));
    } else {
      s_i2s1Stall = false;
    }
#endif

    int64_t acc[AUDIO_MAX_CH] = {0};
    int16_t mn[AUDIO_MAX_CH], mx[AUDIO_MAX_CH];
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) { mn[ch] = INT16_MAX; mx[ch] = INT16_MIN; }

    for (int f = 0; f < frames; f++) {
      for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        // ch0/ch1 = I2S0의 L/R, ch2 = I2S1의 L. INMP441 32-bit 슬롯 상위 24-bit 유효
        // → 디지털 게인(AUDIO_GAIN_SHIFT) 적용 후 16-bit 포화 축소 (config.h 참고)
#if AUDIO_CHANNELS == 3
        const int32_t r = (ch < 2) ? raw0[2 * f + ch] : raw1[2 * f];
#else
        const int32_t r = raw0[2 * f + ch];
#endif
        int32_t v = r >> AUDIO_GAIN_SHIFT;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        const int16_t s16 = (int16_t)v;
        blk.samples[f * AUDIO_CHANNELS + ch] = s16;
        acc[ch] += (int32_t)s16 * s16;
        if (s16 < mn[ch]) mn[ch] = s16;
        if (s16 > mx[ch]) mx[ch] = s16;
      }
    }
    blk.channels = AUDIO_CHANNELS;
    blk.nsamples = (uint16_t)(frames * AUDIO_CHANNELS);
    blk.t_ms = millis();

    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
      s_rms[ch] = sqrtf((float)(acc[ch] / (frames > 0 ? frames : 1)));
      s_min[ch] = mn[ch];
      s_max[ch] = mx[ch];
    }
    memcpy(s_lastBlock, blk.samples, blk.nsamples * sizeof(int16_t));
    s_lastN = blk.nsamples;

    if (warmup > 0) { warmup--; continue; }

    // 큐가 가득 차면(소비자 없음/느림) 가장 오래된 블록을 버리고 최신을 유지 — 스트림은 항상 "현재" 소리
    if (xQueueSend(s_out, &blk, 0) != pdTRUE) {
      audio_block_t dropped;
      xQueueReceive(s_out, &dropped, 0);
      xQueueSend(s_out, &blk, 0);
      s_overruns = s_overruns + 1;  // 구독자 활성 중에 증가하면 전송 병목 신호
    }
  }
}

bool audio_capture_start(QueueHandle_t outQueue) {
  s_out = outQueue;
  constexpr int FRAMES = AUDIO_BLOCK_SAMPLES / AUDIO_CHANNELS;
  esp_err_t err;

  i2s_chan_config_t c0 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  c0.dma_desc_num = 6;
  c0.dma_frame_num = FRAMES;
  if ((err = i2s_new_channel(&c0, nullptr, &s_rx0)) != ESP_OK) {
    Log.printf("[i2s] ERR new ch0: 0x%x\n", err);
    return false;
  }

  i2s_std_config_t cfg = {};
  cfg.clk_cfg.sample_rate_hz = AUDIO_CAPTURE_HZ;
  cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
  cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.bclk = (gpio_num_t)I2S0_PIN_BCLK;
  cfg.gpio_cfg.ws   = (gpio_num_t)I2S0_PIN_WS;
  cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.din  = (gpio_num_t)I2S0_PIN_SD;

#if AUDIO_CHANNELS == 3
  i2s_chan_config_t c1 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
  c1.dma_desc_num = 6;
  c1.dma_frame_num = FRAMES;
  if ((err = i2s_new_channel(&c1, nullptr, &s_rx1)) != ESP_OK) {
    Log.printf("[i2s] ERR new ch1: 0x%x\n", err);
    return false;
  }

  // ⚠ 초기화 순서가 핵심 (2026-07-28 실측): slave의 gpio_config가 41/42의 출력 신호 라우팅을
  // 끊어버리므로(SIG_GPIO_OUT으로 재지정), 반드시 slave를 먼저 init하고 master를 나중에 init해서
  // master가 클럭 출력 라우팅을 마지막에 다시 세우게 한다.
  i2s_std_config_t cfg1 = cfg;                       // 동일 rate/slot 포맷
  cfg1.gpio_cfg.din = (gpio_num_t)I2S1_PIN_SD;       // 데이터만 mic3 라인
  if ((err = i2s_channel_init_std_mode(s_rx1, &cfg1)) != ESP_OK) {
    Log.printf("[i2s] ERR init slave: 0x%x\n", err);
    return false;
  }
#endif

  if ((err = i2s_channel_init_std_mode(s_rx0, &cfg)) != ESP_OK) {  // master를 마지막에 init
    Log.printf("[i2s] ERR init master: 0x%x\n", err);
    return false;
  }

#if AUDIO_CHANNELS == 3
  // 같은 패드(41/42)를 I2S0 출력 + I2S1 입력으로 동시 사용.
  // ⚠ gpio_set_direction()은 출력 신호 선택(out_sel)을 SIG_GPIO_OUT으로 리셋해 I2S0 클럭을 끊으므로 금지!
  // 대신 IO_MUX의 입력 인에이블(FUN_IE) 비트만 켜고, I2S1 입력 신호를 GPIO matrix로 직접 라우팅한다.
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[I2S0_PIN_BCLK]);
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[I2S0_PIN_WS]);
  esp_rom_gpio_connect_in_signal(I2S0_PIN_BCLK, I2S1I_BCK_IN_IDX, false);
  esp_rom_gpio_connect_in_signal(I2S0_PIN_WS,   I2S1I_WS_IN_IDX,  false);

  if ((err = i2s_channel_enable(s_rx1)) != ESP_OK) {  // slave 먼저 arm (클럭 대기)
    Log.printf("[i2s] ERR enable slave: 0x%x\n", err);
    return false;
  }
#endif

  if ((err = i2s_channel_enable(s_rx0)) != ESP_OK) {  // master 클럭 시작 → 동시 출발
    Log.printf("[i2s] ERR enable master: 0x%x\n", err);
    return false;
  }

  // 오디오 실시간 경로는 코어1 (CLAUDE.md §5.1)
  xTaskCreatePinnedToCore(captureTask, "audio_cap", 8192, nullptr, 10, nullptr, 1);
  return true;
}

void audio_capture_stats(float rms[], int16_t mn[], int16_t mx[], uint32_t &overruns) {
  for (int ch = 0; ch < AUDIO_MAX_CH; ch++) {
    rms[ch] = s_rms[ch];
    mn[ch] = s_min[ch];
    mx[ch] = s_max[ch];
  }
  overruns = s_overruns;
}

void audio_capture_dump(size_t n) {
  const uint16_t have = s_lastN;
  if (n > have) n = have;
  Log.printf("[dump] %u samples (%dch interleaved): ", (unsigned)n, AUDIO_CHANNELS);
  for (size_t i = 0; i < n; i++) Log.printf("%d ", s_lastBlock[i]);
  Log.println();
}

bool audio_capture_i2s1_stalled() {
#if AUDIO_CHANNELS == 3
  return s_i2s1Stall;
#else
  return false;
#endif
}
