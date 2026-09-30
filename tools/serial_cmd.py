#!/usr/bin/env python3
"""시리얼로 명령 1줄 전송 후 응답을 N초 읽기.
사용법: python tools/serial_cmd.py COM3 "info" [5]
"""
import sys
import time

import serial

port = sys.argv[1]
cmd = sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

s = serial.Serial(port, 115200, timeout=0.5)
time.sleep(0.3)
s.reset_input_buffer()
s.write((cmd + "\n").encode())
end = time.time() + secs
try:
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode(errors="replace"))
            sys.stdout.flush()
finally:
    s.close()
