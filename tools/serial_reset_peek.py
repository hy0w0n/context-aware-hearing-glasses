#!/usr/bin/env python3
"""UART 브리지 포트로 보드를 리셋하고 부팅 로그를 캡처 (브라운아웃/부트루프 진단용).
사용법: python tools/serial_reset_peek.py COM5 [10]
"""
import sys
import time

import serial

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

s = serial.Serial(port, 115200, timeout=0.3)
# 오토프로그 회로로 리셋 펄스: RTS(EN) 토글, DTR(IO0)은 HIGH 유지 → 일반 부팅
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False
s.reset_input_buffer()
end = time.time() + secs
try:
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode(errors="replace"))
            sys.stdout.flush()
finally:
    s.close()
