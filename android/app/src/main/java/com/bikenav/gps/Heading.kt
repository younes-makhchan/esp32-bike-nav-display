package com.bikenav.gps

import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.sqrt

/**
 * Direction of the arrow on the bike unit. Pure maths (no Android classes) so it can be
 * unit-tested on a computer. Mirrors termux/bike_gps.py.
 */
object HeadingMath {
    private const val FLAT = 0.75

    fun norm360(x: Double): Double = ((x % 360.0) + 360.0) % 360.0

    /** Signed a - b in -180..180. */
    fun angleDiff(a: Double, b: Double): Double = norm360(a - b + 180.0) - 180.0

    /** R maps phone axes to (east, north, up). Flat: top edge (y) is the front; else the back (-z). */
    private fun fromMatrix(r01: Double, r11: Double, r02: Double, r12: Double, r22: Double): Double {
        val east: Double
        val north: Double
        if (abs(r22) > FLAT) { east = r01; north = r11 } else { east = -r02; north = -r12 }
        return norm360(Math.toDegrees(atan2(east, north)))
    }

    /** From Android's rotation-vector quaternion (x, y, z, w). */
    fun fromQuaternion(x: Double, y: Double, z: Double, w: Double): Double = fromMatrix(
        2 * (x * y - z * w), 1 - 2 * (x * x + z * z),
        2 * (x * z + y * w), 2 * (y * z - x * w),
        1 - 2 * (x * x + y * y)
    )

    /** From accelerometer (gravity, pointing up at rest) + magnetometer, both in phone axes. */
    fun fromAccelMag(a: DoubleArray, m: DoubleArray): Double? {
        var ax = a[0]; var ay = a[1]; var az = a[2]
        val ex = m[0]; val ey = m[1]; val ez = m[2]
        var hx = ey * az - ez * ay
        var hy = ez * ax - ex * az
        var hz = ex * ay - ey * ax
        val nh = sqrt(hx * hx + hy * hy + hz * hz)
        val na = sqrt(ax * ax + ay * ay + az * az)
        if (nh < 0.1 || na < 0.1) return null
        hx /= nh; hy /= nh; hz /= nh
        ax /= na; ay /= na; az /= na
        val my = az * hx - ax * hz
        val mz = ax * hy - ay * hx
        return fromMatrix(hy, my, hz, mz, az)
    }
}

/** GPS course while moving; the compass (aligned to the last GPS course) when stopped. */
class Heading {
    companion object {
        const val LOCK_SPEED_KMH = 4.0
        /** From this speed up the arrow is locked to the GPS course (the way the road runs); below it the compass steers. */
        const val MIN_SPEED_FOR_HEADING = LOCK_SPEED_KMH / 3.6   // m/s
        const val LEARN_SPEED = 2.5             // m/s: above this, GPS calibrates the compass
    }

    /** compass -> GPS correction, learned while riding (mount misalignment). */
    var offset: Double? = null
        private set

    /** @return degrees and where they came from ("gps" / "compass"), or null if unknown. */
    fun pick(speed: Double, bearing: Double?, compass: Double?): Pair<Double, String>? {
        if (compass != null && bearing != null && speed >= LEARN_SPEED) {
            val o = offset
            offset = if (o == null) HeadingMath.norm360(bearing - compass)
            else HeadingMath.norm360(o + 0.2 * HeadingMath.angleDiff(bearing, compass + o))
        }
        if (bearing != null && speed >= MIN_SPEED_FOR_HEADING) {
            return Pair(HeadingMath.norm360(bearing), "gps")
        }
        if (compass != null) {
            return Pair(HeadingMath.norm360(compass + (offset ?: 0.0)), "compass")
        }
        return null
    }
}
