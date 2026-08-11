// img_link — M3: TCP:5006 이미지 채널 (CLAUDE.md §6.2 — 이미지+CMD+PING=TCP)
// 클라이언트 접속 시 주기(CAM_PERIOD_MS) JPEG 캡처 → IMG_START/CHUNK/END 프레임 전송.
// 수신 CMD: 0x05=CAPTURE_NOW(즉시 캡처), PING(0x30)→PONG(0x31) 에코
#pragma once
#include <Arduino.h>

void img_link_start();  // TCP 서버 + 캡처/전송 태스크(코어0) 시작 — 중복 호출 안전
void img_link_stats(uint32_t &imgsSent, uint32_t &kbSent, bool &clientConnected);
