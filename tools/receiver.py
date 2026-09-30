#!/usr/bin/env python3
"""M1 UDP 오디오 수신기 — 프로토콜 v0.1 (CLAUDE.md §6.1) 레퍼런스 구현.

사용법:
    python tools/receiver.py <ESP32_IP> [--seconds 10] [--out capture.wav]

동작: ESP32의 UDP:5005로 "SUB"를 주기 송신(구독+keepalive) → AUDIO 프레임 수신
      → CRC/시퀀스 검사 → WAV 저장 + 실시간 레벨 미터 + 드랍 통계 (구 스펙 2·3단계 검증).
표준 라이브러리만 사용 (설치 불필요).
"""
import argparse
import socket
import struct
import sys
import time
import wave
from pathlib import Path

import ima_adpcm  # fmt=1(ADPCM) 디코드 — tools/ima_adpcm.py (같은 폴더)

MAGIC = b"\xA5\x5A"
TYPE_AUDIO = 0x01
META_LEN = 1 + 4 + 2 + 1 + 8  # fmt, t_ms, azimuth, conf, dir_hist[8]


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def main() -> int:
    ap = argparse.ArgumentParser(description="ESP32 AUDIO 프레임 UDP 수신기")
    ap.add_argument("esp_ip", nargs="?", default="auto",
                    help="ESP32 IP. 생략/'auto'면 브로드캐스트로 자동 탐색 (같은 서브넷)")
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--seconds", type=float, default=10.0, help="녹음 길이 (기본 10초)")
    ap.add_argument("--out", default=None,
                    help="저장 경로 (기본: 프로젝트 루트의 capture.wav — 실행 위치 무관)")
    ap.add_argument("--no-crc", action="store_true", help="CRC 검사 생략")
    args = ap.parse_args()
    if args.out is None:
        # 덮어쓰기 방지: wavYYMMDDHHMMSS.wav 형식으로 recordings/ 폴더에 누적 저장
        fname = "wav" + time.strftime("%y%m%d%H%M%S") + ".wav"
        rec_dir = Path(__file__).resolve().parent.parent / "recordings"
        try:
            rec_dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            rec_dir = Path.home()
        args.out = str(rec_dir / fname)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    sock.settimeout(0.5)
    targets = None
    if args.esp_ip == "auto":
        # 브로드캐스트 SUB → ESP32가 첫 AUDIO 프레임을 보내오면 그 주소로 고정.
        # 전역 브로드캐스트는 Windows가 엉뚱한 어댑터(가상 NIC 등)로 보낼 수 있어
        # 로컬 IPv4마다 /24 지향 브로드캐스트(x.y.z.255)에도 함께 송신 (2026-08-11)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        targets = {("255.255.255.255", args.port)}
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                ip = info[4][0]
                if not ip.startswith("127."):
                    targets.add((ip.rsplit(".", 1)[0] + ".255", args.port))
        except OSError:
            pass
        esp = None  # 탐색 중
        print(f"[receiver] IP 미지정 → 브로드캐스트 탐색: {sorted(t[0] for t in targets)}")
    else:
        esp = (args.esp_ip, args.port)

    pcm = bytearray()
    rate = 48000  # flags bit0(FRAME_FLAG_48K)=1이면 48k, 아니면 16k
    nch = 1       # flags bits4-5 = 채널수-1 (인터리브)
    n_pkts = n_drops = n_crc_bad = 0
    last_seq = None
    last_sub = 0.0
    last_meter = 0.0
    t_first = None

    print(f"[receiver] {esp}에 SUB 전송, 오디오 대기... (Ctrl+C 중단)")
    try:
        while True:
            now = time.time()
            if now - last_sub > 2.0:
                for tgt in (targets if esp is None else [esp]):
                    try:
                        sock.sendto(b"SUB", tgt)
                    except OSError:
                        pass
                last_sub = now
            if t_first and now - t_first >= args.seconds:
                break
            try:
                data, _addr = sock.recvfrom(2048)
            except socket.timeout:
                if t_first is None:
                    print("[receiver] 수신 없음 — ESP32 전원/IP/같은 네트워크인지 확인")
                continue

            if len(data) < 10 or data[:2] != MAGIC:
                continue
            if data[2] == 0x10:  # EVENT (BUTTON/DOA_ALERT — blind 2회 전송이라 seq로 중복 제거)
                eseq, eplen = struct.unpack_from("<HH", data, 4)
                if eseq != getattr(main, "_last_evt", None):
                    main._last_evt = eseq
                    sub = data[8]
                    if sub == 0x01 and eplen >= 2:
                        print(f"\n[receiver] EVENT BUTTON {'PRESS' if data[9] else 'RELEASE'} (seq={eseq})")
                    else:
                        print(f"\n[receiver] EVENT subtype=0x{sub:02x} (seq={eseq})")
                continue
            if data[2] != TYPE_AUDIO:
                continue
            flags = data[3]
            seq, plen = struct.unpack_from("<HH", data, 4)
            if len(data) < 8 + plen + 2:
                continue
            if not args.no_crc:
                (crc_rx,) = struct.unpack_from("<H", data, 8 + plen)
                if crc16_ccitt(data[2 : 8 + plen]) != crc_rx:
                    n_crc_bad += 1
                    continue

            if t_first is None:
                t_first = now
                rate = 48000 if (flags & 0x01) else 16000
                nch = ((flags >> 4) & 0x3) + 1
                if esp is None:
                    esp = _addr  # 탐색 완료 → 이후 keepalive는 유니캐스트
                    print(f"[receiver] ESP32 발견: {esp[0]}")
                fmt_name = "ADPCM 64kbps" if data[8] == 1 else "PCM16"
                print(f"[receiver] 첫 패킷 수신! rate={rate}Hz, {nch}ch, {fmt_name}, "
                      f"{args.seconds:.0f}초 녹음 시작")

            if last_seq is not None:
                gap = (seq - last_seq - 1) & 0xFFFF
                if gap and gap < 0x8000:
                    n_drops += gap
            last_seq = seq
            n_pkts += 1
            seg = ima_adpcm.decode_payload(data[8 : 8 + plen], META_LEN)  # fmt=0이면 그대로
            pcm += seg

            if now - last_meter > 0.25:
                last_meter = now
                samples = struct.unpack_from(f"<{len(seg) // 2}h", seg)
                peak = max(1, max(abs(s) for s in samples))
                bar = "#" * min(40, int(peak / 32768 * 40 * 4))  # x4 시각 게인
                sys.stdout.write(f"\r[{bar:<40}] peak={peak:6d} pkts={n_pkts:5d} drops={n_drops}   ")
                sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n[receiver] 중단됨")

    print()
    if not pcm:
        print("[receiver] 수신 데이터 없음. 체크: ① ESP32 시리얼에 '구독자 등록' 로그가 떴는지"
              " ② PC 방화벽(공용 네트워크에서 Python 허용) ③ 같은 공유기/핫스팟인지")
        return 1

    def save(path: str) -> None:
        with wave.open(path, "wb") as w:
            w.setnchannels(nch)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(bytes(pcm))

    try:
        out_path = args.out
        save(out_path)
    except PermissionError:
        out_path = str(Path.home() / Path(args.out).name)
        print(f"[receiver] '{args.out}' 쓰기 권한 없음 → {out_path}로 대체 저장")
        save(out_path)
    args.out = out_path

    dur = len(pcm) / 2 / nch / rate
    total = n_pkts + n_drops
    print(f"[receiver] 저장: {args.out} ({dur:.1f}s @ {rate}Hz {nch}ch, {len(pcm)//1024}KB)")
    print(f"[receiver] 패킷 {n_pkts} | 드랍 {n_drops} ({n_drops/max(1,total)*100:.2f}%) | CRC 오류 {n_crc_bad}")
    print("[receiver] WAV를 재생해 육성 확인: 틱틱거림=패킷 드랍, 속도 이상=샘플레이트 불일치 (구 스펙 2단계)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
