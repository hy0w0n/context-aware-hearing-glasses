#!/usr/bin/env python3
"""초기 로컬 TFLite 인텐트 모델(ptt_module/app/src/main/assets/intent_model.tflite) 재현 시험.

채원_Process.md의 "비정형 발화 환경에서 인식 정확도 40~45%"가 저장소에 근거 기록이 없어,
같은 시험 문장 세트로 로컬 모델을 다시 돌려 확인하기 위한 도구다.

- TensorFlow 없이 tflite 패키지로 가중치를 읽어 numpy로 순전파를 재현한다(그래프: CAST→GATHER→MEAN→FC(ReLU)→FC→SOFTMAX).
- 전처리는 ptt_module/train_model.py와 동일: 공백 분리 어절 → vocab.txt 인덱스(없으면 0) → 길이 20으로 0 패딩.
  ⚠ 앱의 실제 Kotlin/Java 전처리 코드는 저장소에 없어 학습 스크립트 기준으로 재현한 것이다(근사).
- 이 모델은 6개 인텐트(AUTO_MODE_OFF, MODE_ONE_ON_ONE, MODE_TV, UNKNOWN, VOLUME_DOWN, VOLUME_UP)만 알고 있다.
  비교는 정답이 이 6종인 문장만 대상으로 한다(나머지 4종은 모델이 출력할 수 없어 제외).

사용: python tools/intent_local_baseline.py [--tiers core,boundary] [--set tools/intent_testset.csv]
"""
import argparse
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import tflite
from tflite import BuiltinOperator

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "ptt_module/app/src/main/assets"
MAX_LEN = 20
OPNAME = {v: k for k, v in vars(BuiltinOperator).items() if not k.startswith("_") and isinstance(v, int)}


def load_model():
    buf = (ASSETS / "intent_model.tflite").read_bytes()
    m = tflite.Model.GetRootAsModel(buf, 0)
    sg = m.Subgraphs(0)

    def const(i):
        t = sg.Tensors(i)
        data = m.Buffers(t.Buffer()).DataAsNumpy()
        if isinstance(data, int) or data.size == 0:
            return None
        dt = {0: np.float32, 2: np.int32}[t.Type()]
        return np.frombuffer(data.tobytes(), dtype=dt).reshape([t.Shape(j) for j in range(t.ShapeLength())])

    ops = []
    for i in range(sg.OperatorsLength()):
        op = sg.Operators(i)
        code = m.OperatorCodes(op.OpcodeIndex())
        ops.append(OPNAME.get(max(code.BuiltinCode(), code.DeprecatedBuiltinCode())))
    expected = ["CAST", "LESS", "ADD", "SELECT_V2", "GATHER", "MEAN", "FULLY_CONNECTED", "FULLY_CONNECTED", "SOFTMAX"]
    if ops != expected:
        sys.exit("예상과 다른 모델 구조입니다: %s" % ops)
    # 텐서 번호는 위 구조(검사 결과)와 같다: 5=임베딩, 4/1=FC1 가중치/편향, 3/2=FC2 가중치/편향
    return {"emb": const(5), "w1": const(4), "b1": const(1), "w2": const(3), "b2": const(2)}


def forward(p, ids):
    vocab_n = p["emb"].shape[0]
    idx = np.array(ids, dtype=np.int32)
    idx = np.where(idx < 0, idx + vocab_n, idx)            # SELECT_V2(LESS(x,0), x+n, x)
    h = p["emb"][idx].mean(axis=0)                          # GATHER → MEAN(axis=1)
    h = np.maximum(p["w1"] @ h + p["b1"], 0.0)              # FC + ReLU
    z = p["w2"] @ h + p["b2"]                               # FC
    e = np.exp(z - z.max())
    return e / e.sum()


def preprocess(text, vocab_map):
    seq = [vocab_map.get(tok, 0) for tok in text.split()]
    seq += [0] * (MAX_LEN - len(seq))
    return seq[:MAX_LEN]


def train_sentences():
    """ptt_module/train_model.py의 학습 문장 목록(문장, 인텐트)."""
    import ast
    import re
    src = (ROOT / "ptt_module/train_model.py").read_text(encoding="utf-8")
    return ast.literal_eval(re.search(r"data = (\[.*?\n\])", src, re.S).group(1))


def train_check(params, vocab_map, labels):
    """모델이 자기 학습 문장을 얼마나 맞히는지(모델 자체의 재현율)."""
    data = train_sentences()
    ok = sum(1 for t, y in data if labels[int(np.argmax(forward(params, preprocess(t, vocab_map))))] == y)
    print("=== 학습 문장 재현 확인 ===")
    print("train_model.py 학습 문장 %d개 중 %d개 정답 (%.1f%%)" % (len(data), ok, ok / len(data) * 100))
    print("(학습에 쓴 문장도 이 정도라면 모델이 충분히 학습되지 않았거나 구조적 한계가 있다는 뜻이며, 새 문장에서는 더 낮아진다)\n")


def crosscheck(params, vocab_map, texts):
    """ai-edge-litert 공식 인터프리터 출력과 이 스크립트의 순전파 출력을 비교한다."""
    try:
        from ai_edge_litert.interpreter import Interpreter
    except Exception:
        print("crosscheck 건너뜀: pip install ai-edge-litert 필요\n")
        return
    it = Interpreter(model_path=str(ASSETS / "intent_model.tflite"))
    it.allocate_tensors()
    i_in, i_out = it.get_input_details()[0]["index"], it.get_output_details()[0]["index"]
    worst = 0.0
    for t in texts:
        ids = preprocess(t, vocab_map)
        it.set_tensor(i_in, np.array([ids], dtype=np.float32))
        it.invoke()
        worst = max(worst, float(np.abs(it.get_tensor(i_out)[0] - forward(params, ids)).max()))
    print("crosscheck: 공식 인터프리터와 출력 최대 차이 %.2e (%d문장)\n" % (worst, len(texts)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default=str(ROOT / "tools/intent_testset.csv"))
    ap.add_argument("--tiers", default="core,boundary")
    ap.add_argument("--train-check", action="store_true", help="학습 문장(44개)으로 모델 자체 재현율 확인")
    ap.add_argument("--crosscheck", action="store_true", help="ai-edge-litert 공식 인터프리터와 출력 비교")
    args = ap.parse_args()

    vocab = [l.rstrip("\n") for l in (ASSETS / "vocab.txt").read_text(encoding="utf-8").split("\n") if l != ""]
    vocab_map = {w: i for i, w in enumerate(vocab)}
    labels = [l.strip() for l in (ASSETS / "labels.txt").read_text(encoding="utf-8").split("\n") if l.strip()]
    params = load_model()
    tiers = {t.strip() for t in args.tiers.split(",")}
    if args.train_check:
        train_check(params, vocab_map, labels)
    if args.crosscheck:
        crosscheck(params, vocab_map, [t for t, _ in train_sentences()])

    # 학습 문장(44개)은 train_model.py와 겹치지 않는지 확인용으로 읽는다
    rows = []
    with open(args.set, encoding="utf-8-sig", newline="") as fh:
        for r in csv.DictReader(fh):
            if (r.get("tier") or "core") in tiers and r["text"].strip():
                rows.append(r)

    known = [r for r in rows if r["intent"] in labels]
    skipped = len(rows) - len(known)
    ok = 0
    per = defaultdict(lambda: [0, 0])
    by_style = defaultdict(lambda: [0, 0])
    conf = Counter()
    oov_hits = []
    for r in known:
        ids = preprocess(r["text"], vocab_map)
        pred = labels[int(np.argmax(forward(params, ids)))]
        hit = pred == r["intent"]
        ok += hit
        per[r["intent"]][0] += hit
        per[r["intent"]][1] += 1
        by_style[r["style"]][0] += hit
        by_style[r["style"]][1] += 1
        conf[(r["intent"], pred)] += 1
        toks = r["text"].split()
        oov_hits.append(sum(1 for t in toks if t not in vocab_map) / max(1, len(toks)))

    n = len(known)
    lo_hi = _wilson(ok, n)
    print("=== 초기 로컬 TFLite 인텐트 모델 재현 시험 ===")
    print("시험 세트: %s (tier: %s)" % (Path(args.set).name, ",".join(sorted(tiers))))
    print("모델이 아는 6종 정답 문장 %d개 대상 (나머지 4종 정답 %d개는 모델이 출력 불가 → 제외)" % (n, skipped))
    print("정확도: %.1f%% (%d/%d), 95%% 신뢰구간 %.1f%% ~ %.1f%%" % (ok / n * 100, ok, n, lo_hi[0] * 100, lo_hi[1] * 100))
    print("평균 미등록 어절 비율(어휘에 없어 0으로 처리): %.1f%%" % (np.mean(oov_hits) * 100))
    print("\n인텐트별 정확도")
    for lab in labels:
        if per[lab][1]:
            print("  %-16s %5.1f%% (%d/%d)" % (lab, per[lab][0] / per[lab][1] * 100, per[lab][0], per[lab][1]))
    print("\n문장 유형별 정확도")
    for st, (a, b) in sorted(by_style.items()):
        print("  %-16s %5.1f%% (%d/%d)" % (st, a / b * 100, a, b))
    print("\n※ 앱의 실제 전처리 코드가 저장소에 없어 train_model.py 기준 재현(근사)입니다.")
    print("※ 시험 문장은 AI가 작성한 세트이며 채원 님이 당시 사용한 시험 문장과 다릅니다. 40~45%의 직접 검증이 아니라 '같은 모델에 새 문장 세트를 넣은 참고 결과'입니다.")


def _wilson(k, n, z=1.96):
    import math
    if n == 0:
        return 0.0, 0.0
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


if __name__ == "__main__":
    main()
