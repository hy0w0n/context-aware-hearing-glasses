#!/usr/bin/env python3
"""3ch 녹음 WAV 분석 — M2b 검증 도구.
① 초당 채널 RMS: 탭 테스트로 채널 매핑(mic1/2/3) 확인
② 클랩 주변 채널 간 상호상관: 시간 오프셋이 물리 한계(마이크 간 거리) 안인지 확인

사용법: python tools/sync_check.py [경로.wav]   (생략 시 recordings/ 최신 파일)
물리 한계 @48kHz (안경 임시 테이프 14/14/14cm, 2026-09-10): 전 쌍 ±19.6샘플
(구 케이블 배치 25/18/18cm 녹음 분석 시: ±35 / ±25.2로 되돌릴 것)
"""
import glob
import sys
import wave
from pathlib import Path

import numpy as np


def load(path):
    with wave.open(str(path), "rb") as w:
        nch = w.getnchannels()
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return data.reshape(-1, nch).astype(np.float64), nch, rate


def main() -> int:
    if len(sys.argv) > 1:
        path = Path(sys.argv[1])
    else:
        recs = sorted(glob.glob(str(Path(__file__).resolve().parent.parent / "recordings" / "wav*.wav")))
        if not recs:
            print("[sync] recordings/ 폴더에 wav 파일이 없습니다")
            return 1
        path = Path(recs[-1])

    x, nch, rate = load(path)
    n = len(x)
    print(f"[sync] {path.name}: {n / rate:.1f}s @ {rate}Hz, {nch}ch")

    # ① 초당 채널 RMS — 탭한 채널이 그 구간에서 최대가 되어야 함
    print("\n== 초당 채널 RMS (탭 테스트 매핑 확인) ==")
    print("  t(s)" + "".join(f"      ch{c + 1}" for c in range(nch)))
    for s in range(int(n / rate)):
        seg = x[s * rate:(s + 1) * rate]
        if len(seg) == 0:
            break
        rms = np.sqrt((seg ** 2).mean(axis=0))
        cells = "".join(f"  {v:7.0f}" for v in rms)
        print(f"  {s:>4}{cells}   <- ch{int(np.argmax(rms)) + 1}")

    if nch < 2:
        return 0

    # 클리핑 경고 (클리핑된 클랩은 상관분석을 오염시킴 — 2026-07-28 실측 교훈)
    for c in range(nch):
        clip = int((np.abs(x[:, c]) >= 32760).sum())
        if clip > 10:
            print(f"[경고] ch{c + 1} 클리핑 {clip}샘플 — 소리가 너무 큼. 더 멀리/작게 다시 녹음 권장")

    # ② 평균 GCC-PHAT: 녹음 전체를 창으로 잘라 크로스 스펙트럼을 누적 → 채널 간 지연의 강건 추정
    #    (단발 클랩 상관은 잔향에 오염됨 — 2026-07-28 실측으로 확립된 방법.
    #     검증 논리: ch1-ch2는 같은 컨트롤러라 참값 0 → 이 값이 0이면 측정 신뢰 가능)
    hp = np.diff(x, axis=0)  # 1차 차분 = 간이 HPF
    WIN, HOP, NFFT = 4096, 2048, 8192
    nwin = (len(hp) - WIN) // HOP
    if nwin < 4:
        print("[sync] 녹음이 너무 짧습니다 (2초 이상 필요)")
        return 1
    h = np.hanning(WIN)[:, None]
    energies = np.array([np.abs(hp[k * HOP:k * HOP + WIN]).sum() for k in range(nwin)])
    thr = np.median(energies)

    # 물리한계 = 거리×fs/c. 안경 임시 테이프(2026-09-10): 14/14/14cm → 전 쌍 ±19.6샘플 @48k
    limits = {(0, 1): 19.6, (0, 2): 19.6, (1, 2): 19.6}
    acc = {pr: np.zeros(NFFT // 2 + 1, dtype=complex) for pr in limits}
    used = 0
    for k in range(nwin):
        if energies[k] < thr:
            continue
        X = np.fft.rfft(hp[k * HOP:k * HOP + WIN] * h, NFFT, axis=0)
        for (i, j) in limits:
            if j >= nch:
                continue
            cross = X[:, i] * np.conj(X[:, j])
            acc[(i, j)] += cross / (np.abs(cross) + 1e-12)
        used += 1

    print(f"\n== 평균 GCC-PHAT (에너지 상위 {used}/{nwin}개 창, 1샘플=20.8µs @48kHz) ==")
    for (i, j), lim in limits.items():
        if j >= nch:
            continue
        cc = np.fft.irfft(acc[(i, j)], NFFT)
        cc = np.concatenate((cc[-60:], cc[:61]))  # lag -60..+60
        mags = np.abs(cc)
        lag = int(np.argmax(mags)) - 60
        m2 = mags.copy()
        b = int(np.argmax(m2))
        m2[max(0, b - 3):b + 4] = 0
        clarity = mags.max() / (m2.max() + 1e-12)
        ok = "OK" if abs(lag) <= lim + 2 else "!! 범위 밖 — 동기 오프셋 의심"
        print(f"  ch{i + 1}-ch{j + 1}: lag={lag:+d}샘플 | 물리한계 ±{lim:.0f} | 피크 명확도 x{clarity:.1f}  {ok}")
    print("\n해석: ch1-ch2는 같은 컨트롤러라 0이어야 정상(방법 신뢰도 확인용)."
          "\n      ch1-ch3 = 버스 간 시작 오프셋 — 재부팅해도 같은 값이면 정상 (2026-07-28 실측: +1샘플)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
