package com.smartglass.test

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.sin

/**
 * 소리 분류 모델의 입력 특징 — MFE(멜 필터뱅크 에너지).
 *
 * 2026-11-09 다시 작성: Edge Impulse가 공개한 처리 블록(github.com/edgeimpulse/processing-blocks,
 * mfe/dsp.py와 그 안의 speechpy, The Clear BSD License)과 원본 speechpy(Apache-2.0)에 공개된
 * 계산 순서를 근거로 새로 짰다. Edge Impulse SDK의 C++ 코드는 쓰지 않는다.
 *
 *   int16 2.0초(32,000샘플)
 *   → [prepare] 창 최대 크기 맞춤 → 프리엠퍼시스 y[n] = x[n] − 0.98·x[n−1] (n=0은 마지막 샘플) → ÷32768
 *   → [compute] 프레임 320샘플(20 ms)·간격 160(10 ms), 직사각 창 → 199프레임
 *   → 프레임 앞 256샘플 FFT → 파워 |X|²/256 (129빈)
 *   → 멜 필터 32개(0~8000 Hz) 행렬곱 → 0은 1e-10으로
 *   → 10·log10 → (dB + 52) / 64 → [0, 1]로 자름 → 1/256 단위로 양자화
 */
object MelFeatures {
    const val SAMPLE_RATE = 16000
    const val WINDOW = 32000            // 2.0초
    const val FRAME_LEN = 320           // 20 ms
    const val FRAME_STEP = 160          // 10 ms
    const val FFT_SIZE = 256
    const val BINS = FFT_SIZE / 2 + 1   // 129
    const val FILTERS = 32
    const val FRAMES = (WINDOW - FRAME_LEN) / FRAME_STEP + 1   // 199
    const val SIZE = FRAMES * FILTERS                          // 6,368

    private const val NOISE_FLOOR_DB = -52.0
    private const val PREEMPHASIS = 0.98
    private const val PEAK_TARGET = 32767.0   // 학습 클립은 2.0초마다 최대 진폭을 1.0으로 맞췄다

    /** 멜 필터뱅크 [FILTERS][BINS] — speechpy filterbanks()의 정의(멜 = 1127·ln(1 + f/700)) */
    private val filterbank: Array<DoubleArray> = run {
        fun hzToMel(hz: Double) = 1127.0 * ln(1.0 + hz / 700.0)
        fun melToHz(mel: Double) = 700.0 * (exp(mel / 1127.0) - 1.0)
        val points = FILTERS + 2
        val loMel = hzToMel(0.0)
        val hiMel = hzToMel(SAMPLE_RATE / 2.0)
        val edge = IntArray(points) { i ->
            var hz = melToHz(loMel + (hiMel - loMel) * i / (points - 1))
            if (i == points - 1) hz -= 0.001   // 공개 처리 블록과 같이 마지막 경계를 0.001 Hz 내린다
            floor((FFT_SIZE + 1) * hz / SAMPLE_RATE).toInt()
        }
        Array(FILTERS) { f ->
            val left = edge[f]
            val mid = edge[f + 1]
            val right = edge[f + 2]
            DoubleArray(BINS) { k ->
                var w = 0.0
                if (k > left && k <= mid) w = (k - left).toDouble() / (mid - left)
                if (k >= mid && k < right) w = (right - k).toDouble() / (right - mid)
                w
            }
        }
    }

    private val cosTable = DoubleArray(FFT_SIZE / 2) { cos(2.0 * PI * it / FFT_SIZE) }
    private val sinTable = DoubleArray(FFT_SIZE / 2) { sin(2.0 * PI * it / FFT_SIZE) }
    private val bitReversed = IntArray(FFT_SIZE) { i ->
        var r = 0
        var v = i
        repeat(8) { r = (r shl 1) or (v and 1); v = v shr 1 }   // FFT_SIZE = 2^8
        r
    }

    /** int16 창 → 모델에 넣기 전 float 신호 (최대 크기 맞춤, 프리엠퍼시스, ÷32768) */
    fun prepare(pcm: ShortArray): FloatArray {
        var peak = 0
        for (s in pcm) peak = max(peak, abs(s.toInt()))
        val gain = (if (peak > 0) PEAK_TARGET / peak else 1.0) / 32768.0
        var prev = pcm[pcm.size - 1].toDouble()
        return FloatArray(pcm.size) { i ->
            val cur = pcm[i].toDouble()
            val y = (cur - PREEMPHASIS * prev) * gain
            prev = cur
            y.toFloat()
        }
    }

    /** prepare()를 거친 2.0초 신호 → 특징 199 × 32 (프레임 순, 값은 0~255/256) */
    fun compute(signal: FloatArray): FloatArray {
        val out = FloatArray(SIZE)
        val re = DoubleArray(FFT_SIZE)
        val im = DoubleArray(FFT_SIZE)
        val power = DoubleArray(BINS)
        for (t in 0 until FRAMES) {
            val start = t * FRAME_STEP
            for (n in 0 until FFT_SIZE) {   // 프레임은 320샘플이지만 FFT에는 앞 256샘플만 들어간다
                re[n] = signal[start + n].toDouble()
                im[n] = 0.0
            }
            fft(re, im)
            for (k in 0 until BINS) power[k] = (re[k] * re[k] + im[k] * im[k]) / FFT_SIZE
            for (f in 0 until FILTERS) {
                val row = filterbank[f]
                var energy = 0.0
                for (k in 0 until BINS) energy += power[k] * row[k]
                if (energy == 0.0) energy = 1e-10
                out[t * FILTERS + f] = normalize(energy)
            }
        }
        return out
    }

    /** 에너지 → dB → 잡음 바닥 기준 [0,1] → 1/256 양자화 */
    private fun normalize(energy: Double): Float {
        val db = 10.0 * log10(max(energy, 1e-30))
        val v = ((db - NOISE_FLOOR_DB) / (-NOISE_FLOOR_DB + 12.0)).coerceIn(0.0, 1.0)
        // 공개 처리 블록은 uint8로 바꾸며 256을 넘기면 값이 감기는데, 여기서는 255에서 멈춘다(모델 입력 범위의 최댓값)
        val q = Math.rint(v * 256.0).coerceIn(0.0, 255.0)
        return (q / 256.0).toFloat()
    }

    /** 반복형 radix-2 FFT (제자리, 길이 256) */
    private fun fft(re: DoubleArray, im: DoubleArray) {
        for (i in 0 until FFT_SIZE) {
            val j = bitReversed[i]
            if (j > i) {
                var tmp = re[i]; re[i] = re[j]; re[j] = tmp
                tmp = im[i]; im[i] = im[j]; im[j] = tmp
            }
        }
        var size = 2
        while (size <= FFT_SIZE) {
            val half = size / 2
            val stride = FFT_SIZE / size
            for (start in 0 until FFT_SIZE step size) {
                for (k in 0 until half) {
                    val c = cosTable[k * stride]
                    val s = -sinTable[k * stride]
                    val a = start + k
                    val b = a + half
                    val tr = re[b] * c - im[b] * s
                    val ti = re[b] * s + im[b] * c
                    re[b] = re[a] - tr
                    im[b] = im[a] - ti
                    re[a] += tr
                    im[a] += ti
                }
            }
            size *= 2
        }
    }
}
