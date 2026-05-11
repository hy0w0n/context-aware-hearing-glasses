from fastapi import FastAPI, UploadFile, File
from openai import OpenAI
import os
import shutil
import base64
from pydub import AudioSegment

app = FastAPI()

# 학교 API 설정
client = OpenAI(
    api_key="***REMOVED***",
    base_url="https://factchat-cloud.mindlogic.ai/v1/gateway"
)

@app.post("/analyze-audio")
async def analyze_audio(audio_file: UploadFile = File(...)):

    # 업로드된 원본 파일명
    temp_m4a = f"temp_{audio_file.filename}"

    # wav 파일명 생성
    temp_wav = temp_m4a.replace(".m4a", ".wav")

    # 안드로이드에서 보낸 m4a 저장
    with open(temp_m4a, "wb") as buffer:
        shutil.copyfileobj(audio_file.file, buffer)

    try:
        # m4a → wav 변환
        audio = AudioSegment.from_file(temp_m4a)

        # STT 최적화
        audio = audio.set_frame_rate(16000)
        audio = audio.set_channels(1)

        # wav 저장
        audio.export(
            temp_wav,
            format="wav"
        )

        # wav 파일 base64 인코딩
        with open(temp_wav, "rb") as f:
            audio_data = base64.b64encode(f.read()).decode("utf-8")

        # Gemini API 호출
        response = client.chat.completions.create(
            model="gemini-2.5-flash",
            messages=[
                {
                    "role": "system",
                    "content": (
                        "당신은 한국어 음성 인식(STT) 및 상황 분석 AI입니다.\n"
                        "실제 들리는 음성만 정확하게 텍스트로 작성하세요.\n"
                        "추측하거나 지어내지 마세요.\n"
                        "상황을 대화, TV, 음악, 소음, 불명확 중 하나로 분류하세요.\n"
                        "반드시 아래 형식으로만 응답하세요.\n\n"
                        "Text: [인식된 내용]\n"
                        "Situation: [상황]"
                    )
                },
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "text",
                            "text": "이 오디오의 한국어 내용을 텍스트로 변환하고 상황을 분석해줘."
                        },
                        {
                            "type": "input_audio",
                            "input_audio": {
                                "data": audio_data,
                                "format": "wav"
                            }
                        }
                    ]
                }
            ],
            temperature=0.0
        )

        # 모델 응답 추출
        full_content = response.choices[0].message.content

        # 서버 터미널 출력
        print(f"--- Model Response ---\n{full_content}\n----------------------")

        # 기본값
        extracted_text = "인식 실패"
        extracted_situation = "판단 불가"

        # 응답 파싱
        for line in full_content.split('\n'):

            clean_line = line.strip()

            if clean_line.lower().startswith("text:"):
                extracted_text = clean_line[5:].strip()

            elif clean_line.lower().startswith("situation:"):
                extracted_situation = clean_line[10:].strip()

        # 안드로이드로 결과 반환
        return {
            "text": extracted_text,
            "situation": extracted_situation
        }

    except Exception as e:

        print(f"❌ Error: {str(e)}")

        return {
            "error": str(e),
            "text": "에러 발생",
            "situation": "에러"
        }

    finally:

        # 임시 파일 삭제
        if os.path.exists(temp_m4a):
            os.remove(temp_m4a)

        if os.path.exists(temp_wav):
            os.remove(temp_wav)