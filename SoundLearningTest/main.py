import os
os.environ["TF_USE_LEGACY_KERAS"] = "1"

import shutil
import numpy as np
import librosa
import tensorflow as tf
import tensorflow_hub as hub
from fastapi import FastAPI, UploadFile, File

app = FastAPI(title="소리 인식 테스트 서버")

print("YAMNet을 불러옵니다... (10초 정도 대기)")
yamnet_model = hub.load('https://tfhub.dev/google/yamnet/1')

print("코랩에서 가져온 지식을 채웁니다...")
# 코랩에서 만들었던 것과 똑같은 빈 뼈대 만들기
my_model = tf.keras.Sequential([
    tf.keras.layers.InputLayer(input_shape=(1024,)), 
    tf.keras.layers.Dense(256, activation='relu'),
    tf.keras.layers.Dropout(0.3),
    tf.keras.layers.Dense(3, activation='softmax')
])

# 방금 다운받은 가중치(숫자들)만 덮어씌우기!
my_model.load_weights('my_smart_hearing_model.h5')

CLASS_NAMES = ["대화소리", "위험소리", "일상소음"]

@app.post("/api/analyze_sound")
async def analyze_sound(audio_file: UploadFile = File(...)):
    temp_file = f"temp_{audio_file.filename}"
    with open(temp_file, "wb") as buffer:
        shutil.copyfileobj(audio_file.file, buffer)
    
    try:
        wav_data, _ = librosa.load(temp_file, sr=16000, mono=True)
        scores, embeddings, spectrogram = yamnet_model(wav_data)
        mean_embedding = np.mean(embeddings.numpy(), axis=0)
        
        input_data = np.expand_dims(mean_embedding, axis=0)
        prediction = my_model.predict(input_data)
        
        best_index = np.argmax(prediction[0])
        final_result = CLASS_NAMES[best_index]
        
        return {"message": "분석 성공!", "ai_result": final_result}
        
    except Exception as e:
        return {"에러 발생": str(e)}
    finally:
        if os.path.exists(temp_file):
            os.remove(temp_file)