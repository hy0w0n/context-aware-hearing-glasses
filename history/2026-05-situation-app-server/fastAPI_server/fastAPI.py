from fastapi import FastAPI, UploadFile, File
from openai import OpenAI
import whisper  # 로컬 Whisper 라이브러리
import shutil
import os

app = FastAPI()

# 1. 로컬 Whisper 모델 로드 (가장 가벼운 'base' 모델 추천)
# 처음 실행할 때 모델을 다운로드하느라 시간이 좀 걸립니다.
model_stt = whisper.load_model("base")

# 2. 학교 API 설정
client = OpenAI(
    api_key="***REMOVED***",
    base_url="https://factchat-cloud.mindlogic.ai/v1/gateway"
)

@app.post("/analyze-audio")
async def analyze_audio(audio_file: UploadFile = File(...)):
    temp_filename = f"temp_{audio_file.filename}"
    with open(temp_filename, "wb") as buffer:
        shutil.copyfileobj(audio_file.file, buffer)

    try:
        # [단계 1] 내 컴퓨터에서 직접 소리를 글자로 바꾸기 (로컬 Whisper)
        result_stt = model_stt.transcribe(temp_filename)
        user_text = result_stt['text'] 

        # [단계 2] 변환된 글자를 학교 API(GPT)로 보내기
        response = client.chat.completions.create(
            model="gpt-5", # 학교 리스트에 있는 모델명
            messages=[
                {"role": "system", "content": "너는 스마트 보청기 분석가야. 대화 내용을 보고 'TV', '대화' 중 하나로만 대답해."},
                {"role": "user", "content": f"주변 소리 내용: {user_text}"}
            ],
        )

        situation = response.choices[0].message.content
        return {
            "text": user_text,  # Whisper가 알아들은 말
            "situation": situation  # GPT가 판단한 상황
        }

    except Exception as e:
        return {"error": str(e)}
    finally:
        if os.path.exists(temp_filename):
            os.remove(temp_filename)