#!/usr/bin/env python3
"""이미지 상황 분류 모델(image_model/imageModel.tflite) 독립 평가 — 앱과 같은 전처리로 폴더의 JPEG를 분류한다.

폴더 구조(라벨별 하위 폴더)로 넣으면 정확도·클래스별 지표·혼동행렬을 계산한다:
    eval_images/
      conversation/ *.jpg      danger/ *.jpg      indoor/ *.jpg
      negative/ *.jpg          outdoor/ *.jpg     tv/ *.jpg
하위 폴더 없이 JPEG만 넣으면 장별 예측만 출력한다.

전처리는 앱(ImageClassifier.kt)과 동일: 224×224 리사이즈 → 흑백(BT.601 휘도) 3채널 복제 → float32 0~255 원값(전처리 레이어 내장).
후처리 옵션 --app-rule: negative 확신도 0.95 미만이면 2순위 클래스 채택(앱·camera.py 규칙). 기본은 순수 argmax.

사용: python tools/image_eval.py eval_images [--app-rule] [--color]
      (안경 카메라 캡처는 tools/ble_receiver.py 또는 image_receiver.py로 images/ 에 저장된 JPEG를 라벨 폴더로 옮겨 쓰면 된다)
"""
import argparse
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / "image_model/imageModel.tflite"
CLASSES = ["conversation", "danger", "indoor", "negative", "outdoor", "tv"]
KR = {"conversation": "대화", "danger": "위험", "indoor": "실내", "negative": "negative", "outdoor": "야외", "tv": "TV"}
EXTS = {".jpg", ".jpeg", ".png", ".bmp"}


def load_interp():
    from ai_edge_litert.interpreter import Interpreter
    it = Interpreter(model_path=str(MODEL))
    it.allocate_tensors()
    return it, it.get_input_details()[0], it.get_output_details()[0]


def preprocess(path, color=False):
    im = Image.open(path).convert("RGB").resize((224, 224), Image.BILINEAR)
    arr = np.asarray(im).astype(np.float32)
    if not color:
        y = 0.299 * arr[..., 0] + 0.587 * arr[..., 1] + 0.114 * arr[..., 2]   # BT.601 휘도
        arr = np.stack([y, y, y], axis=-1)
    return arr[None, ...]                                                    # (1,224,224,3) 0~255


def predict(it, inp, out, x, app_rule):
    it.set_tensor(inp["index"], x)
    it.invoke()
    p = it.get_tensor(out["index"])[0].astype(np.float64)
    i = int(np.argmax(p))
    if app_rule and CLASSES[i] == "negative" and p[i] < 0.95:
        q = p.copy(); q[i] = -1.0
        i = int(np.argmax(q))
    return CLASSES[i], float(p[i]), p


def wilson(k, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    ph = k / n
    d = 1 + z * z / n
    c = (ph + z * z / (2 * n)) / d
    h = z * math.sqrt(ph * (1 - ph) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--app-rule", action="store_true", help="negative<0.95 → 2순위 채택(앱 규칙)")
    ap.add_argument("--color", action="store_true", help="흑백 변환 없이 컬러 입력(비교용)")
    args = ap.parse_args()
    root = Path(args.folder)
    it, inp, out = load_interp()

    labeled = any((root / c).is_dir() for c in CLASSES)
    items = []
    if labeled:
        for c in CLASSES:
            d = root / c
            if d.is_dir():
                for f in sorted(d.iterdir()):
                    if f.suffix.lower() in EXTS:
                        items.append((c, f))
    else:
        for f in sorted(root.iterdir()):
            if f.suffix.lower() in EXTS:
                items.append((None, f))
    if not items:
        sys.exit("이미지가 없습니다.")

    print(f"모델 {MODEL.name} · 입력 {'컬러' if args.color else '흑백 3채널'} · 후처리 {'앱 규칙' if args.app_rule else 'argmax'} · {len(items)}장\n")
    conf = defaultdict(Counter)
    rows = []
    for label, f in items:
        pred, p, probs = predict(it, inp, out, preprocess(f, args.color), args.app_rule)
        rows.append((label, f, pred, p))
        if label:
            conf[label][pred] += 1
        mark = "" if label is None else ("O" if pred == label else "X")
        print(f"{mark:1s} {str(f.relative_to(root)):40s} → {pred:12s} {p*100:5.1f}%   " + " ".join(f"{CLASSES[i][:4]}={probs[i]:.2f}" for i in range(6)))

    if labeled:
        n = len(rows)
        k = sum(1 for l, _, pr, _ in rows if pr == l)
        lo, hi = wilson(k, n)
        print(f"\n정확도 {k}/{n} = {k/n*100:.1f}% (95% 신뢰구간 {lo*100:.1f}~{hi*100:.1f}%)")
        print("\n클래스별  정밀도 / 재현율 / F1 / 장수")
        for c in CLASSES:
            tp = conf[c][c]
            fn = sum(conf[c].values()) - tp
            fp = sum(conf[o][c] for o in CLASSES if o != c)
            if tp + fn == 0 and fp == 0:
                continue
            P = tp / (tp + fp) if tp + fp else 0.0
            R = tp / (tp + fn) if tp + fn else 0.0
            F = 2 * P * R / (P + R) if P + R else 0.0
            print(f"  {KR[c]:9s} {P:.2f} / {R:.2f} / {F:.2f} / {tp + fn}")
        print("\n혼동행렬 (행=정답, 열=예측)")
        print("          " + " ".join(f"{KR[c]:>6s}" for c in CLASSES))
        for a in CLASSES:
            if sum(conf[a].values()) == 0:
                continue
            print(f"{KR[a]:>9s} " + " ".join(f"{conf[a][b]:6d}" for b in CLASSES))
        print("\n※ 장수가 적으면 신뢰구간이 넓다. 문서에는 장수·촬영 조건(안경 카메라, 장소)과 함께 쓸 것.")


if __name__ == "__main__":
    main()
