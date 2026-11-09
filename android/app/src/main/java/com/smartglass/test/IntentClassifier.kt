package com.smartglass.test

import android.content.Intent
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import androidx.appcompat.app.AppCompatActivity
import okhttp3.Call
import okhttp3.Callback
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException

/**
 * PTT 음성명령 — 송채원 ptt_module 이식 (통합 2단계, proto.md §5.5)
 * 흐름: 안경 PTT PRESS(EVENT) → 폰 SpeechRecognizer(ko) STT → 학교 LLM API 인텐트 분류
 *       → 매핑표 A → CMD 발행(SET_MODE/SET_VOLUME). RELEASE → 인식 종료(구 스펙: press=시작).
 * STT가 폰 마이크인 이유: ESP32-S3에 HFP 없음 + SpeechRecognizer는 외부 PCM 주입 불가 (CLAUDE.md §7.3)
 */
class IntentClassifier(
    private val activity: AppCompatActivity,
    private val onLog: (String) -> Unit,                  // 이벤트 로그 출력 (UI 스레드 보장 안 함)
    private val onCmd: (ByteArray, String) -> Unit,       // CMD payload + 설명 → 전송로로
    private val onAutoMode: (Boolean) -> Unit             // AUTO_MODE_ON/OFF → 앱 상태 (3단계에서 소비)
) {
    companion object {
        // 학교 LLM 게이트웨이 — 키는 저장소 루트 .env의 LLM_API_KEY (git 제외) → BuildConfig
        private val API_KEY = BuildConfig.LLM_API_KEY
        private const val API_URL = "https://factchat-cloud.mindlogic.ai/v1/gateway/chat/completions"
        // 채원 프롬프트 원본 유지 (인텐트 10종 — Q12 모드 슬롯 해소의 핵심)
        private const val SYSTEM_PROMPT =
            "너는 보청기를 제어하는 AI야. 사용자의 말을 듣고 반드시 다음 단어들 중 '하나만' 대답해. 부가 설명은 절대 하지마.\n" +
            "- 소리를 키워달라면: 'VOLUME_UP'\n- 소리를 줄여달라면: 'VOLUME_DOWN'\n" +
            "- TV/테레비 본다면: 'MODE_TV'\n- 한 명이랑 대화한다면: 'MODE_ONE_ON_ONE'\n" +
            "- 여러명이랑 대화한다면: 'MODE_MULTI_TALK'\n- 전화/통화 한다면: 'MODE_CALL'\n" +
            "- 밖/야외/산책 나간다면: 'MODE_OUTDOOR'\n- 자동 모드 켜달라면: 'AUTO_MODE_ON'\n" +
            "- 수동으로 바꾼다거나 자동 모드 꺼달라면: 'AUTO_MODE_OFF'\n- 관련 없는 말이면: 'UNKNOWN'"
    }

    private var recognizer: SpeechRecognizer? = null
    private val client = OkHttpClient()
    @Volatile private var listening = false

    /** 안경 PTT PRESS(또는 앱 음성 버튼) → STT 시작. UI 스레드에서 호출할 것 */
    fun onPttPress() {
        if (listening) return
        if (!SpeechRecognizer.isRecognitionAvailable(activity)) {
            onLog("🎤 이 폰은 음성인식(SpeechRecognizer) 미지원")
            return
        }
        ensureRecognizer()
        val i = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
            putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            putExtra(RecognizerIntent.EXTRA_LANGUAGE, "ko-KR")
            putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
        }
        listening = true
        recognizer?.startListening(i)
        onLog("🎤 듣는 중... (버튼 떼면 종료)")
    }

    /** PTT RELEASE → 인식 마감(들은 데까지 결과 냄). UI 스레드에서 호출할 것 */
    fun onPttRelease() {
        if (listening) recognizer?.stopListening()
    }

    fun destroy() {
        recognizer?.destroy()
        recognizer = null
    }

    private fun ensureRecognizer() {
        if (recognizer != null) return
        recognizer = SpeechRecognizer.createSpeechRecognizer(activity).apply {
            setRecognitionListener(object : RecognitionListener {
                override fun onResults(results: Bundle) {
                    listening = false
                    val t = results.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()
                    if (t.isNullOrBlank()) { onLog("🎤 인식 결과 없음"); return }
                    onLog("👤 \"$t\"")
                    classify(t)
                }
                override fun onError(error: Int) {
                    listening = false
                    val msg = when (error) {
                        SpeechRecognizer.ERROR_NO_MATCH -> "인식 실패 (다시 또박또박)"
                        SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "말이 없었음"
                        SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "마이크 권한 필요"
                        else -> "STT 에러 $error"
                    }
                    onLog("🎤 $msg")
                }
                override fun onPartialResults(p: Bundle) {
                    p.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()
                        ?.let { if (it.isNotBlank()) onLog("🎤 …$it") }
                }
                override fun onReadyForSpeech(params: Bundle?) {}
                override fun onBeginningOfSpeech() {}
                override fun onEndOfSpeech() { onLog("🎤 입력 완료 → 인텐트 분석 중...") }
                override fun onRmsChanged(rmsdB: Float) {}
                override fun onBufferReceived(buffer: ByteArray?) {}
                override fun onEvent(eventType: Int, params: Bundle?) {}
            })
        }
    }

    /** STT 텍스트 → LLM 인텐트 분류 (ptt_module sendTextToLLM 이식) */
    private fun classify(userText: String) {
        val body = JSONObject().apply {
            put("model", "claude-sonnet-5")
            put("temperature", 0.0)
            put("messages", JSONArray()
                .put(JSONObject().put("role", "system").put("content", SYSTEM_PROMPT))
                .put(JSONObject().put("role", "user").put("content", userText)))
        }.toString().toRequestBody("application/json; charset=utf-8".toMediaType())

        val req = Request.Builder().url(API_URL)
            .addHeader("Authorization", "Bearer $API_KEY")
            .addHeader("Content-Type", "application/json")
            .post(body).build()

        client.newCall(req).enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                onLog("🤖 LLM 연결 실패 (${e.message}) — 폰 인터넷 확인")
            }
            override fun onResponse(call: Call, response: Response) {
                val text = response.body?.string() ?: ""
                if (!response.isSuccessful) { onLog("🤖 LLM API 에러 ${response.code}"); return }
                val intent = try {
                    JSONObject(text).getJSONArray("choices").getJSONObject(0)
                        .getJSONObject("message").optString("content", "UNKNOWN").trim()
                } catch (e: Exception) { onLog("🤖 응답 파싱 실패"); return }
                activity.runOnUiThread { mapIntent(intent) }
            }
        })
    }

    /** 매핑표 A (proto.md §5.5): 인텐트 → CMD/앱 상태 */
    private fun mapIntent(intent: String) {
        fun mode(m: Int, name: String) =
            onCmd(byteArrayOf(Proto.CMD_SET_MODE.toByte(), m.toByte()), "🤖 $intent → $name 모드($m)")
        when {
            intent.contains("VOLUME_UP") ->
                onCmd(byteArrayOf(Proto.CMD_SET_VOLUME.toByte(), 3), "🤖 VOLUME_UP → VOL+3")
            intent.contains("VOLUME_DOWN") ->
                onCmd(byteArrayOf(Proto.CMD_SET_VOLUME.toByte(), (-3).toByte()), "🤖 VOLUME_DOWN → VOL-3")
            intent.contains("MODE_TV") -> mode(2, "TV")
            intent.contains("MODE_ONE_ON_ONE") -> mode(3, "일대일")
            intent.contains("MODE_MULTI_TALK") -> mode(4, "다인")
            intent.contains("MODE_OUTDOOR") -> mode(1, "야외")
            intent.contains("MODE_CALL") -> mode(3, "통화(→일대일 매핑)")  // 3단계에서 통화 핸들러로 승격
            intent.contains("AUTO_MODE_ON") -> { onAutoMode(true); onLog("🤖 자동 모드 ON") }
            intent.contains("AUTO_MODE_OFF") -> { onAutoMode(false); onLog("🤖 자동 모드 OFF (수동)") }
            else -> onLog("🤖 알 수 없는 명령 (UNKNOWN) — 다시 말씀해주세요")
        }
    }
}
