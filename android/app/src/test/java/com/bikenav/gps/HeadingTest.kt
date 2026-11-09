package com.bikenav.gps

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.random.Random

class HeadingTest {
    private fun qz(deg: Double) = doubleArrayOf(0.0, 0.0, sin(Math.toRadians(deg) / 2), cos(Math.toRadians(deg) / 2))
    private fun qx(deg: Double) = doubleArrayOf(sin(Math.toRadians(deg) / 2), 0.0, 0.0, cos(Math.toRadians(deg) / 2))
    private fun mul(a: DoubleArray, c: DoubleArray): DoubleArray {
        val (x1, y1, z1, w1) = a
        val (x2, y2, z2, w2) = c
        return doubleArrayOf(
            w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
            w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2
        )
    }
    private fun head(q: DoubleArray) = HeadingMath.fromQuaternion(q[0], q[1], q[2], q[3])
    private fun near(expected: Double, actual: Double) =
        assertEquals(0.0, HeadingMath.angleDiff(actual, expected), 0.01)

    @Test fun flatPhoneTopEdgeDirection() {
        for (az in listOf(0.0, 90.0, 180.0, 270.0, 40.0)) near(az, head(qz(-az)))
    }

    @Test fun uprightPhoneBackFacesDirection() {
        for (az in listOf(0.0, 90.0, 180.0, 270.0, 40.0)) near(az, head(mul(qz(-az), qx(90.0))))
        near(90.0, head(mul(qz(-90.0), qx(45.0))))      // mount tilted back 45 degrees
    }

    @Test fun accelMagAgreesWithRotationVector() {
        val rnd = Random(7)
        repeat(2000) {
            val v = DoubleArray(4) { rnd.nextDouble(-1.0, 1.0) }
            val n = sqrt(v.sumOf { it * it })
            val (x, y, z, w) = v.map { it / n }
            val r = arrayOf(
                doubleArrayOf(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)),
                doubleArrayOf(2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)),
                doubleArrayOf(2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y))
            )
            fun dev(world: DoubleArray) = DoubleArray(3) { j -> (0..2).sumOf { i -> r[i][j] * world[i] } }
            val h2 = HeadingMath.fromAccelMag(dev(doubleArrayOf(0.0, 0.0, 9.81)), dev(doubleArrayOf(0.0, 20.0, -40.0)))
            assertNotNull(h2)
            near(HeadingMath.fromQuaternion(x, y, z, w), h2!!)
        }
    }

    @Test fun mixerUsesGpsWhenMovingAndLearnedCompassWhenStopped() {
        val m = Heading()
        assertNull(m.pick(0.2, null, null))                                  // nothing known
        assertEquals("compass", m.pick(0.2, null, 40.0)!!.second)            // stopped, not learned yet
        val moving = m.pick(5.0, 87.4, 40.0)!!                               // moving: GPS wins and calibrates
        assertEquals("gps", moving.second)
        val stopped = m.pick(0.2, 12.0, 40.0)!!                              // stopped: compass + learned offset
        assertEquals("compass", stopped.second)
        assertEquals(87.4, stopped.first, 0.01)
    }

    @Test fun lockToGpsCourseAtFourKmh() {
        val kmh = 1.0 / 3.6                                             // m/s per km/h
        val m = Heading()
        assertEquals("gps", m.pick(10 * kmh, 80.0, 200.0)!!.second)      // riding: the road direction
        assertEquals("gps", m.pick(4.0 * kmh, 80.0, 200.0)!!.second)     // exactly 4 km/h: locked
        assertEquals("compass", m.pick(3.9 * kmh, 80.0, 200.0)!!.second) // below 4: the compass again
        assertEquals("compass", m.pick(0.0, null, 200.0)!!.second)       // stopped
        assertEquals("gps", m.pick(6 * kmh, 85.0, 205.0)!!.second)       // moving again: back to the road
    }

    @Test fun wrapAroundNorth() {
        val m = Heading()
        m.pick(5.0, 350.0, 10.0)                       // offset = -20 -> 340
        assertEquals(350.0, m.pick(0.2, null, 10.0)!!.first, 0.01)
        assertEquals(10.0, HeadingMath.angleDiff(5.0, 355.0), 1e-9)
    }
}
