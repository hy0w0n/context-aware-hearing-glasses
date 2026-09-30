#!/usr/bin/env python3
"""DOA(방향추정) 프로토타입 — 3ch WAV → GCC-PHAT TDOA → 방위각 그리드 서치.
펌웨어 C 포팅 전 알고리즘 검증용 (CLAUDE.md Phase 2, proto.md M2).

사용법:
    python tools/doa_prototype.py recordings\az000.wav [--expected 0]
    (생략 시 recordings/ 최신 파일)

좌표계: 착용자 정면 = 0°(+y), 시계방향 + (90°=오른쪽). 안경 임시 테이프 부착(2026-09-10):
    mic1=좌(-7cm, 0), mic2=우(+7cm, 0), mic3=후방 중앙(0, -12.1cm)   [정삼각형 14/14/14cm]
주의: mic3는 '뒤' 꼭짓점 — 전방 소리는 머리 그림자로 mic3 고주파 감쇠 예상 (proto.md §2)
"""
import argparse
import glob
import sys
import wave
from pathlib import Path

import numpy as np

C_SOUND = 343.0
# 마이크 좌표 (m) — 배치 변경 시 여기만 수정
# MIRROR: 안경 실장(2026-09-10)에서 사용자가 mic1=좌/mic2=우 실배치 확정 → +1 (반전 불필요)
# 단, 착용 확인 1회 권장: 왼쪽에서 소리 → 추정 ~270°(0°=정면, 시계+)가 나와야 정상.
# 좌우가 뒤집혀 나오면 -1로 되돌릴 것 (구 브레드보드/케이블 녹음 재분석 시에도 -1 + 구 좌표 사용)
MIRROR = 1.0
# 안경 임시 테이프 배치 (2026-09-10): 정삼각형 14/14/14cm, mic3=후방. 최종 고정 후 재실측
# (구 케이블 배치 25/18/18cm 녹음 재분석 시: ±0.125 / +0.1295 / MAX_LAG=40 / MIRROR=-1로 되돌릴 것)
MIC_POS = np.array([
    [MIRROR * -0.07, 0.0],   # mic1 (ch1) = 좌
    [MIRROR * +0.07, 0.0],   # mic2 (ch2) = 우
    [0.0, -0.1212],          # mic3 (ch3) = 후방 중앙 — sqrt(14²-7²)=12.12cm, '뒤'라서 -y
])
PAIRS = [(0, 1), (0, 2), (1, 2)]
# 버스 간 시작 오프셋 보정 — ⚠ 부팅마다 다름이 실증됨(2026-07-28: +1 / 2026-09-10: 0)
# → 매 부팅(재플래시·리셋) 후 클랩 1회 → sync_check.py의 ch1-ch3 값으로 여기를 갱신할 것
BUS_OFFSET = {(0, 1): 0.0, (0, 2): 0.0, (1, 2): 0.0}
MAX_LAG = 26  # 물리한계(14cm → ±19.6샘플) + 여유


def gcc_phat_curve(X_i, X_j, nfft, max_lag):
    """GCC-PHAT 상관 곡선(lag -max_lag..+max_lag)과 피크 명확도.
    SRP-PHAT: 피크 lag 하나만 뽑지 않고 곡선 전체를 각도 채점에 사용 (다중 피크·잔향에 강건)"""
    cross = X_i * np.conj(X_j)
    cross /= np.abs(cross) + 1e-12
    cc = np.fft.irfft(cross, nfft)
    cc = np.abs(np.concatenate((cc[-max_lag:], cc[:max_lag + 1])))
    k = int(np.argmax(cc))
    m2 = cc.copy()
    m2[max(0, k - 2):k + 3] = 0
    clarity = cc[k] / (m2.max() + 1e-12)
    return cc, clarity


def expected_lags(theta_deg, rate):
    """방위각 θ(0°=+y=정면, 시계+)에서 각 쌍의 기대 lag(샘플)"""
    th = np.deg2rad(theta_deg)
    u = np.array([np.sin(th), np.cos(th)])  # 음원 방향 단위벡터
    # 도착시간 t_i = t0 - (p_i·u)/c → lag(i,j) = t_i - t_j = -((p_i-p_j)·u)/c
    out = {}
    for (i, j) in PAIRS:
        out[(i, j)] = -np.dot(MIC_POS[i] - MIC_POS[j], u) / C_SOUND * rate
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?", default=None)
    ap.add_argument("--expected", type=float, default=None, help="실제 각도(오차 계산용)")
    ap.add_argument("--win", type=int, default=4096)
    args = ap.parse_args()

    if args.wav is None:
        recs = sorted(glob.glob(str(Path(__file__).resolve().parent.parent / "recordings" / "*.wav")))
        if not recs:
            print("recordings/ 비어 있음")
            return 1
        args.wav = recs[-1]

    with wave.open(args.wav, "rb") as w:
        nch, rate = w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, nch).astype(np.float64)
    if nch < 3:
        print(f"[doa] 3ch 필요 (현재 {nch}ch)")
        return 1
    print(f"[doa] {Path(args.wav).name}: {len(x) / rate:.1f}s @ {rate}Hz")

    # 기대 lag 테이블 (1° 그리드) — 펌웨어 C 구현도 동일한 사전 계산 테이블 방식 사용 예정
    thetas = np.arange(0, 360, 1.0)
    table = np.zeros((len(thetas), len(PAIRS)))
    for t, th in enumerate(thetas):
        e = expected_lags(th, rate)
        for pi, pr in enumerate(PAIRS):
            table[t, pi] = e[pr]

    hp = np.diff(x, axis=0)
    WIN, HOP = args.win, args.win // 2
    NFFT = 2 * WIN
    h = np.hanning(WIN)[:, None]
    nwin = (len(hp) - WIN) // HOP
    energies = np.array([np.abs(hp[k * HOP:k * HOP + WIN]).sum() for k in range(nwin)])
    # 에너지 게이트: 조용한 창은 버스 간 크로스토크(ch1-ch3 lag=0 가짜 상관)로 60°/240°
    # 유령을 만듦 — 2026-09-10 실측에서 규명. 스냅/직접음 창은 중앙값의 2.5배 이상으로 관찰됨
    thr = max(np.percentile(energies, 75), 2.5 * np.median(energies))

    results = []
    for k in range(nwin):
        if energies[k] < thr:
            continue
        X = np.fft.rfft(hp[k * HOP:k * HOP + WIN] * h, NFFT, axis=0)
        curves = []
        weights = np.zeros(len(PAIRS))
        for pi, (i, j) in enumerate(PAIRS):
            cc, clarity = gcc_phat_curve(X[:, i], X[:, j], NFFT, MAX_LAG)
            curves.append(cc)
            weights[pi] = min(clarity, 5.0)
        # 크로스토크 베토 (2026-09-10 규명): SD 배선 커플링(GPIO2→GPIO1)이 ch1→ch3를 복사해
        # ch1-ch3 쌍에만 lag=0 가짜 피크를 만든다. 실제 60°/240° 음원이라면 ch2-ch3도
        # ±17샘플에서 선명해야 함 — 그 corroboration이 없으면 ch1-ch3 쌍을 채점에서 제외
        lag13 = int(np.argmax(curves[1])) - MAX_LAG
        lag23 = int(np.argmax(curves[2])) - MAX_LAG
        if abs(lag13) <= 1 and weights[1] >= 2.0 and (weights[2] < 1.5 or abs(lag23) <= 2):
            weights[1] = 0.0
        if weights.sum() < 3.5:  # 남은 쌍의 피크가 흐릿하면 스킵 (무음/잔향/크로스토크 구간)
            continue
        if int((weights >= 1.5).sum()) < 2:  # 한 쌍만 선명한 창도 스킵
            continue
        # SRP-PHAT: 각 후보 각도에서 기대 lag 위치의 상관값을 (선형보간으로) 읽어 가중 합산
        score = np.zeros(len(thetas))
        for pi, pr in enumerate(PAIRS):
            pos = table[:, pi] + BUS_OFFSET[pr] + MAX_LAG  # 곡선 인덱스 (버스 오프셋 보정 포함)
            i0 = np.clip(np.floor(pos).astype(int), 0, 2 * MAX_LAG - 1)
            frac = pos - i0
            vals = curves[pi][i0] * (1 - frac) + curves[pi][i0 + 1] * frac
            score += weights[pi] * vals
        best = int(np.argmax(score))
        # 기하 지지 검사: 채택 각도의 기대 lag과 원시 피크 lag이 ±3샘플 내로 일치하는 쌍이
        # 2개 이상이어야 유효 창 (크로스토크·잔향 창은 한 쌍만 우연히 맞음 — 2026-09-10)
        support = 0
        for pi, pr in enumerate(PAIRS):
            peak = int(np.argmax(curves[pi])) - MAX_LAG
            if abs(peak - (table[best, pi] + BUS_OFFSET[pr])) <= 3:
                support += 1
        if support < 2:
            continue
        results.append(thetas[best])

    if not results:
        print("[doa] 유효 창 없음 (신호가 약함)")
        return 1

    results = np.array(results)
    # 원형 중앙값: 벡터 평균
    vx = np.sin(np.deg2rad(results)).mean()
    vy = np.cos(np.deg2rad(results)).mean()
    est = (np.rad2deg(np.arctan2(vx, vy))) % 360
    conc = np.hypot(vx, vy)  # 0~1, 클수록 일관됨

    hist, _ = np.histogram(results, bins=np.arange(0, 361, 22.5))
    print(f"[doa] 유효 창 {len(results)}개 | 추정 방위각 = {est:.0f}° (일관도 {conc:.2f})")
    print("      16방향 분포: " + " ".join(f"{int(b * 22.5)}°:{c}" for b, c in enumerate(hist) if c))
    if args.expected is not None:
        diff = (est - args.expected + 180) % 360 - 180
        print(f"      실제 {args.expected:.0f}° → 오차 {abs(diff):.0f}°")
    return 0


if __name__ == "__main__":
    sys.exit(main())
