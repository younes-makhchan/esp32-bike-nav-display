package com.bikenav.gps

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.view.View
import android.view.animation.LinearInterpolator
import kotlin.math.min

/** Dimmed camera view with a square frame, corner marks and a moving scan line; turns green on success. */
class ViewfinderView(context: Context) : View(context) {
    companion object { const val FRACTION = 0.7f }          // frame size as a share of the screen width

    private val d = resources.displayMetrics.density
    private val dim = Paint().apply { color = 0xAA000000.toInt() }
    private val okFill = Paint().apply { color = 0x4422C55E }
    private val corner = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 4 * d; strokeCap = Paint.Cap.ROUND; color = 0xFFFFFFFF.toInt()
    }
    private val scan = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeWidth = 3 * d; color = 0xFF38BDF8.toInt() }
    private var t = 0f
    private var ok = false
    private val anim = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 1800
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener { t = it.animatedValue as Float; invalidate() }
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); anim.start() }
    override fun onDetachedFromWindow() { anim.cancel(); super.onDetachedFromWindow() }

    fun success() {
        ok = true
        corner.color = 0xFF22C55E.toInt()
        anim.cancel()
        invalidate()
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val s = FRACTION * min(w, h)
        val l = (w - s) / 2
        val top = (h - s) / 2
        val r = l + s
        val b = top + s

        c.drawRect(0f, 0f, w, top, dim)                     // everything but the frame is dimmed
        c.drawRect(0f, b, w, h, dim)
        c.drawRect(0f, top, l, b, dim)
        c.drawRect(r, top, w, b, dim)
        if (ok) c.drawRect(l, top, r, b, okFill)

        val len = 28 * d
        fun mark(x: Float, y: Float, dx: Float, dy: Float) {
            c.drawLine(x, y, x + dx * len, y, corner)
            c.drawLine(x, y, x, y + dy * len, corner)
        }
        mark(l, top, 1f, 1f); mark(r, top, -1f, 1f); mark(l, b, 1f, -1f); mark(r, b, -1f, -1f)

        if (!ok) {
            val y = top + t * s
            c.drawLine(l + 10 * d, y, r - 10 * d, y, scan)
        }
    }
}
