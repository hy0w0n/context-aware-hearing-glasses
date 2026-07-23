# 1. 최소 요구 CMake 버전 설정
cmake_minimum_required(VERSION 3.18.1)

# 2. 프로젝트 이름 설정
project("smarthearing_edge")

# 3. C++ 표준을 14로 설정
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. 소스 디렉토리 변수 설정
set(MODEL_FOLDER ${CMAKE_CURRENT_SOURCE_DIR})

# 5. 헤더 파일 경로들을 컴파일러에게 등록
include_directories(
        ${MODEL_FOLDER}
        ${MODEL_FOLDER}/edge-impulse-sdk
        ${MODEL_FOLDER}/edge-impulse-sdk/classifier
        ${MODEL_FOLDER}/edge-impulse-sdk/porting
        ${MODEL_FOLDER}/model-parameters
        ${MODEL_FOLDER}/tflite-model
)

# 6. 🚨 에러 -24 원인 제거
# 억지로 메모리를 맞추거나 외부 버퍼를 기대하는 옵션들을 싹 지웠습니다.
# 이제 인공지능 뇌가 자신에게 딱 맞는 최적의 메모리를 알아서 가져갑니다.
add_definitions(
        -DEIDSP_USE_CMSIS_DSP=0
        -DEIDSP_QUANTIZE_FILTERBANK=0
        -DNDEBUG
)

# 7. edge-impulse-sdk 내부의 필수 C++ 소스 파일들을 한데 묶음
file(GLOB_RECURSE EI_SDK_SOURCES
        "${MODEL_FOLDER}/edge-impulse-sdk/dsp/kissfft/*.cpp"
        "${MODEL_FOLDER}/edge-impulse-sdk/dsp/kissfft/*.c"
        "${MODEL_FOLDER}/edge-impulse-sdk/dsp/dct/*.cpp"
        "${MODEL_FOLDER}/edge-impulse-sdk/dsp/dct/*.c"
        "${MODEL_FOLDER}/edge-impulse-sdk/tensorflow/**/*.cpp"
        "${MODEL_FOLDER}/edge-impulse-sdk/tensorflow/**/*.cc"
        "${MODEL_FOLDER}/edge-impulse-sdk/tensorflow/**/*.c"
        "${MODEL_FOLDER}/edge-impulse-sdk/porting/android/*.cpp"
        "${MODEL_FOLDER}/edge-impulse-sdk/porting/lib_support/*.cpp"
)

# 8. tflite-model 내부의 모델 연산 코드들을 묶음
file(GLOB_RECURSE EI_MODEL_SOURCES
        "${MODEL_FOLDER}/tflite-model/*.cpp"
)

# 9. 최종 빌드 대상인 native-lib 공유 라이브러리(.so) 파일 정의
add_library(native-lib SHARED
        native-lib.cpp
        ${EI_SDK_SOURCES}
        ${EI_MODEL_SOURCES}
)

# 10. 안드로이드 시스템 로그 기능(Logcat) 연결을 위해 log 라이브러리 탐색
find_library(log-lib log)

# 11. 빌드 대상 라이브러리에 시스템 라이브러리 연결
target_link_libraries(native-lib
        ${log-lib}
        android
)