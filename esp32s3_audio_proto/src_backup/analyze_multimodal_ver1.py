import os
import glob
import base64
import wave
import numpy as np
from dotenv import load_dotenv
from openai import OpenAI

load_dotenv()

API_KEY = os.getenv("AI_GATEWAY_API_KEY")
BASE_URL = os.getenv("AI_GATEWAY_BASE_URL")
MODEL = os.getenv("AI_GATEWAY_MODEL", "claude-sonnet-4-6")

client = OpenAI(
    api_key=API_KEY,
    base_url=BASE_URL,
)

def find_latest_file(folder, patterns):
    files = []
    for pattern in patterns:
        files.extend(glob.glob(os.path.join(folder, pattern)))
    if not files:
        return None
    return max(files, key=os.path.getmtime)

def image_to_data_url(path):
    ext = os.path.splitext(path)[1].lower().replace(".", "")
    if ext == "jpg":
        ext = "jpeg"
    with open(path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode("utf-8")
    return f"data:image/{ext};base64,{b64}"

def analyze_wav(path):
    with wave.open(path, "rb") as wf:
        channels = wf.getnchannels()
        sample_width = wf.getsampwidth()
        sample_rate = wf.getframerate()
        frame_count = wf.getnframes()
        duration = frame_count / sample_rate
        raw = wf.readframes(frame_count)

    if sample_width == 2:
        audio = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    elif sample_width == 1:
        audio = np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128
    else:
        raise ValueError(f"지원하지 않는 sample_width: {sample_width}")

    if channels > 1:
        audio = audio.reshape(-1, channels).mean(axis=1)

    rms = float(np.sqrt(np.mean(audio ** 2)))
    peak = float(np.max(np.abs(audio)))
    mean_abs = float(np.mean(np.abs(audio)))

    if rms < 300:
        loudness = "매우 조용함"
    elif rms < 1000:
        loudness = "작은 소리"
    elif rms < 3000:
        loudness = "보통 크기의 소리"
    elif rms < 8000:
        loudness = "큰 소리"
    else:
        loudness = "매우 큰 소리 또는 충격음 가능성"

    if peak > 30000:
        event = "클리핑 또는 매우 큰 충격음 가능성"
    elif peak > 15000:
        event = "큰 소리 이벤트 가능성"
    elif peak > 6000:
        event = "중간 크기 이상의 소리 변화 있음"
    else:
        event = "뚜렷한 큰 충격음은 감지되지 않음"

    return {
        "channels": channels,
        "sample_width": sample_width,
        "sample_rate": sample_rate,
        "duration": duration,
        "rms": rms,
        "peak": peak,
        "mean_abs": mean_abs,
        "loudness": loudness,
        "event": event,
    }

audio_path = find_latest_file("recordings", ["*.wav"])
image_path = find_latest_file("images", ["*.jpg", "*.jpeg", "*.png", "*.webp"])

if audio_path is None:
    raise RuntimeError("recordings 폴더에서 WAV 파일을 찾지 못했습니다.")

if image_path is None:
    raise RuntimeError("images 폴더에서 이미지 파일을 찾지 못했습니다.")

audio_info = analyze_wav(audio_path)
image_data_url = image_to_data_url(image_path)

audio_summary = f"""
오디오 분석 요약:
- 파일명: {os.path.basename(audio_path)}
- 길이: {audio_info["duration"]:.2f}초
- 샘플링레이트: {audio_info["sample_rate"]} Hz
- 채널 수: {audio_info["channels"]}
- RMS 음량: {audio_info["rms"]:.1f}
- Peak 음량: {audio_info["peak"]:.1f}
- 평균 절대 진폭: {audio_info["mean_abs"]:.1f}
- 추정 음량 수준: {audio_info["loudness"]}
- 이벤트 추정: {audio_info["event"]}
"""

prompt = f"""
너는 시각장애인 또는 보조기기 사용자를 돕는 상황 분석 AI야.

입력으로는 현재 장면 이미지와, 같은 시점에 녹음된 소리 데이터의 분석 요약이 제공된다.
이미지와 오디오 요약을 함께 고려해서 현재 상황을 분석해줘.

{audio_summary}

반드시 아래 형식으로 한국어로 답변해줘.

1. 현재 상황 요약:
2. 이미지에서 보이는 중요한 요소:
3. 소리 데이터에서 추정되는 요소:
4. 위험 요소 또는 주의할 점:
5. 사용자가 바로 취하면 좋은 행동:
6. 확신도:
"""

response = client.chat.completions.create(
    model=MODEL,
    messages=[
        {
            "role": "user",
            "content": [
                {
                    "type": "text",
                    "text": prompt,
                },
                {
                    "type": "image_url",
                    "image_url": {
                        "url": image_data_url,
                    },
                },
            ],
        }
    ],
    max_tokens=1200,
)

print("Audio:", audio_path)
print("Image:", image_path)
print()
print("=== 오디오 요약 ===")
print(audio_summary)
print()
print("=== 상황 분석 결과 ===")
print(response.choices[0].message.content)
