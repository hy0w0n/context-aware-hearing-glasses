#!/usr/bin/env python3
"""소리 상황 분류 모델(soundModel.tflite) 오프라인 평가 — 앱(SoundClassifier.kt)과 같은 전처리로 PCM/WAV를 분류한다.

전처리(Edge Impulse SDK extract_mfe_features v4와 동일, 2026-10-03 검증):
  2.0초(32,000샘플) 창 → [피크 정규화: 최대 진폭 → 32767, 학습 데이터와 동일] → 프리엠퍼시스 y[n]=x[n]−0.98·x[n−1]
  (n=0의 이전값은 창 마지막 샘플) → ÷32768 → MFE(프레임 320/스트라이드 160, FFT 256 절단, 멜 32, noise floor −52dB) → INT8 모델
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


def _mel(f):
    return 1127.0 * math.log(1.0 + f / 700.0)


def _imel(m):
    return 700.0 * (math.exp(m / 1127.0) - 1.0)


_lo, _hi = _mel(0.0), _mel(8000.0)
BINS = []
for _i in range(NF + 2):
    _hz = min(max(_imel(_lo + (_hi - _lo) * _i / (NF + 1)), 0.0), 8000.0)
    if _i == NF + 1:
        _hz -= 0.001
    BINS.append(int(math.floor((NFFT + 1) * _hz / 16000)))


def preprocess(x_i16, peak_norm=True):
    x = x_i16.astype(np.float64)
    peak = float(np.abs(x).max())
    scale = ((32767.0 / peak) if (peak_norm and peak > 0) else 1.0) / 32768.0
    prev = np.concatenate(([x[-1]], x[:-1]))
    return (x - PRE_COF * prev) * scale


def mfe(xf):
    feats = np.zeros(FRAMES * NF, np.float32)
    for fr in range(FRAMES):
        seg = xf[fr * STRIDE: fr * STRIDE + NFFT]
        X = np.fft.fft(seg, NFFT)
        p = (X.real ** 2 + X.imag ** 2)[:NB] / NFFT
        for i in range(NF):
            l, m, r = BINS[i], BINS[i + 1], BINS[i + 2]
            acc = p[m]
            for b in range(l + 1, r):
                if b < m:
                    acc += (b - l) / (m - l) * p[b]
                elif b > m:
                    acc += (r - b) / (r - m) * p[b]
            if acc == 0:
                acc = 1e-10
            f = (10.0 * math.log10(max(acc, 1e-30)) - NOISE_FLOOR) / (-NOISE_FLOOR + 12.0)
            f = round(f * 256.0) / 256.0
            feats[fr * NF + i] = min(max(f, 0.0), 1.0)
    return feats


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
