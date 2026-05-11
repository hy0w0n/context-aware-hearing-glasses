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

    // 에뮬레이터 테스트용 주소
    private final String SERVER_URL =
            "http://10.0.2.2:8000/analyze-audio";

    @Override
    protected void onCreate(Bundle savedInstanceState) {

        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        btnRecord = findViewById(R.id.btnRecord);
        tvResult = findViewById(R.id.tvResult);

        audioFilePath =
                getExternalCacheDir().getAbsolutePath()
                        + "/test_audio.m4a";

        // 마이크 권한 요청
        if (ContextCompat.checkSelfPermission(
                this,
                Manifest.permission.RECORD_AUDIO
        ) != PackageManager.PERMISSION_GRANTED) {

            ActivityCompat.requestPermissions(
                    this,
                    new String[]{Manifest.permission.RECORD_AUDIO},
                    1
            );
        }

        btnRecord.setOnClickListener(v -> {

            if (!isRecording) {
                startRecording();
            } else {
                stopRecording();
            }
        });
    }

    // 녹음 시작
    private void startRecording() {

        mediaRecorder = new MediaRecorder();

        mediaRecorder.setAudioSource(
                MediaRecorder.AudioSource.MIC
        );

        mediaRecorder.setOutputFormat(
                MediaRecorder.OutputFormat.MPEG_4
        );

        mediaRecorder.setOutputFile(audioFilePath);

        mediaRecorder.setAudioEncoder(
                MediaRecorder.AudioEncoder.AAC
        );

        try {

            mediaRecorder.prepare();
            mediaRecorder.start();

            isRecording = true;

            btnRecord.setText("녹음 중지 및 서버 전송");

            tvResult.setText("내 목소리 듣는 중...");

        } catch (IOException e) {

            e.printStackTrace();

            tvResult.setText("녹음 시작 실패");
        }
    }

    // 녹음 종료
    private void stopRecording() {

        if (mediaRecorder != null) {

            try {

                mediaRecorder.stop();

            } catch (Exception e) {

                e.printStackTrace();
            }

            mediaRecorder.release();

            mediaRecorder = null;

            isRecording = false;

            btnRecord.setText("녹음 시작");

            tvResult.setText("서버에서 받아쓰기 중...");

            sendFileToServer();
        }
    }

    // 서버로 파일 전송
    private void sendFileToServer() {

        File file = new File(audioFilePath);

        if (!file.exists()) {

            tvResult.setText("녹음 파일이 존재하지 않습니다.");

            return;
        }

        Log.d("FILE_PATH", file.getAbsolutePath());
        Log.d("FILE_SIZE", String.valueOf(file.length()));

        OkHttpClient client = new OkHttpClient.Builder()
                .connectTimeout(60, java.util.concurrent.TimeUnit.SECONDS)
                .readTimeout(120, java.util.concurrent.TimeUnit.SECONDS)
                .writeTimeout(120, java.util.concurrent.TimeUnit.SECONDS)
                .build();

        RequestBody fileBody = RequestBody.create(
                MediaType.parse("audio/m4a"),
                file
        );

        RequestBody requestBody =
                new MultipartBody.Builder()
                        .setType(MultipartBody.FORM)
                        .addFormDataPart(
                                "audio_file",
                                file.getName(),
                                fileBody
                        )
                        .build();

        Request request = new Request.Builder()
                .url(SERVER_URL)
                .post(requestBody)
                .build();

        client.newCall(request).enqueue(new Callback() {

            // 서버 연결 실패
            @Override
            public void onFailure(Call call, IOException e) {

                e.printStackTrace();

                runOnUiThread(() ->
                        tvResult.setText(
                                "연결 실패:\n" + e.toString()
                        )
                );
            }

            // 서버 응답 성공
            @Override
            public void onResponse(
                    Call call,
                    Response response
            ) throws IOException {

                try {

                    if (response.body() == null) {

                        runOnUiThread(() ->
                                tvResult.setText("응답 body가 비어있음")
                        );

                        return;
                    }

                    String responseData =
                            response.body().string();

                    Log.d(
                            "SERVER_RESPONSE",
                            responseData
                    );

                    if (response.isSuccessful()) {

                        JSONObject jsonObject =
                                new JSONObject(responseData);

                        String situation =
                                jsonObject.optString(
                                        "situation",
                                        "없음"
                                );

                        String recognizedText =
                                jsonObject.optString(
                                        "text",
                                        "없음"
                                );

                        runOnUiThread(() ->
                                tvResult.setText(
                                        "들린 내용:\n"
                                                + recognizedText
                                                + "\n\n결과: "
                                                + situation
                                )
                        );

                    } else {

                        runOnUiThread(() ->
                                tvResult.setText(
                                        "서버 에러:\n"
                                                + responseData
                                )
                        );
                    }

                } catch (Exception e) {

                    e.printStackTrace();

                    runOnUiThread(() ->
                            tvResult.setText(
                                    "JSON 오류:\n"
                                            + e.toString()
                            )
                    );
                }
            }
        });
    }
}