// camera_test — M3 스모크 테스트: FPC 카메라(DCX-OV2640-V2) init + JPEG 캡처 확인
// GOOUUU ESP32-S3-CAM 보드: 카메라 핀 = CAMERA_MODEL_ESP32S3_EYE (config.h와 동일, 2026-08-11 확인)
#pragma once

// 카메라 lazy init (여러 모듈이 공용 — img_link, cam 명령). 성공/이미 초기화 시 true
bool camera_ensure_init();

// 시리얼 명령 `cam`에서 호출: init + 1프레임 캡처, 결과 로그. 성공 시 true
bool camera_test_capture();

// 이미지 프리셋 — 속도 개선 A/B용 런타임 전환, NVS 유지 (proto.md §7.5, 2026-10-01)
// 해상도: true=QVGA(320x240, ~3배 작음 — 모델 입력 224라 무손실), false=VGA(기본)
void camera_set_qvga(bool qvga);
// JPEG 품질(OV2640 스케일 10~40, 낮을수록 고화질·큼. 기본 12, 속도용 18 권장)
void camera_set_quality(int q);
// 모노크롬 전송 (OV2640 grayscale 효과 — JPEG 크로마 제거로 10~25% 추가 절감.
// 이미지 모델이 흑백 학습이라 정확도 무손실 — 2026-10-01 확정)
void camera_set_gray(bool gray);
// 현재 설정 문자열 (stat/info 표시용)
const char *camera_preset_str();
