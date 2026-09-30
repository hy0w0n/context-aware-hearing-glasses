// ble_link — env:ble 전송 계층 (NimBLE GATT 서버, CLAUDE.md §6.2)
// AUDIO(Notify): dsp_pipeline의 빔포밍 16kHz mono PCM16 + DOA 메타, 프레임 1개=notify 1개 (2026-09-10)
// CMD(Write): payload만 수신 (SET_MODE/VOL/ALERT/CAPTURE_NOW/SET_CONFIG → UI 디스패치)
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// GATT UUID (앱/수신기와 공유 — CLAUDE.md §6.2에 기록)
#define BLE_UUID_SERVICE "8f0e1000-04c4-4f4b-a0a5-1e2d3c4b5a69"
#define BLE_UUID_AUDIO   "8f0e1001-04c4-4f4b-a0a5-1e2d3c4b5a69"
#define BLE_UUID_CMD     "8f0e1002-04c4-4f4b-a0a5-1e2d3c4b5a69"
#define BLE_UUID_IMAGE   "8f0e1003-04c4-4f4b-a0a5-1e2d3c4b5a69"
#define BLE_UUID_EVENT   "8f0e1004-04c4-4f4b-a0a5-1e2d3c4b5a69"

// NimBLE 초기화 + 광고 시작 + 오디오/이미지 태스크(코어0) 생성
// beamQueue = dsp_pipeline의 dsp_frame_t 큐 (16kHz mono 빔포밍 + DOA 메타)
void ble_link_start(QueueHandle_t beamQueue);

// EVENT notify (프레임 전체 = notify 1개). GATT 링크 계층이 재전송하므로 blind 2회 불필요
void ble_link_sendEvent(uint8_t subtype, const uint8_t *data, size_t len);

bool ble_link_connected();
bool ble_link_subscribed();
void ble_link_stats(uint32_t &txFrames, uint32_t &txBytes, uint32_t &txFail);
