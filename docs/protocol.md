# 통신·명령 명세 (API)

안경(ESP32-S3)과 스마트폰 앱이 주고받는 데이터의 형식입니다. 이 프로젝트에는 서버와 데이터베이스가 없고, 두 기기가 직접 통신합니다. 그래서 이 문서가 API 명세 역할을 합니다.

- 기준 코드: `firmware/src/proto_frame.*`, `ble_link.*`, `wifi_link.*`, `img_link.*`, `android/.../Proto.kt`, `BleLink.kt`, `WifiLink.kt`
- 작성: 2026-11-09, 코드와 대조해 확인

## 1. 연결 방식

| 연결 | 쓰임 | 내용 |
|---|---|---|
| **BLE 5.0** | 제품 구성 | 스마트폰과 직접 연결. 인터넷·공유기 불필요 |
| **WiFi** (UDP 5005 / TCP 5006) | 실험·디버깅 전용 빌드 | 같은 프레임을 씁니다. 처리량 비교와 PC 수신기 개발에 사용 |

펌웨어는 빌드 설정(`pio run -e ble` / `-e wifi`)으로 둘 중 하나만 켭니다. 무선 칩 하나가 2.4 GHz를 나눠 쓰기 때문입니다.

## 2. 프레임 형식

모든 데이터(소리·사진·버튼·명령)는 같은 틀을 씁니다. 숫자는 모두 little-endian입니다.

```
[A5 5A][type u8][flags u8][seq u16][len u16][payload: len 바이트][crc16 u16]
```

| 필드 | 크기 | 뜻 |
|---|---|---|
| `A5 5A` | 2 | 프레임 시작 표시 |
| `type` | 1 | 데이터 종류 (아래 표) |
| `flags` | 1 | AUDIO에서만 사용: bit0 = 48 kHz 원본, bit4–5 = 채널 수 − 1 |
| `seq` | 2 | 순번. 빠진 패킷을 셀 때 씀 |
| `len` | 2 | payload 길이 |
| `crc16` | 2 | CRC16-CCITT(초깃값 0xFFFF, 다항식 0x1021). `type`부터 payload 끝까지 계산 |

| type | 이름 | 방향 | 내용 |
|---|---|---|---|
| `0x01` | AUDIO | 안경 → 스마트폰 | 소리 + 방향 정보 |
| `0x02` | IMG_START | 안경 → 스마트폰 | 사진 전송 시작 |
| `0x03` | IMG_CHUNK | 안경 → 스마트폰 | 사진 조각 |
| `0x04` | IMG_END | 안경 → 스마트폰 | 사진 전송 끝 + 검사값 |
| `0x10` | EVENT | 안경 → 스마트폰 | 버튼, 큰 소리 방향 |
| `0x20` | CMD | 스마트폰 → 안경 | 모드·볼륨·알림 명령 |
| `0x30` / `0x31` | PING / PONG | 양방향 | 왕복 지연 측정 (WiFi) |

## 3. AUDIO (`0x01`)

```
fmt u8 | t_ms u32 | azimuth i16 | doa_conf u8 | dir_hist u8[8] | (fmt=1이면) predictor i16, step_idx u8 | samples
```

| 필드 | 뜻 |
|---|---|
| `fmt` | 0 = PCM16(압축 없음), 1 = IMA ADPCM(4:1 압축) |
| `t_ms` | 안경이 켜진 뒤 경과 시간(ms) |
| `azimuth` | 최근 소리 방향, 0.1° 단위(정면 0°, 시계 방향) |
| `doa_conf` | 방향 신뢰도(0~255). 최근 방향들이 한쪽으로 모일수록 큼 |
| `dir_hist` | 최근 몇 초 동안 소리가 온 방향을 8칸으로 센 값. 앱이 일대일/다인 대화 구분에 씀 |
| `predictor`, `step_idx` | ADPCM 복원 상태. 패킷마다 실어서 앞 패킷이 빠져도 이 패킷부터 바로 복원 |
| `samples` | PCM16이면 16비트 샘플, ADPCM이면 4비트씩(한 바이트의 하위 4비트가 먼저) |

- BLE에서는 16 kHz 모노만 보냅니다. 프레임 1개를 BLE 알림(notify) 1개에 맞춰 중간에 잘리지 않게 합니다.
- WiFi 실험 빌드는 48 kHz 3채널 원본도 보낼 수 있습니다(`flags` bit0 = 1).

## 4. 사진 (`0x02`~`0x04`)

| type | payload |
|---|---|
| IMG_START | `img_id u16 · total_len u32 · width u16 · height u16 · fmt u8(0 = JPEG)` |
| IMG_CHUNK | `img_id u16 · chunk_idx u16 · data` — 조각 크기 BLE 480바이트, WiFi 1024바이트 |
| IMG_END | `img_id u16 · crc32 u32` (JPEG 전체의 CRC32) |

- 기본 3초마다 VGA(640×480) JPEG를 보냅니다. 해상도·압축률·흑백 전송은 시리얼 명령으로 바꿀 수 있습니다.
- BLE에서는 오디오를 먼저 보내고 남는 틈에 사진 조각을 보냅니다.

## 5. EVENT (`0x10`)

payload 첫 바이트가 종류입니다.

| subtype | 이름 | 내용 |
|---|---|---|
| `0x01` | BUTTON | `state u8` — 1 = 누름(음성인식 시작), 0 = 뗌(종료) |
| `0x02` | DOA_ALERT | `azimuth i16 · energy_db u8 · t_ms u32` — 주변보다 갑자기 큰 소리가 난 방향. 2초에 한 번까지 |

WiFi(UDP)에서는 응답이 없으므로 같은 이벤트를 50 ms 간격으로 두 번 보냅니다. 받는 쪽은 `seq`로 중복을 지웁니다.

## 6. CMD (`0x20`, 스마트폰 → 안경)

payload 첫 바이트가 명령 종류입니다. **BLE에서는 프레임 머리(`A5 5A`~`len`)와 CRC 없이 payload만** CMD 특성에 씁니다. WiFi에서는 프레임 전체를 TCP 5006으로 보냅니다.

| subtype | 이름 | 인자 | 동작 |
|---|---|---|---|
| `0x01` | SET_MODE | `mode u8` (0 실내 기본 · 1 야외 기본 · 2 TV 시청 · 3 일대일 대화 · 4 다인 대화) | 소리 처리 프리셋과 OLED 표시 변경 |
| `0x02` | SET_VOLUME | `gain i8` (지금 값에서 더하거나 뺄 양) | 디지털 게인과 OLED 표시 변경 |
| `0x03` | DISPLAY_ALERT | `azimuth i16(0.1°) · icon u8 · dur_ms u16` | OLED에 아이콘과 방향 화살표 표시. 아이콘 0 차량 · 1 경적 · 2 사이렌 · 3 자전거 · 4 일반 경고 |
| `0x04` | SET_CONFIG | `key u8 · val i32` | key `0x01` = 사진 촬영 주기(ms, 500~60000) |
| `0x05` | CAPTURE_NOW | 없음 | 사진 1장 즉시 촬영 |

## 7. BLE GATT 서비스

| 항목 | 값 |
|---|---|
| 기기 이름 | `SmartGlass-HA` |
| 서비스 UUID | `8f0e1000-04c4-4f4b-a0a5-1e2d3c4b5a69` |
| AUDIO (Notify) | `8f0e1001-04c4-4f4b-a0a5-1e2d3c4b5a69` |
| CMD (Write, Write without response) | `8f0e1002-04c4-4f4b-a0a5-1e2d3c4b5a69` |
| IMAGE (Notify) | `8f0e1003-04c4-4f4b-a0a5-1e2d3c4b5a69` |
| EVENT (Notify) | `8f0e1004-04c4-4f4b-a0a5-1e2d3c4b5a69` |

- 연결 순서(앱): 이름으로 검색 → 연결 → 연결 우선순위 높임·MTU 517 요청 → 서비스 탐색 → AUDIO·IMAGE·EVENT 알림 구독.
- 연결 간격은 7.5~15 ms를 요청합니다.

## 8. WiFi (실험 빌드)

| 포트 | 프로토콜 | 내용 |
|---|---|---|
| 5005 | UDP | 안경 → 스마트폰: AUDIO, EVENT. 받는 쪽이 `"SUB"`(3바이트)를 보내 구독하고 주기적으로 다시 보내 연결을 유지합니다. PING을 받으면 PONG으로 돌려줍니다 |
| 5006 | TCP | 안경 → 스마트폰: 사진 프레임. 스마트폰 → 안경: CMD, PING. 프레임을 이어서 보내고, 받는 쪽은 `A5 5A`와 `len`으로 끊어 읽습니다 |

- 안경 찾기: 같은 네트워크에 `"SUB"`를 브로드캐스트하고, 처음 응답한 주소를 안경으로 씁니다.
- 접속 정보(SSID·비밀번호)는 시리얼 명령으로 안경의 NVS에 저장합니다. 코드에는 없습니다.

## 9. 외부 API: LLM 인텐트 분류

음성 명령에서만 인터넷을 씁니다. 음성이 아니라 **인식된 텍스트만** 보냅니다.

| 항목 | 내용 |
|---|---|
| 호출 | 학교가 제공하는 LLM 게이트웨이, OpenAI 호환 `POST /chat/completions` |
| 모델 | `claude-sonnet-5`, temperature 0 |
| 입력 | system: 인텐트 10종과 고르는 기준(`IntentClassifier.kt`의 프롬프트), user: 인식된 문장 |
| 출력 | 인텐트 이름 하나 (예: `VOLUME_UP`) |
| 인증 | `Authorization: Bearer <키>` — 키는 저장소 루트 `.env`의 `LLM_API_KEY`를 빌드할 때 넣습니다 |

인텐트 10종과 안경 명령의 대응은 [기능 4 문서](features/4-voice-command.md)에 있습니다.

## 10. 안경에 저장되는 설정 (NVS)

| namespace | key | 형식 | 뜻 |
|---|---|---|---|
| `net` | `ssid`, `pass` | 문자열 | WiFi 접속 정보 (WiFi 빌드) |
| `cfg` | `adpcm` | bool | 오디오 압축(ADPCM) 사용 |
| `cfg` | `beam` | bool | WiFi 빌드에서 빔포밍 16 kHz 스트림(켬) / 3채널 원본(끔) |
| `cfg` | `imgqvga` | bool | 사진 해상도 QVGA(켬) / VGA(끔) |
| `cfg` | `imgq` | int | JPEG 압축률(기본 12, 작을수록 고화질) |
| `cfg` | `imggray` | bool | 흑백 전송 |

시리얼 명령 목록은 [`cmd_list.md`](../cmd_list.md)에 있습니다.
