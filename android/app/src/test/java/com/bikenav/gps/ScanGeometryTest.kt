package com.bikenav.gps

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ScanGeometryTest {
    @Test fun previewCoversTheScreenWithoutStretching() {
        // 1280x720 sensor image on a tall 1080x2400 phone screen
        val (w, h) = ScanGeometry.viewSizeCovering(1280, 720, 1080, 2400)
        assertTrue(w >= 1080 && h >= 2400)
        assertEquals(720.0 / 1280.0, w.toDouble() / h, 0.002)           // same shape as the camera image
        // a wide, short screen is covered too
        val (w2, h2) = ScanGeometry.viewSizeCovering(1280, 720, 1080, 1200)
        assertTrue(w2 >= 1080 && h2 >= 1200)
    }

    @Test fun cropIsTheCentreSquareUnderTheFrame() {
        val c = ScanGeometry.cropForFrame(1280, 720, 1080, 2400, 0.7f)
        assertTrue(c.size in 300..720)
        assertEquals((1280 - c.size) / 2, c.left)                        // centred both ways
        assertEquals((720 - c.size) / 2, c.top)
        // on screen the crop is as big as the frame: size * scale = 0.7 * screen width
        val (viewW, _) = ScanGeometry.viewSizeCovering(1280, 720, 1080, 2400)
        assertEquals(0.7 * 1080, c.size * viewW.toDouble() / 720, 3.0)
    }

    @Test fun cropNeverExceedsTheImage() {
        val c = ScanGeometry.cropForFrame(640, 480, 720, 1280, 0.95f)
        assertTrue(c.size <= 480 && c.left >= 0 && c.top >= 0)
    }
}
