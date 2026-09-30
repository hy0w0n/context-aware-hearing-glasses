// wifi_link — env:wifi 전송 계층 (M1: UDP 오디오 스트림 + SUB 구독 + PING 에코)
// 자격증명은 NVS(Preferences) 저장 — 하드코딩 금지 (CLAUDE.md Phase 4 원칙 선반영)
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

bool wifi_link_haveCreds();
void wifi_link_setCreds(const String &ssid, const String &pass);
void wifi_link_forget();

// STA 접속 (blocking). 성공 시 IP 시리얼 출력 + true
bool wifi_link_connect(uint32_t timeoutMs = 15000);

// UDP 소켓 오픈 + 송신 태스크(코어0) 시작. connect 성공 후 1회 호출
// rawQueue = 3ch 48k 원본(audio_block_t, PC 도구용), beamQueue = dsp_frame_t(16k 빔포밍+DOA 메타)
void wifi_link_start(QueueHandle_t rawQueue, QueueHandle_t beamQueue);

// 스트림 선택 (기본 raw): true면 빔포밍 16kHz mono 전송 — 시리얼 'beam'/'raw' 명령
void wifi_link_setBeamMode(bool beam);
bool wifi_link_beamMode();

bool wifi_link_running();
void wifi_link_stats(uint32_t &txFrames, uint32_t &txBytes, bool &hasSubscriber);

// EVENT 프레임 전송 — UDP 손실 대비 blind 2회(50ms 간격, §6.1 프레이밍 규칙). subtype: 0x01=BUTTON 등
void wifi_link_sendEvent(uint8_t subtype, const uint8_t *data, size_t len);
