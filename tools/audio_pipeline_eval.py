#!/usr/bin/env python3
"""오디오 전처리·전송 조건이 소리 분류 모델(soundModel.tflite, INT8, [danger, noise, speech])의
입력(MFE 특징)과 출력(클래스 확률)에 미치는 영향 — 오프라인 재현 실험.

실험 3종 (전부 PC에서 실행하는 시뮬레이션. 실제 BLE 링크 측정이 아님):
  1. 입력 정규화  : (a) 올바른 파이프라인(피크→32767 정규화 → 프리엠퍼시스 0.98 → ÷32768 → MFE)
                   (b) 피크 정규화 없음   (c) 2026-10-03 이전 앱 버그(int16 원값을 MFE에 직접 투입)
                   (d) 입력 −20/−30dB 감쇠 후 (a)와 (b) — 레벨 민감도
  2. ADPCM 압축   : IMA ADPCM 4:1 인코드→디코드(펌웨어 adpcm.cpp 인코더를 그대로 포팅, 디코더=tools/ima_adpcm.py)
                   SNR, 창별 argmax 일치율, 클래스 확률 평균절대차
  3. 패킷 손실    : BLE notify 1개=AUDIO 패킷 1개=240샘플(15ms) 단위로 통째로 유실(0/3/10/28%).
                   앱(SoundClassifier.push)은 받은 프레임을 링버퍼에 그대로 이어붙이므로(seq 기반 0 채움 없음)
                   기본 모델 = '시간 압축 이어붙임'. 비교용으로 '0 채움' 변형도 함께 보고.

모델·전처리는 tools/sound_eval.py(앱 SoundClassifier.kt와 동일 구현)를 import 해서 재사용한다.
난수 시드 고정(합성 백색소음 seed=0, 손실 시드 0~19). 수치는 모두 이 스크립트 실행 결과다.

사용:  python tools/audio_pipeline_eval.py            (약 1~2분)
"""
import math
import struct
import sys
import time
import wave
from pathlib import Path

import numpy as np

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import sound_eval as se      # noqa: E402  Model, preprocess, mfe, windows, load_audio, CLASSES
import ima_adpcm             # noqa: E402  decode(nibbles, predictor, step_idx)

PKT_SAMPLES = 240            # firmware config.h: DSP_OUT_SAMPLES / BLE_AUDIO_SAMPLES = 240 (16kHz, 15ms) — PCM16·ADPCM 공통
LOSS_RATES = [0.0, 0.03, 0.10, 0.28]
LOSS_SEEDS = 20
HOP = 16000                  # 창 이동 1초 (2초 창)
WINDOW = se.WINDOW           # 32000


# ───────────────────────── 모델 호출 (게이트 우회 버전) ─────────────────────────
class Infer:
    """sound_eval.Model의 인터프리터를 재사용하되 특징 벡터를 직접 넣는다(무음 게이트는 따로 보고)."""

    def __init__(self):
        self.m = se.Model()
        self.isc, self.izp = self.m.inp["quantization"]
        self.osc, self.ozp = self.m.out["quantization"]

    def run(self, feats):
        q = np.clip(np.round(feats / self.isc + self.izp), -128, 127).astype(np.int8)[None, :]
        self.m.it.set_tensor(self.m.inp["index"], q)
        self.m.it.invoke()
        o = (self.m.it.get_tensor(self.m.out["index"])[0].astype(np.float32) - self.ozp) * self.osc
        return o


def rms_of(x):
    return math.sqrt(float((x.astype(np.float64) ** 2).mean()))


def decide(o):
    """앱과 동일: argmax 확률 ≥ 0.6 이면 그 클래스, 아니면 noise."""
    b = int(np.argmax(o))
    return se.CLASSES[b] if o[b] >= se.CONF_GATE else "noise"


# ───────────────────────── 전처리 조건 ─────────────────────────
def feats_correct(w):            # (a) 앱 현재 구현: 피크 정규화 + 프리엠퍼시스 + ÷32768
    return se.mfe(se.preprocess(w, peak_norm=True))


def feats_nopeak(w):             # (b) 피크 정규화 없음 (프리엠퍼시스 + ÷32768 만)
    return se.mfe(se.preprocess(w, peak_norm=False))


def feats_old_bug(w):            # (c) 2026-10-03 이전 앱: int16 원값을 MFE에 직접 (프리엠퍼시스·÷32768 없음)
    return se.mfe(w.astype(np.float64))


def attenuate(w, db):
    """입력을 db만큼 감쇠(조용한 소리 모사). int16 양자화까지 포함."""
    return np.clip(np.round(w.astype(np.float64) * (10 ** (db / 20.0))), -32768, 32767).astype(np.int16)


CONDITIONS = [
    ("(a) 올바른 파이프라인",            lambda w: feats_correct(w)),
    ("(b) 피크 정규화 없음",             lambda w: feats_nopeak(w)),
    ("(c) 구 앱 버그(int16 원값 직접)",   lambda w: feats_old_bug(w)),
    ("(d1) −20dB 입력 + (a)",            lambda w: feats_correct(attenuate(w, -20))),
    ("(d2) −30dB 입력 + (a)",            lambda w: feats_correct(attenuate(w, -30))),
    ("(e1) −20dB 입력 + (b)",            lambda w: feats_nopeak(attenuate(w, -20))),
    ("(e2) −30dB 입력 + (b)",            lambda w: feats_nopeak(attenuate(w, -30))),
]


# ───────────────────────── IMA ADPCM 인코더 (firmware/src/adpcm.cpp 포팅) ─────────────────────────
STEP = ima_adpcm.STEP_TABLE
IDX = ima_adpcm.INDEX_TABLE


class AdpcmEncoder:
    def __init__(self):
        self.pred = 0
        self.idx = 0

    def _one(self, s):
        step = STEP[self.idx]
        diff = int(s) - self.pred
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            nib |= 4; diff -= step; delta += step
        if diff >= (step >> 1):
            nib |= 2; diff -= step >> 1; delta += step >> 1
        if diff >= (step >> 2):
            nib |= 1; delta += step >> 2
        p = self.pred + (-delta if (nib & 8) else delta)
        self.pred = max(-32768, min(32767, p))
        self.idx = max(0, min(88, self.idx + IDX[nib]))
        return nib

    def encode_packet(self, samples):
        """패킷(짝수 샘플) → (헤더 상태(predictor, step_idx), 니블 bytes). 상태는 스트림 연속."""
        state = (self.pred, self.idx)
        out = bytearray()
        for i in range(0, len(samples), 2):
            lo = self._one(samples[i])
            hi = self._one(samples[i + 1]) if i + 1 < len(samples) else 0
            out.append(lo | (hi << 4))
        return state, bytes(out)


def packetize(x):
    """스트림 → 240샘플 패킷 리스트(마지막 자투리는 버림: 펌웨어도 프레임 단위로만 전송)."""
    n = (len(x) // PKT_SAMPLES) * PKT_SAMPLES
    return [x[i:i + PKT_SAMPLES] for i in range(0, n, PKT_SAMPLES)]


def adpcm_roundtrip(x):
    """펌웨어와 같은 방식: 패킷마다 인코딩 직전 상태를 헤더에 싣고, 수신측은 패킷마다 헤더 상태로 디코드."""
    enc = AdpcmEncoder()
    out = []
    for pkt in packetize(x):
        (pred, idx), nib = enc.encode_packet(pkt)
        pcm = ima_adpcm.decode(nib, pred, idx)
        out.append(np.frombuffer(pcm, dtype="<i2"))
    return np.concatenate(out)


def snr_db(ref, test):
    n = min(len(ref), len(test))
    r = ref[:n].astype(np.float64)
    e = r - test[:n].astype(np.float64)
    return 10 * math.log10(float((r ** 2).sum()) / max(float((e ** 2).sum()), 1e-9))


# ───────────────────────── 패킷 손실 모델 ─────────────────────────
def drop_mask(npkt, rate, seed):
    rng = np.random.default_rng(seed)
    return rng.random(npkt) >= rate      # True = 수신


def window_concat(pkts, keep, end_pkt):
    """앱 모델(시간 압축): 원래 시각 end_pkt(패킷 인덱스, exclusive)까지 '수신된' 샘플 중 마지막 32000개.
    = 그 시각에 앱 링버퍼가 들고 있는 2초 창. 수신 샘플이 32000개 미만이면 None."""
    got = [pkts[i] for i in range(end_pkt) if keep[i]]
    if not got:
        return None
    s = np.concatenate(got)
    return s[-WINDOW:] if len(s) >= WINDOW else None


def window_zerofill(pkts, keep, end_pkt):
    """비교용(앱이 하지 않는 방식): 유실 패킷 자리를 0으로 채워 시간축을 보존한 뒤 같은 2초 창."""
    s = np.concatenate([pkts[i] if keep[i] else np.zeros(PKT_SAMPLES, np.int16) for i in range(end_pkt)])
    return s[-WINDOW:]


# ───────────────────────── 테스트 오디오 ─────────────────────────
def load_wav16(path):
    with wave.open(str(path), "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2, path
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def derive_from_48k_ch1(path, hpf_hz=120.0):
    """48kHz 3ch raw 녹음의 ch1 → FIR 안티앨리어싱 3:1 디시메이션 → 2차 Butterworth HPF(펌웨어 빔 경로와 같은 120Hz).
    펌웨어 dsp_pipeline의 16kHz 출력에 가깝게 만든 '파생' 신호(빔포밍은 적용하지 않음)."""
    from scipy.signal import decimate, butter, sosfilt
    with wave.open(str(path), "rb") as w:
        assert w.getframerate() == 48000 and w.getnchannels() == 3, path
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 3)[:, 0].astype(np.float64)
    y = decimate(x, 3, ftype="fir", zero_phase=True)
    sos = butter(2, hpf_hz, btype="highpass", fs=16000, output="sos")
    y = sosfilt(sos, y)
    return np.clip(np.round(y), -32768, 32767).astype(np.int16)


def synth_white_noise(seconds=2.0, std=3000.0, seed=0):
    rng = np.random.default_rng(seed)
    return np.clip(np.round(rng.normal(0, std, int(16000 * seconds))), -32768, 32767).astype(np.int16)


def build_clips():
    rec = ROOT / "recordings"
    clips = [
        ("test_sound.pcm", "앱 자가시험 PCM(assets) — danger 기대", "실측(앱 동봉)",
         se.load_audio(ROOT / "android/app/src/main/assets/test_sound.pcm")),
        ("8dos090_ch1→16k", "안경 착용 1m 핑거스냅 8방향 녹음(2026-09-10) ch1을 PC에서 16k 변환+HPF120", "파생",
         derive_from_48k_ch1(rec / "8dos090.wav")),
        ("wav260910073855", "WiFi 16k 빔 스트림 녹음(2026-09-10 청취 시험, 내용 미라벨)", "실측(16k 스트림)",
         load_wav16(rec / "wav260910073855.wav")),
        ("wav260910041425", "WiFi 16k 빔 스트림 저레벨 녹음(2026-09-10, RMS≈190, 내용 미라벨)", "실측(16k 스트림)",
         load_wav16(rec / "wav260910041425.wav")),
        ("blewav260910075554", "BLE 16k 스트림 녹음(2026-09-10 교차 실측 중, 내용 미라벨, ADPCM 경유 가능)", "실측(16k 스트림)",
         load_wav16(rec / "blewav260910075554.wav")),
        ("white_noise_synth", "합성 백색소음 2.0s (가우시안 σ=3000, seed=0)", "합성",
         synth_white_noise()),
    ]
    return clips


def clip_windows(x):
    return [w for _, w in se.windows(x, HOP)]


def fmt_p(o):
    return " ".join(f"{c}={v:.2f}" for c, v in zip(se.CLASSES, o))


def hr(title):
    print("\n" + "=" * 100)
    print(title)
    print("=" * 100)


# ───────────────────────── 실험 1 ─────────────────────────
def exp1(inf, clips):
    hr("실험 1. 입력 정규화 조건별 모델 입력(특징 포화율)·출력(클래스 확률) — 2초 창, 1초 이동")
    print("특징 포화율 = MFE 특징 6368개 중 값이 1.0인 비율(평균). 확률은 창 평균. 판정 = argmax≥0.6 아니면 noise(앱 규칙).")
    print("무음게이트 = 앱이 원값 RMS<150으로 모델을 건너뛰는 창 비율(이 표의 확률은 게이트를 무시하고 모델을 돌린 값).")
    results = {}
    for name, desc, kind, x in clips:
        wins = clip_windows(x)
        print(f"\n[{name}] {desc} | {kind} | {len(x) / 16000:.2f}s 피크 {int(np.abs(x).max())} RMS {rms_of(x):.0f} | 창 {len(wins)}개")
        print(f"  {'조건':34s} {'포화율':>7s} {'무음게이트':>9s} | {'danger':>6s} {'noise':>6s} {'speech':>6s} | 판정 분포")
        results[name] = {}
        for cname, fn in CONDITIONS:
            probs, sat, gated, votes = [], [], 0, {}
            att = -20 if "−20dB" in cname else (-30 if "−30dB" in cname else 0)
            for w in wins:
                win_in = attenuate(w, att) if att else w
                if rms_of(win_in) < se.SILENCE_RMS:
                    gated += 1
                f = fn(w)
                sat.append(float((f >= 1.0).mean()))
                o = inf.run(f)
                probs.append(o)
                d = decide(o)
                votes[d] = votes.get(d, 0) + 1
            P = np.mean(probs, axis=0)
            results[name][cname] = dict(p=P, sat=float(np.mean(sat)), gated=gated / len(wins), votes=votes)
            vs = " ".join(f"{k}:{v}" for k, v in sorted(votes.items()))
            print(f"  {cname:34s} {np.mean(sat) * 100:6.1f}% {gated / len(wins) * 100:8.0f}% | "
                  f"{P[0]:6.2f} {P[1]:6.2f} {P[2]:6.2f} | {vs}")
    return results


# ───────────────────────── 실험 2 ─────────────────────────
def exp2(inf, clips):
    hr("실험 2. IMA ADPCM 4:1 인코드→디코드가 신호·모델 출력에 미치는 영향 (올바른 파이프라인으로 분류)")
    print(f"패킷 = {PKT_SAMPLES}샘플, 패킷마다 인코딩 직전 상태(predictor, step_idx)를 헤더에 실어 패킷 단위 리싱크(§6.1).")
    t = np.arange(WINDOW) / 16000.0
    sine = np.round(10000 * np.sin(2 * np.pi * 1000 * t)).astype(np.int16)
    print(f"참고(코덱 자체 기준값): 1kHz 사인 진폭 10000 왕복 SNR = {snr_db(sine, adpcm_roundtrip(sine)):.1f} dB "
          f"(펌웨어 구현 당시 기록 26.8dB와 비교용)")
    print(f"  {'클립':22s} {'SNR(dB)':>8s} {'창':>4s} {'argmax 일치':>11s} {'|Δp| 평균':>9s} {'|Δp| 최대':>9s} | 원본 판정 → 디코드 판정 (불일치만)")
    out = {}
    for name, desc, kind, x in clips:
        dec = adpcm_roundtrip(x)
        n = len(dec)
        snr = snr_db(x[:n], dec)
        wo, wd = clip_windows(x[:n]), clip_windows(dec)
        agree, dps, mism = 0, [], []
        for a, b in zip(wo, wd):
            po, pd = inf.run(feats_correct(a)), inf.run(feats_correct(b))
            do, dd = decide(po), decide(pd)
            agree += int(do == dd)
            dps.append(np.abs(po - pd))
            if do != dd:
                mism.append(f"{do}→{dd}")
        dps = np.array(dps)
        out[name] = dict(snr=snr, n=len(wo), agree=agree, mad=float(dps.mean()), mx=float(dps.max()))
        print(f"  {name:22s} {snr:8.1f} {len(wo):4d} {agree:4d}/{len(wo):<6d} {dps.mean():9.3f} {dps.max():9.3f} | {', '.join(mism) or '-'}")
    return out


# ───────────────────────── 실험 3 ─────────────────────────
EXP3_FIRST_END_PKT = 200     # 첫 평가 시각 = 3.0초(200패킷): 28% 유실에서도 2초 창을 채울 수신 샘플이 있도록
EXP3_HOP_PKT = 67            # 평가 간격 ≈1.0초(67패킷=1.005s; 16000/240은 정수가 아님)


def exp3(inf, clips, names, quiet_prefix):
    hr("실험 3. BLE 패킷 유실 시뮬레이션 — 240샘플(15ms) 패킷을 iid 확률로 통째로 유실, 시드 20개")
    print("앱 모델(이어붙임): SoundClassifier.push()는 수신 프레임을 링버퍼에 순서대로 넣기만 하므로 유실분은 시간 압축되어 사라진다")
    print("  → 같은 시각의 2초 창에 유실 비율만큼 '더 과거'의 오디오가 끌려 들어오고, 유실 경계마다 파형 불연속이 생긴다.")
    print("0채움(비교용): 유실 자리에 0을 넣어 시간축을 보존하는 가상의 방식(앱 미구현).")
    print(f"기준 = 유실 0%의 같은 시각 창. 평가 시각 = {EXP3_FIRST_END_PKT * PKT_SAMPLES / 16000:.1f}s부터 {EXP3_HOP_PKT * PKT_SAMPLES / 16000:.3f}s 간격.")
    print("일치율 = 판정(argmax≥0.6 규칙)이 기준과 같은 (시드×창) 비율. |Δp| = 클래스 확률 평균절대차.")
    out = {}
    for name, desc, kind, x in clips:
        if name not in names:
            continue
        stream = x
        note = ""
        if len(x) < 2 * WINDOW:
            # 2초짜리 클립은 유실 후 2초 창을 채울 '과거' 샘플이 없다 → 실제 저레벨 스트림 녹음 2초를 앞에 붙인
            # 4초 스트림(조용한 실내 → 소리 발생 시나리오)의 마지막 창(원래 시각 4.0s)을 평가. 유실 0%면 창 = 클립 자체.
            stream = np.concatenate([quiet_prefix[:WINDOW], x])
            note = " (앞에 wav260910041425 첫 2초를 붙인 4초 스트림, 마지막 창 1개)"
        pkts = packetize(stream)
        npkt = len(pkts)
        ends = [npkt] if note else list(range(EXP3_FIRST_END_PKT, npkt + 1, EXP3_HOP_PKT))
        base = {}
        for e in ends:
            w = np.concatenate(pkts[:e])[-WINDOW:]
            base[e] = inf.run(feats_correct(w))
        print(f"\n[{name}] {desc}{note} | 패킷 {npkt}개, 평가 창 {len(ends)}개 | 기준 판정: "
              + ", ".join(f"{decide(p)}({p[0]:.2f})" for p in base.values()))
        print(f"  {'유실률':>6s} | {'── 앱 모델: 이어붙임(시간 압축) ──':^46s} | {'── 비교: 0채움 ──':^46s}")
        print(f"  {'':6s} | {'danger p 평균±sd':>18s} {'일치율':>7s} {'|Δp|':>6s} {'창 부족':>8s} | {'danger p 평균±sd':>18s} {'일치율':>7s} {'|Δp|':>6s}")
        out[name] = {}
        for rate in LOSS_RATES:
            acc = {"concat": [], "zero": []}
            short = 0
            for seed in range(LOSS_SEEDS):
                keep = drop_mask(npkt, rate, seed)
                for e in ends:
                    wc = window_concat(pkts, keep, e)
                    if wc is None:
                        short += 1
                    else:
                        pc = inf.run(feats_correct(wc))
                        acc["concat"].append((pc, decide(pc) == decide(base[e]), np.abs(pc - base[e]).mean()))
                    wz = window_zerofill(pkts, keep, e)
                    pz = inf.run(feats_correct(wz))
                    acc["zero"].append((pz, decide(pz) == decide(base[e]), np.abs(pz - base[e]).mean()))
            row = {}
            for k in ("concat", "zero"):
                ps = np.array([a[0] for a in acc[k]])
                row[k] = dict(dm=float(ps[:, 0].mean()), ds=float(ps[:, 0].std()),
                              agree=float(np.mean([a[1] for a in acc[k]])),
                              dp=float(np.mean([a[2] for a in acc[k]])), n=len(acc[k]))
            out[name][rate] = row
            c, z = row["concat"], row["zero"]
            print(f"  {rate * 100:5.0f}% | {c['dm']:9.3f} ± {c['ds']:5.3f} {c['agree'] * 100:6.1f}% {c['dp']:6.3f} {short:8d} | "
                  f"{z['dm']:9.3f} ± {z['ds']:5.3f} {z['agree'] * 100:6.1f}% {z['dp']:6.3f}")
    return out


def main():
    t0 = time.time()
    print("오디오 전처리·전송 영향 오프라인 실험 — tools/audio_pipeline_eval.py")
    print(f"모델: {se.MODEL.relative_to(ROOT)} | 클래스 {se.CLASSES} | 창 {WINDOW}샘플(2.0s) | 패킷 {PKT_SAMPLES}샘플 | numpy {np.__version__}")
    inf = Infer()
    clips = build_clips()

    hr("테스트 오디오")
    for name, desc, kind, x in clips:
        print(f"  {name:22s} {len(x) / 16000:6.2f}s 피크 {int(np.abs(x).max()):5d} RMS {rms_of(x):7.1f} | {kind:12s} | {desc}")

    r1 = exp1(inf, clips)
    r2 = exp2(inf, clips)
    quiet = next(x for n, _, _, x in clips if n == "wav260910041425")
    r3 = exp3(inf, clips, names={"test_sound.pcm", "8dos090_ch1→16k", "blewav260910075554"}, quiet_prefix=quiet)
    print(f"\n완료 — {time.time() - t0:.0f}s")
    return r1, r2, r3


if __name__ == "__main__":
    main()
