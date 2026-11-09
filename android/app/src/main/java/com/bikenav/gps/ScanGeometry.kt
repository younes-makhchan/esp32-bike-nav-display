package com.bikenav.gps

import kotlin.math.min

/**
 * Where the square frame the rider sees on screen lies in the camera image, so only that part is decoded:
 * faster, and the decoder looks exactly where the rider is aiming. Pure maths (portrait screen, landscape sensor).
 */
object ScanGeometry {
    class Crop(val left: Int, val top: Int, val size: Int)

    /** Size of the preview view so that the (rotated) camera image covers the whole screen without stretching. */
    fun viewSizeCovering(bufW: Int, bufH: Int, screenW: Int, screenH: Int): Pair<Int, Int> {
        val aspect = bufH.toDouble() / bufW             // shown rotated: width / height in portrait
        var w = screenW.toDouble()
        var h = w / aspect
        if (h < screenH) { h = screenH.toDouble(); w = h * aspect }
        return Pair(w.toInt(), h.toInt())
    }

    /** The centre square of the camera image that appears under the on-screen frame. */
    fun cropForFrame(bufW: Int, bufH: Int, screenW: Int, screenH: Int, frameFraction: Float): Crop {
        val (viewW, _) = viewSizeCovering(bufW, bufH, screenW, screenH)
        val scale = viewW.toDouble() / bufH             // screen pixels per camera pixel
        val frame = frameFraction * min(screenW, screenH)
        val q = min(bufH.toDouble(), frame / scale).toInt()
        return Crop((bufW - q) / 2, (bufH - q) / 2, q)
    }
}
