package com.example.smarthearing;

import android.Manifest;
import android.content.pm.PackageManager;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.Locale;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.MediaType;
import okhttp3.MultipartBody;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;

public class MainActivity extends AppCompatActivity {

    private MediaRecorder recorder;
    private boolean isRecording = false;
    private TextToSpeech tts;
    private File audioFile;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // 1. 앱 켜자마자 마이크 사용 허락받기 (필수!)
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, 0);
        }

        // 2. 글씨를 소리로 읽어주는 기능(TTS) 준비
        tts = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int status) {
                if (status == TextToSpeech.SUCCESS) {
                    tts.setLanguage(Locale.KOREAN); // 한국어로 설정
                }
            }
        });

        Button btnRecord = findViewById(R.id.btn_record);
        TextView tvResult = findViewById(R.id.tv_result);

        // 3. 스마트폰 안의 숨겨진 폴더에 녹음 파일 저장할 준비
        audioFile = new File(getExternalCacheDir(), "test_audio.3gp");

        // 4. 버튼을 눌렀을 때의 행동
        btnRecord.setOnClickListener(v -> {
            if (!isRecording) {
                // 녹음 시작!
                startRecording();
                btnRecord.setText("녹음 중지\n(분석 시작)");
                tvResult.setText("🎙️ 녹음 중입니다...");
                isRecording = true;
            } else {
                // 녹음 끝! 서버로 전송
                stopRecording();
                btnRecord.setText("녹음 시작");
                tvResult.setText("📡 서버에서 AI가 분석 중...");
                isRecording = false;

                sendAudioToServer();
            }
        });
    }

    private void startRecording() {
        recorder = new MediaRecorder();
        recorder.setAudioSource(MediaRecorder.AudioSource.MIC);
        recorder.setOutputFormat(MediaRecorder.OutputFormat.THREE_GPP);
        recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AMR_NB);
        recorder.setOutputFile(audioFile.getAbsolutePath());

        try {
            recorder.prepare();
            recorder.start();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    private void stopRecording() {
        if (recorder != null) {
            recorder.stop();
            recorder.release();
            recorder = null;
        }
    }

    private void sendAudioToServer() {
        // ⭐ 주의: 아래 192.168.X.X 부분을 호연님의 진짜 IP 주소로 꼭 바꿔주세요!
        String serverUrl = "http://172.18.41.40:8000/api/analyze_sound";

        RequestBody requestBody = new MultipartBody.Builder()
                .setType(MultipartBody.FORM)
                .addFormDataPart("audio_file", audioFile.getName(),
                        RequestBody.create(audioFile, MediaType.parse("audio/*")))
                .build();

        Request request = new Request.Builder()
                .url(serverUrl)
                .post(requestBody)
                .build();

        OkHttpClient client = new OkHttpClient();
        client.newCall(request).enqueue(new Callback() {
            @Override
            public void onFailure(@NonNull Call call, @NonNull IOException e) {
                // 통신 실패했을 때
                runOnUiThread(() -> {
                    TextView tvResult = findViewById(R.id.tv_result);
                    tvResult.setText("❌ 서버 연결 실패\nIP 주소를 확인하세요!");
                });
            }

            @Override
            public void onResponse(@NonNull Call call, @NonNull Response response) throws IOException {
                // 서버에서 무사히 정답이 돌아왔을 때
                if (response.isSuccessful() && response.body() != null) {
                    String resultString = response.body().string();

                    try {
                        // 서버가 보낸 데이터 덩어리에서 정답 글씨만 쏙 빼오기
                        JSONObject jsonObject = new JSONObject(resultString);
                        String aiResult = jsonObject.getString("ai_result");

                        // 화면에 띄우고 + 소리내서 읽어주기 (메인 쓰레드에서 실행)
                        runOnUiThread(() -> {
                            TextView tvResult = findViewById(R.id.tv_result);
                            tvResult.setText("✅ 분석 결과:\n" + aiResult);

                            String speakText = aiResult + " 가 감지되었습니다.";
                            tts.speak(speakText, TextToSpeech.QUEUE_FLUSH, null, null);
                        });
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                }
            }
        });
    }

    @Override
    protected void onDestroy() {
        if (tts != null) {
            tts.stop();
            tts.shutdown();
        }
        super.onDestroy();
    }
}
