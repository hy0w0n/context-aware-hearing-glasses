import os
import glob
import base64
import requests
from dotenv import load_dotenv

load_dotenv()

BASE = "https://factchat-cloud.mindlogic.ai/v1/gateway"
API_KEY = os.getenv("AI_GATEWAY_API_KEY")
MODEL = os.getenv("AI_GATEWAY_MODEL", "gpt-5.4-mini")

def latest_wav():
    files = glob.glob(os.path.join("recordings", "*.wav"))
    if not files:
        raise RuntimeError("recordings 폴더에 wav 파일이 없습니다.")
    return max(files, key=os.path.getmtime)

audio_path = latest_wav()

with open(audio_path, "rb") as f:
    audio_b64 = base64.b64encode(f.read()).decode("utf-8")

headers = {
    "Authorization": f"Bearer {API_KEY}",
    "Content-Type": "application/json",
}

payload = {
    "model": MODEL,
    "input": [
        {
            "role": "user",
            "content": [
                {
                    "type": "input_text",
                    "text": "이 오디오 파일을 듣고 어떤 소리가 들리는지 한국어로 설명해줘."
                },
                {
                    "type": "input_audio",
                    "input_audio": {
                        "data": audio_b64,
                        "format": "wav"
                    }
                }
            ]
        }
    ],
    "max_output_tokens": 1000
}

r = requests.post(
    f"{BASE}/responses/",
    headers=headers,
    json=payload,
    timeout=120
)

print("Status:", r.status_code)
print("Audio:", audio_path)

try:
    data = r.json()
    print(data)
except Exception:
    print(r.text)
