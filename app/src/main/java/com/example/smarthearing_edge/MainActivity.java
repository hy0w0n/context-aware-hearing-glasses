package com.example.smarthearing_edge;

import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.media.audiofx.AutomaticGainControl;
import android.media.audiofx.NoiseSuppressor;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.util.LinkedHashMap;
import java.util.Map;

public class MainActivity extends AppCompatActivity {

    static {
        System.loadLibrary("native-lib");
    }

    private native String classifyAudio(float[] audioData);

    private static final int SAMPLE_RATE = 16000;
    private static final int RECORD_TIME_SEC = 3;
    private static final int TOTAL_SAMPLES = SAMPLE_RATE * RECORD_TIME_SEC;
    private static final int MODEL_INPUT_SIZE = 16000;

    private TextView resultTextView;
    private Button recordButton;
    private boolean isRecording = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        resultTextView = findViewById(R.id.resultTextView);
        recordButton = findViewById(R.id.recordButton);

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, 1);
        }

        recordButton.setOnClickListener(v -> {
            if (!isRecording) {
                startRecordingAndAnalyze();
            }
        });
    }

    private void startRecordingAndAnalyze() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "마이크 권한이 필요합니다.", Toast.LENGTH_SHORT).show();
            return;
        }

        isRecording = true;
        recordButton.setText("주변 소리 듣는 중... (" + RECORD_TIME_SEC + "초)");
        recordButton.setEnabled(false);
        resultTextView.setText("분석 중입니다. 잠시만 기다려주세요...");

        new Thread(() -> {
            int bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);

            // CAMCORDER (캠코더) 모드 유지
            AudioRecord audioRecord = new AudioRecord(MediaRecorder.AudioSource.CAMCORDER, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bufferSize);

            if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                audioRecord = new AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bufferSize);
            }

            if (NoiseSuppressor.isAvailable()) {
                NoiseSuppressor ns = NoiseSuppressor.create(audioRecord.getAudioSessionId());
                if (ns != null) ns.setEnabled(false);
            }

            if (AutomaticGainControl.isAvailable()) {
                AutomaticGainControl agc = AutomaticGainControl.create(audioRecord.getAudioSessionId());
                if (agc != null) agc.setEnabled(false);
            }

            short[] audioBuffer = new short[TOTAL_SAMPLES];
            audioRecord.startRecording();
            int read = 0;
            while (read < TOTAL_SAMPLES) {
                read += audioRecord.read(audioBuffer, read, TOTAL_SAMPLES - read);
            }
            audioRecord.stop();
            audioRecord.release();

            // 동적 볼륨 정규화 유지
            float maxAmplitude = 0;
            for (int i = 0; i < TOTAL_SAMPLES; i++) {
                float absVal = Math.abs((float) audioBuffer[i]);
                if (absVal > maxAmplitude) {
                    maxAmplitude = absVal;
                }
            }

            float dynamicGain = 1.0f;
            if (maxAmplitude > 0) {
                dynamicGain = 32767.0f / maxAmplitude;
                if (dynamicGain > 15.0f) {
                    dynamicGain = 15.0f;
                }
            }

            float sum = 0;
            float[] floatAudioData = new float[TOTAL_SAMPLES];

            for (int i = 0; i < TOTAL_SAMPLES; i++) {
                float amplified = (float) audioBuffer[i] * dynamicGain;
                floatAudioData[i] = amplified;

                float normalized = amplified / 32768.0f;
                sum += normalized * normalized;
            }

            float rms = (float) Math.sqrt(sum / TOTAL_SAMPLES);
            final String finalResult;

            if (rms < 0.005f) {
                finalResult = "무음 (조용함)\n\n[소리 크기가 너무 작습니다]";
            } else {

                // 🌟 추가: 3초 동안의 결과를 누적해서 합산할 공간 마련
                Map<String, Float> totalPercentages = new LinkedHashMap<>();
                boolean hasError = false;
                String errorMessage = "";

                // 1초씩 3번 분석을 돌리지만, 화면에 바로 출력하지 않고 데이터만 몰래 모읍니다.
                for (int sec = 0; sec < RECORD_TIME_SEC; sec++) {
                    float[] oneSecondData = new float[MODEL_INPUT_SIZE];
                    int startPos = sec * MODEL_INPUT_SIZE;

                    if (startPos + MODEL_INPUT_SIZE <= TOTAL_SAMPLES) {
                        System.arraycopy(floatAudioData, startPos, oneSecondData, 0, MODEL_INPUT_SIZE);

                        String result = classifyAudio(oneSecondData); // C++에서 결과 받아옴

                        if (result.startsWith("분석 실패")) {
                            hasError = true;
                            errorMessage = result;
                            break;
                        }

                        // 결과 문자열("danger (98.0%) noise (2.0%)")을 쪼개서 항목별 퍼센트 수치만 뽑아 합산합니다.
                        String[] parts = result.split("  ");
                        for (String part : parts) {
                            if (part.trim().isEmpty()) continue;
                            int openParen = part.lastIndexOf("(");
                            int closeParen = part.lastIndexOf("%)");

                            if (openParen != -1 && closeParen != -1) {
                                String label = part.substring(0, openParen).trim();
                                try {
                                    float val = Float.parseFloat(part.substring(openParen + 1, closeParen));
                                    // 기존 퍼센트에 새로 나온 퍼센트를 더함
                                    totalPercentages.put(label, totalPercentages.getOrDefault(label, 0f) + val);
                                } catch (NumberFormatException e) {
                                    // 예외 무시
                                }
                            }
                        }
                    }
                }

                // 🌟 최종 출력 텍스트 만들기 (에러가 없을 경우, 총합을 3으로 나누어 평균을 냄)
                if (hasError) {
                    finalResult = errorMessage;
                } else {
                    StringBuilder sb = new StringBuilder("📊 3초 종합 분석 결과 📊\n\n");
                    for (Map.Entry<String, Float> entry : totalPercentages.entrySet()) {
                        float average = entry.getValue() / RECORD_TIME_SEC; // 3초로 나누기
                        sb.append("• ").append(entry.getKey()).append(" : ")
                                .append(String.format("%.1f", average)).append("%\n");
                    }
                    finalResult = sb.toString();
                }
            }

            // 🌟 화면(UI)에 깔끔하게 1개의 결과만 업데이트
            runOnUiThread(() -> {
                resultTextView.setText(finalResult);
                recordButton.setText("다시 소리 듣기 (" + RECORD_TIME_SEC + "초)");
                recordButton.setEnabled(true);
                isRecording = false;
            });

        }).start();
    }
}