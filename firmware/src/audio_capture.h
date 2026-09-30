// audio_capture — I2S0에서 INMP441 캡처 (M1: mic1 단독, Left 슬롯)
// 캡처는 WiFi 없이도 항상 동작 → 시리얼 RMS 로그로 배선 검증 가능 (구 스펙 1단계 검증)
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"

typedef struct {
  uint32_t t_ms;                        // 부팅 기준 ms (프로토콜 AUDIO.t_ms)
  uint8_t  channels;                    // 1~3, 샘플은 채널 인터리브
  uint16_t nsamples;                    // 총 int16 수 (frames × channels)
  int16_t  samples[AUDIO_BLOCK_SAMPLES];
} audio_block_t;

// I2S0 초기화 + 캡처 태스크(코어1) 시작. 블록을 outQueue로 push (가득 차면 최신 유지)
bool audio_capture_start(QueueHandle_t outQueue);

// 최근 블록 채널별 통계 (시리얼 모니터용). 배열 크기 = AUDIO_MAX_CH
void audio_capture_stats(float rms[], int16_t mn[], int16_t mx[], uint32_t &overruns);

// 최근 샘플 N개를 시리얼로 덤프 (파형 육안 확인용, `dump` 명령)
void audio_capture_dump(size_t n);

// I2S1(slave, mic3)이 클럭을 못 받아 멈춰 있으면 true (3ch 구성에서만 의미)
bool audio_capture_i2s1_stalled();
