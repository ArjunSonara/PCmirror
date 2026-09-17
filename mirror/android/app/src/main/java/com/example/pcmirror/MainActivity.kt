package com.example.pcmirror

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.LayoutInflater
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.json.JSONArray
import org.json.JSONObject
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue

data class CustomAspectPreset(
    val id: String,
    val name: String,
    val aspectMode: String,
    val scaleX: Float,
    val scaleY: Float,
    val transX: Float,
    val transY: Float
)

enum class AspectRatioMode(val displayName: String) {
    SAFE_FIT("Safe Fit 16:9"),
    FULL_STRETCH("Full Stretch 20:9"),
    CROP_FILL("Zoom & Crop"),
    PRODUCTIVITY("Productivity 16:10"),
    ULTRAWIDE("Cinematic 21:9")
}

/**
 * PC Mirror Ultra Client
 * Ultra-low latency H.264 hardware mirroring with smooth multi-touch gestures,
 * dynamic resolution/bitrate control, and 5 aspect ratio presets.
 */
class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "PCMirror"
        private const val PORT = 8080
        private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val DEFAULT_WIDTH = 1920
        private const val DEFAULT_HEIGHT = 1080
    }

    // Views
    private lateinit var surfaceView: SurfaceView
    private lateinit var dashboardView: ScrollView
    private lateinit var floatingControls: LinearLayout
    private lateinit var floatingControlsPanel: LinearLayout
    private lateinit var gearMenuBtn: ImageButton
    private lateinit var statusText: TextView
    private lateinit var ipInput: EditText
    private lateinit var autoConnectCheck: CheckBox
    private lateinit var btnStartMirror: Button
    private lateinit var rgAspectRatios: RadioGroup
    private lateinit var rbSafeFit: RadioButton
    private lateinit var rbFullStretch: RadioButton
    private lateinit var rbCropFill: RadioButton
    private lateinit var rbProductivity: RadioButton
    private lateinit var rbUltrawide: RadioButton
    private lateinit var viewportStatusText: TextView
    private lateinit var btnResetAdjustment: Button
    private lateinit var btnQuickZoom125: Button
    private lateinit var spinnerResolution: Spinner
    private lateinit var spinnerBitrate: Spinner
    private lateinit var cardGameMode: LinearLayout

    // Floating in-stream controls
    private lateinit var ratioToggleBtn: Button
    private lateinit var resToggleBtn: Button
    private lateinit var bitrateToggleBtn: Button
    private lateinit var lockScreenBtn: Button
    private lateinit var resetViewBtn: Button
    private lateinit var savePresetBtn: Button
    private lateinit var presetsMenuBtn: Button
    private lateinit var exitStreamBtn: Button
    private var isViewportLocked = false

    // Dashboard preset controls
    private lateinit var btnSavePresetDashboard: Button
    private lateinit var btnOpenPresetsDashboard: Button
    private var layoutSavedPresetsDashboard: LinearLayout? = null

    // Real-time audio streaming state
    private var audioThread: Thread? = null
    private var audioSocket: Socket? = null
    private var audioTrack: AudioTrack? = null

    // Viewport transform state
    private var vScaleX = 1.0f
    private var vScaleY = 1.0f
    private var vTransX = 0f
    private var vTransY = 0f
    private var currentAspectMode = AspectRatioMode.SAFE_FIT

    // Smooth gesture tracking
    private lateinit var scaleDetector: ScaleGestureDetector
    private var isScaling = false
    private var lastFocusX = 0f
    private var lastFocusY = 0f
    private var twoFingerDownTime = 0L

    // Boundary stretch state
    private var lastTapTime = 0L
    private var isEdgeStretching = false
    private var edgeStretchAxis = 0 // 1 = horizontal, 2 = vertical
    private var edgeStretchStartX = 0f
    private var edgeStretchStartY = 0f
    private var baseScaleX = 1.0f
    private var baseScaleY = 1.0f

    // Network & Decode state
    private var activeCodec: MediaCodec? = null
    private var readerThread: Thread? = null
    @Volatile private var running = false
    @Volatile private var surfaceReady = false
    @Volatile private var touchOut: OutputStream? = null
    private var hasExitedToDashboardManually = false

    // Resolution and Bitrate lists
    private val resOptions = listOf(
        Pair(1920, 1080) to "1080p FHD (1920x1080)",
        Pair(3840, 2160) to "4K UHD (3840x2160)",
        Pair(2560, 1440) to "2K QHD (2560x1440)",
        Pair(1600, 900) to "900p HD+ (1600x900)",
        Pair(1280, 720) to "720p HD (1280x720)"
    )
    private var currentResIndex = 0

    private val bitrateOptions = listOf(
        Pair(8000000f, "8 Mbps (Balanced Default)"),
        Pair(50000000f, "50 Mbps (Extreme / Lossless)"),
        Pair(30000000f, "30 Mbps (Ultra Quality)"),
        Pair(20000000f, "20 Mbps (High Quality)"),
        Pair(15000000f, "15 Mbps (Clear)"),
        Pair(4000000f, "4 Mbps (Ultra Performance)")
    )
    private var currentBitrateIndex = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Extend behind notch
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        hideSystemUI()

        setContentView(R.layout.activity_main)

        initViews()
        initGestureDetector()
        initSpinners()
        loadPreferences()
        applyAspectLayout(currentAspectMode)

        surfaceView.holder.setFormat(android.graphics.PixelFormat.OPAQUE)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                Log.i(TAG, "Surface created and ready")
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                Log.i(TAG, "Surface destroyed")
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.i(TAG, "Surface changed: ${width}x${height}")
            }
        })

        // Check for Intent auto-launch IP
        val autoIp = intent.getStringExtra("server_ip")
        if (!autoIp.isNullOrBlank()) {
            ipInput.setText(autoIp)
            startConnection(autoIp)
        } else {
            // Background probe for auto-connection
            checkAndAutoConnect()
        }
    }

    private fun initViews() {
        surfaceView = findViewById(R.id.surfaceView)
        dashboardView = findViewById(R.id.dashboardView)
        floatingControls = findViewById(R.id.floatingControls)
        statusText = findViewById(R.id.statusText)
        ipInput = findViewById(R.id.ipInput)
        autoConnectCheck = findViewById(R.id.autoConnectCheck)
        btnStartMirror = findViewById(R.id.btnStartMirror)

        rgAspectRatios = findViewById(R.id.rgAspectRatios)
        rbSafeFit = findViewById(R.id.rbSafeFit)
        rbFullStretch = findViewById(R.id.rbFullStretch)
        rbCropFill = findViewById(R.id.rbCropFill)
        rbProductivity = findViewById(R.id.rbProductivity)
        rbUltrawide = findViewById(R.id.rbUltrawide)

        viewportStatusText = findViewById(R.id.viewportStatusText)
        btnResetAdjustment = findViewById(R.id.btnResetAdjustment)
        btnQuickZoom125 = findViewById(R.id.btnQuickZoom125)

        spinnerResolution = findViewById(R.id.spinnerResolution)
        spinnerBitrate = findViewById(R.id.spinnerBitrate)
        cardGameMode = findViewById(R.id.cardGameMode)

        ratioToggleBtn = findViewById(R.id.ratioToggleBtn)
        resToggleBtn = findViewById(R.id.resToggleBtn)
        bitrateToggleBtn = findViewById(R.id.bitrateToggleBtn)
        lockScreenBtn = findViewById(R.id.lockScreenBtn)
        resetViewBtn = findViewById(R.id.resetViewBtn)
        savePresetBtn = findViewById(R.id.savePresetBtn)
        presetsMenuBtn = findViewById(R.id.presetsMenuBtn)
        exitStreamBtn = findViewById(R.id.exitStreamBtn)
        floatingControlsPanel = findViewById(R.id.floatingControlsPanel)
        gearMenuBtn = findViewById(R.id.gearMenuBtn)

        btnSavePresetDashboard = findViewById(R.id.btnSavePresetDashboard)
        btnOpenPresetsDashboard = findViewById(R.id.btnOpenPresetsDashboard)
        layoutSavedPresetsDashboard = findViewById(R.id.layoutSavedPresetsDashboard)

        gearMenuBtn.setOnClickListener {
            if (floatingControlsPanel.visibility == View.VISIBLE) {
                // Smooth collapse to the right
                floatingControlsPanel.animate()
                    .alpha(0f)
                    .translationX(40f)
                    .setDuration(180)
                    .withEndAction {
                        floatingControlsPanel.visibility = View.GONE
                    }
                    .start()
                gearMenuBtn.animate().rotationBy(-90f).setDuration(200).start()
            } else {
                // Smooth expand from the right
                floatingControlsPanel.alpha = 0f
                floatingControlsPanel.translationX = 40f
                floatingControlsPanel.visibility = View.VISIBLE
                floatingControlsPanel.animate()
                    .alpha(1f)
                    .translationX(0f)
                    .setDuration(200)
                    .start()
                gearMenuBtn.animate().rotationBy(90f).setDuration(200).start()
            }
        }

        btnStartMirror.setOnClickListener {
            val ip = ipInput.text.toString().trim()
            startConnection(ip)
        }

        btnResetAdjustment.setOnClickListener { resetViewport() }
        btnQuickZoom125.setOnClickListener {
            vScaleX = (vScaleX * 1.25f).coerceIn(0.2f, 5.0f)
            vScaleY = (vScaleY * 1.25f).coerceIn(0.2f, 5.0f)
            applyViewportTransform()
            updateViewportStatus()
            saveViewportPrefs()
            Toast.makeText(this, "Zoomed +25%", Toast.LENGTH_SHORT).show()
        }

        rgAspectRatios.setOnCheckedChangeListener { _, checkedId ->
            val selectedMode = when (checkedId) {
                R.id.rbFullStretch -> AspectRatioMode.FULL_STRETCH
                R.id.rbCropFill -> AspectRatioMode.CROP_FILL
                R.id.rbProductivity -> AspectRatioMode.PRODUCTIVITY
                R.id.rbUltrawide -> AspectRatioMode.ULTRAWIDE
                else -> AspectRatioMode.SAFE_FIT
            }
            applyAspectLayout(selectedMode)
        }

        cardGameMode.setOnClickListener {
            Toast.makeText(this, "Game Mode Coming Soon!\n120Hz direct-touch & fast polling engine will be enabled.", Toast.LENGTH_LONG).show()
        }

        // In-stream floating buttons
        ratioToggleBtn.setOnClickListener {
            val modes = AspectRatioMode.values()
            val next = modes[(currentAspectMode.ordinal + 1) % modes.size]
            applyAspectLayout(next)
            updateRatioRadioSelection(next)
            Toast.makeText(this, "Ratio: ${next.displayName}", Toast.LENGTH_SHORT).show()
        }

        resToggleBtn.setOnClickListener {
            currentResIndex = (currentResIndex + 1) % resOptions.size
            val selected = resOptions[currentResIndex]
            resToggleBtn.text = "Res: ${selected.second.substringBefore(" ")}"
            spinnerResolution.setSelection(currentResIndex)
            sendControlPacket(11.toByte(), selected.first.first.toFloat(), selected.first.second.toFloat())
            Toast.makeText(this, "Resolution: ${selected.second}", Toast.LENGTH_SHORT).show()
        }

        bitrateToggleBtn.setOnClickListener {
            currentBitrateIndex = (currentBitrateIndex + 1) % bitrateOptions.size
            val selected = bitrateOptions[currentBitrateIndex]
            bitrateToggleBtn.text = "Quality: ${selected.second.substringBefore(" ")}"
            spinnerBitrate.setSelection(currentBitrateIndex)
            sendControlPacket(10.toByte(), selected.first, 0.0f)
            Toast.makeText(this, "Bitrate: ${selected.second}", Toast.LENGTH_SHORT).show()
        }

        lockScreenBtn.setOnClickListener {
            isViewportLocked = !isViewportLocked
            if (isViewportLocked) {
                lockScreenBtn.text = "🔒 Lock: ON"
                lockScreenBtn.setBackgroundResource(R.drawable.bg_button_locked)
                Toast.makeText(this, "Screen Locked: 2-finger move/zoom disabled", Toast.LENGTH_SHORT).show()
            } else {
                lockScreenBtn.text = "Lock: OFF"
                lockScreenBtn.setBackgroundResource(R.drawable.bg_button_secondary)
                Toast.makeText(this, "Screen Unlocked: 2-finger zoom/pan enabled", Toast.LENGTH_SHORT).show()
            }
        }

        resetViewBtn.setOnClickListener { resetViewport() }

        savePresetBtn.setOnClickListener { showSavePresetDialog() }
        presetsMenuBtn.setOnClickListener { showPresetsListDialog() }
        btnSavePresetDashboard.setOnClickListener { showSavePresetDialog() }
        btnOpenPresetsDashboard.setOnClickListener { showPresetsListDialog() }

        ratioToggleBtn.setOnLongClickListener {
            showPresetsListDialog()
            true
        }

        exitStreamBtn.setOnClickListener {
            hasExitedToDashboardManually = true
            stopConnection()
            runOnUiThread {
                floatingControls.visibility = View.GONE
                floatingControlsPanel.visibility = View.GONE
                dashboardView.visibility = View.VISIBLE
                statusText.text = "● Stream Disconnected (Dashboard Active)"
            }
        }
    }

    private fun initGestureDetector() {
        scaleDetector = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
                if (isViewportLocked) return false
                isScaling = true
                lastFocusX = detector.focusX
                lastFocusY = detector.focusY
                return true
            }

            override fun onScale(detector: ScaleGestureDetector): Boolean {
                if (isViewportLocked) return true
                val factor = detector.scaleFactor
                if (factor.isFinite() && factor > 0.05f && factor < 20.0f) {
                    vScaleX = (vScaleX * factor).coerceIn(0.2f, 6.0f)
                    vScaleY = (vScaleY * factor).coerceIn(0.2f, 6.0f)

                    val focusX = detector.focusX
                    val focusY = detector.focusY
                    vTransX += (focusX - lastFocusX)
                    vTransY += (focusY - lastFocusY)
                    lastFocusX = focusX
                    lastFocusY = focusY

                    applyViewportTransform()
                    updateViewportStatus()
                }
                return true
            }

            override fun onScaleEnd(detector: ScaleGestureDetector) {
                isScaling = false
                saveViewportPrefs()
            }
        })
    }

    private fun initSpinners() {
        val resNames = resOptions.map { it.second }
        val resAdapter = ArrayAdapter(this, R.layout.item_spinner, R.id.spinnerText, resNames)
        spinnerResolution.adapter = resAdapter
        spinnerResolution.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p0: android.widget.AdapterView<*>?, p1: View?, position: Int, p3: Long) {
                if (position != currentResIndex) {
                    currentResIndex = position
                    val opt = resOptions[position]
                    resToggleBtn.text = "Res: ${opt.second.substringBefore(" ")}"
                    if (running) {
                        sendControlPacket(11.toByte(), opt.first.first.toFloat(), opt.first.second.toFloat())
                    }
                }
            }
            override fun onNothingSelected(p0: android.widget.AdapterView<*>?) {}
        }

        val bitNames = bitrateOptions.map { it.second }
        val bitAdapter = ArrayAdapter(this, R.layout.item_spinner, R.id.spinnerText, bitNames)
        spinnerBitrate.adapter = bitAdapter
        spinnerBitrate.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p0: android.widget.AdapterView<*>?, p1: View?, position: Int, p3: Long) {
                if (position != currentBitrateIndex) {
                    currentBitrateIndex = position
                    val opt = bitrateOptions[position]
                    bitrateToggleBtn.text = "Quality: ${opt.second.substringBefore(" ")}"
                    if (running) {
                        sendControlPacket(10.toByte(), opt.first, 0.0f)
                    }
                }
            }
            override fun onNothingSelected(p0: android.widget.AdapterView<*>?) {}
        }
    }

    private fun checkAndAutoConnect() {
        Thread {
            try { Thread.sleep(600) } catch (_: Exception) {}
            if (running || hasExitedToDashboardManually || !autoConnectCheck.isChecked) return@Thread

            runOnUiThread {
                statusText.text = "● Checking USB connection (127.0.0.1)..."
            }

            val candidates = listOf("127.0.0.1", "10.105.181.132")
            for (cand in candidates) {
                try {
                    val s = Socket()
                    s.connect(InetSocketAddress(cand, PORT), 600)
                    s.close()
                    runOnUiThread {
                        if (!running && !hasExitedToDashboardManually && autoConnectCheck.isChecked) {
                            ipInput.setText(cand)
                            statusText.text = "● Connected! Launching mirror..."
                            startConnection(cand)
                        }
                    }
                    return@Thread
                } catch (_: Exception) {}
            }

            runOnUiThread {
                statusText.text = "● Ready — Waiting for PCMirror stream"
            }
        }.start()
    }

    private fun applyAspectLayout(mode: AspectRatioMode) {
        val dm = resources.displayMetrics
        val screenW = dm.widthPixels
        val screenH = dm.heightPixels
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        lp.gravity = Gravity.CENTER

        when (mode) {
            AspectRatioMode.SAFE_FIT -> {
                val vertMargin = (16 * dm.density).toInt()
                val usableH = screenH - (vertMargin * 2)
                val targetW = ((usableH.toDouble() * 16.0) / 9.0).toInt()
                lp.width = targetW
                lp.height = usableH
                lp.setMargins(0, 0, 0, 0)
            }
            AspectRatioMode.FULL_STRETCH -> {
                lp.width = screenW
                lp.height = screenH
                lp.setMargins(0, 0, 0, 0)
            }
            AspectRatioMode.CROP_FILL -> {
                lp.width = screenW
                lp.height = ((screenW.toDouble() * 9.0) / 16.0).toInt()
                lp.setMargins(0, 0, 0, 0)
            }
            AspectRatioMode.PRODUCTIVITY -> {
                val vertMargin = (10 * dm.density).toInt()
                val usableH = screenH - (vertMargin * 2)
                val targetW = ((usableH.toDouble() * 16.0) / 10.0).toInt()
                lp.width = targetW
                lp.height = usableH
                lp.setMargins(0, 0, 0, 0)
            }
            AspectRatioMode.ULTRAWIDE -> {
                lp.width = screenW
                val targetH = ((screenW.toDouble() * 9.0) / 21.0).toInt()
                lp.height = targetH
                lp.setMargins(0, 0, 0, 0)
            }
        }
        surfaceView.layoutParams = lp
        currentAspectMode = mode
        ratioToggleBtn.text = "Ratio: ${mode.displayName.substringBefore(" ")}"
        saveViewportPrefs()
    }

    private fun updateRatioRadioSelection(mode: AspectRatioMode) {
        when (mode) {
            AspectRatioMode.SAFE_FIT -> rbSafeFit.isChecked = true
            AspectRatioMode.FULL_STRETCH -> rbFullStretch.isChecked = true
            AspectRatioMode.CROP_FILL -> rbCropFill.isChecked = true
            AspectRatioMode.PRODUCTIVITY -> rbProductivity.isChecked = true
            AspectRatioMode.ULTRAWIDE -> rbUltrawide.isChecked = true
        }
    }

    private fun applyViewportTransform() {
        runOnUiThread {
            surfaceView.scaleX = vScaleX
            surfaceView.scaleY = vScaleY
            surfaceView.translationX = vTransX
            surfaceView.translationY = vTransY
        }
    }

    private fun updateViewportStatus() {
        runOnUiThread {
            val pct = (vScaleX * 100).toInt()
            viewportStatusText.text = "Current Scale: ${pct}% | Pos: (${vTransX.toInt()}, ${vTransY.toInt()})"
        }
    }

    private fun resetViewport() {
        vScaleX = 1.0f
        vScaleY = 1.0f
        vTransX = 0f
        vTransY = 0f
        applyViewportTransform()
        updateViewportStatus()
        saveViewportPrefs()
        Toast.makeText(this, "View Reset", Toast.LENGTH_SHORT).show()
    }

    private fun saveViewportPrefs() {
        getSharedPreferences("pcmirror_prefs", MODE_PRIVATE).edit()
            .putFloat("scale_x", vScaleX)
            .putFloat("scale_y", vScaleY)
            .putFloat("trans_x", vTransX)
            .putFloat("trans_y", vTransY)
            .putString("aspect_mode", currentAspectMode.name)
            .putBoolean("auto_connect", autoConnectCheck.isChecked)
            .apply()
    }

    private fun loadPreferences() {
        val prefs = getSharedPreferences("pcmirror_prefs", MODE_PRIVATE)
        vScaleX = prefs.getFloat("scale_x", 1.0f)
        vScaleY = prefs.getFloat("scale_y", 1.0f)
        vTransX = prefs.getFloat("trans_x", 0f)
        vTransY = prefs.getFloat("trans_y", 0f)
        val modeStr = prefs.getString("aspect_mode", AspectRatioMode.SAFE_FIT.name)
        currentAspectMode = try { AspectRatioMode.valueOf(modeStr ?: "") } catch (_: Exception) { AspectRatioMode.SAFE_FIT }
        updateRatioRadioSelection(currentAspectMode)
        autoConnectCheck.isChecked = prefs.getBoolean("auto_connect", true)
        applyViewportTransform()
        updateViewportStatus()
        updateDashboardPresetsView()
    }

    private fun loadCustomPresets(): MutableList<CustomAspectPreset> {
        val list = mutableListOf<CustomAspectPreset>()
        val prefs = getSharedPreferences("pcmirror_prefs", MODE_PRIVATE)
        val jsonStr = prefs.getString("custom_aspect_presets", "[]") ?: "[]"
        try {
            val arr = JSONArray(jsonStr)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                list.add(
                    CustomAspectPreset(
                        id = obj.optString("id", System.currentTimeMillis().toString()),
                        name = obj.optString("name", "Preset ${i + 1}"),
                        aspectMode = obj.optString("aspectMode", AspectRatioMode.SAFE_FIT.name),
                        scaleX = obj.optDouble("scaleX", 1.0).toFloat(),
                        scaleY = obj.optDouble("scaleY", 1.0).toFloat(),
                        transX = obj.optDouble("transX", 0.0).toFloat(),
                        transY = obj.optDouble("transY", 0.0).toFloat()
                    )
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error loading presets", e)
        }
        return list
    }

    private fun saveCustomPresets(presets: List<CustomAspectPreset>) {
        val arr = JSONArray()
        for (p in presets) {
            val obj = JSONObject().apply {
                put("id", p.id)
                put("name", p.name)
                put("aspectMode", p.aspectMode)
                put("scaleX", p.scaleX.toDouble())
                put("scaleY", p.scaleY.toDouble())
                put("transX", p.transX.toDouble())
                put("transY", p.transY.toDouble())
            }
            arr.put(obj)
        }
        getSharedPreferences("pcmirror_prefs", MODE_PRIVATE).edit()
            .putString("custom_aspect_presets", arr.toString())
            .apply()
    }

    private fun showSavePresetDialog() {
        val dialogView = LayoutInflater.from(this).inflate(R.layout.dialog_save_preset, null)
        val tvDetails = dialogView.findViewById<TextView>(R.id.tvPresetDetailsPreview)
        val etName = dialogView.findViewById<EditText>(R.id.etPresetName)
        val btnCancel = dialogView.findViewById<Button>(R.id.btnCancelSave)
        val btnConfirm = dialogView.findViewById<Button>(R.id.btnConfirmSave)

        val scalePct = (vScaleX * 100).toInt()
        tvDetails.text = "Scale: ${scalePct}% | Aspect: ${currentAspectMode.displayName} | Offset: (${vTransX.toInt()}, ${vTransY.toInt()})"

        val dialog = AlertDialog.Builder(this)
            .setView(dialogView)
            .create()

        dialog.window?.setBackgroundDrawableResource(android.R.color.transparent)

        btnCancel.setOnClickListener { dialog.dismiss() }

        btnConfirm.setOnClickListener {
            val name = etName.text.toString().trim()
            if (name.isEmpty()) {
                Toast.makeText(this, "Please enter a preset name", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }

            val presets = loadCustomPresets()
            val newPreset = CustomAspectPreset(
                id = System.currentTimeMillis().toString(),
                name = name,
                aspectMode = currentAspectMode.name,
                scaleX = vScaleX,
                scaleY = vScaleY,
                transX = vTransX,
                transY = vTransY
            )
            presets.add(0, newPreset)
            saveCustomPresets(presets)

            Toast.makeText(this, "Preset \"$name\" saved!", Toast.LENGTH_SHORT).show()
            updateDashboardPresetsView()
            dialog.dismiss()
        }

        dialog.show()
    }

    private fun showPresetsListDialog() {
        val dialogView = LayoutInflater.from(this).inflate(R.layout.dialog_presets_list, null)
        val container = dialogView.findViewById<LinearLayout>(R.id.presetsListContainer)
        val tvEmpty = dialogView.findViewById<TextView>(R.id.tvEmptyPresets)
        val btnClose = dialogView.findViewById<Button>(R.id.btnClosePresets)

        val dialog = AlertDialog.Builder(this)
            .setView(dialogView)
            .create()

        dialog.window?.setBackgroundDrawableResource(android.R.color.transparent)
        btnClose.setOnClickListener { dialog.dismiss() }

        fun populateList() {
            container.removeAllViews()
            val presets = loadCustomPresets()
            if (presets.isEmpty()) {
                tvEmpty.visibility = View.VISIBLE
                container.addView(tvEmpty)
            } else {
                tvEmpty.visibility = View.GONE
                for (preset in presets) {
                    val itemView = LayoutInflater.from(this).inflate(R.layout.item_saved_preset, container, false)
                    val tvName = itemView.findViewById<TextView>(R.id.tvPresetItemName)
                    val tvDetails = itemView.findViewById<TextView>(R.id.tvPresetItemDetails)
                    val btnApply = itemView.findViewById<Button>(R.id.btnApplyPreset)
                    val btnDelete = itemView.findViewById<Button>(R.id.btnDeletePreset)

                    tvName.text = preset.name
                    val scalePct = (preset.scaleX * 100).toInt()
                    tvDetails.text = "Scale: ${scalePct}% | ${preset.aspectMode}"

                    btnApply.setOnClickListener {
                        applyCustomPreset(preset)
                        dialog.dismiss()
                    }

                    btnDelete.setOnClickListener {
                        val currentList = loadCustomPresets()
                        currentList.removeAll { it.id == preset.id }
                        saveCustomPresets(currentList)
                        Toast.makeText(this, "Deleted \"${preset.name}\"", Toast.LENGTH_SHORT).show()
                        populateList()
                        updateDashboardPresetsView()
                    }

                    container.addView(itemView)
                }
            }
        }

        populateList()
        dialog.show()
    }

    private fun applyCustomPreset(preset: CustomAspectPreset) {
        val mode = try { AspectRatioMode.valueOf(preset.aspectMode) } catch (_: Exception) { AspectRatioMode.SAFE_FIT }
        applyAspectLayout(mode)
        updateRatioRadioSelection(mode)
        vScaleX = preset.scaleX
        vScaleY = preset.scaleY
        vTransX = preset.transX
        vTransY = preset.transY
        applyViewportTransform()
        updateViewportStatus()
        saveViewportPrefs()
        ratioToggleBtn.text = "Ratio: ${preset.name.take(10)}"
        Toast.makeText(this, "Applied preset \"${preset.name}\"", Toast.LENGTH_SHORT).show()
    }

    private fun updateDashboardPresetsView() {
        val container = layoutSavedPresetsDashboard ?: return
        container.removeAllViews()
        val presets = loadCustomPresets()
        if (presets.isEmpty()) return

        val header = TextView(this).apply {
            text = "Saved Presets (${presets.size}):"
            setTextColor(android.graphics.Color.parseColor("#8B949E"))
            textSize = 11f
            setPadding(0, 4, 0, 4)
        }
        container.addView(header)

        for (preset in presets.take(6)) {
            val chip = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setBackgroundResource(R.drawable.bg_button_secondary)
                setPadding(16, 8, 12, 8)
                val lp = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, 4, 0, 4) }
                layoutParams = lp
            }

            val tvTitle = TextView(this).apply {
                val scalePct = (preset.scaleX * 100).toInt()
                text = "${preset.name} (${scalePct}%)"
                setTextColor(android.graphics.Color.WHITE)
                textSize = 12f
                val lp = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
                layoutParams = lp
            }

            val btnApply = Button(this).apply {
                text = "Apply"
                textSize = 10f
                setTextColor(android.graphics.Color.parseColor("#090D14"))
                setBackgroundResource(R.drawable.bg_button_glow)
                val lp = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    (30 * resources.displayMetrics.density).toInt()
                ).apply { setMargins(0, 0, 8, 0) }
                layoutParams = lp
                setOnClickListener { applyCustomPreset(preset) }
            }

            val btnDel = Button(this).apply {
                text = "✕"
                textSize = 11f
                setTextColor(android.graphics.Color.parseColor("#FF8A80"))
                setBackgroundColor(android.graphics.Color.TRANSPARENT)
                val lp = LinearLayout.LayoutParams(
                    (32 * resources.displayMetrics.density).toInt(),
                    (30 * resources.displayMetrics.density).toInt()
                )
                layoutParams = lp
                setOnClickListener {
                    val currentList = loadCustomPresets()
                    currentList.removeAll { it.id == preset.id }
                    saveCustomPresets(currentList)
                    updateDashboardPresetsView()
                }
            }

            chip.addView(tvTitle)
            chip.addView(btnApply)
            chip.addView(btnDel)
            container.addView(chip)
        }
    }

    private fun startConnection(ip: String) {
        if (readerThread?.isAlive == true) {
            stopConnection()
            try { readerThread?.join(500) } catch (_: Exception) {}
        }

        dashboardView.visibility = View.GONE
        floatingControls.visibility = View.VISIBLE
        floatingControlsPanel.visibility = View.GONE
        floatingControlsPanel.alpha = 1f
        floatingControlsPanel.translationX = 0f
        gearMenuBtn.rotation = 0f
        running = true

        // Touch backchannel
        Thread {
            try {
                val s = Socket(ip, PORT + 1).apply { tcpNoDelay = true }
                touchOut = s.getOutputStream()
                Log.i(TAG, "Touch backchannel connected to $ip:${PORT + 1}")
            } catch (e: Exception) {
                Log.w(TAG, "Touch backchannel optional: ${e.message}")
            }
        }.start()

        // Real-time audio stream (PC -> Android)
        startAudioStream(ip)

        val touchBuf = ByteBuffer.allocate(9).order(ByteOrder.LITTLE_ENDIAN)

        surfaceView.setOnTouchListener { v, event ->
            val pointerCount = event.pointerCount

            // 1. Pass multi-touch to scale detector for smooth zoom (if not locked)
            if (!isViewportLocked) {
                scaleDetector.onTouchEvent(event)
            }

            // 2. Multi-touch handling (Pinch & Pan & Double-tap reset)
            if (pointerCount >= 2) {
                if (isViewportLocked) {
                    return@setOnTouchListener true
                }
                when (event.actionMasked) {
                    MotionEvent.ACTION_POINTER_DOWN -> {
                        val now = SystemClock.uptimeMillis()
                        if (now - twoFingerDownTime < 350) {
                            resetViewport()
                            twoFingerDownTime = 0L
                            return@setOnTouchListener true
                        }
                        twoFingerDownTime = now
                        lastFocusX = (event.getX(0) + event.getX(1)) / 2f
                        lastFocusY = (event.getY(0) + event.getY(1)) / 2f
                    }
                    MotionEvent.ACTION_MOVE -> {
                        if (!isScaling) {
                            val midX = (event.getX(0) + event.getX(1)) / 2f
                            val midY = (event.getY(0) + event.getY(1)) / 2f
                            val dx = midX - lastFocusX
                            val dy = midY - lastFocusY
                            if (Math.hypot(dx.toDouble(), dy.toDouble()) < 150) {
                                vTransX += dx
                                vTransY += dy
                                applyViewportTransform()
                                updateViewportStatus()
                            }
                            lastFocusX = midX
                            lastFocusY = midY
                        }
                    }
                    MotionEvent.ACTION_POINTER_UP -> {
                        saveViewportPrefs()
                    }
                }
                return@setOnTouchListener true
            }

            // 3. Single-touch handling
            val rawX = event.x
            val rawY = event.y

            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    val now = SystemClock.uptimeMillis()
                    val timeSinceLastTap = now - lastTapTime
                    val isNearEdgeX = rawX < v.width * 0.15f || rawX > v.width * 0.85f
                    val isNearEdgeY = rawY < v.height * 0.15f || rawY > v.height * 0.85f

                    if (timeSinceLastTap < 350 && (isNearEdgeX || isNearEdgeY)) {
                        isEdgeStretching = true
                        edgeStretchAxis = if (isNearEdgeX) 1 else 2
                        edgeStretchStartX = rawX
                        edgeStretchStartY = rawY
                        baseScaleX = vScaleX
                        baseScaleY = vScaleY
                        Toast.makeText(this@MainActivity, if (edgeStretchAxis == 1) "Stretching Width" else "Stretching Height", Toast.LENGTH_SHORT).show()
                        return@setOnTouchListener true
                    }
                    lastTapTime = now
                    isEdgeStretching = false
                }
                MotionEvent.ACTION_MOVE -> {
                    if (isEdgeStretching) {
                        if (edgeStretchAxis == 1) {
                            val deltaX = rawX - edgeStretchStartX
                            val factor = 1.0f + (deltaX / (v.width * 0.4f))
                            vScaleX = (baseScaleX * factor).coerceIn(0.2f, 5.0f)
                        } else {
                            val deltaY = rawY - edgeStretchStartY
                            val factor = 1.0f + (deltaY / (v.height * 0.4f))
                            vScaleY = (baseScaleY * factor).coerceIn(0.2f, 5.0f)
                        }
                        applyViewportTransform()
                        updateViewportStatus()
                        return@setOnTouchListener true
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (isEdgeStretching) {
                        isEdgeStretching = false
                        saveViewportPrefs()
                        return@setOnTouchListener true
                    }
                }
            }

            // 4. Forward single-touch to PC mouse
            val out = touchOut ?: return@setOnTouchListener false
            val action = when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> 0.toByte()
                MotionEvent.ACTION_MOVE -> 1.toByte()
                MotionEvent.ACTION_UP -> 2.toByte()
                else -> return@setOnTouchListener false
            }
            val normX = (rawX / v.width).coerceIn(0f, 1f)
            val normY = (rawY / v.height).coerceIn(0f, 1f)

            synchronized(touchBuf) {
                touchBuf.clear()
                touchBuf.put(action)
                touchBuf.putFloat(normX)
                touchBuf.putFloat(normY)
                try {
                    out.write(touchBuf.array())
                    out.flush()
                } catch (_: Exception) {}
            }
            true
        }

        readerThread = Thread {
            val startTime = SystemClock.uptimeMillis()
            while (!surfaceReady && SystemClock.uptimeMillis() - startTime < 3000) {
                Thread.sleep(20)
            }
            runDecodeLoop(ip, surfaceView.holder.surface)
        }.apply { start() }
    }

    private var activeSocket: Socket? = null

    private fun startAudioStream(ip: String) {
        audioThread = Thread {
            var track: AudioTrack? = null
            var socket: Socket? = null
            try {
                Log.i(TAG, "Connecting to audio stream on $ip:${PORT + 2}...")
                socket = Socket().apply {
                    connect(InetSocketAddress(ip, PORT + 2), 3000)
                    tcpNoDelay = true
                    soTimeout = 8000
                }
                audioSocket = socket
                val inStream = DataInputStream(socket.getInputStream())

                // 8-byte handshake: [magic (4 bytes, 0x50434D41), sampleRate (4 bytes, little endian)]
                val header = ByteArray(8)
                inStream.readFully(header)
                val magic = ByteBuffer.wrap(header, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
                val sampleRate = ByteBuffer.wrap(header, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int
                Log.i(TAG, "Audio handshake: magic=0x${Integer.toHexString(magic)}, sampleRate=$sampleRate Hz")

                val validRate = if (sampleRate in 8000..192000) sampleRate else 48000
                val minBuf = AudioTrack.getMinBufferSize(
                    validRate,
                    AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT
                )
                val bufferSize = (minBuf * 2).coerceAtLeast(4096)

                val attributes = AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()

                val format = AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(validRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build()

                val trackBuilder = AudioTrack.Builder()
                    .setAudioAttributes(attributes)
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(bufferSize)
                    .setTransferMode(AudioTrack.MODE_STREAM)

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    trackBuilder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                }

                track = trackBuilder.build()
                audioTrack = track
                track.play()
                Log.i(TAG, "AudioTrack playing at $validRate Hz (Low Latency)")

                val pcmBuf = ByteArray(4096)
                socket.soTimeout = 0 // Continuous streaming

                while (running) {
                    val n = inStream.read(pcmBuf, 0, pcmBuf.size)
                    if (n <= 0) break
                    track.write(pcmBuf, 0, n)
                }
            } catch (e: Exception) {
                if (running) {
                    Log.w(TAG, "Audio stream optional: ${e.message}")
                }
            } finally {
                try { track?.stop() } catch (_: Exception) {}
                try { track?.release() } catch (_: Exception) {}
                try { socket?.close() } catch (_: Exception) {}
                if (audioTrack == track) audioTrack = null
                if (audioSocket == socket) audioSocket = null
                Log.i(TAG, "Audio stream stopped")
            }
        }.apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun stopConnection() {
        running = false
        try { activeSocket?.close() } catch (_: Exception) {}
        activeSocket = null
        try { activeCodec?.stop(); activeCodec?.release() } catch (_: Exception) {}
        activeCodec = null
        try { touchOut?.close() } catch (_: Exception) {}
        touchOut = null
        try { audioSocket?.close() } catch (_: Exception) {}
        audioSocket = null
        try { audioTrack?.stop(); audioTrack?.release() } catch (_: Exception) {}
        audioTrack = null
        try { audioThread?.interrupt() } catch (_: Exception) {}
        audioThread = null
    }

    private fun sendControlPacket(type: Byte, p1: Float, p2: Float) {
        val out = touchOut ?: return
        val buf = ByteBuffer.allocate(9).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(type)
        buf.putFloat(p1)
        buf.putFloat(p2)
        try {
            out.write(buf.array())
            out.flush()
        } catch (_: Exception) {}
    }

    private fun hideSystemUI() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val insetsController = WindowCompat.getInsetsController(window, window.decorView)
        insetsController.hide(WindowInsetsCompat.Type.systemBars())
        insetsController.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUI()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopConnection()
        readerThread?.join(1000)
    }

    private fun nalType(nal: ByteArray): Int {
        var offset = 0
        while (offset + 3 < nal.size) {
            if (nal[offset] == 0.toByte() && nal[offset + 1] == 0.toByte()) {
                if (nal[offset + 2] == 1.toByte()) {
                    offset += 3
                    break
                } else if (offset + 3 < nal.size && nal[offset + 2] == 0.toByte() && nal[offset + 3] == 1.toByte()) {
                    offset += 4
                    break
                }
            }
            offset++
        }
        return if (offset < nal.size) nal[offset].toInt() and 0x1F else -1
    }

    private fun extractSpsPps(data: ByteArray, length: Int): Pair<ByteArray, ByteArray>? {
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        val startIndices = ArrayList<Int>()
        var i = 0
        while (i + 3 < length) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte()) {
                if (data[i + 2] == 1.toByte()) {
                    startIndices.add(i)
                    i += 3
                    continue
                } else if (i + 3 < length && data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte()) {
                    startIndices.add(i)
                    i += 4
                    continue
                }
            }
            i++
        }
        if (startIndices.isEmpty()) return null

        for (k in 0 until startIndices.size) {
            val start = startIndices[k]
            val end = if (k + 1 < startIndices.size) startIndices[k + 1] else length
            val nal = data.copyOfRange(start, end)
            when (nalType(nal)) {
                7 -> sps = nal
                8 -> pps = nal
            }
        }
        return if (sps != null && pps != null) Pair(sps, pps) else null
    }

    private fun runDecodeLoop(host: String, surface: Surface) {
        var socket: Socket? = null
        var codec: MediaCodec? = null
        try {
            val candidates = if (host == "127.0.0.1") listOf("127.0.0.1") else listOf(host, "127.0.0.1")
            for (candidate in candidates) {
                var attempts = 0
                while (running && attempts < 3) {
                    try {
                        Log.i(TAG, "Connecting to $candidate:$PORT ...")
                        val s = Socket()
                        s.tcpNoDelay = true
                        s.receiveBufferSize = 256 * 1024
                        s.connect(InetSocketAddress(candidate, PORT), 1500)
                        socket = s
                        activeSocket = s
                        break
                    } catch (e: Exception) {
                        attempts++
                        Log.w(TAG, "Connect to $candidate attempt $attempts failed: ${e.message}")
                        Thread.sleep(200)
                    }
                }
                if (socket != null) {
                    Log.i(TAG, "Connected successfully to $candidate:$PORT!")
                    break
                }
            }
            if (socket == null) {
                Log.e(TAG, "All connection candidates failed")
                runOnUiThread {
                    dashboardView.visibility = View.VISIBLE
                    floatingControls.visibility = View.GONE
                    floatingControlsPanel.visibility = View.GONE
                    Toast.makeText(this@MainActivity, "Could not connect to PC. Please ensure PCMirror is running.", Toast.LENGTH_LONG).show()
                }
                return
            }

            val dataIn = DataInputStream(socket.getInputStream())
            val packetBuf = ByteArray(2 * 1024 * 1024)

            var sps: ByteArray? = null
            var pps: ByteArray? = null
            var initialPacketLen = 0

            while (running && (sps == null || pps == null)) {
                val len = dataIn.readInt()
                if (len <= 0 || len > packetBuf.size) {
                    Log.e(TAG, "Invalid initial packet length: $len")
                    return
                }
                dataIn.readFully(packetBuf, 0, len)
                val params = extractSpsPps(packetBuf, len)
                if (params != null) {
                    sps = params.first
                    pps = params.second
                    initialPacketLen = len
                    break
                }
            }
            if (sps == null || pps == null) {
                Log.e(TAG, "Stream ended before SPS/PPS arrived")
                return
            }

            val resolution = SpsParser.parse(sps)
            val streamWidth = resolution?.first ?: DEFAULT_WIDTH
            val streamHeight = resolution?.second ?: DEFAULT_HEIGHT
            Log.i(TAG, "Configuring decoder for ${streamWidth}x${streamHeight}")

            runOnUiThread {
                surfaceView.holder.setFixedSize(streamWidth, streamHeight)
            }

            val format = MediaFormat.createVideoFormat(MIME, streamWidth, streamHeight).apply {
                setByteBuffer("csd-0", ByteBuffer.wrap(sps))
                setByteBuffer("csd-1", ByteBuffer.wrap(pps))
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                }
                setInteger(MediaFormat.KEY_PRIORITY, 0)
                setInteger(MediaFormat.KEY_OPERATING_RATE, 240)
                try {
                    setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, 0)
                    setInteger("vendor.low-latency.enable", 1)
                    setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
                    setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
                    setInteger("vendor.qti-ext-dec-picture-order.enable", 0)
                    setInteger("vendor.mtk-ext-dec-low-latency.enable", 1)
                } catch (_: Exception) {}
            }

            val maxRefreshRate = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                try {
                    val display = windowManager.defaultDisplay
                    display?.supportedModes?.maxOfOrNull { it.refreshRate } ?: 120.0f
                } catch (_: Exception) { 120.0f }
            } else {
                120.0f
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                try {
                    surface.setFrameRate(maxRefreshRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
                } catch (_: Exception) {}
            }

            codec = MediaCodec.createDecoderByType(MIME)
            val availableInputBuffers = LinkedBlockingQueue<Int>()
            var frameCount = 0L
            var totalDecodeUs = 0L
            var lastFpsTime = SystemClock.elapsedRealtime()

            codec.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(c: MediaCodec, index: Int) {
                    availableInputBuffers.offer(index)
                }
                override fun onOutputBufferAvailable(c: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    val renderTimeUs = SystemClock.elapsedRealtimeNanos() / 1000
                    val latencyUs = renderTimeUs - info.presentationTimeUs
                    if (latencyUs in 0..500_000) {
                        totalDecodeUs += latencyUs
                    }
                    c.releaseOutputBuffer(index, true)
                    frameCount++
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastFpsTime >= 2000) {
                        val fps = frameCount * 1000.0 / (now - lastFpsTime)
                        val avgDecodeMs = if (frameCount > 0) (totalDecodeUs / frameCount.toDouble()) / 1000.0 else 0.0
                        Log.i(TAG, "Decoder FPS: ${String.format("%.1f", fps)} | Avg Decode Latency: ${String.format("%.2f", avgDecodeMs)} ms (rendered $frameCount frames)")
                        frameCount = 0L
                        totalDecodeUs = 0L
                        lastFpsTime = now
                    }
                }
                override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
                    Log.e(TAG, "Codec error (diag: ${e.diagnosticInfo})", e)
                }
                override fun onOutputFormatChanged(c: MediaCodec, f: MediaFormat) {
                    val w = f.getInteger(MediaFormat.KEY_WIDTH)
                    val h = f.getInteger(MediaFormat.KEY_HEIGHT)
                    Log.i(TAG, "Output format changed: ${w}x${h}")
                }
            })

            codec.configure(format, surface, null, 0)
            codec.start()
            activeCodec = codec
            applyViewportTransform()
            Log.i(TAG, "Decoder started (length-prefixed zero-latency mode)")

            if (initialPacketLen > 0) {
                val index = availableInputBuffers.take()
                val buf = codec.getInputBuffer(index)
                if (buf != null) {
                    buf.clear()
                    buf.put(packetBuf, 0, initialPacketLen)
                    val queueTimeUs = SystemClock.elapsedRealtimeNanos() / 1000
                    codec.queueInputBuffer(index, 0, initialPacketLen, queueTimeUs, 0)
                }
            }

            while (running) {
                val len = dataIn.readInt()
                if (len <= 0 || len > packetBuf.size) {
                    Log.e(TAG, "Invalid packet length: $len")
                    break
                }
                dataIn.readFully(packetBuf, 0, len)
                val index = availableInputBuffers.take()
                val buf = codec.getInputBuffer(index) ?: continue
                buf.clear()
                buf.put(packetBuf, 0, len)
                val queueTimeUs = SystemClock.elapsedRealtimeNanos() / 1000
                codec.queueInputBuffer(index, 0, len, queueTimeUs, 0)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Decode loop error", e)
        } finally {
            activeCodec = null
            try { codec?.stop(); codec?.release() } catch (_: Exception) {}
            try { socket?.close() } catch (_: Exception) {}
            Log.i(TAG, "Decode loop terminated")
            runOnUiThread {
                if (!hasExitedToDashboardManually) {
                    dashboardView.visibility = View.VISIBLE
                    floatingControls.visibility = View.GONE
                    floatingControlsPanel.visibility = View.GONE
                }
            }
        }
    }
}

/**
 * Minimal H.264 SPS parser for extracting video dimensions dynamically.
 */
object SpsParser {
    fun parse(sps: ByteArray): Pair<Int, Int>? {
        return try {
            var offset = 0
            while (offset + 3 < sps.size) {
                if (sps[offset] == 0.toByte() && sps[offset + 1] == 0.toByte()) {
                    if (sps[offset + 2] == 1.toByte()) { offset += 3; break }
                    if (offset + 3 < sps.size && sps[offset + 2] == 0.toByte() && sps[offset + 3] == 1.toByte()) { offset += 4; break }
                }
                offset++
            }
            offset++

            val rbsp = ArrayList<Byte>()
            var i = offset
            while (i < sps.size) {
                if (i + 2 < sps.size && sps[i] == 0.toByte() && sps[i + 1] == 0.toByte() && sps[i + 2] == 3.toByte()) {
                    rbsp.add(0.toByte()); rbsp.add(0.toByte())
                    i += 3
                } else {
                    rbsp.add(sps[i])
                    i++
                }
            }
            val br = BitReader(rbsp.toByteArray())
            val profileIdc = br.readBits(8)
            br.readBits(8)
            br.readBits(8)
            br.readUe()

            if (profileIdc in listOf(100, 110, 122, 244, 44, 83, 86, 118, 128)) {
                val chromaFormatIdc = br.readUe()
                if (chromaFormatIdc == 3) br.readBits(1)
                br.readUe()
                br.readUe()
                br.readBits(1)
                val seqScaling = br.readBits(1)
                if (seqScaling == 1) {
                    val count = if (chromaFormatIdc != 3) 8 else 12
                    for (j in 0 until count) {
                        if (br.readBits(1) == 1) {
                            val size = if (j < 6) 16 else 64
                            var lastScale = 8; var nextScale = 8
                            for (k in 0 until size) {
                                if (nextScale != 0) nextScale = (lastScale + br.readSe() + 256) % 256
                                lastScale = if (nextScale == 0) lastScale else nextScale
                            }
                        }
                    }
                }
            }
            br.readUe()
            val picOrderCntType = br.readUe()
            if (picOrderCntType == 0) {
                br.readUe()
            } else if (picOrderCntType == 1) {
                br.readBits(1); br.readSe(); br.readSe()
                val numRef = br.readUe()
                for (j in 0 until numRef) br.readSe()
            }
            br.readUe()
            br.readBits(1)
            val picWidthInMbsMinus1 = br.readUe()
            val picHeightInMapUnitsMinus1 = br.readUe()
            val frameMbsOnlyFlag = br.readBits(1)
            if (frameMbsOnlyFlag == 0) br.readBits(1)
            br.readBits(1)
            val frameCropping = br.readBits(1)
            var cropL = 0; var cropR = 0; var cropT = 0; var cropB = 0
            if (frameCropping == 1) {
                cropL = br.readUe(); cropR = br.readUe(); cropT = br.readUe(); cropB = br.readUe()
            }
            val width = (picWidthInMbsMinus1 + 1) * 16 - (cropL + cropR) * 2
            val height = ((2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16) - (cropT + cropB) * 2
            Pair(width, height)
        } catch (e: Exception) {
            Log.w("PCMirror", "Could not parse SPS resolution: ${e.message}")
            null
        }
    }

    private class BitReader(private val bytes: ByteArray) {
        private var byteOffset = 0
        private var bitOffset = 0

        fun readBits(n: Int): Int {
            var res = 0
            for (i in 0 until n) {
                if (byteOffset >= bytes.size) return res
                val bit = (bytes[byteOffset].toInt() ushr (7 - bitOffset)) and 1
                res = (res shl 1) or bit
                bitOffset++
                if (bitOffset == 8) { bitOffset = 0; byteOffset++ }
            }
            return res
        }

        fun readUe(): Int {
            var zeros = 0
            while (readBits(1) == 0 && zeros < 31) zeros++
            return if (zeros == 0) 0 else (1 shl zeros) - 1 + readBits(zeros)
        }

        fun readSe(): Int {
            val v = readUe()
            val sign = ((v and 1) shl 1) - 1
            return ((v + 1) shr 1) * sign
        }
    }
}
