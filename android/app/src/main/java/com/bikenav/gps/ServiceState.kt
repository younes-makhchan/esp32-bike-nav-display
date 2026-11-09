package com.bikenav.gps

import android.os.Handler
import android.os.Looper

/** What the background service tells the screen. Both live in one process. */
object ServiceState {
    @Volatile var running = false
    @Volatile var board: String? = null
    @Volatile var fix: String = ""
    @Volatile var fixAt = 0L                   // when the last position arrived (ms), 0 = none yet
    @Volatile var fixDesc: String = ""         // "gps, +-8 m" / "network, +-900 m"
    @Volatile var lastSend: String = ""        // result of the last message to the unit
    @Volatile var locationOff = false          // the phone's own Location switch is off
    @Volatile var lastGcj: LngLat? = null      // last position in AMap coordinates (search + routing start here)
    @Volatile var onChange: (() -> Unit)? = null

    /** A position for the map page: AMap coordinates, direction (null = unknown), speed. */
    class FixInfo(val lng: Double, val lat: Double, val heading: Double?, val kmh: Double)
    @Volatile var lastFix: FixInfo? = null
    @Volatile var onFix: ((FixInfo) -> Unit)? = null
    @Volatile var pageVisible = false          // the map page is on screen (it re-plans routes itself then)

    @Volatile var lastHeading: Double? = null
    @Volatile var onHeading: ((Double) -> Unit)? = null

    fun notifyHeading(deg: Double) {
        lastHeading = deg
        main.post { onHeading?.invoke(deg) }
    }

    fun notifyFix(f: FixInfo) {
        main.post { onFix?.invoke(f) }
    }

    private val main = Handler(Looper.getMainLooper())
    private val lines = ArrayDeque<String>()

    fun notifyChange() {
        main.post { onChange?.invoke() }
    }

    @Synchronized
    fun log(msg: String) {
        val t = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US).format(java.util.Date())
        lines.addLast("$t  $msg")
        while (lines.size > 12) lines.removeFirst()
        notifyChange()
    }

    @Synchronized
    fun logText(): String = lines.joinToString("\n")
}
