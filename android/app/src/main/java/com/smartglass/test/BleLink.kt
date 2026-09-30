package com.smartglass.test

import android.annotation.SuppressLint
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.util.UUID

/**
 * BLE 전송로: NimBLE GATT 서버(SmartGlass-HA) — AUDIO/IMAGE/EVENT(Notify, notify 1개=프레임 1개),
 * CMD(Write, 프레이밍 없이 payload만: subtype+args). UUID는 CLAUDE.md §6.2.
 * AUDIO = 16kHz mono 빔포밍 스트림 (fmt=0 PCM16 / fmt=1 ADPCM — Proto.audioSamples가 자동 디코드).
 */
@SuppressLint("MissingPermission")  // Activity가 BLUETOOTH_SCAN/CONNECT 권한 확보 후 start() 호출
class BleLink(private val ctx: Context, private val cb: LinkCallback) {

    companion object {
        val SVC: UUID = UUID.fromString("8f0e1000-04c4-4f4b-a0a5-1e2d3c4b5a69")
        val AUDIO: UUID = UUID.fromString("8f0e1001-04c4-4f4b-a0a5-1e2d3c4b5a69")
        val CMD: UUID = UUID.fromString("8f0e1002-04c4-4f4b-a0a5-1e2d3c4b5a69")
        val IMAGE: UUID = UUID.fromString("8f0e1003-04c4-4f4b-a0a5-1e2d3c4b5a69")
        val EVENT: UUID = UUID.fromString("8f0e1004-04c4-4f4b-a0a5-1e2d3c4b5a69")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val NAME = "SmartGlass-HA"
    }

    private var gatt: BluetoothGatt? = null
    private var cmdChar: BluetoothGattCharacteristic? = null
    private val subQueue = ArrayDeque<BluetoothGattCharacteristic>()
    @Volatile private var running = false
    private val handler = Handler(Looper.getMainLooper())

    private var pkts = 0
    private var bytes = 0
    private var drops = 0
    private var imgs = 0
    private var lastSeq = -1
    private var lastEvtSeq = -1
    // 이미지 재조립 상태
    private var imgId = -1
    private var imgTotal = 0
    private val imgChunks = HashMap<Int, ByteArray>()
    private val statTick = object : Runnable {
        override fun run() {
            if (!running) return
            cb.onStats("BLE ${pkts}pkt/s ${bytes * 8 / 1000}kbps 드랍누적 $drops | 이미지 ${imgs}장")
            pkts = 0; bytes = 0
            handler.postDelayed(this, 1000)
        }
    }

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            scanner()?.stopScan(this)
            cb.onStatus("BLE: ${result.device.address} 발견 → 연결 중...")
            gatt = result.device.connectGatt(ctx, false, gattCb, android.bluetooth.BluetoothDevice.TRANSPORT_LE)
        }
        override fun onScanFailed(errorCode: Int) {
            cb.onStatus("BLE 스캔 실패 (code=$errorCode)")
        }
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                cb.onStatus("BLE: 연결됨 → MTU 517 협상...")
                // 펌웨어 서비스 구성이 바뀌어도 안드로이드가 옛 GATT 테이블을 캐시하는 문제 방지
                // (EVENT/IMAGE 특성이 안 보이던 원인 후보 — 2026-09-10). 숨김 API라 리플렉션 사용
                try { g.javaClass.getMethod("refresh").invoke(g) } catch (_: Exception) {}
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                g.requestMtu(517)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                cb.onStatus("BLE: 연결 끊김" + if (running) " — 재스캔" else "")
                g.close()
                if (running) startScan()
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            cb.onStatus("BLE: MTU=$mtu → 서비스 검색...")
            g.discoverServices()
        }

        @Suppress("DEPRECATION")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(SVC)
            if (svc == null) { cb.onStatus("BLE: 서비스 없음 — ble 펌웨어인지 확인"); return }
            cmdChar = svc.getCharacteristic(CMD)
            subQueue.clear()
            val missing = StringBuilder()
            for ((u, name) in listOf(AUDIO to "AUDIO", EVENT to "EVENT", IMAGE to "IMAGE")) {
                val ch = svc.getCharacteristic(u)
                if (ch != null) subQueue.add(ch) else missing.append(name).append(' ')
            }
            if (missing.isNotEmpty())
                cb.onEvent("⚠ 특성 누락: $missing— 폰 블루투스 껐다 켠 뒤 재연결 필요(GATT 캐시)")
            subscribeNext(g)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS)
                cb.onEvent("⚠ 구독 CCCD 쓰기 실패(status=$status) — 다음 특성으로 진행")
            subscribeNext(g)  // GATT 작업은 직렬 — 이전 CCCD 쓰기 완료 후 다음 구독
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val d = ch.value ?: return
            if (d.size < 10 || d[0] != 0xA5.toByte() || d[1] != 0x5A.toByte()) return
            val type = d[2].toInt() and 0xFF
            val flags = d[3].toInt() and 0xFF
            val seq = Proto.u16(d, 4)
            val plen = Proto.u16(d, 6)
            if (d.size < 10 + plen) return
            pkts++; bytes += d.size

            when (ch.uuid) {
                AUDIO -> if (type == Proto.TYPE_AUDIO) {
                    if (lastSeq >= 0) {
                        val gap = (seq - lastSeq - 1) and 0xFFFF
                        if (gap in 1..0x7FFF) drops += gap
                    }
                    lastSeq = seq
                    val rate = if (flags and 0x01 != 0) 48000 else 16000
                    val nch = ((flags shr 4) and 0x3) + 1
                    cb.onAudio(Proto.audioSamples(d, 0, plen), nch, rate)
                }
                EVENT -> if (type == Proto.TYPE_EVENT && seq != lastEvtSeq) {
                    lastEvtSeq = seq
                    val sub = d[8].toInt() and 0xFF
                    when {
                        sub == 0x01 && plen >= 2 ->
                            cb.onEvent("BUTTON " + if (d[9].toInt() != 0) "PRESS" else "RELEASE")
                        sub == 0x02 && plen >= 4 -> {   // DOA_ALERT {azimuth i16, energy_db u8, t_ms u32} §6.1
                            val az = ((d[9].toInt() and 0xFF) or (d[10].toInt() shl 8)).toShort().toInt()
                            cb.onEvent("DOA_ALERT %.0f° %ddB".format(az / 10.0, d[11].toInt() and 0xFF))
                        }
                        else -> cb.onEvent("EVENT 0x%02x".format(sub))
                    }
                }
                IMAGE -> when (type) {
                    Proto.TYPE_IMG_START -> if (plen >= 11) {
                        imgId = Proto.u16(d, 8)
                        imgTotal = Proto.u32(d, 10).toInt()
                        imgChunks.clear()
                    }
                    Proto.TYPE_IMG_CHUNK -> if (plen > 4 && Proto.u16(d, 8) == imgId) {
                        imgChunks[Proto.u16(d, 10)] = d.copyOfRange(12, 8 + plen)
                    }
                    Proto.TYPE_IMG_END -> if (Proto.u16(d, 8) == imgId && imgChunks.isNotEmpty()) {
                        val blob = java.io.ByteArrayOutputStream()
                        for (k in imgChunks.keys.sorted()) blob.write(imgChunks[k]!!)
                        val jb = blob.toByteArray()
                        if (jb.size == imgTotal) {
                            val bmp = android.graphics.BitmapFactory.decodeByteArray(jb, 0, jb.size)
                            if (bmp != null) { imgs++; cb.onImage(bmp, jb.size / 1024) }
                        }
                        imgChunks.clear(); imgId = -1
                    }
                }
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun subscribeNext(g: BluetoothGatt) {
        val ch = subQueue.removeFirstOrNull() ?: run {
            cb.onStatus("BLE: 구독 절차 종료 — 수신 중")
            return
        }
        val name = when (ch.uuid) { AUDIO -> "AUDIO"; EVENT -> "EVENT"; IMAGE -> "IMAGE"; else -> "?" }
        cb.onStatus("BLE: $name 구독 중...")
        g.setCharacteristicNotification(ch, true)
        val d = ch.getDescriptor(CCCD) ?: run {
            cb.onEvent("⚠ $name CCCD 없음 — 건너뜀")
            return subscribeNext(g)
        }
        // Android 13+: 구식 writeDescriptor(d.value 세팅 방식)가 기기에 따라 값 미전송으로 실패
        // → 신형 API 우선 사용 (2026-09-10, EVENT/IMAGE 구독 안 되던 원인 후보)
        val ok: Boolean = if (android.os.Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                android.bluetooth.BluetoothStatusCodes.SUCCESS
        } else {
            d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            g.writeDescriptor(d)
        }
        if (!ok) {  // GATT 바쁨 등 — 300ms 후 1회 재시도 (실패 시 다음 특성으로)
            cb.onEvent("⚠ $name 구독 쓰기 거부 → 재시도")
            handler.postDelayed({
                val retry: Boolean = if (android.os.Build.VERSION.SDK_INT >= 33) {
                    g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                        android.bluetooth.BluetoothStatusCodes.SUCCESS
                } else {
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(d)
                }
                if (!retry) subscribeNext(g)  // 포기하고 다음으로 (onDescriptorWrite 안 옴)
            }, 300)
        }
    }

    private fun scanner() = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager)
        .adapter?.bluetoothLeScanner

    private fun startScan() {
        val sc = scanner() ?: run { cb.onStatus("BLE: 블루투스 꺼져 있음"); return }
        val filter = ScanFilter.Builder().setDeviceName(NAME).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        sc.startScan(listOf(filter), settings, scanCb)
        cb.onStatus("BLE: '$NAME' 스캔 중... (env:ble 펌웨어 필요)")
    }

    fun start() {
        if (running) return
        running = true
        lastSeq = -1
        startScan()
        handler.postDelayed(statTick, 1000)
    }

    fun stop() {
        running = false
        handler.removeCallbacks(statTick)
        try { scanner()?.stopScan(scanCb) } catch (_: Exception) {}
        gatt?.disconnect()
        gatt?.close()
        gatt = null
    }

    /** CMD 쓰기 — BLE는 GATT가 이미 프레이밍하므로 payload(subtype+args)만 전송 */
    @Suppress("DEPRECATION")
    fun sendCmd(payload: ByteArray) {
        val g = gatt ?: run { cb.onStatus("BLE: 미연결 — 명령 무시"); return }
        val c = cmdChar ?: return
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        c.value = payload
        g.writeCharacteristic(c)
    }
}
