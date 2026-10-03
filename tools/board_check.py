#!/usr/bin/env python3
"""보드 복귀/재배선 후 기본 동작 점검 — COM 포트를 한 번만 열어 부팅 로그·info·cam·[stat]을 순서대로 수집한다.
(COM3 네이티브 USB는 열고 닫을 때 보드가 리셋되므로 명령마다 포트를 열면 안 된다.)

사용법: python tools/board_check.py [COM3] [--boot 10] [--stat 12] [--no-cam]
판정: mic1/2/3 RMS가 모두 0이 아니고 포화(>30000)가 아닌지, [i2s1=STALL!] 없는지, cam 캡처 성공, 힙/PSRAM 정상.
"""
import argparse
import re
import sys
import time

import serial

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ap = argparse.ArgumentParser()
ap.add_argument("port", nargs="?", default="COM3")
ap.add_argument("--boot", type=float, default=10.0, help="부팅 로그 수집 시간(초)")
ap.add_argument("--stat", type=float, default=12.0, help="[stat] 수집 시간(초)")
ap.add_argument("--no-cam", action="store_true")
args = ap.parse_args()

log = []


def read_for(s, secs, tag):
    end = time.time() + secs
    buf = ""
    while time.time() < end:
        d = s.read(4096)
        if d:
            t = d.decode(errors="replace")
            buf += t
            for line in t.splitlines():
                if line.strip():
                    log.append((tag, line))
    return buf


s = serial.Serial(args.port, 115200, timeout=0.3)
try:
    print(f"[1] 포트 열림 → 리셋/부팅 로그 {args.boot:.0f}초 수집")
    boot = read_for(s, args.boot, "boot")
    for cmd, wait in (("info", 3.0),) + (() if args.no_cam else (("cam", 7.0),)):
        print(f"[2] 명령 전송: {cmd}")
        s.reset_input_buffer()
        s.write((cmd + "\n").encode())
        read_for(s, wait, cmd)
    print(f"[3] [stat] {args.stat:.0f}초 수집 (이 동안 마이크 근처에서 말하거나 손뼉을 쳐도 됨)")
    read_for(s, args.stat, "stat")
finally:
    s.close()

# ---------------- 분석
text_all = "\n".join(l for _, l in log)
print("\n" + "=" * 70 + "\n원문 로그 (앞 60줄)\n" + "=" * 70)
for tag, l in log[:60]:
    print(f"[{tag:4s}] {l}")
if len(log) > 60:
    print(f"... (총 {len(log)}줄)")

print("\n" + "=" * 70 + "\n판정\n" + "=" * 70)
findings = []
# 부팅/리셋 원인
m = re.search(r"rst:0x([0-9a-f]+) \(([^)]+)\)", text_all)
findings.append(("리셋 원인", m.group(2) if m else "로그 없음(이미 부팅 상태였을 수 있음)"))
if "Brownout" in text_all or "brownout" in text_all:
    findings.append(("⚠ 브라운아웃", "전원 불안정 — 배선/전원 점검"))
if re.search(r"Guru Meditation|panic|abort\(\)", text_all):
    findings.append(("⚠ 패닉/리부트", "크래시 로그 있음"))
# PSRAM/힙
m = re.search(r"psram\D*(\d+)", text_all, re.I)
findings.append(("PSRAM", (m.group(1) + " KB") if m else "info 응답에서 미확인"))
m = re.search(r"heap\D*(\d+)", text_all, re.I)
findings.append(("힙 free", (m.group(1)) if m else "미확인"))
# 마이크
stats = re.findall(r"mic1=(-?\d+)\((-?\d+)\.\.(-?\d+)\)\s+mic2=(-?\d+)\((-?\d+)\.\.(-?\d+)\)\s+mic3=(-?\d+)\((-?\d+)\.\.(-?\d+)\)", text_all)
if stats:
    rms = [[int(x[i]) for x in stats] for i in (0, 3, 6)]
    for k in range(3):
        r = rms[k]
        verdict = "정상" if max(r) > 0 else "⚠ 항상 0 (배선/SD 라인 확인)"
        if max(r) > 30000:
            verdict = "⚠ 포화 (입력 과다 또는 SD 쇼트)"
        if 0 < max(r) and max(r) == min(r):
            verdict = "⚠ 값이 고정됨 (스턱)"
        findings.append((f"mic{k+1} RMS", f"{min(r)}~{max(r)} ({len(r)}회) — {verdict}"))
    # 세 채널이 완전히 같은지(쇼트 의심)
    if all(a == b for a, b in zip(rms[0], rms[1])) and max(rms[0]) > 0:
        findings.append(("⚠ mic1=mic2 동일", "L/R 핀 또는 SD 공유 배선 확인"))
    if all(a == b for a, b in zip(rms[0], rms[2])) and max(rms[0]) > 0:
        findings.append(("⚠ mic1=mic3 동일", "I2S1 SD(GPIO1)와 I2S0 SD(GPIO2) 혼선 의심"))
else:
    findings.append(("마이크 [stat]", "미수신 — 펌웨어 stat 출력 형식 확인 또는 수집 시간 부족"))
findings.append(("I2S1 STALL", "⚠ 발생 — mic3 클럭(BCLK/WS 41·42 병렬) 확인" if "STALL" in text_all else "없음"))
# DOA/ALERT
d = re.findall(r"doa=(-?\d+)°", text_all)
findings.append(("DOA 추정 출력", f"{len(d)}회 (예: {d[:5]})" if d else "없음(조용하면 정상)"))
# 카메라
if not args.no_cam:
    m = re.search(r"(\d+)\s*(bytes|B)\b.*?(\d+)\s*ms", text_all)
    cam_ok = re.search(r"\[cam\].*(성공|ok|OK|bytes|B\b)", text_all) or m
    cam_fail = re.search(r"\[cam\].*(실패|fail|error|ERR)", text_all, re.I)
    findings.append(("카메라 cam", ("⚠ 실패 로그 있음" if cam_fail else ("캡처 로그 있음" if cam_ok else "응답 미확인"))))
# 전송로
m = re.search(r"(wifi|ble)[^\n]*", text_all, re.I)
findings.append(("전송로 상태(info)", m.group(0)[:90] if m else "미확인"))
# OLED/I2C 힌트
if re.search(r"oled|u8g2|ssd1306", text_all, re.I):
    line = re.search(r"[^\n]*(oled|u8g2|ssd1306)[^\n]*", text_all, re.I).group(0)
    findings.append(("OLED 로그", line.strip()[:90]))

for k, v in findings:
    print(f"- {k}: {v}")
