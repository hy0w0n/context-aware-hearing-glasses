import cv2
import numpy as np
import tensorflow as tf

# 1. 티처블 머신에서 만든 부동 소수점 AI 모델 불러오기
# (현재 폴더에 있는 파일명 지정)
interpreter = tf.lite.Interpreter(model_path="model.tflite")
interpreter.allocate_tensors()

input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

# labels.txt 파일에서 상황 이름(conversation, tv, outdoor 등) 읽어오기
with open("labels.txt", "r", encoding="utf-8") as f:
    labels = [line.strip() for line in f.readlines()]

# 2. 컴퓨터 웹캠 켜기
cap = cv2.VideoCapture(0)

print("프로그램을 시작합니다. 'q'를 누르면 종료됩니다.")

while cap.isOpened():
    ret, frame = cap.read()
    if not ret:
        break

    # [중요] 카메라 화면을 흑백(Grayscale)으로 변환 (채널 수 1)
    gray_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    
    # AI 모델 크기에 맞게 이미지 크기 조정 (입력 형태인 96x96으로 자동 연동)
    input_shape = input_details[0]['shape']  # [1, 96, 96, 1]
    resized_frame = cv2.resize(gray_frame, (input_shape[1], input_shape[2]))

    # AI 모델이 인식할 수 있도록 차원 맞추기
    # (96, 96) -> (1, 96, 96, 1) 형태로 늘려줍니다.
    input_data = np.expand_dims(resized_frame, axis=0)       # 앞쪽에 배치 차원 추가
    input_data = np.expand_dims(input_data, axis=-1)      # 뒤쪽에 흑백 채널(1) 차원 추가

    # [중요] 부동 소수점(Float) 모델 전처리
    # 0~255인 픽셀 값을 0.0 ~ 1.0 사이의 소수점 데이터로 정규화하고 float32 타입으로 변환합니다.
    input_data = input_data.astype(np.float32) / 255.0

    # 3. AI에게 사진을 주고 상황 판단 시키기
    interpreter.set_tensor(input_details[0]['index'], input_data)
    interpreter.invoke()
    
    # 결과 받아오기
    output_data = interpreter.get_tensor(output_details[0]['index'])[0]
    best_match_index = np.argmax(output_data)
    result_text = labels[best_match_index] # 가장 가능성 높은 상황
    
    # 부동 소수점 모델은 결과 확률이 0.0 ~ 1.0 사이로 나옵니다.
    confidence = output_data[best_match_index] * 100 # 확신도 (%)

    # 4. 화면에 결과 표시하기 (흑백 화면 위에 텍스트 표시)
    display_text = f"Situation: {result_text} ({confidence:.1f}%)"
    cv2.putText(gray_frame, display_text, (10, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
    
    # 화면 보여주기
    cv2.imshow("Smart Hearing Aid Camera (Black & White)", gray_frame)

    # 'q' 키를 누르면 종료
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()