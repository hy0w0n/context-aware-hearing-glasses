package com.smartglass.test

import android.content.Context
import org.tensorflow.lite.Interpreter
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import kotlin.math.round
import kotlin.math.sqrt

/**
 * 소리 상황 분류 — 안호연 "Smart Hearing AI" (sound_model/, Edge Impulse로 학습한 v5, 통합 4단계 2026-10-01)
 *
 * 모델: INT8 양자화 TFLite, 입력 = MFE 특징 6368개(199프레임×32필터), 출력 = 3클래스 [danger, noise, speech]
 * 오디오 입력: 16kHz mono 2.0초(32,000샘플) — 우리 빔포밍 스트림과 샘플레이트 일치.
 * 특징 계산은 MelFeatures.kt (공개 자료 기준으로 다시 작성, 2026-11-09 — 옛 구현과 모델 결과 동일 확인).
 */
class SoundClassifier(ctx: Context) {

    companion object {
        val CLASSES = arrayOf("danger", "noise", "speech")
        val KR = mapOf("danger" to "위험", "noise" to "소음", "speech" to "대화")
        const val SAMPLE_RATE = 16000
        const val WINDOW = 32000          // 2.0초
        const val SILENCE_RMS = 150.0     // 이 미만이면 '무음' 판정(모델 스킵) — §7.4 무음 열 복원
        const val CONF_GATE = 0.6f        // Edge Impulse 프로젝트의 기본 판정 임계값
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
        // 최대 크기 맞춤 → 프리엠퍼시스 → ÷32768 → MFE. 이 전처리가 빠지면 특징이 포화되어 출력이 입력과 무관해진다(2026-10-03 확인).
        // 무음 판정은 원값 RMS로 먼저 끝냈으므로 정적을 증폭하는 일은 없다.
        val features = MelFeatures.compute(MelFeatures.prepare(window))   // 199×32, 0~255/256
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
}
