#!/usr/bin/env python3
"""소리 상황 분류 모델(soundModel.tflite) 오프라인 평가 — 앱(SoundClassifier.kt)과 같은 전처리로 PCM/WAV를 분류한다.

전처리(앱 MelFeatures.kt와 같은 계산, 2026-11-09 공개 자료 기준으로 다시 작성):
  2.0초(32,000샘플) 창 → [피크 정규화: 최대 진폭 → 32767, 학습 데이터와 동일] → 프리엠퍼시스 y[n]=x[n]−0.98·x[n−1]
  (n=0의 이전값은 창 마지막 샘플) → ÷32768 → MFE(프레임 320/간격 160, 앞 256샘플 FFT, 멜 32, 잡음 바닥 −52dB) → INT8 모델
무음 게이트: 원값 RMS < 150 이면 모델을 돌리지 않고 '무음'.

사용:
  python tools/sound_eval.py                                 # 앱 자가시험 PCM(test_sound.pcm) 확인
  python tools/sound_eval.py recordings/az000_beam16k.wav    # 16kHz mono WAV를 2초 창(1초 간격)으로 분류
  python tools/sound_eval.py some.pcm --no-peak-norm         # 피크 정규화 끄고 비교
  python tools/sound_eval.py folder/  --labels               # 라벨별 하위 폴더(danger/noise/speech)의 WAV를 평가해 정확도 계산
WAV는 16kHz mono여야 한다(48kHz 3ch raw 녹음은 먼저 beam 스트림으로 녹음하거나 변환할 것).
"""
import argparse
import math
import sys
import wave
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / "android/app/src/main/assets/soundModel.tflite"
CLASSES = ["danger", "noise", "speech"]
NFFT, NB, NF, FRAMES, STRIDE, WINDOW = 256, 129, 32, 199, 160, 32000
NOISE_FLOOR, CONF_GATE, SILENCE_RMS, PRE_COF = -52.0, 0.6, 150.0, 0.98


# ── MFE 특징 (앱 MelFeatures.kt와 같은 계산) ──────────────────────────────
# 2026-11-09 다시 작성: Edge Impulse 공개 처리 블록(github.com/edgeimpulse/processing-blocks,
# mfe/dsp.py와 그 안의 speechpy, The Clear BSD License)과 원본 speechpy(Apache-2.0)에 공개된
# 계산 순서를 근거로 했다. Edge Impulse SDK의 C++ 코드는 쓰지 않는다.
FRAME_LEN = 320


def _filterbank():
    """멜 필터 [NF, NB] — speechpy filterbanks()의 정의"""
    mel = lambda hz: 1127.0 * np.log(1.0 + hz / 700.0)
    hz = 700.0 * (np.exp(np.linspace(mel(0.0), mel(8000.0), NF + 2) / 1127.0) - 1.0)
    hz[-1] -= 0.001                                   # 공개 처리 블록과 같이 마지막 경계를 0.001 Hz 내림
    edge = np.floor((NFFT + 1) * hz / 16000).astype(int)
    k = np.arange(NB)
    fb = np.zeros((NF, NB))
    for f in range(NF):
        left, mid, right = edge[f], edge[f + 1], edge[f + 2]
        up = (k > left) & (k <= mid)
        fb[f, up] = (k[up] - left) / (mid - left)
        down = (k >= mid) & (k < right)
        fb[f, down] = (right - k[down]) / (right - mid)
    return fb


FILTERBANK = _filterbank()
_FRAME_IDX = np.arange(FRAMES)[:, None] * STRIDE + np.arange(NFFT)[None, :]   # 프레임마다 앞 256샘플


def preprocess(x_i16, peak_norm=True):
    """int16 창 → 최대 크기 맞춤 → 프리엠퍼시스(n=0은 마지막 샘플) → ÷32768"""
    x = x_i16.astype(np.float64)
    peak = float(np.abs(x).max())
    gain = ((32767.0 / peak) if (peak_norm and peak > 0) else 1.0) / 32768.0
    return (x - PRE_COF * np.roll(x, 1)) * gain


def mfe(xf):
    """2.0초 신호 → 특징 199×32 (프레임 순, 0~255/256)"""
    frames = np.asarray(xf, np.float64)[_FRAME_IDX]
    power = np.abs(np.fft.rfft(frames, n=NFFT, axis=1)) ** 2 / NFFT
    energy = power @ FILTERBANK.T
    energy[energy == 0] = 1e-10
    db = 10.0 * np.log10(np.maximum(energy, 1e-30))
    v = np.clip((db - NOISE_FLOOR) / (-NOISE_FLOOR + 12.0), 0.0, 1.0)
    q = np.clip(np.rint(v * 256.0), 0, 255)          # 255에서 멈춤(모델 입력 범위의 최댓값)
    return (q / 256.0).astype(np.float32).reshape(-1)


class Model:
    def __init__(self):
        from ai_edge_litert.interpreter import Interpreter
        self.it = Interpreter(model_path=str(MODEL))
        self.it.allocate_tensors()
        self.inp = self.it.get_input_details()[0]
        self.out = self.it.get_output_details()[0]

    def classify(self, window_i16, peak_norm=True):
        rms = math.sqrt(float((window_i16.astype(np.float64) ** 2).mean()))
        if rms < SILENCE_RMS:
            return None, np.zeros(3), rms
        f = mfe(preprocess(window_i16, peak_norm))
        isc, izp = self.inp["quantization"]
        osc, ozp = self.out["quantization"]
        q = np.clip(np.round(f / isc + izp), -128, 127).astype(np.int8)[None, :]
        self.it.set_tensor(self.inp["index"], q)
        self.it.invoke()
        o = (self.it.get_tensor(self.out["index"])[0].astype(np.float32) - ozp) * osc
        best = int(np.argmax(o))
        cls = CLASSES[best] if o[best] >= CONF_GATE else "noise"
        return cls, o, rms


def load_audio(path):
    p = Path(path)
    if p.suffix.lower() == ".pcm":
        return np.frombuffer(p.read_bytes(), dtype="<i2")
    w = wave.open(str(p), "rb")
    if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
        sys.exit(f"{p.name}: 16kHz mono 16bit WAV만 지원 (현재 {w.getframerate()}Hz {w.getnchannels()}ch)")
    return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def windows(x, hop=16000):
    if len(x) < WINDOW:
        x = np.pad(x, (0, WINDOW - len(x)))
    for s in range(0, len(x) - WINDOW + 1, hop):
        yield s, x[s:s + WINDOW]


def fmt(o):
    return " ".join(f"{c}={v:.2f}" for c, v in zip(CLASSES, o))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", default=str(ROOT / "android/app/src/main/assets/test_sound.pcm"))
    ap.add_argument("--no-peak-norm", action="store_true")
    ap.add_argument("--hop", type=float, default=1.0, help="창 이동 간격(초)")
    ap.add_argument("--labels", action="store_true", help="path가 라벨별 하위 폴더(danger/noise/speech)를 가진 폴더")
    args = ap.parse_args()
    m = Model()
    pn = not args.no_peak_norm
    hop = int(args.hop * 16000)

    if args.labels:
        root = Path(args.path)
        conf = defaultdict(Counter)
        total = correct = 0
        for lab in CLASSES:
            for f in sorted((root / lab).glob("*")):
                if f.suffix.lower() not in (".wav", ".pcm"):
                    continue
                x = load_audio(f)
                votes = Counter()
                for _, w in windows(x, hop):
                    cls, o, rms = m.classify(w, pn)
                    if cls:
                        votes[cls] += 1
                pred = votes.most_common(1)[0][0] if votes else "silence"
                conf[lab][pred] += 1
                total += 1
                correct += int(pred == lab)
                print(f"{'O' if pred == lab else 'X'} {lab:7s} {f.name:40s} → {pred} {dict(votes)}")
        print(f"\n파일 단위 정확도 {correct}/{total} = {correct / max(1, total) * 100:.1f}%")
        print("혼동행렬 (행=정답)  " + "  ".join(f"{c:>7s}" for c in CLASSES + ['silence']))
        for a in CLASSES:
            print(f"  {a:7s}            " + "  ".join(f"{conf[a][b]:7d}" for b in CLASSES + ['silence']))
        return

    x = load_audio(args.path)
    print(f"{Path(args.path).name}: {len(x) / 16000:.1f}s, 피크 {int(np.abs(x).max())}, 피크 정규화 {'ON' if pn else 'OFF'}")
    for s, w in windows(x, hop):
        cls, o, rms = m.classify(w, pn)
        t = s / 16000
        if cls is None:
            print(f"  {t:5.1f}s  무음 (RMS {rms:.0f})")
        else:
            print(f"  {t:5.1f}s  {cls:7s} | {fmt(o)} | RMS {rms:.0f}")


if __name__ == "__main__":
    main()
