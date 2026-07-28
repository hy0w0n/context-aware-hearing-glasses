#!/usr/bin/env python3
"""빔포밍(delay-and-sum) + 3:1 decimation 프로토타입 — 펌웨어 C 포팅 전 검증.
3ch 48kHz WAV → 지정 방향으로 조향한 mono 16kHz WAV (YAMNet 입력 규격).

사용법: python tools/beamform_prototype.py recordings\az000.wav --steer 0
출력:   입력파일명_beam16k.wav (+ 비교용 ch1 단독 _ch1_16k.wav)
"""
import argparse
import sys
import wave
from pathlib import Path

import numpy as np

# doa_prototype와 동일한 좌표계/보정 (변경 시 양쪽 동기화)
C_SOUND = 343.0
MIRROR = -1.0
MIC_POS = np.array([[MIRROR * -0.04, 0.0], [MIRROR * +0.04, 0.0], [0.0, 0.0447]])
BUS_OFFSET_CH = [0.0, 0.0, 1.0]  # 채널별 시작 오프셋(샘플): ch3=+1 (2026-07-28 실측)


def steer_delays(theta_deg, rate):
    """각 채널의 조향 지연(샘플). 가장 늦게 듣는 마이크 기준으로 양수 정렬"""
    th = np.deg2rad(theta_deg)
    u = np.array([np.sin(th), np.cos(th)])
    t = -MIC_POS @ u / C_SOUND * rate  # 도착 지연 (샘플, 상대값)
    t = t + np.array(BUS_OFFSET_CH)    # 버스 오프셋 보정
    return t - t.min()


def fractional_shift(sig, delay):
    """선형 보간 소수 지연 (펌웨어도 동일 방식 예정 — 음성 대역에서 충분)"""
    n = len(sig)
    idx = np.arange(n) - delay
    i0 = np.clip(np.floor(idx).astype(int), 0, n - 1)
    i1 = np.clip(i0 + 1, 0, n - 1)
    frac = idx - np.floor(idx)
    return sig[i0] * (1 - frac) + sig[i1] * frac


def lowpass_fir(cutoff_hz, rate, ntaps=63):
    """윈도우드 싱크 FIR (decimation 안티앨리어싱, 펌웨어는 esp-dsp dsps_fird 사용 예정)"""
    fc = cutoff_hz / (rate / 2)
    m = np.arange(ntaps) - (ntaps - 1) / 2
    h = np.sinc(fc * m) * fc * np.hamming(ntaps)
    return h / h.sum()


def measured_delays(x, rate):
    """전체 파일 평균 GCC-PHAT로 ch1 기준 실측 지연 추정 (기하 왜곡 우회용, --auto)"""
    hp = np.diff(x, axis=0)
    WIN, HOP, NFFT, ML = 4096, 2048, 8192, 20
    h = np.hanning(WIN)[:, None]
    nwin = (len(hp) - WIN) // HOP
    energies = np.array([np.abs(hp[k * HOP:k * HOP + WIN]).sum() for k in range(nwin)])
    thr = np.median(energies)
    acc = [np.zeros(NFFT // 2 + 1, dtype=complex) for _ in range(2)]
    for k in range(nwin):
        if energies[k] < thr:
            continue
        X = np.fft.rfft(hp[k * HOP:k * HOP + WIN] * h, NFFT, axis=0)
        for c in (1, 2):
            cross = X[:, 0] * np.conj(X[:, c])
            acc[c - 1] += cross / (np.abs(cross) + 1e-12)
    d = [0.0]
    for c in (1, 2):
        cc = np.fft.irfft(acc[c - 1], NFFT)
        cc = np.abs(np.concatenate((cc[-ML:], cc[:ML + 1])))
        d.append(float(np.argmax(cc)) - ML)  # ch1 대비 chC의 지연
    return np.array(d)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--steer", type=float, default=0.0, help="조향 방위각(도)")
    ap.add_argument("--auto", action="store_true",
                    help="기하 대신 실측 지연으로 정렬 (브레드보드 음향 왜곡 우회)")
    args = ap.parse_args()

    with wave.open(args.wav, "rb") as w:
        nch, rate = w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, nch).astype(np.float64)
    if nch < 3:
        print("[beam] 3ch 필요")
        return 1

    # ① delay-and-sum: 조향 방향의 파면이 정렬되도록 채널별 지연 후 평균
    if args.auto:
        m = measured_delays(x, rate)
        d = -m  # ch1 대비 지연을 상쇄하도록 반대 부호 적용
        d = d - d.min()
        print(f"[beam] 실측 지연(ch1 기준): ch2={m[1]:+.1f} ch3={m[2]:+.1f}샘플 → auto 정렬")
    else:
        d = steer_delays(args.steer, rate)
    aligned = np.stack([fractional_shift(x[:, c], d[c]) for c in range(3)], axis=1)
    beam = aligned.mean(axis=1)

    # ② 3:1 decimation (48k → 16k), 안티앨리어싱 FIR 컷오프 7.2kHz
    fir = lowpass_fir(7200, rate)
    beam16 = np.convolve(beam, fir, "same")[::3]
    ch116 = np.convolve(x[:, 0], fir, "same")[::3]

    base = Path(args.wav)
    for name, sig in [("_beam16k", beam16), ("_ch1_16k", ch116)]:
        out = base.with_name(base.stem + name + ".wav")
        with wave.open(str(out), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate // 3)
            w.writeframes(np.clip(sig, -32768, 32767).astype(np.int16).tobytes())
        print(f"[beam] 저장: {out.name}")

    # 조향 이득 근사: 빔 출력 RMS vs 단일 채널 RMS (동일 신호면 최대 +4.8dB, 무상관 잡음은 0dB)
    r_beam = np.sqrt((beam ** 2).mean())
    r_ch1 = np.sqrt((x[:, 0] ** 2).mean())
    print(f"[beam] steer={args.steer:.0f}° | RMS beam={r_beam:.0f} vs ch1={r_ch1:.0f} "
          f"({20 * np.log10(r_beam / (r_ch1 + 1e-9)):+.1f}dB, 정렬 잘 되면 0dB 초과)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
