package com.bikenav.gps

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.text.InputType
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.webkit.ConsoleMessage
import android.webkit.JavascriptInterface
import android.webkit.JsResult
import android.webkit.WebChromeClient
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.Executors

/**
 * The map page (search, route preview, start riding) shown full screen, plus:
 *  - a first-run checklist: connect the bike unit (scan its QR code or type its IP), location, background running;
 *  - a settings dialog (gear next to the search bar) holding the AMap keys and the unit's address.
 * GPS runs in GpsService, so it keeps going with the screen off; the page only draws.
 */
class MainActivity : Activity() {
    companion object {
        private const val REQ_PERM = 1
        private const val REQ_SCAN = 2
    }

    private lateinit var prefs: SharedPreferences
    private lateinit var web: WebView
    private lateinit var setup: View
    private lateinit var banner: TextView
    private val worker = Executors.newFixedThreadPool(2)
    private var pageReady = false
    private var unitOk = false

    private lateinit var unitStatus: TextView
    private lateinit var locStatus: TextView
    private lateinit var locButton: Button
    private lateinit var bgStatus: TextView
    private lateinit var bgButton: Button
    private lateinit var bgSkip: Button

    private fun dp(v: Int) = TypedValue.applyDimension(
        TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    private fun label(size: Float, color: Int = Color.WHITE, bold: Boolean = false) = TextView(this).apply {
        setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
        setTextColor(color)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }

    private fun button(text: String, onClick: () -> Unit) = Button(this).apply {
        this.text = text
        setOnClickListener { onClick() }
    }

    private fun card(parent: LinearLayout, title: String): LinearLayout {
        val c = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(14), dp(16), dp(14))
            background = GradientDrawable().apply { setColor(0xFF1E293B.toInt()); cornerRadius = dp(14).toFloat() }
        }
        c.addView(label(17f, bold = true).apply { text = title })
        parent.addView(c, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(12) })
        return c
    }

    // ----------------------------------------------------------------- screen

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences(GpsService.PREFS, MODE_PRIVATE)

        web = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
            addJavascriptInterface(Bridge(), "Android")
            webViewClient = WebViewClient()
            webChromeClient = object : WebChromeClient() {
                override fun onJsAlert(view: WebView, url: String, message: String, result: JsResult): Boolean {
                    AlertDialog.Builder(this@MainActivity).setMessage(message)
                        .setPositiveButton("OK") { _, _ -> result.confirm() }
                        .setOnCancelListener { result.cancel() }.show()
                    return true
                }
                override fun onConsoleMessage(m: ConsoleMessage): Boolean {
                    Log.d("BikePage", "${m.message()} (${m.sourceId()}:${m.lineNumber()})")
                    return true
                }
            }
        }
        WebView.setWebContentsDebuggingEnabled(true)        // chrome://inspect shows the page when debugging

        banner = label(14f, Color.WHITE, true).apply {
            text = "Bike unit not reachable. Is the hotspot on and the unit connected?  Tap to change."
            setBackgroundColor(0xEEB45309.toInt())
            setPadding(dp(16), dp(12), dp(16), dp(12))
            visibility = View.GONE
            setOnClickListener { showSettings() }
        }
        setup = buildSetup()

        val root = FrameLayout(this)
        root.addView(web, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(banner, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM))
        root.addView(setup, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(root)
        loadPage()
    }

    private fun loadPage() {
        pageReady = false
        val html = assets.open("index.html").bufferedReader().use { it.readText() }
        // An https base URL gives the page a secure origin, like a normal website.
        web.loadDataWithBaseURL("https://appassets.local/", html, "text/html", "UTF-8", null)
    }

    /** First-run checklist, shown until the unit, location and background running are set up. */
    private fun buildSetup(): View {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(40), dp(20), dp(24))
        }
        col.addView(label(28f, bold = true).apply { text = "Bike GPS" })
        col.addView(label(15f, 0xFF94A3B8.toInt()).apply {
            text = "Set up once, then ride with the phone locked."
            setPadding(0, dp(4), 0, 0)
        })

        val unit = card(col, "1.  Give me the bike unit")
        unitStatus = label(14f, 0xFF94A3B8.toInt()).apply { setPadding(0, dp(6), 0, dp(6)) }
        unit.addView(unitStatus)
        val row = LinearLayout(this)
        row.addView(button("Scan QR code") { scanQr() }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(button("Type IP") { typeIp() }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        unit.addView(row)

        val loc = card(col, "2.  Location")
        locStatus = label(14f, 0xFF94A3B8.toInt()).apply { setPadding(0, dp(6), 0, dp(6)) }
        loc.addView(locStatus)
        locButton = button("Allow location") { requestPermissionsThen { refreshSetup() } }
        loc.addView(locButton)

        val bg = card(col, "3.  Run in the background")
        bgStatus = label(14f, 0xFF94A3B8.toInt()).apply { setPadding(0, dp(6), 0, dp(6)) }
        bg.addView(bgStatus)
        bgButton = button("Allow background running") { openBatterySettings() }
        bg.addView(bgButton)
        bgSkip = button("Skip") { prefs.edit().putBoolean("bg_skipped", true).apply(); refreshSetup() }
        bg.addView(bgSkip)

        return ScrollView(this).apply {
            setBackgroundColor(0xFF0F172A.toInt())
            isClickable = true                                  // do not let touches through to the page
            addView(col)
        }
    }

    // ----------------------------------------------------------------- lifecycle

    override fun onStart() {
        super.onStart()
        ServiceState.onChange = { render() }
        ServiceState.onFix = { pushFix(it) }
        ServiceState.onHeading = { pushHeading(it) }
        ServiceState.pageVisible = true
        web.onResume()
    }

    override fun onResume() {
        super.onResume()
        refreshSetup()
        checkUnit()
    }

    override fun onStop() {
        ServiceState.onChange = null
        ServiceState.onFix = null
        ServiceState.onHeading = null
        ServiceState.pageVisible = false
        web.onPause()                                           // the page sleeps; the service carries on
        super.onStop()
    }

    override fun onDestroy() {
        worker.shutdownNow()
        web.destroy()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (setup.visibility == View.VISIBLE) { moveTaskToBack(true); return }
        // Back closes the route preview; otherwise it just hides the app (never ends a ride).
        web.evaluateJavascript(
            "(function(){ if (typeof state !== 'undefined' && state.mode === 'preview') { cancelPreview(); return 'y'; } return 'n'; })()"
        ) { r -> if (r != "\"y\"") moveTaskToBack(true) }
    }

    // ----------------------------------------------------------------- state

    private fun isIgnoringBattery(): Boolean =
        (getSystemService(Context.POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(packageName)

    private fun locationGranted() =
        checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    private fun refreshSetup() {
        val ip = prefs.getString("ip", "").orEmpty()
        val ipSet = ip.isNotEmpty()
        val locOk = locationGranted()
        val ignoring = isIgnoringBattery()
        val bgOk = ignoring || prefs.getBoolean("bg_skipped", false)

        unitStatus.text = if (ipSet) "Connected to $ip"
        else "Not connected yet. Open the navigation app on the unit and scan its QR code, or type its IP address."
        unitStatus.setTextColor(if (ipSet) 0xFF4ADE80.toInt() else 0xFF94A3B8.toInt())

        locStatus.text = if (locOk) "Allowed" else "Needed to know where you are (also asks to show the 'running' notification)."
        locStatus.setTextColor(if (locOk) 0xFF4ADE80.toInt() else 0xFF94A3B8.toInt())
        locButton.visibility = if (locOk) View.GONE else View.VISIBLE

        bgStatus.text = when {
            ignoring -> "Allowed"
            bgOk -> "Skipped. Android may stop the app when the screen is off."
            else -> "Lets the app keep sending GPS with the screen off. Tap Allow, then choose \"Allow\"."
        }
        bgStatus.setTextColor(if (bgOk) 0xFF4ADE80.toInt() else 0xFF94A3B8.toInt())
        bgButton.visibility = if (ignoring) View.GONE else View.VISIBLE
        bgSkip.visibility = if (bgOk) View.GONE else View.VISIBLE

        val all = ipSet && locOk && bgOk
        setup.visibility = if (all) View.GONE else View.VISIBLE
        if (all && !ServiceState.running) startGps()
        render()
    }

    private fun render() {
        val ip = prefs.getString("ip", "").orEmpty()
        val reachable = unitOk || ServiceState.board != null
        banner.visibility = if (setup.visibility != View.VISIBLE && ip.isNotEmpty() && !reachable) View.VISIBLE else View.GONE
        if (pageReady) {
            val txt = when {
                !ServiceState.running -> "GPS 未开启"
                ServiceState.locationOff -> "手机定位已关闭"
                ServiceState.fixAt == 0L -> "等待定位…"
                !reachable -> "手表离线"
                else -> "手表已连接"
            }
            web.evaluateJavascript("window.nativeStatus && nativeStatus(${JSONObject.quote(txt)})", null)
        }
    }

    private fun pushFix(f: ServiceState.FixInfo) {
        if (!pageReady) return
        val h = f.heading?.let { String.format(Locale.US, "%.0f", it) } ?: "null"
        web.evaluateJavascript(String.format(Locale.US, "window.nativeFix && nativeFix(%.6f,%.6f,%s,%.1f)",
            f.lng, f.lat, h, f.kmh), null)
    }

    private fun pushHeading(deg: Double) {
        if (!pageReady) return
        web.evaluateJavascript(String.format(Locale.US, "window.nativeHeading && nativeHeading(%.0f)", deg), null)
    }

    private fun toastInPage(msg: String) {
        runOnUiThread { web.evaluateJavascript("toast(${JSONObject.quote(msg)}, 2500)", null) }
    }

    // ----------------------------------------------------------------- the bike unit

    private fun checkUnit() {
        val ip = prefs.getString("ip", "").orEmpty()
        if (ip.isEmpty()) { unitOk = false; return }
        worker.execute {
            val ok = Board.ping(ip)
            runOnUiThread { unitOk = ok; render() }
        }
    }

    private fun scanQr() {
        startActivityForResult(Intent(this, ScanActivity::class.java), REQ_SCAN)
    }

    private fun typeIp() {
        val input = EditText(this).apply {
            setText(prefs.getString("ip", ""))
            hint = "e.g. 192.168.43.57"
            inputType = InputType.TYPE_CLASS_TEXT
            setSingleLine()
        }
        AlertDialog.Builder(this)
            .setTitle("Bike unit IP")
            .setView(input)
            .setPositiveButton("OK") { _, _ -> setUnitIp(input.text.toString().trim()) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun setUnitIp(ip: String) {
        prefs.edit().putString("ip", ip).apply()
        unitOk = false
        refreshSetup()
        checkUnit()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_SCAN) return
        when (resultCode) {
            RESULT_OK -> data?.getStringExtra("ip")?.let { setUnitIp(it) }
            RESULT_FIRST_USER -> typeIp()                       // "Type the IP instead" in the scanner
        }
    }

    // ----------------------------------------------------------------- permissions and GPS

    private fun missingPermissions(): Array<String> {
        val want = ArrayList<String>()
        want.add(Manifest.permission.ACCESS_FINE_LOCATION)
        want.add(Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) want.add(Manifest.permission.POST_NOTIFICATIONS)
        return want.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }.toTypedArray()
    }

    private var afterPermission: (() -> Unit)? = null

    private fun requestPermissionsThen(then: () -> Unit) {
        val missing = missingPermissions()
        if (missing.isEmpty()) { then(); return }
        afterPermission = then
        requestPermissions(missing, REQ_PERM)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERM) return
        afterPermission?.invoke()
        afterPermission = null
    }

    private fun startGps() {
        if (!locationGranted()) return
        startForegroundService(Intent(this, GpsService::class.java))
    }

    private fun stopGps() {
        stopService(Intent(this, GpsService::class.java))
    }

    private fun openBatterySettings() {
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        } catch (e: Exception) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }

    // ----------------------------------------------------------------- settings (the gear)

    private fun field(parent: LinearLayout, title: String, value: String, hint: String): EditText {
        parent.addView(label(13f, 0xFF64748B.toInt()).apply { text = title; setPadding(0, dp(12), 0, 0) })
        val e = EditText(this).apply {
            setText(value)
            this.hint = hint
            setSingleLine()
            inputType = InputType.TYPE_CLASS_TEXT
        }
        parent.addView(e)
        return e
    }

    /** What is going on, in plain words: where a "nothing happens" comes from. */
    private fun statusReport(): String {
        val sb = StringBuilder()
        sb.append("GPS service:  ").append(if (ServiceState.running) "running" else "stopped").append('\n')
        val age = if (ServiceState.fixAt == 0L) null else (System.currentTimeMillis() - ServiceState.fixAt) / 1000
        sb.append("Position:     ").append(
            when {
                ServiceState.locationOff -> "Location is OFF in the phone settings"
                age == null -> "none yet - go outside or near a window"
                else -> "${ServiceState.fixDesc}, ${age}s ago"
            }).append('\n')
        val ip = prefs.getString("ip", "").orEmpty()
        sb.append("Bike unit:    ").append(if (ip.isEmpty()) "not set" else ip)
        if (ServiceState.lastSend.isNotEmpty()) sb.append("  (last send: ").append(ServiceState.lastSend).append(')')
        val log = ServiceState.logText()
        if (log.isNotEmpty()) sb.append("\n\n").append(log)
        return sb.toString()
    }

    private fun showSettings() {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), dp(8))
        }
        val jsKey = field(col, "AMap Web端 key (map, search, routes)", prefs.getString("js_key", "").orEmpty(), "built-in key")
        val jsSec = field(col, "AMap security code (belongs to that key)", prefs.getString("js_sec", "").orEmpty(), "built-in code")
        val restKey = field(col, "AMap Web服务 key (optional: re-plan the route while the screen is off)",
            prefs.getString("amap_key", "").orEmpty(), "")

        val ip = prefs.getString("ip", "").orEmpty()
        col.addView(label(13f, 0xFF64748B.toInt()).apply { text = "Bike unit"; setPadding(0, dp(16), 0, 0) })
        col.addView(TextView(this).apply { text = if (ip.isEmpty()) "not set" else ip })
        val row = LinearLayout(this)
        val dialogHolder = arrayOfNulls<AlertDialog>(1)
        row.addView(button("Scan QR code") { dialogHolder[0]?.dismiss(); scanQr() }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(button("Type IP") { dialogHolder[0]?.dismiss(); typeIp() }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        col.addView(row)

        col.addView(label(13f, 0xFF64748B.toInt()).apply { text = "Status"; setPadding(0, dp(16), 0, 0) })
        col.addView(label(12f, 0xFFCBD5E1.toInt()).apply {
            typeface = Typeface.MONOSPACE
            text = statusReport()
        })

        val compass = CheckBox(this).apply {
            text = "Use the compass when slower than 4 km/h (phone must face the way the bike points)"
            isChecked = prefs.getBoolean("compass", true)
        }
        col.addView(compass)
        col.addView(button(if (ServiceState.running) "Stop sharing GPS" else "Start sharing GPS") {
            dialogHolder[0]?.dismiss()
            if (ServiceState.running) stopGps() else startGps()
        })

        dialogHolder[0] = AlertDialog.Builder(this)
            .setTitle("Settings")
            .setView(ScrollView(this).apply { addView(col) })
            .setPositiveButton("Save") { _, _ ->
                val changedPage = jsKey.text.toString().trim() != prefs.getString("js_key", "").orEmpty() ||
                    jsSec.text.toString().trim() != prefs.getString("js_sec", "").orEmpty()
                val compassChanged = compass.isChecked != prefs.getBoolean("compass", true)
                prefs.edit()
                    .putString("js_key", jsKey.text.toString().trim())
                    .putString("js_sec", jsSec.text.toString().trim())
                    .putString("amap_key", restKey.text.toString().trim())
                    .putBoolean("compass", compass.isChecked)
                    .apply()
                if (compassChanged && ServiceState.running) { stopGps(); startGps() }   // the service reads it at start
                if (changedPage) loadPage()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    // ----------------------------------------------------------------- the page calls these

    private inner class Bridge {
        @JavascriptInterface
        fun getConfig(): String = JSONObject()
            .put("key", prefs.getString("js_key", "").orEmpty())
            .put("sec", prefs.getString("js_sec", "").orEmpty())
            .toString()

        @JavascriptInterface
        fun pageReady() {
            runOnUiThread {
                pageReady = true
                render()
                ServiceState.lastFix?.let { pushFix(it) }
                ServiceState.lastHeading?.let { pushHeading(it) }
            }
        }

        @JavascriptInterface
        fun openSettings() { runOnUiThread { showSettings() } }

        private val simBusy = java.util.concurrent.atomic.AtomicBoolean(false)

        /** One position of a simulated ride: relayed to the unit. If the rider stopped the simulation on the unit,
         *  its reply says so and the page stops too. A fix is dropped while the previous one is still on the way. */
        @JavascriptInterface
        fun simFix(json: String) {
            val host = ServiceState.board ?: prefs.getString("ip", "").orEmpty()
            if (host.isEmpty() || !simBusy.compareAndSet(false, true)) return
            worker.execute {
                try {
                    val reply = Http.postRead("http://$host/api/gps", json)
                    if (reply != null && reply.contains("\"stop\":true"))
                        runOnUiThread { web.evaluateJavascript("window.nativeSimStop && nativeSimStop()", null) }
                } catch (_: Exception) {
                } finally {
                    simBusy.set(false)
                }
            }
        }

        /** "Start riding" (and a re-plan while riding): send the route to the unit and follow it. */
        @JavascriptInterface
        fun startRide(json: String) {
            worker.execute {
                val route = try { routeFromPageJson(JSONObject(json)) } catch (e: Exception) {
                    toastInPage("路线数据有误"); return@execute
                }
                val host = ServiceState.board ?: prefs.getString("ip", "").orEmpty()
                if (host.isEmpty()) { toastInPage("手表未连接"); return@execute }
                if (Board.post(host, "/api/route", route.toDeviceJson())) {
                    Ride.active = route
                    toastInPage("路线已同步至手表 ⌚")
                } else {
                    toastInPage("手表同步失败，请检查热点和手表连接")
                }
            }
        }

        @JavascriptInterface
        fun endRide() {
            val host = ServiceState.board ?: prefs.getString("ip", "").orEmpty()
            Ride.active = null
            if (host.isNotEmpty()) worker.execute { Board.post(host, "/api/clear", "{}") }
        }
    }
}
