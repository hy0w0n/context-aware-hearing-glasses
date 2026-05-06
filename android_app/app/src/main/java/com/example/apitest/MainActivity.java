package com.example.apitest;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import android.Manifest;
import android.content.pm.PackageManager;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;
import org.json.JSONObject;
import java.io.File;
import java.io.IOException;
import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.MediaType;
import okhttp3.MultipartBody;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;

public class MainActivity extends AppCompatActivity {
    private Button btnRecord;
    private TextView tvResult;
    private MediaRecorder mediaRecorder;
    private String audioFilePath;
    private boolean isRecording = false;

    // 🚨 [아주 중요!] 실행 중인 서버의 진짜 IP로 바꿔야 함! (예: http://192.168.0.x:8000/api/stt)
    private final String SERVER_URL = "http://10.0.2.2:8000/analyze-audio"; // 에뮬레이터 테스트용

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        btnRecord = findViewById(R.id.btnRecord);
        tvResult = findViewById(R.id.tvResult);
        audioFilePath = getExternalCacheDir().getAbsolutePath() + "/test_audio.m4a";

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, 1);
        }

        btnRecord.setOnClickListener(v -> {
            if (!isRecording) startRecording();
            else stopRecording();
        });
    }

    private void startRecording() {
        mediaRecorder = new MediaRecorder();
        mediaRecorder.setAudioSource(MediaRecorder.AudioSource.MIC);
        mediaRecorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
        mediaRecorder.setOutputFile(audioFilePath);
        mediaRecorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
        try {
            mediaRecorder.prepare();
            mediaRecorder.start();
            isRecording = true;
            btnRecord.setText("녹음 중지 및 서버 전송");
            tvResult.setText("내 목소리 듣는 중...");
        } catch (IOException e) { e.printStackTrace(); }
    }

    private void stopRecording() {
        if (mediaRecorder != null) {
            mediaRecorder.stop();
            mediaRecorder.release();
            mediaRecorder = null;
            isRecording = false;
            btnRecord.setText("녹음 시작");
            tvResult.setText("서버에서 받아쓰기 중...");
            sendFileToServer();
        }
    }

    private void sendFileToServer() {
        File file = new File(audioFilePath);
        OkHttpClient client = new OkHttpClient();

        RequestBody requestBody = new MultipartBody.Builder().setType(MultipartBody.FORM)
                .addFormDataPart("audio_file", file.getName(),
                        RequestBody.create(MediaType.parse("audio/mp4"), file)).build();

        Request request = new Request.Builder().url(SERVER_URL).post(requestBody).build();

        client.newCall(request).enqueue(new Callback() {
            @Override
            public void onFailure(Call call, IOException e) {
                runOnUiThread(() -> tvResult.setText("연결 실패! IP를 확인하세요."));
            }

            @Override
            public void onResponse(Call call, Response response) throws IOException {
                if (response.isSuccessful()) {
                    try {
                        String responseData = response.body().string();
                        JSONObject jsonObject = new JSONObject(responseData);

                        String situation = jsonObject.getString("situation");
                        String recognizedText = jsonObject.getString("text");

                        runOnUiThread(() -> tvResult.setText("들린 내용: " + recognizedText + "\n결과: " + situation));
                    } catch (Exception e) { e.printStackTrace(); }
                } else {
                    runOnUiThread(() -> tvResult.setText("서버 에러!"));
                }
            }
        });
    }
}