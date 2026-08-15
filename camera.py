import cv2
import numpy as np
import tensorflow as tf
from collections import deque

# 1. TFLite 모델 로드 및 텐서 하드웨어 할당
MODEL_PATH = "imageModel.tflite"
interpreter = tf.lite.Interpreter(model_path=MODEL_PATH)
interpreter.allocate_tensors()

# 모델의 입력 및 출력 규격 정보 추출
input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

# 데이터셋 로드 알파벳 순서와 정확히 일치해야 함
class_names = ['conversation', 'danger', 'indoor', 'negative', 'outdoor', 'tv']
class_labels_kr = {
    'conversation': '대화 모드',
    'danger': '위험 상황',
    'indoor': '실내 모드',
    'negative': '분석 불가 (기존 모드 유지)',
    'outdoor': '야외 모드',
    'tv': 'TV 시청 모드'
}

# 시계열 스무딩을 위한 큐 (최근 7프레임의 결과를 저장하여 급격한 변화 방지)
WINDOW_SIZE = 7
prediction_queue = deque(maxlen=WINDOW_SIZE)
current_stable_mode = "indoor"  # 초기 기본 모드

# 2. 노트북 기본 웹캠 개방 (0번)
cap = cv2.VideoCapture(0)

print("=== 스마트 보청기 비전 제어 시스템 실시간 테스트 시작 ===")
print("종료하려면 웹캠 창을 선택하고 'q'를 누르십시오.")

while cap.isOpened():
    ret, frame = cap.read()
    if not ret:
        print("카메라 피드를 가져올 수 없습니다.")
        break

    # 좌우 반전 (사용자 거울 모드)
    frame = cv2.flip(frame, 1)
    h, w, _ = frame.shape

    # 3. 이미지 전처리 (모델 입력 규격 동기화)
    # 224x224 크기 조정 및 BGR에서 RGB 채널 전환
    input_img = cv2.resize(frame, (224, 224))
    input_img = cv2.cvtColor(input_img, cv2.COLOR_BGR2RGB)
    
    # [핵심 수정 완료]
    # 모델 학습 시(Colab) preprocess_input 레이어를 네트워크 내부에 이미 포함시켰으므로,
    # 외부에서 (input_img / 127.5) - 1.0 수식을 적용하면 이중 전처리가 되어 영상이 암흑 파손됩니다.
    # 따라서 원본 픽셀값(0~255)을 float32 형태로 그대로 투입합니다.
    input_img = input_img.astype(np.float32)
    input_img = np.expand_dims(input_img, axis=0)  # 배치 차원 주입 (1, 224, 224, 3)

    # 4. TFLite 인터프리터 기반 고속 추론
    interpreter.set_tensor(input_details[0]['index'], input_img)
    interpreter.invoke()
    output_data = interpreter.get_tensor(output_details[0]['index'])[0]

    # 현재 프레임의 최댓값 연산
    raw_idx = np.argmax(output_data)
    raw_class = class_names[raw_idx]
    confidence = output_data[raw_idx]

    # 5. negative 과적합 방지 필터
    # negative로 판단했더라도 확신도가 95% 미만이라면, 해당 값을 제외하고 2등 클래스를 채택합니다.
    if raw_class == 'negative' and confidence < 0.95:
        temp_data = np.copy(output_data)
        temp_data[raw_idx] = -1.0  # 1등(negative) 확률을 음수로 지움
        second_idx = np.argmax(temp_data)
        raw_class = class_names[second_idx]
        confidence = output_data[second_idx]

    # 6. 시계열 다수결 알고리즘 (Temporal Majority Voting) 적용
    prediction_queue.append(raw_class)
    
    # 분석 불가(negative) 프레임은 최종 제어 모드 연산에서 제외
    valid_predictions = [p for p in prediction_queue if p != 'negative']
    
    if valid_predictions:
        # 가장 빈도수가 높은 상황을 최종 제어 상태로 결정
        final_class = max(set(valid_predictions), key=valid_predictions.count)
    else:
        final_class = current_stable_mode

    current_stable_mode = final_class
    display_text = class_labels_kr[current_stable_mode]

    # 7. 사용자 화면 텍스트 및 가이드 UI 오버레이
    # 실시간 프레임별 원본(또는 2순위 보정) 예측 데이터 값
    cv2.putText(frame, f"인식된 상황: {raw_class} ({confidence:.2f})", (30, 40),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)
    
    # 스무딩 필터를 통과한 보청기 실제 제어 모드 상태
    cv2.putText(frame, f"현재 모드: {display_text}", (30, 80),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)
    
    # 큐 데이터 상태 시각화 (디버깅용)
    cv2.putText(frame, f"Buffer: {list(prediction_queue)}", (30, h - 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)

    # 결과 프레임 출력
    cv2.imshow("Smart Hearing Aid Context Verification", frame)

    # 'q' 키 입력 시 루프 탈출
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

# 자원 해제
cap.release()
cv2.destroyAllWindows()