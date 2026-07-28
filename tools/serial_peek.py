#!/usr/bin/env python3
"""시리얼 로그를 N초 동안 읽어 출력 (비대화형 — Claude Code 검증용).
사용법: python tools/serial_peek.py [COM3] [12]
"""
import sys
import time

import serial  # pyserial (platformio 의존성으로 이미 설치됨)

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

s = serial.Serial(port, 115200, timeout=0.5)
end = time.time() + secs
try:
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode(errors="replace"))
            sys.stdout.flush()
finally:
    s.close()
