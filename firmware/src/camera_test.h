// camera_test — M3 스모크 테스트: FPC 카메라(DCX-OV2640-V2) init + JPEG 캡처 확인
// GOOUUU ESP32-S3-CAM 보드: 카메라 핀 = CAMERA_MODEL_ESP32S3_EYE (config.h와 동일, 2026-08-11 확인)
#pragma once

// 카메라 lazy init (여러 모듈이 공용 — img_link, cam 명령). 성공/이미 초기화 시 true
bool camera_ensure_init();

// 시리얼 명령 `cam`에서 호출: init + 1프레임 캡처, 결과 로그. 성공 시 true
bool camera_test_capture();
