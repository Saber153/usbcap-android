package com.usbcap.viewer

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.SurfaceTexture
import android.hardware.usb.*
import android.os.*
import android.util.Log
import android.util.Size
import android.view.*
import android.os.Build
import android.view.WindowManager
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.*
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit
import android.hardware.camera2.*
import android.media.ImageReader
import android.media.MediaRecorder
import android.view.Surface

class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "USBCap"
        private const val REQUEST_PERMISSIONS = 10
        private val REQUIRED_PERMISSIONS = arrayOf(
            Manifest.permission.CAMERA,
            Manifest.permission.WRITE_EXTERNAL_STORAGE,
            Manifest.permission.RECORD_AUDIO
        )
    }

    private lateinit var surfaceView: SurfaceView
    private lateinit var tvFps: TextView
    private lateinit var tvLatency: TextView
    private lateinit var tvResolution: TextView
    private lateinit var tvStatus: TextView
    private lateinit var noDeviceLayout: LinearLayout
    private lateinit var controlBar: LinearLayout
    private lateinit var tvDeviceName: TextView
    private lateinit var btnHud: ImageButton
    private lateinit var btnRecord: ImageButton
    private lateinit var btnScreenshot: ImageButton

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var cameraManager: CameraManager? = null
    private val cameraOpenLock = Semaphore(1)

    private var previewSurface: Surface? = null
    private var imageReader: ImageReader? = null

    // FPS 计算
    private var frameCount = 0L
    private var lastFpsTime = System.nanoTime()
    private var currentFps = 0f
    private var lastFrameLatency = 0f
    private var showHud = false
    private var isRecording = false
    private var mediaRecorder: MediaRecorder? = null

    // 状态回调
    private val cameraStateCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
            cameraOpenLock.release()
            cameraDevice = camera
            Log.i(TAG, "Camera opened: ${camera.id}")
            runOnUiThread {
                tvStatus.text = "采集卡已连接"
                controlBar.visibility = View.VISIBLE
                tvDeviceName.text = camera.id
            }
            createCaptureSession()
        }

        override fun onDisconnected(camera: CameraDevice) {
            cameraOpenLock.release()
            camera.close()
            cameraDevice = null
            Log.w(TAG, "Camera disconnected")
            runOnUiThread {
                tvStatus.text = "采集卡已断开，等待重新接入..."
                noDeviceLayout.visibility = View.VISIBLE
                controlBar.visibility = View.GONE
            }
        }

        override fun onError(camera: CameraDevice, error: Int) {
            cameraOpenLock.release()
            camera.close()
            cameraDevice = null
            Log.e(TAG, "Camera error: $error")
            runOnUiThread {
                tvStatus.text = "设备错误: $error"
            }
        }
    }

    private val surfaceHolderCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            previewSurface = holder.surface
            openCamera()
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            Log.i(TAG, "Surface changed: ${width}x${height}")
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            previewSurface = null
        }
    }

    // 连续拍摄回调 - 最低延迟
    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(
            session: CameraCaptureSession,
            request: CaptureRequest,
            result: TotalCaptureResult
        ) {
            frameCount++
            val now = System.nanoTime()

            // 获取传感器时间戳(纳秒)计算真实延迟
            val sensorTimestamp = result.get(CaptureResult.SENSOR_TIMESTAMP) ?: 0L
            val latencyNs = now - sensorTimestamp
            lastFrameLatency = latencyNs / 1_000_000f

            val elapsed = now - lastFpsTime
            if (elapsed >= 1_000_000_000L) {
                currentFps = frameCount * 1_000_000_000f / elapsed
                frameCount = 0
                lastFpsTime = now

                if (showHud) {
                    runOnUiThread {
                        tvFps.text = "FPS: ${String.format("%.1f", currentFps)}"
                        tvLatency.text = "延迟: ${String.format("%.1f", lastFrameLatency)}ms"
                    }
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 全屏沉浸模式
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        )

        setContentView(R.layout.activity_main)

if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
    window.attributes.layoutInDisplayCutoutMode =
        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
}
        
        // 绑定视图
        surfaceView = findViewById(R.id.surfaceView)
        tvFps = findViewById(R.id.tvFps)
        tvLatency = findViewById(R.id.tvLatency)
        tvResolution = findViewById(R.id.tvResolution)
        tvStatus = findViewById(R.id.tvStatus)
        noDeviceLayout = findViewById(R.id.noDeviceLayout)
        controlBar = findViewById(R.id.controlBar)
        tvDeviceName = findViewById(R.id.tvDeviceName)
        btnHud = findViewById(R.id.btnHud)
        btnRecord = findViewById(R.id.btnRecord)
        btnScreenshot = findViewById(R.id.btnScreenshot)

        cameraManager = getSystemService(Context.CAMERA_SERVICE) as CameraManager

        // SurfaceView 低延迟配置
        surfaceView.holder.addCallback(surfaceHolderCallback)
        surfaceView.holder.setFormat(SurfaceFormat.TRANSLUCENT)
        // 关键：设置固定缓冲区大小，避免动态分配
        surfaceView.holder.setFixedSize(1920, 1080)

        // 按钮事件
        btnHud.setOnClickListener {
            showHud = !showHud
            tvFps.visibility = if (showHud) View.VISIBLE else View.GONE
            tvLatency.visibility = if (showHud) View.VISIBLE else View.GONE
            tvResolution.visibility = if (showHud) View.VISIBLE else View.GONE
        }

        btnScreenshot.setOnClickListener { takeScreenshot() }
        btnRecord.setOnClickListener { toggleRecord() }

        // 请求权限
        if (allPermissionsGranted()) {
            Log.i(TAG, "Permissions granted")
        } else {
            ActivityCompat.requestPermissions(this, REQUIRED_PERMISSIONS, REQUEST_PERMISSIONS)
        }

        // 监听USB设备插拔
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        registerReceiver(usbReceiver, filter)

        Log.i(TAG, "Activity created")
    }

    private val usbReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    Log.i(TAG, "USB device attached, reopening camera...")
                    openCamera()
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    Log.i(TAG, "USB device detached")
                    closeCamera()
                }
            }
        }
    }

    private fun openCamera() {
        if (!cameraOpenLock.tryAcquire(2500, TimeUnit.MILLISECONDS)) {
            Log.e(TAG, "Camera open lock timeout")
            return
        }

        val manager = cameraManager ?: return

        try {
            // 遍历所有相机设备，找到USB采集卡
            for (cameraId in manager.cameraIdList) {
                val characteristics = manager.getCameraCharacteristics(cameraId)
                val facing = characteristics.get(CameraCharacteristics.LENS_FACING)

                // USB采集卡通常不是内置摄像头(FACING_BACK/FACING_FRONT)
                // 或者通过设备名称匹配
                val isUsbCapture = facing == null ||
                    facing == CameraCharacteristics.LENS_FACING_EXTERNAL ||
                    cameraId.contains("usb", ignoreCase = true) ||
                    cameraId.contains("uvc", ignoreCase = true)

                if (isUsbCapture) {
                    val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    val sizes = map?.getOutputSizes(SurfaceHolder::class.java)
                    Log.i(TAG, "Found USB camera: $cameraId, sizes: ${sizes?.joinToString()}")

                    runOnUiThread {
                        tvResolution.text = "可用分辨率: ${sizes?.joinToString { "${it.width}x${it.height}" }}"
                    }

                    // 打开指定相机，优先使用外部摄像头
                    manager.openCamera(cameraId, cameraStateCallback, null)
                    return
                }
            }

            // 没找到外部设备，尝试第一个相机
            if (manager.cameraIdList.isNotEmpty()) {
                val fallbackId = manager.cameraIdList[0]
                Log.i(TAG, "Fallback to camera: $fallbackId")
                manager.openCamera(fallbackId, cameraStateCallback, null)
            } else {
                Log.w(TAG, "No camera devices found")
                runOnUiThread {
                    tvStatus.text = "未发现采集卡设备"
                }
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "Camera permission denied", e)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to open camera", e)
        }
    }

    private fun createCaptureSession() {
        val camera = cameraDevice ?: return
        val surface = previewSurface ?: return

        try {
            // 最小化缓冲：只用1个Surface，不加ImageReader
            camera.createCaptureSession(
                listOf(surface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        if (camera == null) return
                        captureSession = session
                        startPreview(session)
                    }

                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        Log.e(TAG, "Capture session config failed")
                    }
                },
                null
            )
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to create capture session", e)
        }
    }

    private fun startPreview(session: CameraCaptureSession) {
        val camera = cameraDevice ?: return
        val surface = previewSurface ?: return

        try {
            // 创建重复拍摄请求（连续帧）
            val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(surface)

                // ====== 低延迟关键设置 ======

                // 1. 关闭3A自动处理（减少每帧计算量）
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)

                // 2. 手动曝光（避免自动调整延迟）
                set(CaptureRequest.SENSOR_EXPOSURE_TIME, 10_000_000L) // 10ms
                set(CaptureRequest.SENSOR_SENSITIVITY, 200)

                // 3. 关闭降噪和HDR
                set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_OFF)
                set(CaptureRequest.TONE_MAP_MODE, CaptureRequest.TONE_MAP_MODE_FAST)
                set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE,
                    CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_OFF)

                // 4. 最高帧率
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(60, 60))

                // 5. 关闭人脸检测
                set(CaptureRequest.STATISTICS_FACE_DETECT_MODE,
                    CaptureRequest.STATISTICS_FACE_DETECT_MODE_OFF)
            }

            // 设置为连续重复请求
            session.setRepeatingRequest(request.build(), captureCallback, null)
            Log.i(TAG, "Preview started with low-latency config")

        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to start preview", e)
        }
    }

    private fun takeScreenshot() {
        val camera = cameraDevice ?: return
        val surface = previewSurface ?: return

        // 用ImageReader临时抓一帧
        try {
            val reader = ImageReader.newInstance(1920, 1080, android.graphics.ImageFormat.JPEG, 2)
            val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE).apply {
                addTarget(reader.surface)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
            }

            camera.createCaptureSession(
                listOf(reader.surface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) {
                        s.capture(request.build(), object : CameraCaptureSession.CaptureCallback() {
                            override fun onCaptureCompleted(
                                session: CameraCaptureSession,
                                request: CaptureRequest,
                                result: TotalCaptureResult
                            ) {
                                val image = reader.acquireLatestImage()
                                if (image != null) {
                                    val buffer = image.planes[0].buffer
                                    val bytes = ByteArray(buffer.remaining())
                                    buffer.get(bytes)

                                    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
                                    val file = File(getExternalFilesDir(Environment.DIRECTORY_PICTURES), "USB_$timestamp.jpg")
                                    FileOutputStream(file).use { it.write(bytes) }

                                    image.close()
                                    runOnUiThread {
                                        Toast.makeText(this@MainActivity, "截图已保存: ${file.name}", Toast.LENGTH_SHORT).show()
                                    }
                                    Log.i(TAG, "Screenshot saved: ${file.absolutePath}")

                                    // 截图完成，关闭临时session
                                    session.close()
                                }
                            }
                        }, null)
                    }
                    override fun onConfigureFailed(session: CameraCaptureSession) {}
                },
                null
            )
        } catch (e: Exception) {
            Log.e(TAG, "Screenshot failed", e)
        }
    }

    private fun toggleRecord() {
        if (isRecording) {
            stopRecording()
        } else {
            startRecording()
        }
    }

    private fun startRecording() {
        try {
            mediaRecorder = MediaRecorder().apply {
                setVideoSource(MediaRecorder.VideoSource.SURFACE)
                setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
                val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
                val file = File(getExternalFilesDir(Environment.DIRECTORY_MOVIES), "USB_$timestamp.mp4")
                setOutputFile(file.absolutePath)
                setVideoEncodingBitRate(20_000_000) // 20Mbps
                setVideoFrameRate(60)
                setVideoEncoder(MediaRecorder.VideoEncoder.H264)
                setOrientationHint(0)
                prepare()
            }

            val camera = cameraDevice ?: return
            val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(mediaRecorder!!.surface)
                previewSurface?.let { addTarget(it) }
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(60, 60))
            }

            camera.createCaptureSession(
                listOf(mediaRecorder!!.surface, previewSurface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        session.setRepeatingRequest(request.build(), null, null)
                        mediaRecorder?.start()
                        isRecording = true
                        runOnUiThread {
                            btnRecord.setImageResource(android.R.drawable.ic_media_pause)
                            Toast.makeText(this@MainActivity, "开始录制", Toast.LENGTH_SHORT).show()
                        }
                        Log.i(TAG, "Recording started")
                    }
                    override fun onConfigureFailed(session: CameraCaptureSession) {}
                },
                null
            )
        } catch (e: Exception) {
            Log.e(TAG, "Recording failed", e)
        }
    }

    private fun stopRecording() {
        try {
            mediaRecorder?.apply {
                stop()
                release()
            }
            mediaRecorder = null
            isRecording = false

            // 恢复预览session
            createCaptureSession()

            runOnUiThread {
                btnRecord.setImageResource(android.R.drawable.ic_media_play)
                Toast.makeText(this, "录制已保存", Toast.LENGTH_SHORT).show()
            }
            Log.i(TAG, "Recording stopped")
        } catch (e: Exception) {
            Log.e(TAG, "Stop recording failed", e)
        }
    }

    private fun closeCamera() {
        try {
            cameraOpenLock.tryAcquire(2500, TimeUnit.MILLISECONDS)
            captureSession?.close()
            captureSession = null
            cameraDevice?.close()
            cameraDevice = null
        } catch (e: InterruptedException) {
            Log.e(TAG, "Close camera interrupted", e)
        } finally {
            cameraOpenLock.release()
        }
    }

    private fun allPermissionsGranted() = REQUIRED_PERMISSIONS.all {
        ContextCompat.checkSelfPermission(baseContext, it) == PackageManager.PERMISSION_GRANTED
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) {
            if (allPermissionsGranted()) {
                openCamera()
            } else {
                Toast.makeText(this, "需要相机权限才能使用采集卡", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // 重新进入沉浸模式
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        )
        openCamera()
    }

    override fun onPause() {
        closeCamera()
        super.onPause()
    }

    override fun onDestroy() {
        closeCamera()
        unregisterReceiver(usbReceiver)
        super.onDestroy()
    }
}
