package com.smartglass.test

import android.content.Context
import org.tensorflow.lite.Interpreter
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.round
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * 소리 상황 분류 — 안호연 "Smart Hearing AI" (sound_model/, Edge Impulse 내보내기 v5, 통합 4단계 2026-10-01)
 *
 * 모델: INT8 양자화 TFLite, 입력 = MFE 특징 6368개(199프레임×32필터), 출력 = 3클래스 [danger, noise, speech]
 * 오디오 입력: 16kHz mono 2.0초(32,000샘플) — 우리 빔포밍 스트림과 샘플레이트 일치.
 *   ⚠ EI 관례: 샘플을 int16 원값 그대로 float로 사용 (÷32768 정규화 금지 — dB 스케일이 어긋남)
 *
 * MFE v4 전처리 — sound_model/edge-impulse-sdk 소스(feature.hpp/processing.hpp/numpy.hpp)에서 그대로 포팅:
 *   프레임 320샘플/스트라이드 160 → 각 프레임 앞 256샘플만 FFT(256) → 파워 |X|²/256 (129빈)
 *   멜 필터 32개: mel=1127·ln(1+f/700), 빈 = floor(257·hz/16000), 삼각 가중(middle=1.0, 양끝 제외)
 *   0→1e-10 → 10·log10 → (dB+52)/64 → round(×256)/256 → [0,1] 클립 (noise_floor=-52dB)
 */
class SoundClassifier(ctx: Context) {

    companion object {
        val CLASSES = arrayOf("danger", "noise", "speech")
        val KR = mapOf("danger" to "위험", "noise" to "소음", "speech" to "대화")
        const val SAMPLE_RATE = 16000
        const val WINDOW = 32000          // 2.0초
        private const val FRAMES = 199
        private const val FRAME_STRIDE = 160
        private const val NUM_FILTERS = 32
        private const val NFFT = 256
        private const val NBINS = NFFT / 2 + 1   // 129
        private const val NOISE_FLOOR = -52.0f
        const val SILENCE_RMS = 150.0     // 이 미만이면 '무음' 판정(모델 스킵) — §7.4 무음 열 복원
        const val CONF_GATE = 0.6f        // EI 기본 임계 (model_metadata EI_CLASSIFIER_THRESHOLD)
    }

    data class Result(val cls: String?, val scores: FloatArray, val rms: Double, val ms: Long) {
        val isSilence get() = cls == null
    }

    private val interp: Interpreter
    private val inScale: Float
    private val inZp: Int
    private val outScale: Float
    private val outZp: Int

    // 링버퍼 (전송 스레드 push / 분류 스레드 read — 스냅샷 복사로 사용)
    private val ring = ShortArray(WINDOW)
    @Volatile private var wr = 0
    @Volatile private var filled = 0

    // MFE 사전 계산 (v4는 윈도함수 없음 — rect)
    private val bins = IntArray(NUM_FILTERS + 2)
    private val twc = FloatArray(NFFT / 2)
    private val tws = FloatArray(NFFT / 2)
    private val brev = IntArray(NFFT)

    init {
        val fd = ctx.assets.openFd("soundModel.tflite")
        val buf = fd.createInputStream().channel.map(
            FileChannel.MapMode.READ_ONLY, fd.startOffset, fd.declaredLength)
        interp = Interpreter(buf)
        val it = interp.getInputTensor(0)
        val ot = interp.getOutputTensor(0)
        inScale = it.quantizationParams().scale
        inZp = it.quantizationParams().zeroPoint
        outScale = ot.quantizationParams().scale
        outZp = ot.quantizationParams().zeroPoint

        // 멜 빈 경계 (feature.hpp mfe v4와 동일 수식)
        fun freqToMel(f: Double) = 1127.0 * ln(1.0 + f / 700.0)
        fun melToFreq(m: Double) = 700.0 * (exp(m / 1127.0) - 1.0)
        val lowMel = freqToMel(0.0)
        val highMel = freqToMel(8000.0)
        for (i in 0 until NUM_FILTERS + 2) {
            val mel = lowMel + (highMel - lowMel) * i / (NUM_FILTERS + 1)
            var hz = melToFreq(mel).coerceIn(0.0, 8000.0)
            if (i == NUM_FILTERS + 1) hz -= 0.001   // speechpy 마지막 빈 버그 보존 (원본 주석 계승)
            bins[i] = floor((NFFT + 1) * hz / SAMPLE_RATE).toInt()
        }

        // FFT 테이블 (radix-2, N=256)
        val bits = 8
        for (i in 0 until NFFT) {
            var r = 0
            for (b in 0 until bits) if (i and (1 shl b) != 0) r = r or (1 shl (bits - 1 - b))
            brev[i] = r
        }
        for (k in 0 until NFFT / 2) {
            twc[k] = cos(2.0 * Math.PI * k / NFFT).toFloat()
            tws[k] = sin(2.0 * Math.PI * k / NFFT).toFloat()
        }
    }

    /** 전송로 오디오 유입 (16kHz mono만 — raw 3ch 48k는 무시) */
    fun push(samples: ShortArray, nch: Int, rate: Int) {
        if (nch != 1 || rate != SAMPLE_RATE) return
        for (s in samples) {
            ring[wr] = s
            wr = (wr + 1) % WINDOW
        }
        if (filled < WINDOW) filled = minOf(WINDOW, filled + samples.size)
    }

    fun ready() = filled >= WINDOW

    /** 외부 버퍼(자가시험용 PCM 등)를 직접 분류 */
    fun classifyBuffer(window: ShortArray): Result {
        val t0 = android.os.SystemClock.elapsedRealtime()
        var sq = 0.0
        for (s in window) sq += s.toDouble() * s
        val rms = sqrt(sq / window.size)
        if (rms < SILENCE_RMS) {
            return Result(null, FloatArray(3), rms, android.os.SystemClock.elapsedRealtime() - t0)
        }

        val features = mfe(window)                       // 199×32, [0,1]
        val input = ByteBuffer.allocateDirect(features.size).order(ByteOrder.nativeOrder())
        for (f in features) {
            val q = round(f / inScale + inZp).toInt().coerceIn(-128, 127)
            input.put(q.toByte())
        }
        input.rewind()
        val out = ByteBuffer.allocateDirect(CLASSES.size).order(ByteOrder.nativeOrder())
        interp.run(input, out)
        out.rewind()
        val scores = FloatArray(CLASSES.size) { (out.get(it) - outZp) * outScale }
        var best = 0
        for (i in scores.indices) if (scores[i] > scores[best]) best = i
        val cls = if (scores[best] >= CONF_GATE) CLASSES[best] else "noise"  // 임계 미달 → 소음 취급
        return Result(cls, scores, rms, android.os.SystemClock.elapsedRealtime() - t0)
    }

    /** 링버퍼의 최근 2초 창을 분류 (백그라운드 스레드에서 호출) */
    fun classify(): Result {
        val window = ShortArray(WINDOW)
        val w = wr
        for (i in 0 until WINDOW) window[i] = ring[(w + i) % WINDOW]  // 오래된→최신 순
        return classifyBuffer(window)
    }

    // ── MFE v4 (Edge Impulse SDK 충실 포팅) ────────────────────────────
    private fun mfe(window: ShortArray): FloatArray {
        val features = FloatArray(FRAMES * NUM_FILTERS)
        val re = FloatArray(NFFT)
        val im = FloatArray(NFFT)
        val power = FloatArray(NBINS)

        for (frame in 0 until FRAMES) {
            val off = frame * FRAME_STRIDE
            // rfft는 프레임(320)을 fft 길이(256)로 절단 — numpy.hpp rfft "truncate if needed" 계승
            for (i in 0 until NFFT) {
                re[i] = window[off + i].toFloat()   // int16 원값 그대로 (EI 오디오 관례)
                im[i] = 0f
            }
            fft(re, im)
            for (k in 0 until NBINS) {
                power[k] = (re[k] * re[k] + im[k] * im[k]) / NFFT   // |X|²/N (power_spectrum)
            }
            val base = frame * NUM_FILTERS
            for (i in 0 until NUM_FILTERS) {
                val left = bins[i]
                val middle = bins[i + 1]
                val right = bins[i + 2]
                var acc = power[middle]              // middle 가중 1.0, left/right 가중 0 제외
                for (bin in left + 1 until right) {
                    if (bin < middle) acc += (bin - left).toFloat() / (middle - left) * power[bin]
                    else if (bin > middle) acc += (right - bin).toFloat() / (right - middle) * power[bin]
                }
                if (acc == 0f) acc = 1e-10f          // zero_handling
                // mfe_normalization: 10·log10 → (dB+52)/64 → 1/256 반올림 → [0,1]
                var f = 10.0f * log10(acc.coerceAtLeast(1e-30f))
                f = (f - NOISE_FLOOR) / (-NOISE_FLOOR + 12.0f)
                f = round(f * 256.0f) / 256.0f
                features[base + i] = f.coerceIn(0.0f, 1.0f)
            }
        }
        return features
    }

    private fun fft(re: FloatArray, im: FloatArray) {
        for (i in 0 until NFFT) {
            val j = brev[i]
            if (j > i) {
                var t = re[i]; re[i] = re[j]; re[j] = t
                t = im[i]; im[i] = im[j]; im[j] = t
            }
        }
        var len = 2
        while (len <= NFFT) {
            val half = len shr 1
            val step = NFFT / len
            var i = 0
            while (i < NFFT) {
                for (k in 0 until half) {
                    val t = k * step
                    val wr2 = twc[t]
                    val wi2 = -tws[t]
                    val br = re[i + k + half]; val bi = im[i + k + half]
                    val vr = br * wr2 - bi * wi2
                    val vi = br * wi2 + bi * wr2
                    re[i + k + half] = re[i + k] - vr
                    im[i + k + half] = im[i + k] - vi
                    re[i + k] += vr
                    im[i + k] += vi
                }
                i += len
            }
            len = len shl 1
        }
    }
}
