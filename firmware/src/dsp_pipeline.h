// dsp_pipeline — M2 DSP C 포팅 (2026-09-10)
// 3ch 48kHz 블록 → ① DAS 빔포밍(전방 고정 광폭 빔) → 33탭 FIR 3:1 decimation → 16kHz mono
//                → ② SRP-PHAT DOA (게이트 3종: 에너지·기하 지지·크로스토크 베토 — tools/doa_prototype.py 포팅)
// 알고리즘·게이트는 2026-09-10 안경 실장 8방향 검증(평균 9.1°)에서 확립된 것과 동일 (proto.md 작업 로그)
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"
#include "audio_capture.h"

typedef struct {
  uint32_t t_ms;                       // 프레임 첫 샘플의 캡처 시각 (부팅 기준 ms)
  uint16_t nsamples;                   // 16kHz mono 샘플 수 (== DSP_OUT_SAMPLES)
  int16_t  azimuth01;                  // 최근 DOA, 0.1° 단위 -1800~1800 (conf=0이면 무효)
  uint8_t  conf;                       // 0~255 (최근 3초 추정 일관도 × 표본 수)
  uint8_t  hist[8];                    // 8방향 히스토그램 (최근 3초 추정 분포, bin 중심 0/45/...°)
  int16_t  samples[DSP_OUT_SAMPLES];
} dsp_frame_t;

// capQ(3ch 48k audio_block_t) 소비 → rawQ로 원본 전달(nullptr 허용, WiFi raw 스트림용)
// + beamQ로 dsp_frame_t 출력. 태스크는 코어1(캡처보다 낮은 우선순위)
bool dsp_pipeline_start(QueueHandle_t capQ, QueueHandle_t rawQ, QueueHandle_t beamQ);

// 최근 DOA (3초 내 유효 추정 없으면 conf=0)
void dsp_pipeline_getDoa(int16_t &az01, uint8_t &conf);

// 큰소리 이벤트(DOA_ALERT §6.1) — 있으면 true + 인자 채움. 내부 스로틀 2초
bool dsp_pipeline_pollAlert(int16_t &az01, uint8_t &energyDb, uint32_t &t_ms);

// 모드 프리셋 (SET_MODE 0~4, §5.3 — 2026-09-10): 빔 가중/스티어링/HPF 전환.
// 0 실내=광폭 / 1 야외=광폭+HPF300(소음컷) / 2 TV=전방페어 강화 / 3 일대일=전방 협빔 / 4 다인=DOA 추적 빔
void dsp_pipeline_setMode(uint8_t mode);

// SET_VOLUME 델타 누적(±15 클램프, 2dB/스텝) → 16k 출력 스트림 디지털 게인 (통화 VOL- 실효화)
void dsp_pipeline_addVolume(int8_t delta);

// 부하/게이트 통계: DOA 실행 횟수, 1회 평균 소요 µs, 게이트 통과(유효) 창 수
void dsp_pipeline_stats(uint32_t &runs, uint32_t &avgUs, uint32_t &validWins);

// 버스 간 시작 오프셋(샘플) — 부팅마다 다름(proto.md 2026-09-10 실증). 기본 0.
// 재부팅 후 클랩 1회 → tools/sync_check.py의 ch1-ch3 값으로 시리얼 'doaoff <n>' 설정
void dsp_pipeline_setBusOffset(int off);
