package com.smartglass.test

import android.Manifest
import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.ImageView
import android.widget.RadioButton
import android.widget.ScrollView
import android.widget.TextView
import android.widget.ToggleButton
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.sqrt

/**
 * SmartGlass 수신 테스트 앱 (proto.md M5 §5)
 * 펌웨어 → 앱 데이터 적재 확인: 오디오 파형/레벨/청취, 이미지, PTT 이벤트, 통계
 * 앱 → 펌웨어 명령: SET_MODE(0~4), SET_VOLUME, DISPLAY_ALERT, CAPTURE_NOW, PING
 */
class MainActivity : AppCompatActivity(), LinkCallback {

    private var wifi: WifiLink? = null
    private var ble: BleLink? = null
    private var connected = false

    private var track: AudioTrack? = null
    private var trackRate = 0
    @Volatile private var listenOn = false

    private lateinit var tvStatus: TextView
    private lateinit var tvStats: TextView
    private lateinit var tvLevels: TextView
    private lateinit var tvEvents: TextView
    private lateinit var tvImgInfo: TextView
    private lateinit var tvClassify: TextView
    private lateinit var svEvents: ScrollView
    private lateinit var wave: WaveformView
    private lateinit var img: ImageView
    private val timeFmt = SimpleDateFormat("HH:mm:ss", Locale.KOREA)

    // 이미지 분류 (통합 1단계 — proto.md §5.5): 수신 이미지마다 백그라운드 추론
    private var classifier: ImageClassifier? = null
    private val clsExec = java.util.concurrent.Executors.newSingleThreadExecutor()

    // 소리 분류 (통합 4단계 — 호연 모델): 16k 스트림 2초 창, 2초 주기 추론
    private var soundCls: SoundClassifier? = null
    private val sndExec = java.util.concurrent.Executors.newSingleThreadExecutor()
    @Volatile private var lastSndAt = 0L
    private lateinit var tvSound: TextView

    // PTT 음성명령 (통합 2단계): 안경 버튼 → 폰 STT → LLM → CMD
    private var intentCls: IntentClassifier? = null

    // 모드 결정기 (통합 3단계): 통화 > 수동 > 자동(이미지) 우선순위
    private lateinit var modeCtl: ModeController
    private val callHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private var wasInCall = false
    private val callPoll = object : Runnable {
        override fun run() {  // AudioManager 오디오 모드 = 통화 감지 (권한 불필요, VoIP 포함 — §5.5)
            val am = getSystemService(AUDIO_SERVICE) as android.media.AudioManager
            val active = am.mode == android.media.AudioManager.MODE_IN_CALL ||
                         am.mode == android.media.AudioManager.MODE_IN_COMMUNICATION
            if (active != wasInCall) { wasInCall = active; modeCtl.callChanged(active) }
            callHandler.postDelayed(this, 2000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        tvStats = findViewById(R.id.tvStats)
        tvLevels = findViewById(R.id.tvLevels)
        tvEvents = findViewById(R.id.tvEvents)
        tvImgInfo = findViewById(R.id.tvImgInfo)
        tvClassify = findViewById(R.id.tvClassify)
        svEvents = findViewById(R.id.svEvents)
        wave = findViewById(R.id.wave)
        img = findViewById(R.id.img)

        classifier = try {
            ImageClassifier(this)
        } catch (e: Exception) {
            tvClassify.text = "분류: 모델 로드 실패 (${e.message})"
            null
        }

        tvSound = findViewById(R.id.tvSound)
        soundCls = try {
            SoundClassifier(this)
        } catch (e: Exception) {
            tvSound.text = "소리: 모델 로드 실패 (${e.message})"
            null
        }
        findViewById<Button>(R.id.btnImgAb).setOnClickListener {
            val cls = classifier ?: return@setOnClickListener
            clsExec.execute {   // 내장 실촬영 3장: 컬러 vs 흑백(휘도 3채널) 판정 비교 — 보드 불필요
                for (i in 1..3) {
                    try {
                        val bmp = assets.open("test_img$i.jpg").use {
                            android.graphics.BitmapFactory.decodeStream(it)
                        } ?: continue
                        val c = cls.classify(bmp, gray = false, smooth = false)
                        val g = cls.classify(bmp, gray = true, smooth = false)
                        runOnUiThread {
                            val match = if (c.rawClass == g.rawClass) "일치" else "❌불일치"
                            log("🖼 img$i: 컬러 ${ImageClassifier.KR[c.rawClass]}(${(c.conf * 100).toInt()}%)" +
                                " vs 흑백 ${ImageClassifier.KR[g.rawClass]}(${(g.conf * 100).toInt()}%) [$match]")
                        }
                    } catch (e: Exception) {
                        runOnUiThread { log("🖼 img$i 시험 실패: ${e.message}") }
                    }
                }
            }
        }
        findViewById<Button>(R.id.btnSoundTest).setOnClickListener {
            val snd = soundCls ?: return@setOnClickListener
            sndExec.execute {   // 내장 테스트 PCM(빔포밍 녹음 2초)으로 파이프라인 자가시험 — 보드 불필요
                try {
                    val bytes = assets.open("test_sound.pcm").readBytes()
                    val pcm = ShortArray(bytes.size / 2)
                    java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                        .asShortBuffer().get(pcm)
                    val r = snd.classifyBuffer(pcm)
                    runOnUiThread {
                        log("🔊 자가시험: " + SoundClassifier.CLASSES.indices.joinToString(" ") {
                            "${SoundClassifier.CLASSES[it]}=${"%.2f".format(r.scores[it])}"
                        } + " → ${r.cls ?: "무음"} (${r.ms}ms, rms=${r.rms.toInt()})")
                    }
                } catch (e: Exception) {
                    runOnUiThread { log("🔊 자가시험 실패: ${e.message}") }
                }
            }
        }

        val tvAutoInfo = findViewById<TextView>(R.id.tvAutoInfo)
        val tbAuto = findViewById<ToggleButton>(R.id.tbAuto)
        modeCtl = ModeController(
            onLog = { msg -> log(msg) },
            sendCmd = { payload -> sendCmd(payload) },
            onUiState = { s -> tvAutoInfo.text = s }
        )
        tbAuto.setOnCheckedChangeListener { _, on -> modeCtl.setAuto(on) }

        intentCls = IntentClassifier(
            this,
            onLog = { msg -> runOnUiThread { log(msg) } },
            onCmd = { payload, desc -> runOnUiThread {
                // 음성 SET_MODE는 수동 명령으로 취급 (자동 30초 억제) — 나머지는 그대로 통과
                if (payload.isNotEmpty() && payload[0].toInt() == Proto.CMD_SET_MODE && payload.size >= 2)
                    modeCtl.manualMode(payload[1].toInt(), desc)
                else
                    modeCtl.manualCmd(payload, desc)
            } },
            onAutoMode = { on -> runOnUiThread { tbAuto.isChecked = on } }  // 토글 동기화 → setAuto 호출됨
        )
        findViewById<Button>(R.id.btnVoice).setOnClickListener {
            if (ensureMicPerm()) intentCls?.onPttPress()  // 앱 버튼은 VAD 자동 종료 (떼기 불필요)
        }
        callHandler.postDelayed(callPoll, 2000)  // 통화 오버라이드 폴링 시작

        val btnConnect = findViewById<Button>(R.id.btnConnect)
        btnConnect.setOnClickListener {
            if (!connected) connect() else disconnect()
            btnConnect.text = if (connected) "해제" else "연결"
        }

        findViewById<ToggleButton>(R.id.tbListen).setOnCheckedChangeListener { _, on ->
            listenOn = on
            if (!on) { track?.pause(); track?.flush() } else track?.play()
        }

        val modes = listOf(R.id.btnMode0, R.id.btnMode1, R.id.btnMode2, R.id.btnMode3, R.id.btnMode4)
        modes.forEachIndexed { m, id ->
            findViewById<Button>(id).setOnClickListener {
                modeCtl.manualMode(m, "버튼")   // 수동 명령 — 자동 30초 억제 (통합 3단계)
            }
        }
        findViewById<Button>(R.id.btnVolUp).setOnClickListener {
            sendCmd(byteArrayOf(Proto.CMD_SET_VOLUME.toByte(), 3)); log("CMD VOL +3 전송")
        }
        findViewById<Button>(R.id.btnVolDown).setOnClickListener {
            sendCmd(byteArrayOf(Proto.CMD_SET_VOLUME.toByte(), (-3).toByte())); log("CMD VOL -3 전송")
        }
        findViewById<Button>(R.id.btnAlert).setOnClickListener {
            // DISPLAY_ALERT: az=135.0°(우후방), icon=2(SIREN), 3000ms — §6.1
            val az = 1350
            sendCmd(byteArrayOf(Proto.CMD_DISPLAY_ALERT.toByte(),
                (az and 0xFF).toByte(), ((az shr 8) and 0xFF).toByte(), 2, 0xB8.toByte(), 0x0B))
            log("CMD DISPLAY_ALERT 135° SIREN 전송")
        }
        findViewById<Button>(R.id.btnCapture).setOnClickListener {
            sendCmd(byteArrayOf(Proto.CMD_CAPTURE_NOW.toByte())); log("CMD CAPTURE_NOW 전송")
        }
        findViewById<Button>(R.id.btnPing).setOnClickListener {
            val w = wifi
            if (w != null) w.sendPing() else log("PING은 WiFi 모드 전용")
        }
    }

    private fun connect() {
        val useBle = findViewById<RadioButton>(R.id.rbBle).isChecked
        if (useBle) {
            if (!ensureBlePerms()) return
            ble = BleLink(this, this).also { it.start() }
        } else {
            wifi = WifiLink(this).also { it.start() }
        }
        connected = true
        log(if (useBle) "BLE 연결 시작" else "WiFi 연결 시작")
    }

    private fun disconnect() {
        wifi?.stop(); wifi = null
        ble?.stop(); ble = null
        connected = false
        onStatus("해제됨")
    }

    private fun sendCmd(payload: ByteArray) {
        wifi?.sendCmd(payload)
        ble?.sendCmd(payload)
        if (wifi == null && ble == null) log("미연결 — 먼저 [연결]")
    }

    private fun ensureMicPerm(): Boolean {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            android.content.pm.PackageManager.PERMISSION_GRANTED) return true
        ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 2)
        log("🎤 마이크 권한 허용 후 다시 시도하세요")
        return false
    }

    private fun ensureBlePerms(): Boolean {
        val need = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = need.filter {
            ContextCompat.checkSelfPermission(this, it) != android.content.pm.PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return true
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
        onStatus("BLE 권한 허용 후 다시 [연결]을 누르세요")
        return false
    }

    // ── LinkCallback (링크 스레드에서 호출됨) ─────────────────────────
    override fun onStatus(text: String) = runOnUiThread { tvStatus.text = text }
    override fun onStats(text: String) = runOnUiThread { tvStats.text = text }
    override fun onRtt(ms: Double) = runOnUiThread { log("PONG — RTT %.1fms".format(ms)) }

    override fun onAudio(samples: ShortArray, nch: Int, rate: Int) {
        wave.push(samples, nch)
        // 소리 분류 (4단계): 16k mono 스트림 축적 → 2초마다 백그라운드 추론
        val snd = soundCls
        if (snd != null) {
            snd.push(samples, nch, rate)
            val now = System.currentTimeMillis()
            if (snd.ready() && now - lastSndAt >= 2000) {
                lastSndAt = now
                sndExec.execute {
                    try {
                        val r = snd.classify()
                        runOnUiThread {
                            tvSound.text = if (r.isSilence)
                                "소리: 무음 (rms=${r.rms.toInt()})"
                            else
                                "소리: ${SoundClassifier.KR[r.cls]}(${(r.scores.max() * 100).toInt()}%)" +
                                " · " + SoundClassifier.CLASSES.indices.joinToString(" ") {
                                    "${SoundClassifier.CLASSES[it]}=${(r.scores[it] * 100).toInt()}"
                                } + " · ${r.ms}ms"
                            modeCtl.onSoundResult(r.cls)
                        }
                    } catch (_: Exception) {}
                }
            }
        }
        // 채널별 RMS 게이지
        val rms = DoubleArray(nch)
        val cnt = samples.size / nch
        if (cnt > 0) {
            for (f in 0 until cnt) for (c in 0 until nch) {
                val v = samples[f * nch + c].toDouble(); rms[c] += v * v
            }
            for (c in 0 until nch) rms[c] = sqrt(rms[c] / cnt)
        }
        // 재생 (ch1만)
        if (listenOn) {
            ensureTrack(rate)
            val mono = ShortArray(cnt)
            for (f in 0 until cnt) mono[f] = samples[f * nch]
            track?.write(mono, 0, mono.size, AudioTrack.WRITE_NON_BLOCKING)
        }
        runOnUiThread {
            val sb = StringBuilder()
            for (c in 0 until nch) {
                val bars = (rms[c] / 2000).toInt().coerceIn(0, 10)
                // 16kHz mono = 3-마이크 빔포밍 합성 스트림 (mic1 아님 — 2026-09-10 dsp_pipeline)
                val name = if (nch == 1 && rate == 16000) "beam(3mic)" else "mic${c + 1}"
                sb.append(name).append(' ').append("▮".repeat(bars).padEnd(10, '·')).append("  ")
            }
            tvLevels.text = sb.toString()
        }
    }

    override fun onEvent(text: String) = runOnUiThread {
        log("★ $text")
        // 기능 4 E2E: 안경 PTT press=STT 시작, release=종료 (구 스펙 계승 — CLAUDE.md §6.1)
        when {
            text.contains("BUTTON PRESS") -> if (ensureMicPerm()) intentCls?.onPttPress()
            text.contains("BUTTON RELEASE") -> intentCls?.onPttRelease()
        }
    }

    override fun onImage(bmp: Bitmap, kb: Int) {
        runOnUiThread {
            img.setImageBitmap(bmp)
            tvImgInfo.text = "이미지 ${bmp.width}x${bmp.height} ${kb}KB @ ${timeFmt.format(Date())}"
        }
        // 수신 즉시 백그라운드 분류 (3초 주기라 단일 스레드로 충분 — 밀리면 최신만 처리돼도 무방)
        val cls = classifier ?: return
        clsExec.execute {
            try {
                // 모델이 흑백(3채널 복제)으로 학습됨(다현 확인 2026-10-01) → 추론 입력은 항상 흑백화
                val r = cls.classify(bmp, gray = true)
                runOnUiThread {
                    tvClassify.text = "분류: ${ImageClassifier.KR[r.rawClass]}(${(r.conf * 100).toInt()}%)" +
                        " → 제어판정: ${ImageClassifier.KR[r.stableClass]} · ${r.ms}ms"
                    // 3단계: 제어판정 → 모드 결정기 (자동 모드 ON이면 SET_MODE 자동 발행)
                    modeCtl.onImageResult(r.stableClass, Proto.lastDirHist, Proto.lastDoaConf)
                }
            } catch (e: Exception) {
                runOnUiThread { tvClassify.text = "분류: 추론 오류 (${e.message})" }
            }
        }
    }

    private fun ensureTrack(rate: Int) {
        if (track != null && trackRate == rate) return
        track?.release()
        trackRate = rate
        track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(rate / 2)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track?.play()
    }

    private fun log(msg: String) {
        val line = "[${timeFmt.format(Date())}] $msg\n"
        tvEvents.append(line)
        if (tvEvents.lineCount > 60) {
            val t = tvEvents.text.toString()
            tvEvents.text = t.substring(t.length / 2)
        }
        svEvents.post { svEvents.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    override fun onDestroy() {
        super.onDestroy()
        disconnect()
        track?.release()
        intentCls?.destroy()
        callHandler.removeCallbacks(callPoll)
    }
}
