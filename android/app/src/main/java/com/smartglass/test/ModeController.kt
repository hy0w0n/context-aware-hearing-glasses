package com.smartglass.test

import android.os.SystemClock

/**
 * 모드 결정기 (통합 3단계, proto.md §5.5) — 앙상블의 골격.
 * 우선순위: ① 통화 오버라이드 > ② 수동 명령(버튼/음성, 30초 유지) > ③ 자동(이미지 앙상블)
 * 현 단계는 이미지 단독 결정 + 방향 히스토그램(일대일/다인 분기).
 * 소리 모델(안호연) 도착 시 decide()에 soundClass 인자만 추가하면 §7.4 2D 매핑으로 확장.
 */
class ModeController(
    private val onLog: (String) -> Unit,          // 이벤트 로그 (UI 스레드에서 호출됨)
    private val sendCmd: (ByteArray) -> Unit,     // CMD payload → 전송로
    private val onUiState: (String) -> Unit       // 자동모드 상태줄 갱신
) {
    companion object {
        val MODE_NAMES = arrayOf("실내", "야외", "TV", "일대일", "다인")
        private const val MANUAL_HOLD_MS = 30_000L   // 수동 설정 후 자동 억제 시간
        private const val ALERT_THROTTLE_MS = 10_000L
        private const val CALL_VOL_DELTA = 3          // 통화 시 볼륨 감소량 (종료 시 복원)
    }

    var autoMode = false
        private set
    private var currentMode = 0        // 마지막으로 발행한(=안경에 있는) 모드
    private var manualHoldUntil = 0L
    private var inCall = false
    private var preCallMode = -1
    private var lastAlertAt = 0L
    private var lastAutoInfo = "판정 대기"

    // 소리 분류 상태 (통합 4단계 — 호연 모델): "danger"/"noise"/"speech"/"silence", 5초 내 값만 유효
    private var lastSound = "silence"
    private var lastSoundAt = 0L

    /** 소리 분류 유입 (2초 주기). cls=null은 무음 */
    fun onSoundResult(cls: String?) {
        lastSound = cls ?: "silence"
        lastSoundAt = SystemClock.elapsedRealtime()
        // 위험 소리는 이미지와 무관하게 즉시 OLED 알림 (§7.4 위험 열 — DOA 방위각 결합)
        if (cls == "danger") fireAlert("🔊 위험 소리 감지")
    }

    private fun soundNow(): String =
        if (SystemClock.elapsedRealtime() - lastSoundAt < 5000) lastSound else "silence"

    private fun fireAlert(why: String) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastAlertAt <= ALERT_THROTTLE_MS) return
        lastAlertAt = now
        val az = Proto.lastAzimuth01
        sendCmd(byteArrayOf(Proto.CMD_DISPLAY_ALERT.toByte(),
            (az and 0xFF).toByte(), ((az shr 8) and 0xFF).toByte(),
            4 /*일반경고*/, 0xB8.toByte(), 0x0B /*3000ms LE*/))
        onLog("⚠ $why → OLED 알림 (az=${az / 10.0}°)")
    }

    private fun setMode(m: Int) {
        currentMode = m
        sendCmd(byteArrayOf(Proto.CMD_SET_MODE.toByte(), m.toByte()))
    }

    /** 자동/수동 전환 (앱 토글·음성 AUTO_MODE_ON/OFF 공용) */
    fun setAuto(on: Boolean) {
        autoMode = on
        onLog(if (on) "🧠 자동 모드 ON — 이미지 판정으로 모드 자동 전환" else "🧠 자동 모드 OFF (수동)")
        refreshUi()
    }

    /** 수동 모드 변경 (앱 모드 버튼·음성 MODE_* 인텐트) — 자동을 30초 억제 */
    fun manualMode(m: Int, source: String) {
        if (inCall) { onLog("📞 통화 중 — 모드 변경 보류"); return }
        setMode(m)
        manualHoldUntil = SystemClock.elapsedRealtime() + MANUAL_HOLD_MS
        onLog("$source → ${MODE_NAMES[m]}($m) 전송 (자동 30초 억제)")
        refreshUi()
    }

    /** 볼륨 등 모드 외 수동 명령 통과용 */
    fun manualCmd(payload: ByteArray, desc: String) {
        sendCmd(payload)
        onLog("$desc 전송")
    }

    /** 통화 상태 변화 (AudioManager 감지 — 최상위 오버라이드, proto.md §5.5) */
    fun callChanged(active: Boolean) {
        if (active == inCall) return
        inCall = active
        if (active) {
            preCallMode = currentMode
            setMode(3)  // 통화 프리셋 = 일대일 + 볼륨 감소 (펌웨어 무수정 조합)
            sendCmd(byteArrayOf(Proto.CMD_SET_VOLUME.toByte(), (-CALL_VOL_DELTA).toByte()))
            onLog("📞 통화 감지 → 통화 프리셋(일대일+VOL-$CALL_VOL_DELTA)")
        } else {
            sendCmd(byteArrayOf(Proto.CMD_SET_VOLUME.toByte(), CALL_VOL_DELTA.toByte()))
            if (preCallMode in 0..4) setMode(preCallMode)
            onLog("📞 통화 종료 → ${MODE_NAMES[currentMode]} 복원")
        }
        refreshUi()
    }

    /** 이미지 분류 결과 유입 (3초 주기) — §7.4 2차원 앙상블(이미지×소리), 통합 4단계에서 확장 */
    fun onImageResult(stableClass: String, dirHist: ByteArray, doaConf: Int) {
        val sound = soundNow()   // "speech"/"danger"/"noise"/"silence" (호연 모델 + 무음 자체판정)
        // 위험 이미지: 모드 변경 대신 OLED 알림 (소리도 위험이면 '강' — §7.4 위험 행)
        if (stableClass == "danger") {
            if (autoMode) fireAlert(if (sound == "danger") "📷+🔊 위험(이미지+소리 일치)" else "📷 위험 장면")
            lastAutoInfo = "⚠ 위험 감지 (소리: $sound)"
            refreshUi(); return
        }
        if (!autoMode) { lastAutoInfo = "판정: $stableClass·$sound (자동 OFF)"; refreshUi(); return }
        if (inCall) { lastAutoInfo = "통화 중 — 자동 정지"; refreshUi(); return }
        if (SystemClock.elapsedRealtime() < manualHoldUntil) {
            lastAutoInfo = "판정: $stableClass·$sound (수동 유지 중)"; refreshUi(); return
        }
        // 일대일/다인 분기: 방향 히스토그램 다양성 (Q5 사용안)
        val conv = if (doaConf > 0 && histDiversity(dirHist) >= 2) 4 else 3
        val target = when (stableClass) {
            "conversation" -> if (sound == "silence") 0 else conv   // 대화 장면+무음 → 실내기본
            "tv" -> 2
            "indoor" -> if (sound == "speech") 3 else 0             // 실내+대화 소리 → 일대일
            "outdoor" -> 1
            "negative" -> if (sound == "speech") 3 else -1          // 장면 불명 → 소리만으로 판단
            else -> -1                                              // 유지
        }
        if (target >= 0 && target != currentMode) {
            setMode(target)
            onLog("🧠 자동: $stableClass·$sound → ${MODE_NAMES[target]}($target) 전송")
        }
        lastAutoInfo = "판정: $stableClass·$sound → ${if (target >= 0) MODE_NAMES[target] else "유지"}"
        refreshUi()
    }

    /** 활성 방향 빈 수 — 화자가 여러 방향에서 잡히면 다인 (빈 카운트 ≥2 기준) */
    private fun histDiversity(hist: ByteArray): Int =
        hist.count { (it.toInt() and 0xFF) >= 2 }

    private fun refreshUi() {
        val head = when {
            inCall -> "📞 통화 오버라이드"
            !autoMode -> "수동"
            SystemClock.elapsedRealtime() < manualHoldUntil -> "자동(수동 유지 중)"
            else -> "자동"
        }
        onUiState("$head · 현재 모드: ${MODE_NAMES[currentMode]} · $lastAutoInfo")
    }
}
