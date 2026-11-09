package com.bikenav.gps

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.GeomagneticField
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.sqrt

/**
 * Keeps sending the phone's GPS (and optionally compass) to the bike unit while the screen is off.
 * A foreground service with a wake lock: Android lets it keep using location in the background.
 * Same message as termux/bike_gps.py, so the firmware needs nothing new.
 *
 * It also follows the route being ridden ([Ride.active]): it notices when you leave it (3 fixes in a
 * row beyond 60 m) and plans a new one from where you are - with the screen off, unlike the old web
 * page - and it ends the ride on arrival.
 */
class GpsService : Service(), LocationListener, SensorEventListener {
    companion object {
        const val ACTION_STOP = "com.bikenav.gps.STOP"
        const val PREFS = "bike"
        private const val CHANNEL = "bikegps"
        private const val NOTIF_ID = 1
        private const val MAX_GPS_ACCURACY_M = 100f     // a satellite fix worse than this is not trusted
        private const val GPS_RECENT_MS = 15000L        // while GPS is this fresh, the coarse network position is ignored
        private const val COMPASS_MAX_AGE_MS = 2000L
    }

    private val io = Executors.newSingleThreadExecutor()
    private val finder = Executors.newSingleThreadExecutor()
    private val planner = Executors.newSingleThreadExecutor()
    private var matcher: RouteMatcher? = null
    @Volatile private var rerouting = false
    private var lastReroute = 0L
    private var wake: PowerManager.WakeLock? = null
    private var started = false

    @Volatile private var host: String? = null
    @Volatile private var finding = false
    @Volatile private var researching = false      // after a failure: scan instead of trusting the saved address
    @Volatile private var fails = 0
    private var ipHint: String? = null          // the address from the QR code / typing; checked first
    private var useCompass = false

    private val mixer = Heading()
    @Volatile private var compass: Double? = null
    @Volatile private var compassTs = 0L
    private var accel: DoubleArray? = null
    private var mag: DoubleArray? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (started) return START_STICKY
        started = true

        startInForeground("Starting...")

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        ipHint = prefs.getString("ip", "")?.trim()?.ifEmpty { null }
        useCompass = prefs.getBoolean("compass", true)       // on by default; GPS course takes over while moving

        wake = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "bikegps:service").also { it.acquire() }

        ServiceState.running = true
        ServiceState.log("started")

        try {
            val lm = getSystemService(Context.LOCATION_SERVICE) as LocationManager
            var listening = false
            // Satellites are the accurate source but need sky; the network position (Wi-Fi / cell) works
            // indoors and in the first minute, so the map and the unit show you straight away.
            for ((provider, every) in listOf(LocationManager.GPS_PROVIDER to 1000L, LocationManager.NETWORK_PROVIDER to 2000L)) {
                if (!lm.allProviders.contains(provider)) continue
                try {
                    lm.requestLocationUpdates(provider, every, 0f, this, Looper.getMainLooper())
                    listening = true
                } catch (e: IllegalArgumentException) {
                    // provider vanished
                }
            }
            ServiceState.locationOff = !(lm.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
                (lm.allProviders.contains(LocationManager.NETWORK_PROVIDER) && lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER)))
            if (!listening) {
                ServiceState.log("this phone offers no location source")
                stopSelf()
                return START_NOT_STICKY
            }
            if (ServiceState.locationOff) ServiceState.log("Location is turned OFF in the phone settings - turn it on")
            // Show the last known position at once (if it is recent) instead of waiting for the first fix.
            for (provider in listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)) {
                val last = try { lm.getLastKnownLocation(provider) } catch (e: Exception) { null }
                if (last != null && System.currentTimeMillis() - last.time < 5 * 60_000L) {
                    onLocationChanged(last)
                    break
                }
            }
        } catch (e: SecurityException) {
            ServiceState.log("no location permission")
            stopSelf()
            return START_NOT_STICKY
        } catch (e: Exception) {
            ServiceState.log("GPS unavailable: ${e.message}")
            stopSelf()
            return START_NOT_STICKY
        }

        if (useCompass) startCompass()
        findBoard()
        return START_STICKY
    }

    override fun onDestroy() {
        try {
            (getSystemService(Context.LOCATION_SERVICE) as LocationManager).removeUpdates(this)
            (getSystemService(Context.SENSOR_SERVICE) as SensorManager).unregisterListener(this)
        } catch (_: Exception) {
        }
        wake?.let { if (it.isHeld) it.release() }
        val h = host
        if (Ride.active != null && h != null) {                // leaving while riding: the unit goes back to idle
            Ride.active = null
            Thread { Board.post(h, "/api/clear", "{}") }.start()
        }
        io.shutdownNow()
        finder.shutdownNow()
        planner.shutdownNow()
        ServiceState.running = false
        ServiceState.board = null
        ServiceState.log("stopped")
        super.onDestroy()
    }

    // ---------------------------------------------------------------- notification

    private fun startInForeground(text: String) {
        if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Bike GPS", NotificationManager.IMPORTANCE_LOW))
        }
        val n = buildNotification(text)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(
            this, 1, Intent(this, GpsService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL) else Notification.Builder(this)
        return b.setContentTitle("Bike GPS")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_mylocation)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIF_ID, buildNotification(text))
    }

    // ---------------------------------------------------------------- finding the unit

    private fun findBoard() {
        if (finding) return
        host = null
        ServiceState.board = null
        finding = true
        finder.execute {
            try {
                val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
                var rounds = 0
                var trustHint = ipHint != null && !researching
                while (ServiceState.running && host == null) {
                    // The address from the QR code / typing is right: use it at once (older firmware has
                    // no /api/ping, and scanning the whole hotspot takes many seconds). Only when sending
                    // to it fails do we search again.
                    var h = if (trustHint) ipHint else Board.find(ipHint ?: prefs.getString("last", null))
                    trustHint = false
                    if (h != null) {
                        prefs.edit().putString("last", h).apply()
                        fails = 0
                        host = h
                        ServiceState.board = h
                        ServiceState.log("bike unit at $h")
                        updateNotification("Sending GPS to $h")
                    } else {
                        if (rounds == 0) {
                            ServiceState.log("looking for the bike unit... (hotspot on? unit connected?)")
                            updateNotification("Looking for the bike unit...")
                        }
                        rounds++
                        Thread.sleep(3000)
                    }
                }
            } catch (_: InterruptedException) {
            } finally {
                finding = false
                ServiceState.notifyChange()
            }
        }
    }

    // ---------------------------------------------------------------- GPS

    @Volatile private var lastGpsMs = 0L
    @Volatile private var lastLoc: Location? = null
    @Volatile private var lastSpeed = 0.0                // m/s, from the last fix
    @Volatile private var declination = 0f               // magnetic north -> true north, from the position
    private var lastPush = 0L
    private var lastBoardPush = 0L
    private var lastPushedDeg: Double? = null

    override fun onLocationChanged(loc: Location) {
        val now = System.currentTimeMillis()
        val isGps = loc.provider == LocationManager.GPS_PROVIDER
        if (isGps) {
            if (loc.hasAccuracy() && loc.accuracy > MAX_GPS_ACCURACY_M) return
            lastGpsMs = now
        } else if (now - lastGpsMs < GPS_RECENT_MS) {
            return                                   // a real GPS fix is flowing: ignore the coarse one
        }
        ServiceState.fixAt = now
        ServiceState.fixDesc = String.format(java.util.Locale.US, "%s%s",
            if (isGps) "gps" else "network (rough)",
            if (loc.hasAccuracy()) ", +-${Math.round(loc.accuracy)} m" else "")

        lastLoc = loc
        try {
            declination = GeomagneticField(loc.latitude.toFloat(), loc.longitude.toFloat(), loc.altitude.toFloat(), now).declination
        } catch (_: Exception) {
        }
        val speed = if (loc.hasSpeed()) loc.speed.toDouble() else 0.0       // m/s
        lastSpeed = speed
        val bearing = if (loc.hasBearing()) loc.bearing.toDouble() else null
        val c = if (useCompass && System.currentTimeMillis() - compassTs < COMPASS_MAX_AGE_MS) compass else null
        val dir = mixer.pick(speed, bearing, c)

        val kmh = Math.round(speed * 36.0) / 10.0

        val gcj = Geo.wgs84ToGcj02(loc.latitude, loc.longitude)
        ServiceState.lastGcj = gcj
        val fix = ServiceState.FixInfo(gcj.lng, gcj.lat, dir?.first, Math.round(speed * 36.0) / 10.0)
        ServiceState.lastFix = fix
        ServiceState.notifyFix(fix)
        followRoute(gcj)

        ServiceState.fix = String.format(java.util.Locale.US, "%.5f, %.5f   %.0f km/h   %s",
            loc.latitude, loc.longitude, kmh,
            if (dir != null) "${Math.round(dir.first)}° (${dir.second})" else "no direction")
        ServiceState.notifyChange()
        postFix(loc, kmh, dir?.first)
    }

    /** One position message for the unit. */
    private fun postFix(loc: Location, kmh: Double, heading: Double?) {
        val json = JSONObject()
            .put("lat", loc.latitude)
            .put("lon", loc.longitude)
            .put("wgs", true)                  // raw GPS: the unit converts it to AMap coordinates
            .put("src", "t")                   // background feeder: the unit then ignores the web page's GPS
            .put("speed", kmh)
        if (heading != null) json.put("heading", Math.round(heading).toInt() % 360)
        send(json.toString())
    }

    /** Progress, off-route re-planning and arrival for the route being ridden. */
    private fun followRoute(p: LngLat) {
        val route = Ride.active
        if (route == null) { matcher = null; return }
        var m = matcher
        if (m == null || m.route !== route) { m = RouteMatcher(route); matcher = m }
        val r = m.update(p)

        if (r.arrived) {
            Ride.active = null
            matcher = null
            ServiceState.log("arrived: ${route.destName}")
            host?.let { h -> planner.execute { Board.post(h, "/api/clear", "{}") } }
            updateNotification("Arrived")
            return
        }
        if (r.offRouteConfirmed) reroute(p, route)
    }

    private fun reroute(from: LngLat, route: Route) {
        val key = getSharedPreferences(PREFS, MODE_PRIVATE).getString("amap_key", "")?.trim().orEmpty()
        if (rerouting || key.isEmpty() || ServiceState.pageVisible ||     // page on screen: it re-plans itself
             System.currentTimeMillis() - lastReroute < 15000) return
        rerouting = true
        ServiceState.log("off route: planning a new one")
        planner.execute {
            try {
                val nr = Amap(key).bicycling(from, route.destPos, route.destName)
                host?.let { h ->
                    if (!Board.post(h, "/api/route", nr.toDeviceJson())) throw RuntimeException("the unit did not accept the new route")
                }
                Ride.active = nr
                ServiceState.log("new route: ${nr.distanceM.toInt()} m")
            } catch (e: Exception) {
                ServiceState.log("re-planning failed: ${e.message}")
            } finally {
                lastReroute = System.currentTimeMillis()
                rerouting = false
            }
        }
    }

    private fun send(body: String) {
        val h = host ?: return
        try {
            io.execute {
                try {
                    val c = URL("http://$h/api/gps").openConnection() as HttpURLConnection
                    c.requestMethod = "POST"
                    c.doOutput = true
                    c.connectTimeout = 1500
                    c.readTimeout = 2000
                    c.setRequestProperty("Content-Type", "application/json")
                    c.outputStream.use { it.write(body.toByteArray()) }
                    val ok = c.responseCode == 200
                    c.disconnect()
                    if (!ok) throw RuntimeException("HTTP error")
                    fails = 0
                    ServiceState.lastSend = "OK"
                } catch (e: Exception) {
                    fails++
                    ServiceState.lastSend = "failed: ${e.message}"
                    if (fails == 1) ServiceState.log("send failed (${e.message})")
                    if (fails >= 3) {
                        ServiceState.log("lost the bike unit, searching again")
                        researching = true
                        findBoard()
                    }
                }
            }
        } catch (_: Exception) {
            // executor already shut down
        }
    }

    override fun onProviderEnabled(provider: String) {}
    override fun onProviderDisabled(provider: String) {
        ServiceState.log("GPS is turned off in the phone settings")
    }
    @Deprecated("Deprecated in Java")
    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}

    // ---------------------------------------------------------------- compass

    private fun startCompass() {
        val sm = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        val rot = sm.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        if (rot != null) {
            sm.registerListener(this, rot, SensorManager.SENSOR_DELAY_UI)
            ServiceState.log("compass: rotation vector")
            return
        }
        val a = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val m = sm.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)
        if (a != null && m != null) {
            sm.registerListener(this, a, SensorManager.SENSOR_DELAY_UI)
            sm.registerListener(this, m, SensorManager.SENSOR_DELAY_UI)
            ServiceState.log("compass: accelerometer + magnetometer")
        } else {
            useCompass = false
            ServiceState.log("no compass sensor on this phone: GPS direction only")
        }
    }

    private fun setCompass(deg: Double?) {
        if (deg == null) return
        compass = HeadingMath.norm360(deg + declination)     // true north, like the GPS course
        compassTs = System.currentTimeMillis()
        onCompassMoved()
    }

    /**
     * The phone was turned: show the new direction at once (page, and the unit), without waiting for a GPS fix.
     * While moving, the GPS course steers instead, so this stays quiet.
     */
    private fun onCompassMoved() {
        if (!useCompass) return
        val now = System.currentTimeMillis()
        val moving = now - ServiceState.fixAt < 3000 && lastSpeed >= Heading.MIN_SPEED_FOR_HEADING
        if (moving || now - lastPush < 100) return
        val c = compass ?: return
        val deg = mixer.pick(0.0, null, c)?.first ?: return
        val prev = lastPushedDeg
        if (prev != null && abs(HeadingMath.angleDiff(deg, prev)) < 1.5) return
        lastPush = now
        lastPushedDeg = deg
        ServiceState.notifyHeading(deg)
        val loc = lastLoc
        if (loc != null && now - lastBoardPush >= 250) {      // same position, new direction (at most 4 per second)
            lastBoardPush = now
            postFix(loc, 0.0, deg)
        }
    }

    override fun onSensorChanged(e: SensorEvent) {
        val v = e.values
        when (e.sensor.type) {
            Sensor.TYPE_ROTATION_VECTOR -> {
                val x = v[0].toDouble(); val y = v[1].toDouble(); val z = v[2].toDouble()
                val w = if (v.size > 3) v[3].toDouble() else sqrt(max(0.0, 1 - x * x - y * y - z * z))
                setCompass(HeadingMath.fromQuaternion(x, y, z, w))
            }
            Sensor.TYPE_ACCELEROMETER -> {
                accel = doubleArrayOf(v[0].toDouble(), v[1].toDouble(), v[2].toDouble())
                val m = mag
                if (m != null) setCompass(HeadingMath.fromAccelMag(accel!!, m))
            }
            Sensor.TYPE_MAGNETIC_FIELD -> {
                mag = doubleArrayOf(v[0].toDouble(), v[1].toDouble(), v[2].toDouble())
                val a = accel
                if (a != null) setCompass(HeadingMath.fromAccelMag(a, mag!!))
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}
