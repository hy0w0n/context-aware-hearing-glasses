#!/usr/bin/env python3
"""M3 이미지 수신기 — TCP:5006에서 IMG_START/CHUNK/END 프레임 재조립 → JPEG 저장.
저장 형식: images/imageYYYYMMDDHHMMSS.jpg (고유 타임스탬프, 덮어쓰기 없음)

사용법:
    python tools/image_receiver.py [ESP32_IP|auto] [--count 3] [--now]
    --count 0 = 무한 (Ctrl+C 종료), --now = 접속 즉시 CAPTURE_NOW 명령 전송
"""
import argparse
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

MAGIC = b"\xA5\x5A"
T_IMG_START, T_IMG_CHUNK, T_IMG_END = 0x02, 0x03, 0x04
T_CMD, T_PING, T_PONG = 0x20, 0x30, 0x31
UDP_PORT, TCP_PORT = 5005, 5006


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, seq: int, payload: bytes) -> bytes:
    head = bytes([ftype, 0]) + struct.pack("<HH", seq, len(payload))
    crc = crc16_ccitt(head + payload)
    return MAGIC + head + payload + struct.pack("<H", crc)


def discover_ip() -> str:
    """UDP SUB 브로드캐스트로 ESP32 IP 탐색 (receiver.py와 동일 기법)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.settimeout(0.5)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    targets = {("255.255.255.255", UDP_PORT)}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                targets.add((ip.rsplit(".", 1)[0] + ".255", UDP_PORT))
    except OSError:
        pass
    print(f"[img-rx] ESP32 탐색 중: {sorted(t[0] for t in targets)}")
    deadline = time.time() + 15
    last = 0.0
    try:
        while time.time() < deadline:
            if time.time() - last > 2.0:
                for t in targets:
                    try:
                        s.sendto(b"SUB", t)
                    except OSError:
                        pass
                last = time.time()
            try:
                _d, addr = s.recvfrom(2048)
                print(f"[img-rx] 발견: {addr[0]}")
                return addr[0]
            except socket.timeout:
                continue
    finally:
        s.close()
    print("[img-rx] 탐색 실패 — IP를 직접 지정하세요")
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("esp_ip", nargs="?", default="auto")
    ap.add_argument("--count", type=int, default=0, help="저장할 이미지 '장수' (0=무한, Ctrl+C 종료)")
    ap.add_argument("--now", action="store_true", help="접속 즉시 CAPTURE_NOW 전송")
    ap.add_argument("--period", type=float, default=None,
                    help="촬영 간격(초, 0.5~60) — SET_CONFIG로 펌웨어에 전달 (기본: 펌웨어 설정 3초)")
    args = ap.parse_args()

    ip = discover_ip() if args.esp_ip == "auto" else args.esp_ip
    out_dir = Path(__file__).resolve().parent.parent / "images"
    out_dir.mkdir(parents=True, exist_ok=True)

    sock = socket.create_connection((ip, TCP_PORT), timeout=10)
    sock.settimeout(1.0)
    print(f"[img-rx] {ip}:{TCP_PORT} 접속 — 주기 캡처 수신 대기 (Ctrl+C 종료)")
    if args.period is not None:
        ms = int(max(0.5, min(60.0, args.period)) * 1000)
        sock.sendall(build_frame(T_CMD, 0, bytes([0x04, 0x01]) + struct.pack("<i", ms)))
        print(f"[img-rx] 촬영 간격 설정: {ms}ms")
    if args.now:
        sock.sendall(build_frame(T_CMD, 0, bytes([0x05])))
        print("[img-rx] CAPTURE_NOW 전송")

    buf = bytearray()
    cur = None  # {id, total, w, h, chunks{idx: bytes}}
    saved = 0
    try:
        while args.count == 0 or saved < args.count:
            try:
                data = sock.recv(8192)
            except socket.timeout:
                continue
            if not data:
                print("[img-rx] 연결 종료됨")
                break
            buf += data

            pos = 0
            while len(buf) - pos >= 10:
                if buf[pos:pos + 2] != MAGIC:
                    pos += 1
                    continue
                ftype = buf[pos + 2]
                seq, plen = struct.unpack_from("<HH", buf, pos + 4)
                if len(buf) - pos < 10 + plen:
                    break
                payload = bytes(buf[pos + 8:pos + 8 + plen])
                (crc_rx,) = struct.unpack_from("<H", buf, pos + 8 + plen)
                pos += 10 + plen
                if crc16_ccitt(bytes(buf[pos - 10 - plen + 2:pos - 2])) != crc_rx:
                    print("[img-rx] CRC 불일치 프레임 무시")
                    continue

                if ftype == T_IMG_START and plen >= 11:
                    img_id, total, w, h, fmt = struct.unpack_from("<HIHHB", payload, 0)
                    cur = {"id": img_id, "total": total, "w": w, "h": h, "chunks": {}}
                elif ftype == T_IMG_CHUNK and cur and plen > 4:
                    img_id, idx = struct.unpack_from("<HH", payload, 0)
                    if img_id == cur["id"]:
                        cur["chunks"][idx] = payload[4:]
                elif ftype == T_IMG_END and cur and plen >= 6:
                    img_id, crc32_rx = struct.unpack_from("<HI", payload, 0)
                    if img_id != cur["id"]:
                        continue
                    blob = b"".join(cur["chunks"][i] for i in sorted(cur["chunks"]))
                    miss = (cur["total"] + 1023) // 1024 - len(cur["chunks"])
                    if len(blob) != cur["total"] or miss:
                        print(f"[img-rx] #{img_id} 청크 누락({miss}) → 저장 생략")
                        cur = None
                        continue
                    crc_ok = zlib.crc32(blob) & 0xFFFFFFFF == crc32_rx
                    name = "image" + time.strftime("%Y%m%d%H%M%S")
                    path = out_dir / f"{name}.jpg"
                    n = 1
                    while path.exists():  # 같은 초에 여러 장 → 접미사
                        path = out_dir / f"{name}_{n}.jpg"
                        n += 1
                    path.write_bytes(blob)
                    saved += 1
                    print(f"[img-rx] 저장 {path.name}: {cur['w']}x{cur['h']} {len(blob)//1024}KB"
                          f" crc={'OK' if crc_ok else 'MISMATCH(저장은 함)'} ({saved}{'/' + str(args.count) if args.count else ''})")
                    cur = None
            del buf[:pos]
    except KeyboardInterrupt:
        print("\n[img-rx] 중단됨")
    finally:
        sock.close()
    print(f"[img-rx] 총 {saved}장 저장 → {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
