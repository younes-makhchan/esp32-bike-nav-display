package com.bikenav.gps

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.SurfaceTexture
import android.graphics.drawable.GradientDrawable
import android.hardware.Camera
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.Gravity
import android.view.Surface
import android.view.TextureView
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.MultiFormatReader
import com.google.zxing.NotFoundException
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.common.GlobalHistogramBinarizer
import com.google.zxing.common.HybridBinarizer
import java.util.concurrent.Executors

/**
 * Scans the QR code on the bike unit and returns its IP address as the "ip" extra.
 * Result codes: RESULT_OK (found), RESULT_CANCELED (back), RESULT_FIRST_USER ("Type IP instead").
 */
@Suppress("DEPRECATION")
class ScanActivity : Activity(), TextureView.SurfaceTextureListener, Camera.PreviewCallback {
    private lateinit var root: FrameLayout
    private lateinit var preview: TextureView
    private lateinit var finder: ViewfinderView
    private lateinit var hint: TextView
    private var camera: Camera? = null
    private var texture: SurfaceTexture? = null
    private var bufW = 0
    private var bufH = 0
    private var crop: ScanGeometry.Crop? = null
    private var busy = false
    private var done = false
    private var misses = 0
    private val ui = Handler(Looper.getMainLooper())
    private val decoder = Executors.newSingleThreadExecutor()
    private val reader = MultiFormatReader().apply {
        setHints(mapOf(
            DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE),
            DecodeHintType.TRY_HARDER to true))
    }

    /** Phones that focus once and stay blurry: ask for a fresh focus every couple of seconds. */
    private val refocus = object : Runnable {
        override fun run() {
            if (done) return
            try { camera?.autoFocus(null) } catch (_: Exception) {}
            ui.postDelayed(this, 2500)
        }
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        preview = TextureView(this).apply {
            surfaceTextureListener = this@ScanActivity
            setOnClickListener { try { camera?.autoFocus(null) } catch (_: Exception) {} }   // tap to focus
        }
        root.addView(preview, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        finder = ViewfinderView(this)
        root.addView(finder, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        val bottom = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dp(24), 0, dp(24), dp(32))
        }
        hint = TextView(this).apply {
            text = "Show the QR code on the bike unit inside the frame"
            setTextColor(Color.WHITE)
            textSize = 17f
            gravity = Gravity.CENTER
        }
        bottom.addView(hint)
        bottom.addView(TextView(this).apply {
            text = "Hold it about 15-25 cm away. Tap the screen to focus."
            setTextColor(0xFFCBD5E1.toInt())
            textSize = 13f
            gravity = Gravity.CENTER
            setPadding(0, dp(6), 0, dp(16))
        })
        bottom.addView(Button(this).apply {
            text = "Type the IP instead"
            setOnClickListener { setResult(RESULT_FIRST_USER); finish() }
        })
        root.addView(bottom, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM))
        setContentView(root)

        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) openCamera()
        else hint.text = "The camera permission is needed to scan. You can type the IP instead."
    }

    // ----------------------------------------------------------------- camera

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) { texture = st; openCamera() }
    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) {}
    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { texture = null; closeCamera(); return true }

    private fun openCamera() {
        val st = texture ?: return
        if (camera != null || done) return
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return
        try {
            val info = Camera.CameraInfo()
            var id = 0
            for (i in 0 until Camera.getNumberOfCameras()) {
                Camera.getCameraInfo(i, info)
                if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) { id = i; break }
            }
            Camera.getCameraInfo(id, info)
            val cam = Camera.open(id)

            val degrees = when (windowManager.defaultDisplay.rotation) {
                Surface.ROTATION_90 -> 90
                Surface.ROTATION_180 -> 180
                Surface.ROTATION_270 -> 270
                else -> 0
            }
            cam.setDisplayOrientation((info.orientation - degrees + 360) % 360)

            val p = cam.parameters
            // Sharp enough for a QR code on a small screen, small enough to decode quickly.
            val size = p.supportedPreviewSizes.filter { it.width * it.height <= 1280 * 720 }
                .maxByOrNull { it.width * it.height } ?: p.previewSize
            p.setPreviewSize(size.width, size.height)
            if (p.supportedFocusModes?.contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE) == true) {
                p.focusMode = Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE
            }
            cam.parameters = p
            bufW = size.width
            bufH = size.height

            // Show the image undistorted, covering the screen; decode only what is under the frame.
            val sw = if (root.width > 0) root.width else resources.displayMetrics.widthPixels
            val sh = if (root.height > 0) root.height else resources.displayMetrics.heightPixels
            val (vw, vh) = ScanGeometry.viewSizeCovering(bufW, bufH, sw, sh)
            preview.layoutParams = FrameLayout.LayoutParams(vw, vh, Gravity.CENTER)
            crop = ScanGeometry.cropForFrame(bufW, bufH, sw, sh, ViewfinderView.FRACTION)

            cam.setPreviewTexture(st)
            cam.setPreviewCallback(this)
            cam.startPreview()
            camera = cam
            ui.postDelayed(refocus, 1200)
        } catch (e: Exception) {
            hint.text = "Cannot open the camera: ${e.message}. You can type the IP instead."
        }
    }

    private fun closeCamera() {
        ui.removeCallbacks(refocus)
        try {
            camera?.setPreviewCallback(null)
            camera?.stopPreview()
            camera?.release()
        } catch (_: Exception) {
        }
        camera = null
    }

    // ----------------------------------------------------------------- decoding

    private fun decode(src: PlanarYUVLuminanceSource): String? {
        try {
            return reader.decodeWithState(BinaryBitmap(HybridBinarizer(src))).text
        } catch (_: NotFoundException) {
        }
        try {                                               // a different threshold copes with screen glare
            return reader.decodeWithState(BinaryBitmap(GlobalHistogramBinarizer(src))).text
        } catch (_: NotFoundException) {
        }
        return null
    }

    override fun onPreviewFrame(data: ByteArray, cam: Camera) {
        val c = crop ?: return
        if (done || busy) return
        busy = true
        decoder.execute {
            try {
                // Mostly the framed square; every 5th miss the whole image, in case the code is held big.
                val whole = misses % 5 == 4
                val src = if (whole) PlanarYUVLuminanceSource(data, bufW, bufH, 0, 0, bufW, bufH, false)
                else PlanarYUVLuminanceSource(data, bufW, bufH, c.left, c.top, c.size, c.size, false)
                val text = decode(src)
                if (text != null) onText(text) else misses++
            } catch (_: Exception) {
                // nothing readable in this frame
            } finally {
                reader.reset()
                busy = false
            }
        }
    }

    private fun onText(text: String) {
        val ip = Regex("""\b(\d{1,3}(?:\.\d{1,3}){3})(:\d+)?\b""").find(text)?.value
        runOnUiThread {
            if (ip == null) {
                hint.text = "That QR code is not from the bike unit"
            } else if (!done) {
                done = true
                finder.success()
                hint.text = "Bike unit found: $ip"
                buzz()
                ui.postDelayed({
                    setResult(RESULT_OK, Intent().putExtra("ip", ip))
                    finish()
                }, 650)
            }
        }
    }

    private fun buzz() {
        try {
            val v = getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
            if (Build.VERSION.SDK_INT >= 26) v.vibrate(VibrationEffect.createOneShot(60, VibrationEffect.DEFAULT_AMPLITUDE))
            else v.vibrate(60)
        } catch (_: Exception) {
        }
    }

    override fun onPause() {
        closeCamera()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        openCamera()
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        decoder.shutdownNow()
        super.onDestroy()
    }
}
