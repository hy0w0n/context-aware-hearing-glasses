// dsp_pipeline.cpp — M2 DSP C 포팅 (설계: CLAUDE.md §5.2, 검증: proto.md 2026-09-10 작업 로그)
// FFT는 자체 radix-2 float 구현 — esp-dsp 어셈블리 최적화는 부하 실측 후 필요 시 교체
// (모든 DSP 버퍼는 내부 SRAM 정적 할당 — PSRAM 캐시 미스 회피, §5.1)
#include "dsp_pipeline.h"
#include <math.h>
#include <string.h>
#include "log_util.h"

// ── 상태: 큐/출력 ──────────────────────────────────────────
static QueueHandle_t s_capQ = nullptr, s_rawQ = nullptr, s_beamQ = nullptr;
static volatile int16_t s_azOut01 = 0;
static volatile uint8_t s_confOut = 0;
static uint8_t s_histOut[8] = {0};
static volatile uint32_t s_lastValidMs = 0;
static volatile int s_busOffset = 0;

static volatile uint32_t s_runs = 0, s_avgUs = 0, s_valid = 0;

// DOA_ALERT (§6.1 EVENT 0x02)
static volatile bool s_alertPending = false;
static int16_t s_alertAz01 = 0;
static uint8_t s_alertDb = 0;
static uint32_t s_alertTms = 0;
static uint32_t s_alertLastMs = 0;

// ── 빔포밍(DAS) 상태 ──────────────────────────────────────
#define BEAM_HIST 32                       // ≥ 최대 스티어링 지연(임의 방향 ≤20샘플)
static int16_t s_bhist[BEAM_HIST][3];      // 직전 블록 마지막 프레임들 (지연 참조용)
static volatile int s_bdelay[3];           // 채널별 정렬 지연 (스티어링 방향에 따라 런타임 변경)
static volatile int16_t s_bwrt[3];         // 채널 가중 Q15 (모드 프리셋이 런타임 변경)
static const float s_px[3] = MIC_POS_X;
static const float s_py[3] = MIC_POS_Y;

// ── 모드 프리셋 (§5.3 — SET_MODE 0~4, 2026-09-10) ────────
typedef struct {
  int16_t w[3];      // 채널 가중 Q15 (합≈1.0)
  float steerAz;     // 스티어링 방위각 (0=정면)
  bool followDoa;    // true=빔이 DOA 추적 (다인 대화)
  float hpfHz;       // 출력 HPF 컷오프
} beam_preset_t;
static const beam_preset_t PRESETS[5] = {
    /*0 실내: 광폭(균등)          */ {{10923, 10923, 10922}, 0.0f, false, 120.0f},
    /*1 야외: 광폭+저역 소음컷    */ {{10923, 10923, 10922}, 0.0f, false, 300.0f},
    /*2 TV:  전방 페어 강화       */ {{14746, 14746, 3276},  0.0f, false, 150.0f},
    /*3 1:1: 전방 협빔            */ {{13107, 13107, 6554},  0.0f, false, 150.0f},
    /*4 다인: 전방 협빔+DOA 추적  */ {{13107, 13107, 6554},  0.0f, true,  150.0f},
};
static const char *PRESET_NAMES[5] = {"실내(광폭)", "야외(광폭+HPF300)", "TV(전방페어)",
                                      "일대일(전방협빔)", "다인(DOA추적)"};
static volatile int s_mode = 0;
static volatile bool s_followDoa = false;
static float s_steerAz = 0.0f;             // 현재 빔 방향 (0~360)
static volatile float s_hpfPend = -1.0f;   // >0이면 dsp 태스크가 블록 경계에서 HPF 재설계
static volatile int s_volStep = 0;         // SET_VOLUME 누적 (±15, 2dB/스텝)
static volatile int32_t s_volQ8 = 256;     // 선형 게인 Q8

// 3:1 FIR decimator (33탭 Q15, cutoff 7kHz — 구 ble_link 구현 이관, 상태 블록 간 이월)
static const int16_t FIR[33] = {
    45, 57, 22, -77, -186, -164, 98, 481, 608, 122, -878, -1641, -1125, 1224, 4849, 8181,
    9534, 8181, 4849, 1224, -1125, -1641, -878, 122, 608, 481, 98, -164, -186, -77, 22, 57, 45};
static int16_t s_fhist[32] = {0};
static int s_fphase = 0;

static dsp_frame_t s_outFrame;
static int s_outN = 0;

// 출력 HPF (2차 Butterworth 하이패스 biquad, DSP_HPF_HZ) — 럼블 제거 (2026-09-10 진단)
static float s_hb0, s_hb1, s_hb2, s_ha1, s_ha2;
static float s_hx1 = 0, s_hx2 = 0, s_hy1 = 0, s_hy2 = 0;

static void hpf_init(float hz) {
  const float w0 = 2.0f * (float)M_PI * hz / AUDIO_CAPTURE_HZ;
  const float cw = cosf(w0), sw = sinf(w0);
  const float alpha = sw / (2.0f * 0.70710678f);  // Q=1/√2 Butterworth
  const float a0 = 1.0f + alpha;
  s_hb0 = (1.0f + cw) / 2.0f / a0;
  s_hb1 = -(1.0f + cw) / a0;
  s_hb2 = s_hb0;
  s_ha1 = -2.0f * cw / a0;
  s_ha2 = (1.0f - alpha) / a0;
  s_hx1 = 0; s_hx2 = 0; s_hy1 = 0; s_hy2 = 0;
}

static inline int16_t hpf_step(int16_t in) {
  const float x = (float)in;
  float y = s_hb0 * x + s_hb1 * s_hx1 + s_hb2 * s_hx2 - s_ha1 * s_hy1 - s_ha2 * s_hy2;
  s_hx2 = s_hx1; s_hx1 = x;
  s_hy2 = s_hy1; s_hy1 = y;
  if (y > 32767.0f) y = 32767.0f;
  if (y < -32768.0f) y = -32768.0f;
  return (int16_t)y;
}

// ── DOA 상태 ──────────────────────────────────────────────
static int16_t s_ring[DOA_WIN][3];         // 3ch 순환 창 (6KB)
static int s_ringWr = 0, s_ringNew = 0;
static float s_re[3][DOA_WIN], s_im[3][DOA_WIN];   // 채널 스펙트럼 (24KB)
static float s_cre[DOA_WIN], s_cim[DOA_WIN];       // 크로스 스펙트럼 작업 (8KB)
static float s_hann[DOA_WIN];
static uint16_t s_bitrev[DOA_WIN];
static float s_twc[DOA_WIN / 2], s_tws[DOA_WIN / 2];
#define NPAIR 3
static const int PAIR_I[NPAIR] = {0, 0, 1};
static const int PAIR_J[NPAIR] = {1, 2, 2};
static float s_tabLag[360][NPAIR];         // 1° 그리드 기대 lag (doa_prototype.py와 동일)
#define NLAG (2 * DOA_MAX_LAG + 1)
static float s_curve[NPAIR][NLAG];

static float s_ambient = 0.0f;             // 조용한 구간 에너지 EMA (에너지 게이트 기준)
static int s_warm = 0;

// 최근 추정 링 (3초 창 스무딩 → azimuth/conf/hist)
#define EST_N 16
static float s_estAz[EST_N];
static uint32_t s_estMs[EST_N];
static int s_estWr = 0;

// ── FFT (radix-2, in-place) ───────────────────────────────
static void fft_init() {
  const int N = DOA_WIN;
  int bits = 0;
  while ((1 << bits) < N) bits++;
  for (int i = 0; i < N; i++) {
    uint32_t r = 0;
    for (int b = 0; b < bits; b++)
      if (i & (1 << b)) r |= 1u << (bits - 1 - b);
    s_bitrev[i] = (uint16_t)r;
  }
  for (int k = 0; k < N / 2; k++) {
    s_twc[k] = cosf(2.0f * (float)M_PI * k / N);
    s_tws[k] = sinf(2.0f * (float)M_PI * k / N);
  }
  for (int k = 0; k < N; k++)
    s_hann[k] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * k / (N - 1)));
}

static void fft(float *re, float *im, bool inv) {
  const int N = DOA_WIN;
  for (int i = 0; i < N; i++) {
    const int j = s_bitrev[i];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= N; len <<= 1) {
    const int half = len >> 1;
    const int step = N / len;
    for (int i = 0; i < N; i += len) {
      for (int k = 0; k < half; k++) {
        const int t = k * step;
        const float wr = s_twc[t];
        const float wi = inv ? s_tws[t] : -s_tws[t];   // forward: e^{-j2πt/N}
        const float br = re[i + k + half], bi = im[i + k + half];
        const float vr = br * wr - bi * wi;
        const float vi = br * wi + bi * wr;
        re[i + k + half] = re[i + k] - vr;
        im[i + k + half] = im[i + k] - vi;
        re[i + k] += vr;
        im[i + k] += vi;
      }
    }
  }
  if (inv) {
    const float s = 1.0f / N;
    for (int i = 0; i < N; i++) { re[i] *= s; im[i] *= s; }
  }
}

// DAS 스티어링: azDeg 방향에서 오는 소리가 정렬되도록 채널별 지연 계산 (모드 프리셋·DOA 추적 공용)
static void steer_to(float azDeg) {
  const float th = azDeg * (float)M_PI / 180.0f;
  const float ux = sinf(th), uy = cosf(th);
  float arr[3], amax = -1e9f;
  for (int c = 0; c < 3; c++) {
    arr[c] = -(s_px[c] * ux + s_py[c] * uy) / SOUND_SPEED * AUDIO_CAPTURE_HZ;
    if (arr[c] > amax) amax = arr[c];
  }
  for (int c = 0; c < 3; c++) {
    int d = (int)lroundf(amax - arr[c]);
    if (d < 0) d = 0;
    if (d > BEAM_HIST - 1) d = BEAM_HIST - 1;
    s_bdelay[c] = d;
  }
  s_steerAz = azDeg;
}

// ── 기하 테이블 초기화 (doa_prototype.py expected_lags와 동일 수식) ──
static void geometry_init() {
  for (int t = 0; t < 360; t++) {
    const float th = t * (float)M_PI / 180.0f;
    const float ux = sinf(th), uy = cosf(th);   // 0°=정면(+y), 시계+
    for (int p = 0; p < NPAIR; p++) {
      const int i = PAIR_I[p], j = PAIR_J[p];
      // lag(i,j) = -((p_i - p_j)·u)/c × fs
      s_tabLag[t][p] = -((s_px[i] - s_px[j]) * ux + (s_py[i] - s_py[j]) * uy)
                       / SOUND_SPEED * AUDIO_CAPTURE_HZ;
    }
  }
}

// ── DOA 1회 실행 (창 = 링버퍼 전체) ────────────────────────
static void doa_run(uint32_t blk_t_ms) {
  const uint32_t t0us = micros();
  const int N = DOA_WIN;

  // 1) 채널별 1차 차분(간이 HPF) + Hann → s_re/s_im, 에너지 합산
  float energy = 0.0f, sq = 0.0f;
  for (int c = 0; c < 3; c++) {
    float prev = (float)s_ring[s_ringWr % N][c];
    s_re[c][0] = 0.0f;
    s_im[c][0] = 0.0f;
    for (int k = 1; k < N; k++) {
      const float v = (float)s_ring[(s_ringWr + k) % N][c];
      const float hp = v - prev;
      prev = v;
      energy += fabsf(hp);
      sq += hp * hp;
      s_re[c][k] = hp * s_hann[k];
      s_im[c][k] = 0.0f;
    }
  }

  // 2) 에너지 게이트: 조용한 구간 EMA(ambient) 대비 배수 (조용할 땐 ambient만 갱신)
  if (s_warm < 10) {  // 초기 워밍업: ambient를 현재 수준으로 수렴
    s_ambient = (s_warm == 0) ? energy : (s_ambient * 0.7f + energy * 0.3f);
    s_warm++;
    return;
  }
  if (energy < s_ambient)
    s_ambient += 0.2f * (energy - s_ambient);     // 하강은 빠르게 (조용한 바닥 추적)
  else
    s_ambient += 0.005f * (energy - s_ambient);   // 상승은 느리게 (큰 소리에 안 끌려감)
  if (energy < DOA_GATE_MULT * s_ambient || energy < 3.0f * N) return;

  // 과부하 안전장치: FFT 6회 ≈ 11.7ms 실측(2026-09-10) — 캡처 큐가 밀려 있으면 이번 창은
  // 건너뛰어 오디오 경로를 보호 (esp-dsp 최적화 도입 시 재검토)
  if (s_capQ != nullptr && uxQueueMessagesWaiting(s_capQ) > 4) return;

  // 3) 채널 스펙트럼
  for (int c = 0; c < 3; c++) fft(s_re[c], s_im[c], false);

  // 4) 쌍별 GCC-PHAT 곡선 + 피크 명확도(clarity)
  float weights[NPAIR];
  int peakLag[NPAIR];
  for (int p = 0; p < NPAIR; p++) {
    const int i = PAIR_I[p], j = PAIR_J[p];
    for (int k = 0; k < N; k++) {
      const float gr = s_re[i][k] * s_re[j][k] + s_im[i][k] * s_im[j][k];
      const float gi = s_im[i][k] * s_re[j][k] - s_re[i][k] * s_im[j][k];
      const float mag = sqrtf(gr * gr + gi * gi) + 1e-12f;
      s_cre[k] = gr / mag;
      s_cim[k] = gi / mag;
    }
    fft(s_cre, s_cim, true);
    for (int L = -DOA_MAX_LAG; L <= DOA_MAX_LAG; L++)
      s_curve[p][L + DOA_MAX_LAG] = fabsf(s_cre[(L + N) % N]);
    int best = 0;
    for (int k = 1; k < NLAG; k++)
      if (s_curve[p][k] > s_curve[p][best]) best = k;
    peakLag[p] = best - DOA_MAX_LAG;
    float second = 0.0f;
    for (int k = 0; k < NLAG; k++) {
      if (k >= best - 2 && k <= best + 2) continue;
      if (s_curve[p][k] > second) second = s_curve[p][k];
    }
    float clarity = s_curve[p][best] / (second + 1e-12f);
    weights[p] = (clarity > 5.0f) ? 5.0f : clarity;
  }

  // 5) 크로스토크 베토 (SD 배선 커플링 → ch1-ch3(lag=0)만 선명 — 2026-09-10 규명):
  //    실제 60°/240° 음원이면 ch2-ch3도 ±17샘플에서 선명해야 함. 아니면 해당 쌍 제외
  if (abs(peakLag[1]) <= 1 && weights[1] >= 2.0f &&
      (weights[2] < 1.5f || abs(peakLag[2]) <= 2))
    weights[1] = 0.0f;
  float wsum = weights[0] + weights[1] + weights[2];
  int nClear = 0;
  for (int p = 0; p < NPAIR; p++)
    if (weights[p] >= 1.5f) nClear++;
  if (wsum < 3.5f || nClear < 2) return;

  // 6) SRP-PHAT 채점 (1° 그리드, 버스 오프셋 보정 + 선형 보간)
  const float off[NPAIR] = {0.0f, (float)s_busOffset, (float)s_busOffset};
  int bestT = 0;
  float bestScore = -1.0f;
  for (int t = 0; t < 360; t++) {
    float score = 0.0f;
    for (int p = 0; p < NPAIR; p++) {
      float pos = s_tabLag[t][p] + off[p] + DOA_MAX_LAG;
      if (pos < 0.0f) pos = 0.0f;
      if (pos > NLAG - 1.001f) pos = NLAG - 1.001f;
      const int i0 = (int)pos;
      const float fr = pos - i0;
      score += weights[p] * (s_curve[p][i0] * (1.0f - fr) + s_curve[p][i0 + 1] * fr);
    }
    if (score > bestScore) { bestScore = score; bestT = t; }
  }

  // 7) 기하 지지 검사: 채택 각도의 기대 lag과 원시 피크가 ±3샘플 일치하는 쌍 ≥2
  int support = 0;
  for (int p = 0; p < NPAIR; p++)
    if (fabsf((float)peakLag[p] - (s_tabLag[bestT][p] + off[p])) <= DOA_SUPPORT_TOL) support++;
  if (support < 2) return;

  // 8) 유효 추정 → 링에 저장, 3초 창 벡터 평균으로 azimuth/conf/hist 갱신
  s_valid = s_valid + 1;
  const uint32_t now = millis();
  s_estAz[s_estWr] = (float)bestT;
  s_estMs[s_estWr] = now;
  s_estWr = (s_estWr + 1) % EST_N;

  float vx = 0.0f, vy = 0.0f;
  int n = 0;
  uint8_t hist[8] = {0};
  for (int k = 0; k < EST_N; k++) {
    if (s_estMs[k] == 0 || now - s_estMs[k] > 3000) continue;
    const float a = s_estAz[k] * (float)M_PI / 180.0f;
    vx += sinf(a);
    vy += cosf(a);
    const int b = ((int)((s_estAz[k] + 22.5f) / 45.0f)) % 8;  // bin 중심 0/45/90...°
    if (hist[b] < 255) hist[b]++;
    n++;
  }
  if (n > 0) {
    float az = atan2f(vx / n, vy / n) * 180.0f / (float)M_PI;  // -180..180
    const float conc = sqrtf(vx * vx + vy * vy) / n;           // 0..1 일관도
    float cf = conc * 255.0f * ((n >= 6) ? 1.0f : n / 6.0f);
    s_azOut01 = (int16_t)lroundf(az * 10.0f);
    s_confOut = (uint8_t)((cf > 255.0f) ? 255 : cf);
    memcpy(s_histOut, hist, 8);
    s_lastValidMs = now;

    // 다인 대화(모드 4): 빔이 화자 방향을 추적 — 신뢰도 충분+20° 이상 이동 시 재스티어 (§5.3)
    if (s_followDoa && s_confOut >= 80) {
      float sAz = az;
      if (sAz < 0) sAz += 360.0f;
      float diff = fabsf(sAz - s_steerAz);
      if (diff > 180.0f) diff = 360.0f - diff;
      if (diff > 20.0f) {
        steer_to(sAz);
        Log.printf("[dsp] 다인: 빔 스티어 → %.0f°\n", sAz);
      }
    }
  }

  // 9) 큰소리 이벤트 (스로틀 2초): 게이트+지지를 통과한 창이 ambient의 8배 이상
  if (energy >= DOA_ALERT_MULT * s_ambient && now - s_alertLastMs > DOA_ALERT_THROTTLE_MS) {
    s_alertLastMs = now;
    float azS = (float)bestT;
    if (azS > 180.0f) azS -= 360.0f;
    s_alertAz01 = (int16_t)lroundf(azS * 10.0f);
    const float rms = sqrtf(sq / (3.0f * N));
    float db = 20.0f * log10f(rms + 1.0f);
    if (db < 0) db = 0;
    if (db > 255) db = 255;
    s_alertDb = (uint8_t)db;
    s_alertTms = blk_t_ms;
    s_alertPending = true;
  }

  // 부하 통계 (EMA)
  const uint32_t us = micros() - t0us;
  s_avgUs = (s_runs == 0) ? us : (s_avgUs * 7 + us) / 8;
  s_runs = s_runs + 1;
}

// ── 블록 처리: DAS 빔포밍 → FIR 3:1 → 16k 프레임 출력 ─────
static void process_block(const audio_block_t &blk) {
  const int nch = blk.channels;
  const int frames = blk.nsamples / nch;
  if (frames <= 0) return;

  static int16_t mono48[AUDIO_BLOCK_SAMPLES];

  if (nch == 3) {
    int bd[3];
    int16_t bw[3];
    for (int c = 0; c < 3; c++) { bd[c] = s_bdelay[c]; bw[c] = s_bwrt[c]; }  // 블록 단위 스냅샷
    for (int f = 0; f < frames; f++) {
      int32_t acc = 0;
      for (int c = 0; c < 3; c++) {
        const int src = f - bd[c];
        const int16_t v = (src >= 0) ? blk.samples[src * 3 + c]
                                     : s_bhist[BEAM_HIST + src][c];
        acc += (int32_t)bw[c] * v;
      }
      acc >>= 15;
      if (acc > 32767) acc = 32767;
      if (acc < -32768) acc = -32768;
      mono48[f] = (int16_t)acc;
    }
    // 지연 참조 히스토리 갱신 (블록 마지막 BEAM_HIST 프레임)
    if (frames >= BEAM_HIST) {
      for (int k = 0; k < BEAM_HIST; k++)
        for (int c = 0; c < 3; c++)
          s_bhist[k][c] = blk.samples[(frames - BEAM_HIST + k) * 3 + c];
    } else {
      memmove(s_bhist, s_bhist[frames], (BEAM_HIST - frames) * 3 * sizeof(int16_t));
      for (int k = 0; k < frames; k++)
        for (int c = 0; c < 3; c++)
          s_bhist[BEAM_HIST - frames + k][c] = blk.samples[k * 3 + c];
    }
    // DOA 링 채우기 + 창이 차면 실행
    for (int f = 0; f < frames; f++) {
      for (int c = 0; c < 3; c++) s_ring[s_ringWr][c] = blk.samples[f * 3 + c];
      s_ringWr = (s_ringWr + 1) % DOA_WIN;
      s_ringNew++;
    }
    if (s_ringNew >= DOA_WIN) {
      s_ringNew = 0;
      doa_run(blk.t_ms);
    }
  } else {
    for (int f = 0; f < frames; f++) mono48[f] = blk.samples[f * nch];  // 3ch 아니면 ch1 통과
  }

  // 모드 프리셋의 HPF 변경은 블록 경계에서 적용 (계수 교체 중 레이스 방지)
  if (s_hpfPend > 0.0f) { hpf_init(s_hpfPend); s_hpfPend = -1.0f; }
  // 볼륨 게인(SET_VOLUME 누적, 2dB/스텝) → 출력 HPF (럼블 제거) → 33탭 FIR 3:1 decimation
  const int32_t vol = s_volQ8;
  for (int f = 0; f < frames; f++) {
    int32_t g = ((int32_t)mono48[f] * vol) >> 8;
    if (g > 32767) g = 32767;
    if (g < -32768) g = -32768;
    mono48[f] = hpf_step((int16_t)g);
  }
  static int16_t work[32 + AUDIO_BLOCK_SAMPLES];
  memcpy(work, s_fhist, sizeof(s_fhist));
  memcpy(work + 32, mono48, frames * sizeof(int16_t));
  for (int k = s_fphase; k < frames; k += 3) {
    int32_t acc = 0;
    const int base = 32 + k;
    for (int t = 0; t < 33; t++) acc += (int32_t)FIR[t] * work[base - t];
    if (s_outN == 0) {
      s_outFrame.t_ms = blk.t_ms;
      s_outFrame.azimuth01 = (millis() - s_lastValidMs < 3000) ? s_azOut01 : 0;
      s_outFrame.conf = (millis() - s_lastValidMs < 3000) ? s_confOut : 0;
      memcpy(s_outFrame.hist, s_histOut, 8);
    }
    s_outFrame.samples[s_outN++] = (int16_t)(acc >> 15);
    if (s_outN == DSP_OUT_SAMPLES) {
      s_outFrame.nsamples = DSP_OUT_SAMPLES;
      s_outN = 0;
      if (s_beamQ != nullptr && xQueueSend(s_beamQ, &s_outFrame, 0) != pdTRUE) {
        dsp_frame_t drop;
        xQueueReceive(s_beamQ, &drop, 0);   // drop-oldest — 스트림은 항상 "현재" 소리
        xQueueSend(s_beamQ, &s_outFrame, 0);
      }
    }
  }
  s_fphase = (3 - ((frames - s_fphase) % 3)) % 3;
  memcpy(s_fhist, &work[frames], sizeof(s_fhist));
}

static void dspTask(void *) {
  static audio_block_t blk;
  for (;;) {
    if (xQueueReceive(s_capQ, &blk, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    // 원본 3ch 전달 (WiFi raw 스트림/PC 도구용) — drop-oldest
    if (s_rawQ != nullptr && xQueueSend(s_rawQ, &blk, 0) != pdTRUE) {
      audio_block_t drop;
      xQueueReceive(s_rawQ, &drop, 0);
      xQueueSend(s_rawQ, &blk, 0);
    }
    process_block(blk);
  }
}

bool dsp_pipeline_start(QueueHandle_t capQ, QueueHandle_t rawQ, QueueHandle_t beamQ) {
  s_capQ = capQ;
  s_rawQ = rawQ;
  s_beamQ = beamQ;
  fft_init();
  geometry_init();
  dsp_pipeline_setMode(0);           // 기본 프리셋: 실내(광폭, HPF 120)
  hpf_init(PRESETS[0].hpfHz);        // 태스크 시작 전이라 pending 없이 직접 설계
  s_hpfPend = -1.0f;
  memset(s_bhist, 0, sizeof(s_bhist));
  memset(s_ring, 0, sizeof(s_ring));
  memset((void *)s_estMs, 0, sizeof(s_estMs));
  Log.printf("[dsp] 시작: DAS 빔(지연 %d/%d/%d) + SRP-PHAT(창 %d, ±%d샘플) + 모드 프리셋 5종\n",
             (int)s_bdelay[0], (int)s_bdelay[1], (int)s_bdelay[2], DOA_WIN, DOA_MAX_LAG);
  // 코어1, 캡처(10)보다 낮은 우선순위 — 오디오 실시간 경로 (CLAUDE.md §5.1)
  return xTaskCreatePinnedToCore(dspTask, "audio_dsp", 12288, nullptr, 8, nullptr, 1) == pdPASS;
}

void dsp_pipeline_getDoa(int16_t &az01, uint8_t &conf) {
  if (millis() - s_lastValidMs < 3000 && s_lastValidMs != 0) {
    az01 = s_azOut01;
    conf = s_confOut;
  } else {
    az01 = 0;
    conf = 0;
  }
}

bool dsp_pipeline_pollAlert(int16_t &az01, uint8_t &energyDb, uint32_t &t_ms) {
  if (!s_alertPending) return false;
  s_alertPending = false;
  az01 = s_alertAz01;
  energyDb = s_alertDb;
  t_ms = s_alertTms;
  return true;
}

void dsp_pipeline_stats(uint32_t &runs, uint32_t &avgUs, uint32_t &validWins) {
  runs = s_runs;
  avgUs = s_avgUs;
  validWins = s_valid;
}

void dsp_pipeline_setMode(uint8_t m) {
  if (m > 4) m = 4;
  s_mode = m;
  const beam_preset_t &p = PRESETS[m];
  for (int c = 0; c < 3; c++) s_bwrt[c] = p.w[c];
  s_followDoa = p.followDoa;
  if (!p.followDoa) steer_to(p.steerAz);
  s_hpfPend = p.hpfHz;   // dsp 태스크가 블록 경계에서 HPF 재설계
  Log.printf("[dsp] 프리셋 적용: %s\n", PRESET_NAMES[m]);
}

void dsp_pipeline_addVolume(int8_t delta) {
  int v = s_volStep + delta;
  if (v > 15) v = 15;
  if (v < -15) v = -15;
  s_volStep = v;
  s_volQ8 = (int32_t)lroundf(256.0f * powf(10.0f, v * 2.0f / 20.0f));  // 2dB/스텝
  Log.printf("[dsp] 볼륨 %+d스텝 (%+ddB, x%.2f)\n", v, v * 2, s_volQ8 / 256.0f);
}

void dsp_pipeline_setBusOffset(int off) {
  if (off < -4) off = -4;
  if (off > 4) off = 4;
  s_busOffset = off;
  Log.printf("[dsp] 버스 오프셋 = %d샘플\n", off);
}
