#!/usr/bin/env python3
"""LLM 인텐트 분류 시험 — 앱(IntentClassifier.kt)과 같은 프롬프트·요청·판정 규칙으로
시험 문장 세트를 학교 LLM 게이트웨이에 보내고 정확도·P/R/F1·혼동행렬·응답 시간을 계산한다.

사용 예 (프로젝트 루트에서)
  python tools/intent_eval.py --dry-run                      # API 호출 없이 스크립트 점검(모의 분류기, 결과 인용 금지)
  python tools/intent_eval.py --key-from-source --quick      # 약 30문장 빠른 시험
  python tools/intent_eval.py --key-from-source              # 전체 세트(core+boundary)

API 키: 환경변수 INTENT_API_KEY, 또는 --key-from-source(IntentClassifier.kt에서 읽음). 키는 출력·결과 파일에 기록하지 않는다.
시험 범위: PC → 게이트웨이 텍스트 입력. STT·BLE·앱 구간은 포함하지 않는다(응답 시간도 PC 기준 네트워크 왕복).
"""
import argparse
import csv
import hashlib
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
KT = ROOT / "android/app/src/main/java/com/smartglass/test/IntentClassifier.kt"
DEFAULT_SET = ROOT / "tools/intent_testset.csv"
OUT_ROOT = ROOT / "tools/intent_eval_results"

# 보고서·혼동행렬 표시 순서
LABELS = ["VOLUME_UP", "VOLUME_DOWN", "MODE_TV", "MODE_ONE_ON_ONE", "MODE_MULTI_TALK",
          "MODE_CALL", "MODE_OUTDOOR", "AUTO_MODE_ON", "AUTO_MODE_OFF", "UNKNOWN"]
# 앱 mapIntent()의 판정 순서(첫 번째로 포함된 토큰이 채택됨). 어느 것도 없으면 UNKNOWN
APP_ORDER = ["VOLUME_UP", "VOLUME_DOWN", "MODE_TV", "MODE_ONE_ON_ONE", "MODE_MULTI_TALK",
             "MODE_OUTDOOR", "MODE_CALL", "AUTO_MODE_ON", "AUTO_MODE_OFF"]
# 앱에서 같은 동작으로 처리되는 인텐트(통화는 일대일 모드로 매핑됨)
ACTION_ALIAS = {"MODE_CALL": "MODE_ONE_ON_ONE"}

FALLBACK_PROMPT = (
    "너는 보청기를 제어하는 AI야. 사용자의 말을 듣고 반드시 다음 단어들 중 '하나만' 대답해. 부가 설명은 절대 하지마.\n"
    "- 소리를 키워달라면: 'VOLUME_UP'\n- 소리를 줄여달라면: 'VOLUME_DOWN'\n"
    "- TV/테레비 본다면: 'MODE_TV'\n- 한 명이랑 대화한다면: 'MODE_ONE_ON_ONE'\n"
    "- 여러명이랑 대화한다면: 'MODE_MULTI_TALK'\n- 전화/통화 한다면: 'MODE_CALL'\n"
    "- 밖/야외/산책 나간다면: 'MODE_OUTDOOR'\n- 자동 모드 켜달라면: 'AUTO_MODE_ON'\n"
    "- 수동으로 바꾼다거나 자동 모드 꺼달라면: 'AUTO_MODE_OFF'\n- 관련 없는 말이면: 'UNKNOWN'"
)


# ------------------------------------------------------------------ 앱 소스에서 설정 읽기
def read_app_config():
    """IntentClassifier.kt에서 프롬프트·URL·모델명을 읽는다. 실패하면 내장 사본을 쓴다."""
    cfg = {"prompt": FALLBACK_PROMPT, "prompt_source": "내장 사본", "url": None, "model": "claude-sonnet-5",
           "key": None}
    if not KT.exists():
        return cfg
    src = KT.read_text(encoding="utf-8")
    m = re.search(r'API_URL\s*=\s*"([^"]+)"', src)
    if m:
        cfg["url"] = m.group(1)
    m = re.search(r'put\("model",\s*"([^"]+)"\)', src)
    if m:
        cfg["model"] = m.group(1)
    m = re.search(r'API_KEY\s*=\s*"([^"]+)"', src)
    if m:
        cfg["key"] = m.group(1)
    m = re.search(r"SYSTEM_PROMPT\s*=\s*(.*?)\n\s*\}", src, re.S)
    if m:
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
        text = "".join(lits).replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\")
        if text.strip():
            cfg["prompt"] = text
            cfg["prompt_source"] = "IntentClassifier.kt"
    return cfg


# ------------------------------------------------------------------ 판정·통신
def app_decision(raw):
    """앱 mapIntent()와 동일: 응답에 포함된 첫 토큰(APP_ORDER 순서). 없으면 UNKNOWN."""
    for t in APP_ORDER:
        if t in raw:
            return t
    return "UNKNOWN"


def call_llm(cfg, key, text, timeout, retries=2):
    body = json.dumps({
        "model": cfg["model"], "temperature": 0.0,
        "messages": [{"role": "system", "content": cfg["prompt"]}, {"role": "user", "content": text}],
    }, ensure_ascii=False).encode("utf-8")
    last_err = None
    for attempt in range(retries + 1):
        req = urllib.request.Request(cfg["url"], data=body, method="POST", headers={
            "Authorization": "Bearer " + key, "Content-Type": "application/json"})
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
            dt = time.perf_counter() - t0
            content = payload["choices"][0]["message"].get("content", "").strip()
            return {"raw": content, "latency": dt, "retries": attempt, "error": None}
        except urllib.error.HTTPError as e:
            last_err = "HTTP %d" % e.code
            if e.code not in (429, 500, 502, 503, 504):
                break
        except Exception as e:  # 네트워크·파싱 오류. 키가 들어갈 수 있는 요청 헤더는 메시지에 쓰지 않는다
            last_err = type(e).__name__
        time.sleep(1.0 * (attempt + 1))
    return {"raw": None, "latency": None, "retries": retries, "error": last_err}


def mock_llm(text):
    """--dry-run 전용 모의 분류기. 일부러 불완전하게 만들어 지표 계산 코드를 점검한다. 실제 결과가 아니다."""
    rules = [("키워", "VOLUME_UP"), ("올려", "VOLUME_UP"), ("크게", "VOLUME_UP"), ("줄여", "VOLUME_DOWN"),
             ("시끄", "VOLUME_DOWN"), ("TV", "MODE_TV"), ("티비", "MODE_TV"), ("테레비", "MODE_TV"),
             ("텔레비", "MODE_TV"), ("전화", "MODE_CALL"), ("통화", "MODE_CALL"), ("산책", "MODE_OUTDOOR"),
             ("밖", "MODE_OUTDOOR"), ("야외", "MODE_OUTDOOR"), ("자동", "AUTO_MODE_ON"), ("수동", "AUTO_MODE_OFF"),
             ("같이", "MODE_MULTI_TALK"), ("다들", "MODE_MULTI_TALK"), ("둘이", "MODE_ONE_ON_ONE")]
    label = "UNKNOWN"
    for k, v in rules:
        if k in text:
            label = v
            break
    if "꺼" in text and "자동" in text:
        label = "AUTO_MODE_OFF"
    raw = label + "입니다." if len(text) % 11 == 0 else label  # 형식 위반 사례도 섞는다
    return {"raw": raw, "latency": 0.001 * (len(text) % 7 + 1), "retries": 0, "error": None}


# ------------------------------------------------------------------ 통계
def wilson(k, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


def pct(sorted_vals, q):
    if not sorted_vals:
        return None
    idx = max(0, min(len(sorted_vals) - 1, math.ceil(q * len(sorted_vals)) - 1))
    return sorted_vals[idx]


def compute_metrics(rows):
    ok = [r for r in rows if r["error"] is None]
    n = len(ok)
    conf = {a: {b: 0 for b in LABELS} for a in LABELS}
    for r in ok:
        conf[r["expected"]][r["predicted"]] += 1
    per = {}
    for lab in LABELS:
        tp = conf[lab][lab]
        fp = sum(conf[o][lab] for o in LABELS if o != lab)
        fn = sum(conf[lab][o] for o in LABELS if o != lab)
        p = tp / (tp + fp) if tp + fp else 0.0
        r_ = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * p * r_ / (p + r_) if p + r_ else 0.0
        per[lab] = {"precision": p, "recall": r_, "f1": f1, "support": tp + fn, "tp": tp, "fp": fp, "fn": fn}
    sup = [per[l] for l in LABELS if per[l]["support"] > 0]
    correct = sum(1 for r in ok if r["correct"])
    act_correct = sum(1 for r in ok if r["action_correct"])
    lo, hi = wilson(correct, n)
    unk = [r for r in ok if r["expected"] == "UNKNOWN"]
    cmd = [r for r in ok if r["expected"] != "UNKNOWN"]
    lat = sorted(r["latency"] for r in ok if r["latency"] is not None)
    by = lambda key: {k: _acc([r for r in ok if (r[key] or "-") == k]) for k in sorted({(r[key] or "-") for r in ok})}
    return {
        "n_total": len(rows), "n_valid": n, "n_error": len(rows) - n,
        "accuracy": correct / n if n else 0.0, "correct": correct, "acc_ci95": [lo, hi],
        "action_accuracy": act_correct / n if n else 0.0,
        "macro_precision": sum(x["precision"] for x in sup) / len(sup) if sup else 0.0,
        "macro_recall": sum(x["recall"] for x in sup) / len(sup) if sup else 0.0,
        "macro_f1": sum(x["f1"] for x in sup) / len(sup) if sup else 0.0,
        "format_ok_rate": sum(1 for r in ok if r["format_ok"]) / n if n else 0.0,
        "unknown_reject_rate": (sum(1 for r in unk if r["predicted"] == "UNKNOWN") / len(unk)) if unk else None,
        "false_trigger_rate": (sum(1 for r in unk if r["predicted"] != "UNKNOWN") / len(unk)) if unk else None,
        "command_missed_as_unknown_rate": (sum(1 for r in cmd if r["predicted"] == "UNKNOWN") / len(cmd)) if cmd else None,
        "latency_ms": {"p50": _ms(pct(lat, .50)), "p95": _ms(pct(lat, .95)), "p99": _ms(pct(lat, .99)),
                       "max": _ms(lat[-1] if lat else None), "n": len(lat)},
        "per_intent": per, "confusion": conf, "by_style": by("style"), "by_tier": by("tier"),
    }


def _acc(rs):
    k = sum(1 for r in rs if r["correct"])
    lo, hi = wilson(k, len(rs))
    return {"n": len(rs), "correct": k, "accuracy": k / len(rs) if rs else 0.0, "ci95": [lo, hi]}


def _ms(v):
    return None if v is None else round(v * 1000, 1)


# ------------------------------------------------------------------ 보고서
def f(x, d=3):
    return "-" if x is None else ("%.*f" % (d, x))


def p1(x):
    return "-" if x is None else "%.1f%%" % (x * 100)


def build_report(m, meta):
    L = []
    L.append("# LLM 인텐트 분류 시험 결과")
    L.append("")
    if meta["dry_run"]:
        L.append("> ⚠ **DRY-RUN 결과 — 모의 분류기로 스크립트만 점검한 것이며 실제 성능이 아닙니다. 어디에도 인용하지 마세요.**")
        L.append("")
    L.append("## 시험 조건")
    L.append("")
    L.append("| 항목 | 값 |")
    L.append("|---|---|")
    L.append("| 일시 | %s |" % meta["when"])
    L.append("| 모델 | %s (temperature 0.0) |" % meta["model"])
    L.append("| 호출 대상 | %s |" % meta["host"])
    L.append("| 프롬프트 출처 | %s |" % meta["prompt_source"])
    L.append("| 시험 세트 | %s (sha256 %s) |" % (meta["set_name"], meta["set_hash"]))
    L.append("| 포함 구분(tier) | %s |" % ", ".join(meta["tiers"]))
    L.append("| 문장 수 | 전체 %d, 유효 응답 %d, 오류 %d |" % (m["n_total"], m["n_valid"], m["n_error"]))
    L.append("| 판정 규칙 | 앱 mapIntent()와 동일(응답에 포함된 첫 인텐트 토큰, 없으면 UNKNOWN) |")
    L.append("| 범위 | PC에서 텍스트 직접 입력. STT·BLE·앱 구간 제외 |")
    L.append("")
    L.append("## 핵심 결과")
    L.append("")
    lo, hi = m["acc_ci95"]
    L.append("| 지표 | 값 |")
    L.append("|---|---|")
    L.append("| 인텐트 분류 정확도 | **%s** (%d/%d, 95%% 신뢰구간 %s ~ %s) |" % (p1(m["accuracy"]), m["correct"], m["n_valid"], p1(lo), p1(hi)))
    L.append("| 동작 기준 정확도(통화=일대일 동일 처리) | %s |" % p1(m["action_accuracy"]))
    L.append("| Macro precision / recall / F1 | %s / %s / %s |" % (f(m["macro_precision"]), f(m["macro_recall"]), f(m["macro_f1"])))
    L.append("| 응답 형식 준수율(토큰 하나만 반환) | %s |" % p1(m["format_ok_rate"]))
    L.append("| 무관 발화 거부율(UNKNOWN 재현율) | %s |" % p1(m["unknown_reject_rate"]))
    L.append("| 오작동률(무관 발화인데 명령으로 실행) | %s |" % p1(m["false_trigger_rate"]))
    L.append("| 명령 미인식률(명령인데 UNKNOWN 처리) | %s |" % p1(m["command_missed_as_unknown_rate"]))
    lt = m["latency_ms"]
    L.append("| 응답 시간 p50 / p95 / p99 / max (ms) | %s / %s / %s / %s (n=%d) |" % (lt["p50"], lt["p95"], lt["p99"], lt["max"], lt["n"]))
    L.append("")
    L.append("## 인텐트별 지표")
    L.append("")
    L.append("| 인텐트 | Precision | Recall | F1 | 문장 수 |")
    L.append("|---|---|---|---|---|")
    for lab in LABELS:
        x = m["per_intent"][lab]
        L.append("| %s | %s | %s | %s | %d |" % (lab, f(x["precision"]), f(x["recall"]), f(x["f1"]), x["support"]))
    L.append("")
    L.append("## 혼동행렬 (행=정답, 열=예측)")
    L.append("")
    L.append("| 정답＼예측 | " + " | ".join(LABELS) + " |")
    L.append("|---|" + "---|" * len(LABELS))
    for a in LABELS:
        L.append("| %s | " % a + " | ".join(str(m["confusion"][a][b]) for b in LABELS) + " |")
    L.append("")
    for title, key in (("문장 유형(style)별 정확도", "by_style"), ("검증 구분(tier)별 정확도", "by_tier")):
        L.append("## " + title)
        L.append("")
        L.append("| 구분 | 문장 수 | 정답 | 정확도 | 95% 신뢰구간 |")
        L.append("|---|---|---|---|---|")
        for k, v in m[key].items():
            L.append("| %s | %d | %d | %s | %s ~ %s |" % (k, v["n"], v["correct"], p1(v["accuracy"]), p1(v["ci95"][0]), p1(v["ci95"][1])))
        L.append("")
    L.append("## 오분류 목록")
    L.append("")
    wrong = [r for r in meta["rows"] if r["error"] is None and not r["correct"]]
    if not wrong:
        L.append("오분류 없음.")
    else:
        L.append("| id | 문장 | 정답 | 예측 | 원본 응답 | 유형 | 구분 |")
        L.append("|---|---|---|---|---|---|---|")
        for r in wrong:
            L.append("| %s | %s | %s | %s | %s | %s | %s |" % (r["id"], r["text"].replace("|", "/"), r["expected"], r["predicted"],
                                                               (r["raw"] or "").replace("|", "/").replace("\n", " ")[:60], r["style"], r["tier"]))
    L.append("")
    errs = [r for r in meta["rows"] if r["error"] is not None]
    if errs:
        L.append("## 호출 오류 (정확도 계산에서 제외)")
        L.append("")
        for r in errs:
            L.append("- id %s: %s" % (r["id"], r["error"]))
        L.append("")
    L.append("## 해석 시 주의")
    L.append("")
    L.append("- 시험 문장은 AI가 작성하고 AI 라벨러 3인이 교차 검증한 것입니다. 같은 계열 모델에 유리하게 쉬울 수 있으므로, **사람이 검토한 문장·실제 발화로 교차 확인하기 전에는 '높은 정확도'라고 단정하지 마세요.**")
    L.append("- core는 라벨러 전원이 정답과 일치하고 모호하다는 표시가 없던 문장, boundary는 그렇지 않은 경계 사례입니다. 두 구분의 정확도를 따로 보고하세요.")
    L.append("- 응답 시간은 PC→게이트웨이 왕복이며 폰의 STT 구간은 포함하지 않습니다.")
    L.append("- 문장 수가 작으면 신뢰구간이 넓습니다. 정확도는 반드시 문장 수·신뢰구간과 함께 쓰세요.")
    return "\n".join(L) + "\n"


# ------------------------------------------------------------------ 메인
def load_set(path, tiers):
    rows = []
    with open(path, encoding="utf-8-sig", newline="") as fh:
        for r in csv.DictReader(fh):
            text = (r.get("text") or "").strip()
            intent = (r.get("intent") or "").strip()
            if not text or (r.get("tier") or "core") not in tiers:
                continue
            if intent not in LABELS:
                sys.exit("시험 세트의 intent 값이 올바르지 않습니다: id=%s intent=%r" % (r.get("id"), intent))
            rows.append({"id": r.get("id", ""), "text": text, "expected": intent,
                         "style": (r.get("style") or "").strip(), "tier": (r.get("tier") or "core").strip()})
    return rows


def quick_subset(rows, per_intent=3):
    out, cnt = [], {}
    for r in sorted(rows, key=lambda x: (x["tier"] != "core", int(x["id"]) if str(x["id"]).isdigit() else 0)):
        if cnt.get(r["expected"], 0) < per_intent:
            cnt[r["expected"]] = cnt.get(r["expected"], 0) + 1
            out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(description="LLM 인텐트 분류 시험")
    ap.add_argument("--set", default=str(DEFAULT_SET), help="시험 세트 CSV")
    ap.add_argument("--tiers", default="core,boundary", help="포함할 tier (쉼표 구분)")
    ap.add_argument("--quick", action="store_true", help="인텐트별 3문장(약 30문장)만")
    ap.add_argument("--limit", type=int, default=0, help="앞에서 N문장만")
    ap.add_argument("--workers", type=int, default=2, help="동시 요청 수(기본 2)")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--key-env", default="INTENT_API_KEY", help="API 키 환경변수 이름")
    ap.add_argument("--key-from-source", action="store_true", help="IntentClassifier.kt에서 키를 읽음")
    ap.add_argument("--dry-run", action="store_true", help="API 호출 없이 모의 분류기로 점검")
    ap.add_argument("--yes", action="store_true", help="전송 확인 질문 생략")
    args = ap.parse_args()

    cfg = read_app_config()
    tiers = [t.strip() for t in args.tiers.split(",") if t.strip()]
    rows = load_set(args.set, tiers)
    if args.quick:
        rows = quick_subset(rows)
    if args.limit:
        rows = rows[: args.limit]
    if not rows:
        sys.exit("시험 문장이 없습니다. --set / --tiers 를 확인하세요.")

    key = None
    if not args.dry_run:
        key = os.environ.get(args.key_env) or (cfg["key"] if args.key_from_source else None)
        if not key:
            sys.exit("API 키가 없습니다. 환경변수 %s 를 설정하거나 --key-from-source 를 사용하세요." % args.key_env)
        if not cfg["url"]:
            sys.exit("API URL을 읽지 못했습니다(IntentClassifier.kt 확인).")
        host = urlparse(cfg["url"]).netloc
        print("프롬프트 출처: %s / 모델: %s" % (cfg["prompt_source"], cfg["model"]))
        print("%d문장을 %s 로 전송합니다(문장 텍스트만 전송, 음성·영상 없음)." % (len(rows), host))
        if not args.yes and input("계속하려면 y 입력: ").strip().lower() != "y":
            sys.exit("취소했습니다.")
    else:
        host = "(dry-run: 호출 없음)"

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S") + ("_quick" if args.quick else "") + ("_DRYRUN" if args.dry_run else "")
    out = OUT_ROOT / stamp
    n_dup = 1
    while out.exists():
        n_dup += 1
        out = OUT_ROOT / ("%s_%d" % (stamp, n_dup))
    out.mkdir(parents=True)
    jl = open(out / "predictions.jsonl", "w", encoding="utf-8")

    def work(r):
        res = mock_llm(r["text"]) if args.dry_run else call_llm(cfg, key, r["text"], args.timeout)
        row = dict(r)
        row.update(res)
        if res["error"] is None:
            raw = res["raw"] or ""
            row["predicted"] = app_decision(raw)
            row["format_ok"] = raw.strip() in LABELS
            row["correct"] = row["predicted"] == r["expected"]
            row["action_correct"] = ACTION_ALIAS.get(row["predicted"], row["predicted"]) == ACTION_ALIAS.get(r["expected"], r["expected"])
        else:
            row.update({"predicted": None, "format_ok": False, "correct": False, "action_correct": False})
        return row

    done = []
    t_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as ex:
        futs = [ex.submit(work, r) for r in rows]
        try:
            for i, fu in enumerate(as_completed(futs), 1):
                row = fu.result()
                done.append(row)
                jl.write(json.dumps(row, ensure_ascii=False) + "\n")
                jl.flush()
                if i % 10 == 0 or i == len(rows):
                    print("  %d/%d 완료 (%.0fs)" % (i, len(rows), time.perf_counter() - t_start))
        except KeyboardInterrupt:
            print("중단됨 — 지금까지의 결과로 보고서를 만듭니다.")
            for fu in futs:
                fu.cancel()
    jl.close()

    done.sort(key=lambda r: int(r["id"]) if str(r["id"]).isdigit() else 0)
    m = compute_metrics(done)
    meta = {"dry_run": args.dry_run, "when": datetime.now().strftime("%Y-%m-%d %H:%M"), "model": cfg["model"], "host": host,
            "prompt_source": cfg["prompt_source"], "set_name": Path(args.set).name,
            "set_hash": hashlib.sha256(Path(args.set).read_bytes()).hexdigest()[:12], "tiers": tiers, "rows": done}
    (out / "report.md").write_text(build_report(m, meta), encoding="utf-8")
    (out / "summary.json").write_text(json.dumps(m, ensure_ascii=False, indent=2), encoding="utf-8")
    with open(out / "predictions.csv", "w", encoding="utf-8-sig", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["id", "text", "expected", "predicted", "raw", "format_ok", "correct", "action_correct", "style", "tier",
                    "latency_ms", "retries", "error"])
        for r in done:
            w.writerow([r["id"], r["text"], r["expected"], r["predicted"], r["raw"], r["format_ok"], r["correct"],
                        r["action_correct"], r["style"], r["tier"], _ms(r["latency"]), r["retries"], r["error"]])
    with open(out / "confusion.csv", "w", encoding="utf-8-sig", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["정답＼예측"] + LABELS)
        for a in LABELS:
            w.writerow([a] + [m["confusion"][a][b] for b in LABELS])

    print("\n결과 폴더: %s" % out)
    print("정확도 %s (%d/%d), 오류 %d건" % (p1(m["accuracy"]), m["correct"], m["n_valid"], m["n_error"]))
    if args.dry_run:
        print("※ DRY-RUN: 모의 분류기 결과이므로 실제 성능이 아닙니다.")


if __name__ == "__main__":
    main()
