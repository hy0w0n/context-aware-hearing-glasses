package com.smartglass.test

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.SystemClock
import android.util.AttributeSet
import android.view.View

/** 최근 0.5초 오디오(ch1) 파형 — 열마다 min/max 세로선 */
class WaveformView(ctx: Context, attrs: AttributeSet?) : View(ctx, attrs) {
    private val ring = ShortArray(24000)  // 0.5s @48k
    private var wpos = 0
    private var lastInv = 0L
    private val paint = Paint().apply {
        color = Color.rgb(11, 122, 99)
        strokeWidth = 2f
    }
    private val bgPaint = Paint().apply { color = Color.rgb(20, 26, 24) }
    private val midPaint = Paint().apply { color = Color.rgb(60, 70, 66); strokeWidth = 1f }

    fun push(samples: ShortArray, nch: Int) {
        var i = 0
        while (i < samples.size) {
            ring[wpos] = samples[i]  // ch1만
            wpos = (wpos + 1) % ring.size
            i += nch
        }
        val now = SystemClock.elapsedRealtime()
        if (now - lastInv > 66) {  // ~15fps
            lastInv = now
            postInvalidate()
        }
    }

    override fun onDraw(canvas: Canvas) {
        val w = width
        val h = height
        canvas.drawRect(0f, 0f, w.toFloat(), h.toFloat(), bgPaint)
        canvas.drawLine(0f, h / 2f, w.toFloat(), h / 2f, midPaint)
        if (w <= 0) return
        val step = ring.size / w
        for (x in 0 until w) {
            var mn = 32767
            var mx = -32768
            val base = (wpos + x * step) % ring.size
            for (k in 0 until step) {
                val v = ring[(base + k) % ring.size].toInt()
                if (v < mn) mn = v
                if (v > mx) mx = v
            }
            val y1 = h / 2f - mx / 32768f * (h / 2f)
            val y2 = h / 2f - mn / 32768f * (h / 2f)
            canvas.drawLine(x.toFloat(), y1, x.toFloat(), y2, paint)
        }
    }
}
