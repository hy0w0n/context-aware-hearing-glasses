// adpcm — IMA ADPCM 4:1 인코더 (CLAUDE.md §6.1 AUDIO fmt=1, 2026-09-10)
// 16kHz mono PCM16 256kbps → 64kbps. 정수 연산만 사용(부하 미미).
// 패킷 단위 리싱크: 인코딩 직전 상태(predictor+step_idx)를 매 패킷 헤더에 실어
// 손실 허용 전송(UDP/Notify)에서 패킷 하나가 사라져도 다음 패킷부터 정상 복원.
#pragma once
#include <Arduino.h>

typedef struct {
  int16_t predictor;   // 직전 복원 샘플 값
  uint8_t stepIdx;     // step size 테이블 인덱스 (0~88)
} adpcm_state_t;

// NVS에서 활성 플래그 로드 (부팅 시 1회)
void adpcm_init();
bool adpcm_enabled();
void adpcm_setEnabled(bool on);   // NVS 저장 — 재부팅에도 유지 (beam 모드와 동일 이유)

// samples[n] (n=짝수) → out[n/2] 니블 패킹(첫 샘플=하위 니블, IMA/WAV 관례).
// stateOut = 인코딩 "직전" 상태 스냅샷 (패킷 헤더용). 내부 상태는 스트림 연속 유지
void adpcm_encode(const int16_t *samples, int n, uint8_t *out, adpcm_state_t *stateOut);
