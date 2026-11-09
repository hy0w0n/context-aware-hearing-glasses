package com.example.nlp; // 본인 패키지명 확인!

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.util.ArrayList;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "NLP_DEBUG";
    private static final int PERMISSION_REQUEST_CODE = 1000;

    private TextView tvStatus;
    private TextView tvChatLog;
    private ScrollView scrollView;
    private Button btnRecord;

    private SpeechRecognizer speechRecognizer;
    private Intent speechRecognizerIntent;
    private OkHttpClient client;

    // 💡 1. API_KEY는 저장소 루트 .env의 LLM_API_KEY에서 읽음 (git에 올리지 않음)
    private static final String API_KEY = BuildConfig.LLM_API_KEY;

    // 💡 2. 학교 API Base URL
    private static final String API_URL = "https://factchat-cloud.mindlogic.ai/v1/gateway/chat/completions";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvStatus = findViewById(R.id.tv_status);
        tvChatLog = findViewById(R.id.tv_chat_log);
        scrollView = findViewById(R.id.scroll_view);
        btnRecord = findViewById(R.id.btn_record);

        client = new OkHttpClient();

        checkAudioPermission();
        initSpeechRecognizer();

        btnRecord.setOnClickListener(v -> startListening());

        appendLog("🤖 AI: 안녕하세요! 스마트 보청기입니다.\n버튼을 누르고 말씀해주세요.\n-------------------------");
    }

    private void appendLog(String message) {
        tvChatLog.append(message + "\n");
        scrollView.post(() -> scrollView.fullScroll(View.FOCUS_DOWN));
    }

    private void checkAudioPermission() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, PERMISSION_REQUEST_CODE);
        }
    }

    private void initSpeechRecognizer() {
        speechRecognizer = SpeechRecognizer.createSpeechRecognizer(this);
        speechRecognizerIntent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        speechRecognizerIntent.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
        speechRecognizerIntent.putExtra(RecognizerIntent.EXTRA_LANGUAGE, "ko-KR");
        speechRecognizerIntent.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);

        speechRecognizer.setRecognitionListener(new RecognitionListener() {
            @Override
            public void onReadyForSpeech(Bundle params) {
                tvStatus.setText("듣고 있어요... 또박또박 말씀하세요!");
            }
            @Override
            public void onBeginningOfSpeech() { tvStatus.setText("음성 입력 중..."); }
            @Override
            public void onEndOfSpeech() { tvStatus.setText("음성 입력 완료. 서버 전송 준비..."); }

            @Override
            public void onError(int error) {
                String errorMsg;
                switch (error) {
                    case SpeechRecognizer.ERROR_NO_MATCH: errorMsg = "인식 실패 (무슨 말인지 못 들었어요)"; break;
                    case SpeechRecognizer.ERROR_SPEECH_TIMEOUT: errorMsg = "아무 말도 하지 않았습니다."; break;
                    case 9: errorMsg = "마이크 권한 문제 (앱 설정에서 켜주세요)"; break;
                    default: errorMsg = "에러 코드: " + error; break;
                }
                tvStatus.setText("🔴 " + errorMsg);
            }

            @Override
            public void onResults(Bundle results) {
                ArrayList<String> matches = results.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
                if (matches != null && !matches.isEmpty()) {
                    String spokenText = matches.get(0);

                    // 1. 내가 한 말을 먼저 찍음
                    appendLog("👤 나: \"" + spokenText + "\"");

                    // 2. 서버로 전송!
                    sendTextToLLM(spokenText);
                }
            }

            @Override
            public void onPartialResults(Bundle partialResults) {
                ArrayList<String> matches = partialResults.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
                if (matches != null && !matches.isEmpty()) {
                    tvStatus.setText("실시간: \"" + matches.get(0) + "\"");
                }
            }
            @Override public void onRmsChanged(float rmsdB) {}
            @Override public void onBufferReceived(byte[] buffer) {}
            @Override public void onEvent(int eventType, Bundle params) {}
        });
    }

    private void startListening() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            checkAudioPermission();
            return;
        }
        speechRecognizer.startListening(speechRecognizerIntent);
    }

    private void sendTextToLLM(String userText) {
        tvStatus.setText("AI가 생각 중입니다... 🧠");

        try {
            // 프롬프트 세팅
            String systemPrompt = "너는 보청기를 제어하는 AI야. 사용자의 말을 듣고 반드시 다음 단어들 중 '하나만' 대답해. 부가 설명은 절대 하지마.\n" +
                    "- 소리를 키워달라면: 'VOLUME_UP'\n" +
                    "- 소리를 줄여달라면: 'VOLUME_DOWN'\n" +
                    "- TV/테레비 본다면: 'MODE_TV'\n" +
                    "- 한 명이랑 대화한다면: 'MODE_ONE_ON_ONE'\n" +
                    "- 여러명이랑 대화한다면: 'MODE_MULTI_TALK'\n" +
                    "- 전화/통화 한다면: 'MODE_CALL'\n" +
                    "- 밖/야외/산책 나간다면: 'MODE_OUTDOOR'\n" +
                    "- 자동 모드 켜달라면: 'AUTO_MODE_ON'\n" +
                    "- 수동으로 바꾼다거나 자동 모드 꺼달라면: 'AUTO_MODE_OFF'\n" +
                    "- 관련 없는 말이면: 'UNKNOWN'";

            JSONObject systemMessage = new JSONObject();
            systemMessage.put("role", "system");
            systemMessage.put("content", systemPrompt);

            JSONObject userMessage = new JSONObject();
            userMessage.put("role", "user");
            userMessage.put("content", userText);

            JSONArray messagesArray = new JSONArray();
            messagesArray.put(systemMessage);
            messagesArray.put(userMessage);

            // 💡 3. JSON 규격 조립 (모델 이름 claude-sonnet-5 로 변경 완료!!)
            JSONObject requestBodyJson = new JSONObject();
            requestBodyJson.put("model", "claude-sonnet-5");
            requestBodyJson.put("messages", messagesArray);
            requestBodyJson.put("temperature", 0.0);

            RequestBody body = RequestBody.create(
                    MediaType.parse("application/json; charset=utf-8"),
                    requestBodyJson.toString()
            );

            // 💡 4. 정확한 헤더 규격 (Bearer + 띄어쓰기 1칸 + API_KEY)
            Request request = new Request.Builder()
                    .url(API_URL)
                    .addHeader("Authorization", "Bearer " + API_KEY)
                    .addHeader("Content-Type", "application/json")
                    .post(body)
                    .build();

            client.newCall(request).enqueue(new Callback() {
                @Override
                public void onFailure(@NonNull Call call, @NonNull IOException e) {
                    runOnUiThread(() -> {
                        appendLog("🤖 AI: 🔴 서버 연결 실패 (네트워크를 확인하세요)\n-------------------------");
                        tvStatus.setText("대기 중...");
                    });
                }

                @Override
                public void onResponse(@NonNull Call call, @NonNull Response response) throws IOException {
                    // 서버가 뱉어내는 원문 메시지
                    String responseData = response.body() != null ? response.body().string() : "응답 없음";

                    if (response.isSuccessful()) {
                        try {
                            JSONObject jsonObject = new JSONObject(responseData);
                            JSONArray choices = jsonObject.getJSONArray("choices");
                            String intent = choices.getJSONObject(0).getJSONObject("message").optString("content", "UNKNOWN").trim();

                            runOnUiThread(() -> updateResultUI(intent));
                        } catch (Exception e) {
                            runOnUiThread(() -> {
                                appendLog("🤖 AI: 🔴 결과 파싱 에러\n원문: " + responseData + "\n-------------------------");
                                tvStatus.setText("대기 중...");
                            });
                        }
                    } else {
                        final int code = response.code();
                        runOnUiThread(() -> {
                            appendLog("🤖 AI: 🔴 API 에러 (" + code + ")\n원인: " + responseData + "\n-------------------------");
                            tvStatus.setText("대기 중...");
                        });
                    }
                }
            });
        } catch (Exception e) {
            Log.e(TAG, "Request 에러", e);
        }
    }

    private void updateResultUI(String intent) {
        String aiResponse = "";

        if (intent.contains("UNKNOWN")) {
            aiResponse = "🤖 AI: 🔴 알 수 없는 명령 (다시 말씀해주세요)";
        } else {
            String actionKor = "";
            if (intent.contains("VOLUME_UP")) actionKor = "[소리 키움]";
            else if (intent.contains("VOLUME_DOWN")) actionKor = "[소리 줄임]";
            else if (intent.contains("MODE_TV")) actionKor = "[TV 시청 모드]로 변경";
            else if (intent.contains("MODE_ONE_ON_ONE")) actionKor = "[일대일 대화 모드]로 변경";
            else if (intent.contains("MODE_MULTI_TALK")) actionKor = "[다인 대화 모드]로 변경";
            else if (intent.contains("MODE_CALL")) actionKor = "[통화 모드]로 변경";
            else if (intent.contains("MODE_OUTDOOR")) actionKor = "[야외 모드]로 변경";
            else if (intent.contains("AUTO_MODE_ON")) actionKor = "[자동 모드 켜짐]";
            else if (intent.contains("AUTO_MODE_OFF")) actionKor = "[수동 전환]";
            else actionKor = intent;

            aiResponse = "🤖 AI: 🟢 실행 ➔ " + actionKor;
        }

        appendLog(aiResponse + "\n-------------------------");
        tvStatus.setText("대기 중...");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (speechRecognizer != null) {
            speechRecognizer.destroy();
        }
    }
}
