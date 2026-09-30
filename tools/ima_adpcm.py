#!/usr/bin/env python3
"""IMA ADPCM 디코더 — 펌웨어 firmware/src/adpcm.cpp 인코더와 대칭 (프로토콜 §6.1 AUDIO fmt=1).
패킷마다 헤더의 adpcm_state{predictor, step_idx}로 초기화 → 패킷 단위 리싱크 (유실 허용).
니블 순서: 첫 샘플 = 하위 니블 (IMA/WAV 관례).
"""
import struct

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def decode(nibbles: bytes, predictor: int, step_idx: int) -> bytes:
    """4bit 니블 스트림 → PCM16 LE bytes (샘플 수 = len(nibbles)*2)."""
    out = bytearray()
    pred, idx = predictor, step_idx
    for b in nibbles:
        for nib in (b & 0xF, b >> 4):
            step = STEP_TABLE[idx]
            delta = step >> 3
            if nib & 4:
                delta += step
            if nib & 2:
                delta += step >> 1
            if nib & 1:
                delta += step >> 2
            pred = pred - delta if (nib & 8) else pred + delta
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + INDEX_TABLE[nib]))
            out += struct.pack("<h", pred)
    return bytes(out)


def decode_payload(payload: bytes, meta_len: int) -> bytes:
    """AUDIO payload 전체(fmt 포함) → PCM16 bytes. fmt=0이면 그대로, fmt=1이면 디코드."""
    if payload[0] == 1:
        (pred,) = struct.unpack_from("<h", payload, meta_len)
        return decode(payload[meta_len + 3:], pred, payload[meta_len + 2])
    return payload[meta_len:]
