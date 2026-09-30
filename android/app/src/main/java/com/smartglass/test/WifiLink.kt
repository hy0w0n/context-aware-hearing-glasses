package com.smartglass.test

import android.graphics.BitmapFactory
import android.os.SystemClock
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.util.concurrent.ConcurrentLinkedQueue
import kotlin.concurrent.thread

/**
 * WiFi 전송로: 오디오+이벤트 = UDP:5005 (SUB 구독), 이미지+CMD+PING = TCP:5006 (CLAUDE.md §6.2)
 * ESP32 탐색: 브로드캐스트 SUB → 첫 프레임의 발신 주소로 고정 (PC tools/receiver.py와 동일 기법)
 */
class WifiLink(private val cb: LinkCallback) {

    @Volatile private var running = false
    @Volatile private var espIp: InetAddress? = null
    private val txQueue = ConcurrentLinkedQueue<ByteArray>()
    private var udpThread: Thread? = null
    private var tcpThread: Thread? = null
    @Volatile private var pingT0 = 0L

    // 통계 (1초 창)
    private var pkts = 0
    private var bytes = 0
    private var drops = 0
    private var imgs = 0
    private var lastSeq = -1
    private var lastEvtSeq = -1

    fun start() {
        if (running) return
        running = true
        cb.onStatus("WiFi: ESP32 탐색 중 (브로드캐스트)...")
        udpThread = thread(name = "udp") { udpLoop() }
        tcpThread = thread(name = "tcp") { tcpLoop() }
    }

    fun stop() {
        running = false
        udpThread?.interrupt()
        tcpThread?.interrupt()
        espIp = null
    }

    fun sendCmd(payload: ByteArray) {
        txQueue.add(Proto.build(Proto.TYPE_CMD, 0, payload))
    }

    fun sendPing() {
        pingT0 = SystemClock.elapsedRealtimeNanos()
        txQueue.add(Proto.build(Proto.TYPE_PING, 0, ByteArray(8)))
    }

    private fun broadcastTargets(): List<InetSocketAddress> {
        val out = mutableListOf(InetSocketAddress("255.255.255.255", 5005))
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) {
                    val b = ia.broadcast ?: continue
                    out.add(InetSocketAddress(b, 5005))
                }
            }
        } catch (_: Exception) {}
        return out
    }

    private fun udpLoop() {
        try {
            DatagramSocket().use { sock ->
                sock.broadcast = true
                sock.soTimeout = 500
                var lastSub = 0L
                var lastStat = 0L
                val buf = ByteArray(2048)
                val pkt = DatagramPacket(buf, buf.size)

                while (running) {
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastSub > 2000) {
                        lastSub = now
                        val sub = "SUB".toByteArray()
                        val ip = espIp
                        val targets = if (ip != null) listOf(InetSocketAddress(ip, 5005)) else broadcastTargets()
                        for (t in targets) {
                            try { sock.send(DatagramPacket(sub, sub.size, t)) } catch (_: Exception) {}
                        }
                    }
                    if (now - lastStat > 1000) {
                        lastStat = now
                        cb.onStats("UDP ${pkts}pkt/s ${bytes * 8 / 1000}kbps 드랍누적 $drops | 이미지 ${imgs}장")
                        pkts = 0; bytes = 0
                    }
                    try {
                        sock.receive(pkt)
                    } catch (_: java.net.SocketTimeoutException) {
                        continue
                    }
                    val d = pkt.data
                    val n = pkt.length
                    if (n < 10 || d[0] != 0xA5.toByte() || d[1] != 0x5A.toByte()) continue
                    val type = d[2].toInt() and 0xFF
                    val flags = d[3].toInt() and 0xFF
                    val seq = Proto.u16(d, 4)
                    val plen = Proto.u16(d, 6)
                    if (n < 10 + plen) continue

                    if (espIp == null) {
                        espIp = pkt.address
                        cb.onStatus("WiFi: ESP32 발견 ${pkt.address.hostAddress} — 스트림 수신 중")
                    }

                    when (type) {
                        Proto.TYPE_AUDIO -> {
                            pkts++; bytes += n
                            if (lastSeq >= 0) {
                                val gap = (seq - lastSeq - 1) and 0xFFFF
                                if (gap in 1..0x7FFF) drops += gap
                            }
                            lastSeq = seq
                            val rate = if (flags and 0x01 != 0) 48000 else 16000
                            val nch = ((flags shr 4) and 0x3) + 1
                            cb.onAudio(Proto.audioSamples(d, 0, plen), nch, rate)
                        }
                        Proto.TYPE_EVENT -> {
                            if (seq != lastEvtSeq) {  // blind 2회 전송 → seq 중복 제거
                                lastEvtSeq = seq
                                val sub = d[8].toInt() and 0xFF
                                if (sub == 0x01 && plen >= 2) {
                                    cb.onEvent("BUTTON " + if (d[9].toInt() != 0) "PRESS" else "RELEASE")
                                } else {
                                    cb.onEvent("EVENT 0x%02x".format(sub))
                                }
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            if (running) cb.onStatus("WiFi UDP 오류: ${e.message}")
        }
    }

    private fun tcpLoop() {
        val chunks = HashMap<Int, ByteArray>()
        var curId = -1
        var curTotal = 0
        while (running) {
            val ip = espIp
            if (ip == null) { SystemClock.sleep(300); continue }
            try {
                Socket().use { s ->
                    s.connect(InetSocketAddress(ip, 5006), 5000)
                    s.soTimeout = 200
                    s.tcpNoDelay = true
                    val input = s.getInputStream()
                    val output = s.getOutputStream()
                    val acc = java.io.ByteArrayOutputStream()
                    val rb = ByteArray(8192)

                    while (running && s.isConnected) {
                        while (true) {
                            val f = txQueue.poll() ?: break
                            output.write(f); output.flush()
                        }
                        val n = try { input.read(rb) } catch (_: java.net.SocketTimeoutException) { continue }
                        if (n < 0) break
                        acc.write(rb, 0, n)

                        var data = acc.toByteArray()
                        var pos = 0
                        while (data.size - pos >= 10) {
                            if (data[pos] != 0xA5.toByte() || data[pos + 1] != 0x5A.toByte()) { pos++; continue }
                            val type = data[pos + 2].toInt() and 0xFF
                            val plen = Proto.u16(data, pos + 6)
                            if (data.size - pos < 10 + plen) break
                            val p = pos + 8
                            when (type) {
                                Proto.TYPE_IMG_START -> {
                                    curId = Proto.u16(data, p)
                                    curTotal = Proto.u32(data, p + 2).toInt()
                                    chunks.clear()
                                }
                                Proto.TYPE_IMG_CHUNK -> {
                                    if (Proto.u16(data, p) == curId) {
                                        chunks[Proto.u16(data, p + 2)] = data.copyOfRange(p + 4, p + plen)
                                    }
                                }
                                Proto.TYPE_IMG_END -> {
                                    if (Proto.u16(data, p) == curId && chunks.isNotEmpty()) {
                                        val blob = java.io.ByteArrayOutputStream()
                                        for (k in chunks.keys.sorted()) blob.write(chunks[k]!!)
                                        val jb = blob.toByteArray()
                                        if (jb.size == curTotal) {
                                            val bmp = BitmapFactory.decodeByteArray(jb, 0, jb.size)
                                            if (bmp != null) { imgs++; cb.onImage(bmp, jb.size / 1024) }
                                        }
                                    }
                                    chunks.clear(); curId = -1
                                }
                                Proto.TYPE_PONG -> {
                                    val rtt = (SystemClock.elapsedRealtimeNanos() - pingT0) / 1e6
                                    cb.onRtt(rtt)
                                }
                            }
                            pos += 10 + plen
                        }
                        if (pos > 0) {
                            acc.reset()
                            acc.write(data, pos, data.size - pos)
                        }
                    }
                }
            } catch (e: Exception) {
                if (running) cb.onStatus("WiFi TCP 재접속 대기 (${e.javaClass.simpleName})")
                SystemClock.sleep(1000)
            }
        }
    }
}
