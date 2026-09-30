# cmd_list.md — 명령어 치트시트

> 자주 쓰는 명령 모음 (PowerShell 기준, 복붙용). 상세 배경은 CLAUDE.md / proto.md 참조.
> 최종 업데이트: 2026-09-10 (DSP 파이프라인 — beam/raw, doaoff, DOA stat 필드 추가)

---

## 0. 한눈에 보는 구조

```
[ESP32 보드] ←USB→ [PC: 빌드/플래시/시리얼]        [ESP32] ←무선→ [PC 도구 / Android 앱]
  COM3 = 네이티브 USB (로그/업로드, 열고닫으면 보드 리셋됨)      WiFi: UDP 5005(오디오·이벤트) + TCP 5006(이미지·명령)
  COM5 = CH343 UART   (로그/업로드, 둘 다 가능)                BLE : SmartGlass-HA (오디오·이미지·이벤트·명령)
```

---

## 1. 펌웨어 빌드 / 플래시

```powershell
# WiFi 버전 (평소 기본)
pio run -d D:\capstone\firmware -e wifi -t upload --upload-port COM3

# BLE 버전 (BLE 테스트할 때)
pio run -d D:\capstone\firmware -e ble -t upload --upload-port COM3

# 빌드만 (업로드 없이 컴파일 확인)
pio run -d D:\capstone\firmware -e wifi

# 연결된 포트 확인
pio device list
```

- 포트가 COM3에 없으면 COM5(CH343 쪽 USB 단자)로 꽂힌 것 → `--upload-port COM5`로 동일하게 동작
- **업로드 실패 "port is busy"** → 시리얼 모니터/serial_peek가 포트를 잡고 있음. 먼저 닫기 (Ctrl+C)
- **업로드 자체가 안 됨** → 보드의 BOOT 누른 채 RESET(다운로드 모드) 후 재시도

## 2. 시리얼 (로그 보기 / 명령 보내기)

```powershell
# 실시간 로그 + 직접 타이핑 (종료: Ctrl+C)
pio device monitor -p COM3 -b 115200

# N초만 로그 캡처 (비대화형)
python D:\capstone\tools\serial_peek.py COM3 12

# 명령 1개 보내고 응답 보기
python D:\capstone\tools\serial_cmd.py COM3 "info" 5
```

**펌웨어 시리얼 명령** (모니터에서 타이핑 — 입력 시작하면 stat 출력이 10초 멈춤):

| 명령 | 동작 |
|---|---|
| `help` | 명령 목록 |
| `info` | 전송로/WiFi/힙/PSRAM 상태 |
| `dump` | 최근 오디오 샘플 64개 출력 |
| `cam` | 카메라 1장 캡처 테스트 (크기/시간 로그) |
| `wifi <ssid>;<pass>` | WiFi 저장+접속 (**wifi 빌드 전용**, 2.4GHz만, 구분자는 세미콜론) |
| `forget` | 저장된 WiFi 삭제 |
| `beam` / `raw` | (wifi 빌드) 오디오 스트림 전환 — **NVS 저장이라 재부팅/재플래시에도 유지**. `beam`=빔포밍 16kHz mono(+DOA 메타, 청취/앱용), `raw`=3ch 48kHz 원본(기본 아님! 분석 도구 sync_check/doa_prototype용) — 2026-09-10 |
| `adpcm` / `pcm` | 16k 스트림 압축 전환 — **NVS 유지**. `adpcm`=IMA 4:1(64kbps, BLE 대역 절약·비교실험용), `pcm`=무압축 256kbps(기본). raw 3ch 스트림엔 미적용. PC 도구·앱 모두 자동 인식(fmt 바이트) — 2026-09-10 |
| `doaoff <n>` | DOA 버스 오프셋 설정 (부팅마다 0 또는 ±1로 바뀜! 재부팅 후 클랩 1회 녹음(raw) → sync_check.py의 ch1-ch3 값 입력. 기본 0, ±1 오차는 각도 3~6° 수준) |

- 로그/명령은 COM3·COM5 **어느 포트에서도 동작** (듀얼 콘솔)
- 모니터 없이 명령 1개만: `python D:\capstone\tools\serial_cmd.py COM3 "beam" 4` (⚠ COM3는 세션을 닫을 때 보드가 리셋됨 — beam/wifi 설정은 NVS 저장이라 무관하지만, doaoff는 RAM이라 리셋되면 다시 입력)
- `[stat]` 줄 읽는 법: `mic1/2/3=RMS(min..max)` 소리에 반응하면 정상, `doa=각도°(c신뢰도)`=최근 3초 방향추정(-180~180, 음수=왼쪽, 조용하면 안 뜸), `dsp=µs/유효창수`=DOA 1회 소요시간(현재 ~11700µs)과 게이트 통과 누적 창 수, `ovr` 구독자 있는데 증가하면 병목, `[i2s1=STALL!]` 뜨면 mic3 클럭 문제
- `[doa] ALERT az=각도° NNdB` = 큰소리 이벤트(EVENT 0x02) 발화 로그 (2초 스로틀)

## 3. WiFi 설정 (처음 또는 네트워크 바뀔 때)

1. **폰 핫스팟 켜기** (2.4GHz! 최신 폰은 기본 5GHz인 경우 있음 → 핫스팟 설정에서 2.4GHz 선택)
2. **PC도 같은 핫스팟에 연결** ← 제일 자주 틀리는 부분. ESP32와 PC가 다른 네트워크면 아무것도 안 됨
3. ESP32에 자격증명 입력 (한 번만 — NVS에 저장돼 재부팅해도 유지):

```powershell
pio device monitor -p COM3 -b 115200
# 모니터에서:  wifi 핫스팟이름;비밀번호
```

4. `[wifi] 접속됨. IP=...` 확인. 이후엔 부팅 시 자동 접속 + **끊겨도 20초마다 자동 재시도** (핫스팟을 늦게 켜도 알아서 붙음)

## 4. BLE 설정

1. BLE 펌웨어 플래시: `pio run -d D:\capstone\firmware -e ble -t upload --upload-port COM3`
2. 끝 — 광고는 자동 시작(`SmartGlass-HA`), 설정할 것 없음
3. 수신: Android 앱에서 [BLE 선택→연결], 또는 PC에서:

```powershell
python D:\capstone\tools\ble_receiver.py --seconds 10
```

- BLE는 오디오(16kHz mono)+이미지+이벤트+명령 전부 지원, 단 이미지는 오디오와 대역 공유라 장당 1~3초
- 폰 nRF Connect 앱으로도 접속 확인 가능 (스캔 → SmartGlass-HA)

## 5. PC 수신/검증 도구 (`D:\capstone\tools\`)

```powershell
# ① 오디오 녹음 + 드랍 통계 (WiFi) — recordings\wavYYMMDDHHMMSS.wav 저장
python D:\capstone\tools\receiver.py auto --seconds 10
python D:\capstone\tools\receiver.py auto --seconds 16 --out D:\capstone\recordings\clap.wav

# ② 실시간 청취 (WiFi, C++ 저지연) — 스트림 종류는 시리얼 beam/raw로 결정(자동 인식)
D:\capstone\tools\listen_live.exe                # auto 탐색, 게인 x2 (beam이면 빔포밍 mono 재생)
D:\capstone\tools\listen_live.exe auto 1         # 게인 x1 (스피커 하울링 시 낮추기)
D:\capstone\tools\listen_live.exe auto 2 5005 3  # (raw 3ch일 때만) mic3 골라 듣기
# ⚠ 스피커로 틀면 마이크로 되먹임(하울링, 삐~) — 반드시 이어폰으로! (2026-09-10 실증)
# 빔포밍 효과 A/B: 시리얼 raw(=mic1 단독 청취) ↔ beam(3-mic 합성) 전환하며 비교

# ③ 이미지 수신 (WiFi) — images\imageYYYYMMDDHHMMSS.jpg 저장
python D:\capstone\tools\image_receiver.py auto --count 3              # 3장 (기본 3초 간격)
python D:\capstone\tools\image_receiver.py auto --count 10 --period 1  # 1초 간격 10장 (학습데이터 수집)
python D:\capstone\tools\image_receiver.py auto --count 1 --now        # 즉시 1장

# ④ 명령 전송 (WiFi) — 안경 OLED가 바뀌는지 확인
python D:\capstone\tools\control.py auto mode 2          # 모드: 0실내 1야외 2TV 3일대일 4다인
python D:\capstone\tools\control.py auto vol +3
python D:\capstone\tools\control.py auto alert 135 2     # 135° 방향 사이렌 화살표
python D:\capstone\tools\control.py auto capture
python D:\capstone\tools\control.py auto ping            # RTT 측정

# ⑤ 분석
python D:\capstone\tools\sync_check.py                   # 최신 녹음의 채널 매핑+동기 오프셋
python D:\capstone\tools\doa_prototype.py D:\capstone\recordings\az000.wav --expected 0   # 방향추정
```

- `auto` = 브로드캐스트 자동 탐색 (IP 몰라도 됨). 실패 시 시리얼 `info`로 IP 확인 후 `auto` 자리에 IP
- ③과 ④는 같은 TCP 포트를 쓰므로 **동시 실행 불가** (하나 닫고 실행)
- BLE 수신: `ble_receiver.py` (§4)

## 6. Android 앱 (`D:\capstone\android\`)

- **설치**: `android\app\build\outputs\apk\debug\app-debug.apk`를 폰으로 복사 → 설치 (알 수 없는 앱 허용)
- **사용**: WiFi/BLE 선택 → [연결]. WiFi 모드는 폰 핫스팟이 켜져 있으면 자동 탐색 (폰=핫스팟 호스트라 네트워크 불일치 문제 없음)
- **재빌드**:

```powershell
& "$env:USERPROFILE\.gradle\wrapper\dists\gradle-9.3.1-bin\23ovyewtku6u96viwx3xl3oks\gradle-9.3.1\bin\gradle.bat" -p D:\capstone\android assembleDebug
```

  (또는 Android Studio로 `D:\capstone\android` 열어서 Run)

## 7. 트러블슈팅 요약

| 증상 | 원인/해결 |
|---|---|
| 수신 도구 "수신 없음" | ①PC와 ESP32가 같은 네트워크인지 (`netsh wlan show interfaces`로 PC SSID 확인) ②ESP32 `info`로 wifi=UP 확인 ③방화벽에서 Python 허용 |
| `auto` 탐색 실패, IP 직접 지정은 됨 | 가상 어댑터(VirtualBox 등)로 브로드캐스트가 샘 — IP 직접 지정 사용 |
| 업로드 "port busy" | 모니터/peek가 포트 점유 → 닫기 |
| COM3에서 로그 도구 실행하면 보드 재부팅 | 네이티브 USB 특성(정상). 재부팅 없이 보려면 COM5 사용 |
| 보드 빨간불 | 전원 LED (정상, 항상 켜짐) |
| 버튼 누른 채 리셋하면 부팅 안 됨 | GPIO0=BOOT — 다운로드 모드 진입. 그냥 RESET 한 번 더 |
| WiFi 접속 실패 | 2.4GHz인지, 비밀번호 오타(`wifi 이름;비번` 세미콜론), 핫스팟 켜져 있는지 |
| 카메라 corrupted JPEG | `cam`으로 재현 확인 → xclk 낮추기 (config.h, 20→16/10MHz) |
| 실시간 청취에서 삐이이~ 연속 톤 | **스피커 하울링** (재생음→마이크 루프) → 이어폰으로 듣기, 또는 게인 1로 |
| 청취 소리가 웅웅/출렁임 | 초저역 럼블 — beam 스트림엔 120Hz HPF 내장(2026-09-10). raw 직청취에는 없음(정상) |
| sync_check/doa_prototype 값이 이상함 | 스트림이 beam(16k mono)으로 녹음됨 — 분석용 녹음은 시리얼 `raw` 후에! (WAV 헤더 48000Hz 3ch인지 확인) |
| 콘솔 한글 깨짐 | listen_live는 UTF-8 출력 내장(2026-09-10 재빌드). 다른 도구가 깨지면 `chcp 65001` 후 실행 |
| BLE 속도가 유난히 느림/드랍 폭증 | **PC 핫스팟(2.4GHz)과 블루투스 공존 간섭** 의심 — BLE 실험 때는 핫스팟 끄기 (같은 대역·안테나 공유). PCM이 버거우면 `adpcm` |
| 앱 BLE 파형이 잡음처럼 나옴 | 펌웨어가 ADPCM인데 앱은 PCM만 해석(디코더 추가 전) — 시리얼 `pcm` 전환 후 앱 테스트 |
