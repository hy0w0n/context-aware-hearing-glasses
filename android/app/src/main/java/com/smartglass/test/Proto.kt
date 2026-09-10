package com.smartglass.test

/**
 * 통신 프로토콜 v0.1 (CLAUDE.md §6.1) 프레임 빌더/파서.
 * [0xA5 0x5A][type u8][flags u8][seq u16 LE][len u16 LE][payload][crc16 LE]
 * CRC16-CCITT: magic 제외, type부터 payload 끝까지.
 */
object Proto {
    const val TYPE_AUDIO = 0x01
    const val TYPE_IMG_START = 0x02
    const val TYPE_IMG_CHUNK = 0x03
    const val TYPE_IMG_END = 0x04
    const val TYPE_EVENT = 0x10
    const val TYPE_CMD = 0x20
    const val TYPE_PING = 0x30
    const val TYPE_PONG = 0x31

    const val AUDIO_META_LEN = 1 + 4 + 2 + 1 + 8  // fmt, t_ms, azimuth, conf, dir_hist[8]

    // CMD subtypes (§6.1)
    const val CMD_SET_MODE = 0x01
    const val CMD_SET_VOLUME = 0x02
    const val CMD_DISPLAY_ALERT = 0x03
    const val CMD_SET_CONFIG = 0x04
    const val CMD_CAPTURE_NOW = 0x05

    fun crc16(d: ByteArray, off: Int, len: Int): Int {
        var crc = 0xFFFF
        for (i in off until off + len) {
            crc = crc xor ((d[i].toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF
                else (crc shl 1) and 0xFFFF
            }
        }
        return crc
    }

    fun build(type: Int, seq: Int, payload: ByteArray): ByteArray {
        val out = ByteArray(10 + payload.size)
        out[0] = 0xA5.toByte(); out[1] = 0x5A.toByte()
        out[2] = type.toByte(); out[3] = 0
        out[4] = (seq and 0xFF).toByte(); out[5] = ((seq shr 8) and 0xFF).toByte()
        out[6] = (payload.size and 0xFF).toByte(); out[7] = ((payload.size shr 8) and 0xFF).toByte()
        System.arraycopy(payload, 0, out, 8, payload.size)
        val c = crc16(out, 2, 6 + payload.size)
        out[8 + payload.size] = (c and 0xFF).toByte()
        out[9 + payload.size] = ((c shr 8) and 0xFF).toByte()
        return out
    }

    fun u16(d: ByteArray, i: Int): Int = (d[i].toInt() and 0xFF) or ((d[i + 1].toInt() and 0xFF) shl 8)

    fun u32(d: ByteArray, i: Int): Long =
        (d[i].toLong() and 0xFF) or ((d[i + 1].toLong() and 0xFF) shl 8) or
        ((d[i + 2].toLong() and 0xFF) shl 16) or ((d[i + 3].toLong() and 0xFF) shl 24)

    // IMA ADPCM 디코드용 테이블 (§6.1 fmt=1 — 펌웨어 adpcm.cpp와 대칭, 2026-09-10)
    private val ADPCM_STEP = intArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767)
    private val ADPCM_IDX = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

    // 최근 AUDIO 프레임의 DOA 메타 (§6.1) — ModeController가 일대일/다인 분기에 사용 (통합 3단계)
    // audioSamples()가 파싱 김에 갱신하는 사이드채널 (테스트 앱 단순화 — 콜백 시그니처 불변 유지)
    @Volatile var lastAzimuth01: Int = 0      // 0.1° 단위 -1800~1800
    @Volatile var lastDoaConf: Int = 0        // 0~255 (0=무효)
    @Volatile var lastDirHist: ByteArray = ByteArray(8)

    /** AUDIO 프레임(전체 버퍼, off=프레임 시작)에서 샘플 추출.
     *  payload 첫 바이트 fmt: 0=PCM16 그대로, 1=IMA ADPCM(헤더의 predictor/step_idx로 패킷 단위 리싱크) */
    fun audioSamples(d: ByteArray, off: Int, plen: Int): ShortArray {
        // 메타 파싱: fmt(1) t_ms(4) azimuth(2) conf(1) hist(8)
        lastAzimuth01 = ((d[off + 13].toInt() and 0xFF) or (d[off + 14].toInt() shl 8)).toShort().toInt()
        lastDoaConf = d[off + 15].toInt() and 0xFF
        lastDirHist = d.copyOfRange(off + 16, off + 24)
        val fmt = d[off + 8].toInt() and 0xFF
        if (fmt == 1) {
            val base = off + 8 + AUDIO_META_LEN  // adpcm_state{predictor i16 LE, step_idx u8} + 니블
            var pred = ((d[base].toInt() and 0xFF) or (d[base + 1].toInt() shl 8)).toShort().toInt()
            var idx = d[base + 2].toInt() and 0xFF
            val nBytes = plen - AUDIO_META_LEN - 3
            if (idx > 88 || nBytes <= 0) return ShortArray(0)
            val out = ShortArray(nBytes * 2)
            var o = 0
            for (i in 0 until nBytes) {
                val b = d[base + 3 + i].toInt() and 0xFF
                for (half in 0..1) {
                    val nib = if (half == 0) b and 0xF else b shr 4  // 첫 샘플=하위 니블
                    val step = ADPCM_STEP[idx]
                    var delta = step shr 3
                    if (nib and 4 != 0) delta += step
                    if (nib and 2 != 0) delta += step shr 1
                    if (nib and 1 != 0) delta += step shr 2
                    pred += if (nib and 8 != 0) -delta else delta
                    if (pred > 32767) pred = 32767
                    if (pred < -32768) pred = -32768
                    idx = (idx + ADPCM_IDX[nib]).coerceIn(0, 88)
                    out[o++] = pred.toShort()
                }
            }
            return out
        }
        val n = (plen - AUDIO_META_LEN) / 2
        val out = ShortArray(n)
        var p = off + 8 + AUDIO_META_LEN
        for (k in 0 until n) {
            out[k] = ((d[p].toInt() and 0xFF) or ((d[p + 1].toInt()) shl 8)).toShort()
            p += 2
        }
        return out
    }
}

/** 전송로(WiFi/BLE) 공통 콜백 — UI 스레드 보장 없음 (Activity에서 runOnUiThread) */
interface LinkCallback {
    fun onStatus(text: String)
    fun onAudio(samples: ShortArray, nch: Int, rate: Int)
    fun onEvent(text: String)
    fun onImage(bmp: android.graphics.Bitmap, kb: Int)
    fun onStats(text: String)
    fun onRtt(ms: Double)
}
