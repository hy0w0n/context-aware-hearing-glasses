// adpcm.cpp — IMA ADPCM 표준 인코더 (디코더는 PC tools/ima_adpcm.py · 앱 Proto.kt에 대칭 구현)
#include "adpcm.h"
#include <Preferences.h>
#include "log_util.h"

static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t INDEX_TABLE[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                       -1, -1, -1, -1, 2, 4, 6, 8};

static adpcm_state_t s_st = {0, 0};
static volatile bool s_on = false;

void adpcm_init() {
  Preferences p;
  p.begin("cfg", true);
  s_on = p.getBool("adpcm", false);
  p.end();
  if (s_on) Log.println("[adpcm] 활성 (NVS) — AUDIO fmt=1, 64kbps");
}

bool adpcm_enabled() { return s_on; }

void adpcm_setEnabled(bool on) {
  s_on = on;
  Preferences p;
  p.begin("cfg", false);
  p.putBool("adpcm", on);
  p.end();
  Log.printf("[adpcm] %s (재부팅에도 유지) — 오디오 %s\n",
             on ? "ON" : "OFF", on ? "IMA ADPCM 64kbps" : "PCM16 256kbps");
}

static inline uint8_t encode_one(int16_t sample) {
  const int16_t step = STEP_TABLE[s_st.stepIdx];
  int diff = (int)sample - (int)s_st.predictor;
  uint8_t nib = 0;
  if (diff < 0) {
    nib = 8;
    diff = -diff;
  }
  // 양자화 + 동일 로직으로 predictor 갱신 (인코더/디코더 상태 일치가 핵심)
  int delta = step >> 3;
  if (diff >= step) { nib |= 4; diff -= step; delta += step; }
  if (diff >= (step >> 1)) { nib |= 2; diff -= step >> 1; delta += step >> 1; }
  if (diff >= (step >> 2)) { nib |= 1; delta += step >> 2; }
  int pred = (int)s_st.predictor + ((nib & 8) ? -delta : delta);
  if (pred > 32767) pred = 32767;
  if (pred < -32768) pred = -32768;
  s_st.predictor = (int16_t)pred;
  int idx = (int)s_st.stepIdx + INDEX_TABLE[nib];
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  s_st.stepIdx = (uint8_t)idx;
  return nib;
}

void adpcm_encode(const int16_t *samples, int n, uint8_t *out, adpcm_state_t *stateOut) {
  *stateOut = s_st;  // 패킷 리싱크용: 이 패킷 첫 샘플 인코딩 "직전" 상태
  for (int i = 0; i < n; i += 2) {
    const uint8_t lo = encode_one(samples[i]);
    const uint8_t hi = (i + 1 < n) ? encode_one(samples[i + 1]) : 0;
    out[i / 2] = (uint8_t)(lo | (hi << 4));
  }
}
