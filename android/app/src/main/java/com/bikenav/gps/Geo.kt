package com.bikenav.gps

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

data class LngLat(val lng: Double, val lat: Double)

/** Geometry on AMap (GCJ-02) coordinates. Pure Kotlin so it can be unit-tested. */
object Geo {
    private const val M_PER_DEG = 111320.0

    // ---- GPS (WGS-84) -> AMap (GCJ-02): the two differ by 300-500 m inside China ----
    private fun outOfChina(lat: Double, lon: Double) =
        lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271

    private fun tLat(x: Double, y: Double): Double {
        var r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * sqrt(abs(x))
        r += (20.0 * sin(6.0 * x * PI) + 20.0 * sin(2.0 * x * PI)) * 2.0 / 3.0
        r += (20.0 * sin(y * PI) + 40.0 * sin(y / 3.0 * PI)) * 2.0 / 3.0
        r += (160.0 * sin(y / 12.0 * PI) + 320.0 * sin(y * PI / 30.0)) * 2.0 / 3.0
        return r
    }

    private fun tLon(x: Double, y: Double): Double {
        var r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(abs(x))
        r += (20.0 * sin(6.0 * x * PI) + 20.0 * sin(2.0 * x * PI)) * 2.0 / 3.0
        r += (20.0 * sin(x * PI) + 40.0 * sin(x / 3.0 * PI)) * 2.0 / 3.0
        r += (150.0 * sin(x / 12.0 * PI) + 300.0 * sin(x / 30.0 * PI)) * 2.0 / 3.0
        return r
    }

    fun wgs84ToGcj02(lat: Double, lon: Double): LngLat {
        if (outOfChina(lat, lon)) return LngLat(lon, lat)
        val a = 6378245.0
        val ee = 0.00669342162296594323
        var dLat = tLat(lon - 105.0, lat - 35.0)
        var dLon = tLon(lon - 105.0, lat - 35.0)
        val radLat = lat / 180.0 * PI
        var magic = sin(radLat)
        magic = 1.0 - ee * magic * magic
        val sqrtMagic = sqrt(magic)
        dLat = (dLat * 180.0) / ((a * (1.0 - ee)) / (magic * sqrtMagic) * PI)
        dLon = (dLon * 180.0) / (a / sqrtMagic * cos(radLat) * PI)
        return LngLat(lon + dLon, lat + dLat)
    }

    // ---- distances and directions ----
    fun distanceM(a: LngLat, b: LngLat): Double {
        val r = 6371000.0
        val p1 = Math.toRadians(a.lat)
        val p2 = Math.toRadians(b.lat)
        val dp = p2 - p1
        val dl = Math.toRadians(b.lng - a.lng)
        val h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2)
        return 2 * r * atan2(sqrt(h), sqrt(1 - h))
    }

    /** Initial bearing a -> b, degrees clockwise from north, 0..360. */
    fun bearingDeg(a: LngLat, b: LngLat): Double {
        val p1 = Math.toRadians(a.lat)
        val p2 = Math.toRadians(b.lat)
        val dl = Math.toRadians(b.lng - a.lng)
        val y = sin(dl) * cos(p2)
        val x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)
        return (Math.toDegrees(atan2(y, x)) + 360.0) % 360.0
    }

    fun norm180(x: Double): Double {
        val a = ((x % 360.0) + 360.0) % 360.0
        return if (a > 180.0) a - 360.0 else a
    }

    class Proj(val distM: Double, val t: Double, val lenM: Double)

    /** Distance from p to segment a-b, and the position t (0..1) of the closest point on it. */
    fun project(p: LngLat, a: LngLat, b: LngLat): Proj {
        val k = cos(Math.toRadians(a.lat))
        val ax = (b.lng - a.lng) * k * M_PER_DEG
        val ay = (b.lat - a.lat) * M_PER_DEG
        val px = (p.lng - a.lng) * k * M_PER_DEG
        val py = (p.lat - a.lat) * M_PER_DEG
        val l2 = ax * ax + ay * ay
        var t = if (l2 > 0) (px * ax + py * ay) / l2 else 0.0
        t = t.coerceIn(0.0, 1.0)
        return Proj(hypot(px - t * ax, py - t * ay), t, sqrt(l2))
    }

    /**
     * Douglas-Peucker in metres. Keeps every bend (taking every Nth point cuts corners and pushes the
     * line off the road centre). Loosens the tolerance until at most [maxPts] points remain.
     */
    fun simplify(pts: List<LngLat>, tolM: Double, maxPts: Int): List<LngLat> {
        val n = pts.size
        if (n <= 2) return pts
        val k = cos(Math.toRadians(pts[0].lat))
        val x = DoubleArray(n) { pts[it].lng * k * M_PER_DEG }
        val y = DoubleArray(n) { pts[it].lat * M_PER_DEG }
        var tol = tolM
        var out: List<LngLat> = pts
        for (pass in 0 until 10) {
            val keep = BooleanArray(n)
            keep[0] = true
            keep[n - 1] = true
            val stack = ArrayDeque<IntArray>()
            stack.addLast(intArrayOf(0, n - 1))
            while (stack.isNotEmpty()) {
                val (a, b) = stack.removeLast()
                val dx = x[b] - x[a]
                val dy = y[b] - y[a]
                val l2 = dx * dx + dy * dy
                var md = -1.0
                var mi = -1
                for (i in a + 1 until b) {
                    val d = if (l2 == 0.0) hypot(x[i] - x[a], y[i] - y[a]) else {
                        val t = (((x[i] - x[a]) * dx + (y[i] - y[a]) * dy) / l2).coerceIn(0.0, 1.0)
                        hypot(x[i] - x[a] - t * dx, y[i] - y[a] - t * dy)
                    }
                    if (d > md) { md = d; mi = i }
                }
                if (md > tol) {
                    keep[mi] = true
                    stack.addLast(intArrayOf(a, mi))
                    stack.addLast(intArrayOf(mi, b))
                }
            }
            out = pts.filterIndexed { i, _ -> keep[i] }
            if (out.size <= maxPts) return out
            tol *= 1.8
        }
        return out
    }
}
