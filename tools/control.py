#!/usr/bin/env python3
"""M5 제어 도구 — TCP:5006으로 CMD 프레임 전송 (앱의 명령 발행을 PC에서 시뮬레이션).

사용법:
    python tools/control.py [ip|auto] mode <0-4>          # 0=실내 1=야외 2=TV 3=일대일 4=다인
    python tools/control.py [ip|auto] vol <±n>            # 볼륨 델타 (예: vol +3, vol -2)
    python tools/control.py [ip|auto] alert <각도> <아이콘 0-4> [지속ms=2000]
                                                          # 아이콘: 0=CAR 1=HORN 2=SIREN 3=BIKE 4=WARN
    python tools/control.py [ip|auto] capture             # 즉시 1장 캡처 (image_receiver로 수신)
    python tools/control.py [ip|auto] ping                # RTT 측정 1회

주의: image_receiver.py가 접속 중이면 서버가 1클라이언트라 대기됨 — 종료 후 실행.
"""
import socket
import struct
import sys
import time

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from image_receiver import build_frame, discover_ip, T_CMD, T_PING, T_PONG, MAGIC, TCP_PORT  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ip = sys.argv[1]
    cmd = sys.argv[2]
    args = sys.argv[3:]

    if cmd == "mode":
        payload = bytes([0x01, int(args[0]) & 0xFF])
        desc = f"SET_MODE {args[0]}"
    elif cmd == "vol":
        payload = bytes([0x02]) + struct.pack("<b", int(args[0]))
        desc = f"SET_VOLUME {int(args[0]):+d}"
    elif cmd == "alert":
        deg = int(float(args[0]) * 10)  # 0.1° 단위 (§6.1)
        icon = int(args[1]) if len(args) > 1 else 4
        dur = int(args[2]) if len(args) > 2 else 2000
        payload = bytes([0x03]) + struct.pack("<hBH", deg, icon, dur)
        desc = f"DISPLAY_ALERT az={args[0]}° icon={icon} dur={dur}ms"
    elif cmd == "capture":
        payload = bytes([0x05])
        desc = "CAPTURE_NOW"
    elif cmd == "ping":
        payload = struct.pack("<Q", time.perf_counter_ns() // 1000)
        desc = "PING"
    else:
        print(f"알 수 없는 명령: {cmd}")
        return 1

    if ip == "auto":
        ip = discover_ip()

    try:
        sock = socket.create_connection((ip, TCP_PORT), timeout=5)
    except OSError:
        print(f"[ctl] {ip}:{TCP_PORT} 접속 실패 — ① PC가 ESP32와 같은 네트워크(핫스팟)인지"
              " ② image_receiver가 접속을 점유 중인지 확인")
        return 1
    sock.settimeout(2.0)
    ftype = T_PING if cmd == "ping" else T_CMD
    sock.sendall(build_frame(ftype, 0, payload))
    print(f"[ctl] {ip}:{TCP_PORT} → {desc} 전송")

    if cmd == "ping":
        t0 = time.perf_counter()
        buf = b""
        try:
            while len(buf) < 10:
                buf += sock.recv(1024)
            if buf[:2] == MAGIC and buf[2] == T_PONG:
                print(f"[ctl] PONG 수신 — RTT {(time.perf_counter() - t0) * 1000:.1f}ms")
        except socket.timeout:
            print("[ctl] PONG 타임아웃")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
