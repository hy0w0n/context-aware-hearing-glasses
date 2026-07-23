#include <jni.h>
#include <string>
#include <vector>
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "model-parameters/model_metadata.h"

// 🚨 링키지 에러 해결: Edge Impulse 내부 선언과 동일하게 C++ 링키지(extern "C" 제거)로 맞춥니다!
void ei_printf(const char *format, ...) {}
void ei_printf_float(float f) {}

// 자바 단에서 마이크 소리를 받아서 C++ 내부의 신호 처리기로 흘려주기 위한 임시 글로벌 버퍼
static std::vector<float> input_audio;

// Edge Impulse 라이브러리가 필요할 때마다 이 콜백 함수를 호출하여 소리 파동 조각을 가져갑니다.
static int get_signal_data(size_t offset, size_t length, float *out_ptr) {
    for (size_t i = 0; i < length; i++) {
        // 🌟 에러 -24 완벽 방어: 자바에서 넘어온 데이터가 모자라거나 초과하면 무조건 0.0(무음)으로 안전하게 채움!
        if (offset + i < input_audio.size()) {
            out_ptr[i] = input_audio[offset + i];
        } else {
            out_ptr[i] = 0.0f;
        }
    }
    return 0;
}

/*
 * 🚨 [극도로 중요] UnsatisfiedLinkError 방지용 JNI 네이밍 규칙!
 * 패키지명에 언더바(_)가 포함된 경우 반드시 "_1"로 변환해서 적어주어야 합니다.
 * 패키지명: com.example.smarthearing_edge -> C++ 함수명: com_example_smarthearing_1edge
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_smarthearing_1edge_MainActivity_classifyAudio(JNIEnv *env, jobject thiz, jfloatArray audio_data) {

    // 1. 자바 단에서 들어온 float[] 오디오 배열 정보 추출
    jsize len = env->GetArrayLength(audio_data);
    jfloat *body = env->GetFloatArrayElements(audio_data, 0);

    // 2. 글로벌 vector 버퍼로 메모리 카피 진행
    input_audio.assign(body, body + len);

    // 3. 사용 완료한 자바 힙 영역 배열 요소 반환 및 메모리 해제
    env->ReleaseFloatArrayElements(audio_data, body, JNI_ABORT);

    // 4. Edge Impulse SDK가 인식할 수 있는 Signal 구조체 포장
    signal_t signal;
    // 🚨 핵심 해결책: 자바에서 몇 개를 던지든 무조건 "뇌가 딱 원하는 크기"로 강제 세팅합니다!
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data = &get_signal_data;

    // 5. 대망의 실시간 추론 연산 실행 (MFE 전처리 + 모델 추론 동시 수행!)
    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);

    if (r != EI_IMPULSE_OK) {
        char err_msg[256];
        if (r == -24) {
            // 🌟 에러 -24 전용 안내 메시지
            snprintf(err_msg, sizeof(err_msg), "분석 실패 (에러 -24)\n원인: AI 모델이 너무 복잡하여 메모리(Arena Size)가 부족합니다.\n해결법: CMakeLists.txt에 메모리 강제 할당 옵션을 확인하세요.");
        } else {
            snprintf(err_msg, sizeof(err_msg), "분석 실패 (에러 코드: %d)", r);
        }
        return env->NewStringUTF(err_msg);
    }

    // 6. 결과를 예쁘게 포맷팅하여 자바(UI) 단으로 전송할 문자열 생성
    std::string output = "";
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        char buffer[100];
        snprintf(buffer, sizeof(buffer), "%s (%.1f%%)  ",
                 result.classification[ix].label,
                 result.classification[ix].value * 100.0f);
        output += buffer;
    }

    return env->NewStringUTF(output.c_str());
}