#!/usr/bin/env python3
"""BLE AUDIO 수신기 — env:ble 검증용 (프로토콜 §6.1, notify 1개 = 프레임 1개).
스캔 → 연결 → AUDIO notify 구독 → WAV 저장 + 드랍/처리량 통계.

사용법: python tools/ble_receiver.py [--seconds 10] [--name SmartGlass-HA]
필요: pip install bleak
"""
import argparse
import asyncio
import struct
import sys
import time
import wave
from pathlib import Path

from bleak import BleakClient, BleakScanner

import ima_adpcm  # fmt=1(ADPCM) 디코드 — tools/ima_adpcm.py

AUDIO_UUID = "8f0e1001-04c4-4f4b-a0a5-1e2d3c4b5a69"
IMAGE_UUID = "8f0e1003-04c4-4f4b-a0a5-1e2d3c4b5a69"
EVENT_UUID = "8f0e1004-04c4-4f4b-a0a5-1e2d3c4b5a69"
MAGIC = b"\xA5\x5A"
META_LEN = 1 + 4 + 2 + 1 + 8


class Stats:
    def __init__(self) -> None:
        self.pcm = bytearray()
        self.n_pkts = 0
        self.n_drops = 0
        self.n_bad = 0
        self.last_seq = None
        self.rate = 16000
        self.nch = 1
        self.t_first = None


def on_notify(stats: Stats):
    def handler(_char, data: bytearray) -> None:
        if len(data) < 10 or data[:2] != MAGIC or data[2] != 0x01:
            stats.n_bad += 1
            return
        flags = data[3]
        seq, plen = struct.unpack_from("<HH", data, 4)
        if len(data) < 8 + plen + 2:
            stats.n_bad += 1
            return
        if stats.t_first is None:
            stats.t_first = time.time()
            stats.rate = 48000 if (flags & 0x01) else 16000
            stats.nch = ((flags >> 4) & 0x3) + 1
            fmt_name = "ADPCM 64kbps" if data[8] == 1 else "PCM16"
            print(f"[ble-rx] 첫 프레임! rate={stats.rate}Hz {stats.nch}ch {fmt_name}")
        if stats.last_seq is not None:
            gap = (seq - stats.last_seq - 1) & 0xFFFF
            if gap and gap < 0x8000:
                stats.n_drops += gap
        stats.last_seq = seq
        stats.n_pkts += 1
        stats.pcm += ima_adpcm.decode_payload(bytes(data[8 : 8 + plen]), META_LEN)

    return handler


def on_image(stats: Stats):
    state = {"id": -1, "total": 0, "chunks": {}, "t0": 0.0}

    def handler(_char, data: bytearray) -> None:
        if len(data) < 10 or data[:2] != MAGIC:
            return
        ftype = data[2]
        _seq, plen = struct.unpack_from("<HH", data, 4)
        if len(data) < 8 + plen + 2:
            return
        if ftype == 0x02 and plen >= 11:  # IMG_START
            state["id"], total = struct.unpack_from("<HI", data, 8)[0], struct.unpack_from("<I", data, 10)[0]
            state["total"] = total
            state["chunks"] = {}
            state["t0"] = time.time()
        elif ftype == 0x03 and plen > 4:  # IMG_CHUNK
            img_id, idx = struct.unpack_from("<HH", data, 8)
            if img_id == state["id"]:
                state["chunks"][idx] = bytes(data[12:8 + plen])
        elif ftype == 0x04 and plen >= 6:  # IMG_END
            img_id = struct.unpack_from("<H", data, 8)[0]
            if img_id != state["id"]:
                return
            blob = b"".join(state["chunks"][k] for k in sorted(state["chunks"]))
            dur = time.time() - state["t0"]
            if len(blob) == state["total"]:
                out_dir = Path(__file__).resolve().parent.parent / "images"
                out_dir.mkdir(parents=True, exist_ok=True)
                p = out_dir / ("bleimage" + time.strftime("%Y%m%d%H%M%S") + ".jpg")
                p.write_bytes(blob)
                print(f"\n[ble-rx] 이미지 #{img_id}: {len(blob)//1024}KB, {dur:.2f}s "
                      f"({len(blob)*8/1000/max(dur,1e-9):.0f}kbps) → {p.name}")
            else:
                print(f"\n[ble-rx] 이미지 #{img_id} 불완전 ({len(blob)}/{state['total']}B) — 저장 생략")
            state["id"] = -1

    return handler


def on_event(_stats: Stats):
    def handler(_char, data: bytearray) -> None:
        if len(data) >= 10 and data[:2] == MAGIC and data[2] == 0x10:
            sub = data[8]
            if sub == 0x01 and len(data) > 9:
                print(f"\n[ble-rx] ★ BUTTON {'PRESS' if data[9] else 'RELEASE'}")
            else:
                print(f"\n[ble-rx] ★ EVENT 0x{sub:02x}")
    return handler


async def run(args) -> int:
    print(f"[ble-rx] '{args.name}' 스캔 중 (8초)...")
    dev = await BleakScanner.find_device_by_name(args.name, timeout=8.0)
    if dev is None:
        print("[ble-rx] 못 찾음 — ESP32가 env:ble 펌웨어로 광고 중인지, PC 블루투스 켜져 있는지 확인")
        return 1
    print(f"[ble-rx] 발견: {dev.address} → 연결 중...")

    stats = Stats()
    async with BleakClient(dev) as client:
        mtu = getattr(client, "mtu_size", None)
        print(f"[ble-rx] 연결됨 (MTU={mtu}) → AUDIO 구독, {args.seconds:.0f}초 수신...")
        await client.start_notify(AUDIO_UUID, on_notify(stats))
        subs = () if args.audio_only else ((EVENT_UUID, on_event), (IMAGE_UUID, on_image))
        for uuid, factory in subs:
            try:
                await client.start_notify(uuid, factory(stats))
            except Exception as e:  # 어댑터 자원 한계 등 — 오디오만이라도 계속
                print(f"[ble-rx] {uuid[4:8]} 구독 실패({e}) — 건너뜀")
        t0 = time.time()
        while time.time() - t0 < args.seconds:
            await asyncio.sleep(0.5)
            if stats.t_first:
                dur = time.time() - stats.t_first
                kbps = len(stats.pcm) * 8 / 1000 / max(dur, 1e-9)
                sys.stdout.write(f"\r[ble-rx] pkts={stats.n_pkts} drops={stats.n_drops} "
                                 f"payload={kbps:.0f}kbps   ")
                sys.stdout.flush()
        try:
            await client.stop_notify(AUDIO_UUID)
        except Exception:
            pass  # Windows 스택이 가끔 해제에서 오류 — 연결 종료로 어차피 정리됨
    print()

    if not stats.pcm:
        print("[ble-rx] 수신 데이터 없음 (구독은 됐는데 스트림이 없으면 ESP32 시리얼 로그 확인)")
        return 1

    fname = "blewav" + time.strftime("%y%m%d%H%M%S") + ".wav"
    rec_dir = Path(__file__).resolve().parent.parent / "recordings"
    rec_dir.mkdir(parents=True, exist_ok=True)
    out = rec_dir / fname
    with wave.open(str(out), "wb") as w:
        w.setnchannels(stats.nch)
        w.setsampwidth(2)
        w.setframerate(stats.rate)
        w.writeframes(bytes(stats.pcm))

    total = stats.n_pkts + stats.n_drops
    dur = len(stats.pcm) / 2 / stats.nch / stats.rate
    print(f"[ble-rx] 저장: {out} ({dur:.1f}s @ {stats.rate}Hz)")
    print(f"[ble-rx] 패킷 {stats.n_pkts} | 드랍 {stats.n_drops} ({stats.n_drops / max(1, total) * 100:.2f}%)"
          f" | 형식오류 {stats.n_bad}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--name", default="SmartGlass-HA")
    ap.add_argument("--audio-only", action="store_true",
                    help="AUDIO만 구독 (이미지 동시 전송 없는 단독 대역 측정용 — §9 비교실험)")
    args = ap.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
