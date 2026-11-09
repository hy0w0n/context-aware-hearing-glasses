# 소리 상황 분류 모델 (Edge Impulse, v5)

안호연이 Edge Impulse로 학습한 소리 분류 모델입니다. 스마트폰 앱은 `android/app/src/main/assets/soundModel.tflite`(이 폴더의 `.tflite`와 같은 파일)를 LiteRT로 실행합니다.

| 항목 | 값 |
|---|---|
| 파일 | `tflite-model/tflite_learn_1062911_90.tflite` (INT8 양자화, 490 KB) |
| 클래스 | `danger`(위험) · `noise`(소음) · `speech`(말소리) |
| 입력 소리 | 16 kHz 모노, 2.0초(32,000샘플) |
| 특징 | MFE: 프레임 20 ms · 간격 10 ms · FFT 256 · 멜 필터 32개 · 잡음 바닥 −52 dB → 199프레임 × 32 = 6,368개 |
| 앱의 전처리 | 프리엠퍼시스(0.98) → ÷32768 → 2초 창마다 최대 크기 맞춤. 소리가 작으면(RMS 150 미만) 모델을 돌리지 않고 무음으로 판정 |

정확도와 측정 조건은 [README의 측정 결과](../README.md#ai-모델)에 있습니다. 앱 쪽 구현은 `android/.../SoundClassifier.kt`(분류)와 `MelFeatures.kt`(특징 계산), PC 평가는 `tools/sound_eval.py`에 있습니다.

특징 계산은 Edge Impulse가 공개한 처리 블록([processing-blocks](https://github.com/edgeimpulse/processing-blocks), The Clear BSD License)과 speechpy(Apache-2.0)의 계산 순서를 근거로 직접 작성했습니다(2026-11-09). 이전 구현과 비교해 2초 창 542개에서 모델 출력이 모두 같았습니다.

## Edge Impulse 생성 파일을 넣지 않은 이유

Edge Impulse에서 모델을 내보내면 C++ 라이브러리(`model-parameters/`, `tflite-model/*.cpp`·`*.h`, SDK의 일부 파일)가 함께 생성됩니다. 무료(Developer) 플랜 약관에서 이 생성 파일은 Edge Impulse의 SDK에 해당하고 공개 배포가 금지되어 있어, 저장소와 이전 이력에서 뺐습니다(2026-11-09). 학습된 모델 자체는 약관상 만든 사람의 결과물(Output)이라 그대로 둡니다.

앱과 PC 도구는 `.tflite`만 쓰므로 생성 파일이 없어도 동작합니다. 원본 내보내기 파일은 팀 내부에 보관합니다.
