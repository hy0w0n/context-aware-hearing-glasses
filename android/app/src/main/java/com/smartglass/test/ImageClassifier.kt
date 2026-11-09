package com.smartglass.test

import android.content.Context
import android.graphics.Bitmap
import org.tensorflow.lite.Interpreter
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * 이미지 상황 분류 — 최다현 MobileNetV2 (image_model/imageModel.tflite, 통합 1단계 2026-09-10)
 *
 * 입력: 224×224 RGB float32 **0~255 원본값** — 전처리(preprocess_input)가 모델 내부에 내장돼 있어
 *       외부에서 ÷127.5-1 정규화를 하면 이중 전처리로 파손됨 (camera.py 주석 계승).
 * 출력: 6클래스 확률, 알파벳순 [conversation, danger, indoor, negative, outdoor, tv].
 * 후처리(camera.py 로직 포팅): ① negative 확신도 <0.95 → 2순위 클래스 채택
 *                              ② 최근 N장 다수결(negative 제외) — PC 7프레임, 앱은 3초 주기라 3장(~9초)
 */
class ImageClassifier(ctx: Context) {

    companion object {
        val CLASSES = arrayOf("conversation", "danger", "indoor", "negative", "outdoor", "tv")
        val KR = mapOf(
            "conversation" to "대화", "danger" to "위험", "indoor" to "실내",
            "negative" to "분석불가", "outdoor" to "야외", "tv" to "TV")
        private const val SIZE = 224
        private const val SMOOTH_N = 3
        private const val NEG_CONF_GATE = 0.95f
    }

    /** raw=이번 장 판정(+확신도), stable=다수결 스무딩 후 제어용 판정 */
    data class Result(val rawClass: String, val conf: Float, val stableClass: String, val ms: Long)

    private val interp: Interpreter
    private val input = ByteBuffer.allocateDirect(SIZE * SIZE * 3 * 4).order(ByteOrder.nativeOrder())
    private val pixels = IntArray(SIZE * SIZE)
    private val recent = ArrayDeque<String>()
    private var stable = "indoor"   // camera.py와 동일한 초기 기본 모드

    init {
        val fd = ctx.assets.openFd("imageModel.tflite")
        val buf = fd.createInputStream().channel.map(
            FileChannel.MapMode.READ_ONLY, fd.startOffset, fd.declaredLength)
        interp = Interpreter(buf)
    }

    /** 수신 Bitmap 1장 분류. 추론 수십 ms — UI 스레드에서 호출 금지 (executor 사용)
     *  gray=true: 흑백(휘도) 3채널 복제 입력 — 흑백 전송 가능성 A/B 검증용 (2026-10-01)
     *  smooth=false: 다수결 상태 미갱신 (자가시험이 실스트림 제어판정을 오염하지 않게) */
    @JvmOverloads
    fun classify(bmp: Bitmap, gray: Boolean = false, smooth: Boolean = true): Result {
        val t0 = android.os.SystemClock.elapsedRealtime()
        val scaled = Bitmap.createScaledBitmap(bmp, SIZE, SIZE, true)
        scaled.getPixels(pixels, 0, SIZE, 0, 0, SIZE, SIZE)
        input.rewind()
        for (p in pixels) {  // ARGB int → R,G,B float 0~255 그대로
            val r = ((p shr 16) and 0xFF).toFloat()
            val g = ((p shr 8) and 0xFF).toFloat()
            val b = (p and 0xFF).toFloat()
            if (gray) {  // ITU-R BT.601 휘도 — ColorMatrix setSaturation(0)과 동일 가중
                val y = 0.299f * r + 0.587f * g + 0.114f * b
                input.putFloat(y); input.putFloat(y); input.putFloat(y)
            } else {
                input.putFloat(r); input.putFloat(g); input.putFloat(b)
            }
        }
        val out = Array(1) { FloatArray(CLASSES.size) }
        interp.run(input, out)
        val probs = out[0]

        var best = 0
        for (i in probs.indices) if (probs[i] > probs[best]) best = i
        // ① negative 과적합 방지: 확신도 낮으면 2순위 채택
        if (CLASSES[best] == "negative" && probs[best] < NEG_CONF_GATE) {
            val negIdx = best
            val negProb = probs[negIdx]
            probs[negIdx] = -1f
            best = 0
            for (i in probs.indices) if (probs[i] > probs[best]) best = i
            probs[negIdx] = negProb
        }
        val raw = CLASSES[best]

        // ② 다수결 스무딩 (negative 프레임은 제어 판정에서 제외)
        if (smooth) {
            recent.addLast(raw)
            while (recent.size > SMOOTH_N) recent.removeFirst()
            val valid = recent.filter { it != "negative" }
            if (valid.isNotEmpty())
                stable = valid.groupingBy { it }.eachCount().maxByOrNull { it.value }!!.key
        }

        return Result(raw, probs[best], stable, android.os.SystemClock.elapsedRealtime() - t0)
    }
}
