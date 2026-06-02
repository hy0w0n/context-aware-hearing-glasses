# ESP32 3-Mic Direction + OLED Display Test

이 프로젝트는 ESP32 보드에 아날로그 마이크 모듈 3개와 I2C OLED 디스플레이를 연결해서, 소리가 들어오는 방향을 `LEFT`, `CENTER`, `RIGHT`, `QUIET`, `UNKNOWN`으로 추정하고 OLED 화면에 표시하는 펌웨어 테스트입니다.

## 1. 프로젝트 구성

- `src/main.cpp`: 마이크 3개의 아날로그 값을 읽고, OLED에 방향 결과를 출력하는 메인 코드입니다.
- `platformio.ini`: PlatformIO 프로젝트 설정 파일입니다. 보드 종류, 업로드 포트, 라이브러리 설정이 들어 있습니다.

## 2. 필요한 프로그램

- Visual Studio Code
- PlatformIO IDE 확장 프로그램
- USB 드라이버, 필요한 경우만 설치
- 데이터 전송 가능한 USB 케이블

## 3. Visual Studio Code 설치

- Visual Studio Code를 설치합니다.
- 설치 후 VS Code를 실행합니다.
- 왼쪽 Extensions 메뉴에서 `PlatformIO IDE`를 검색해 설치합니다.
- 설치가 끝나면 VS Code를 재시작합니다.
- 왼쪽 사이드바에 개미 모양의 PlatformIO 아이콘이 보이면 정상입니다.

## 4. 보드 USB 연결 확인

- ESP32 보드를 USB 케이블로 PC에 연결합니다.

- Windows 기준으로 아래 경로에서 COM 포트를 확인합니다.

```check
장치 관리자 > 포트(COM & LPT)
```
![장치 관리자](./images/img01.png)

정상적으로 인식되면 아래와 비슷하게 표시됩니다.

```check
CP210x USB to UART Bridge (COM5)
USB Serial Device (COM3)
USB JTAG/serial debug unit (COM7)
CH340 USB-SERIAL (COM4)
```

![포트(COM & LPT)](./images/img02.png)

여기서 중요한 것은 COM5, COM3, COM7 같은 COM 번호입니다.

## 5. platformio.ini 확인

- GitHub에서 받아온 프로젝트 폴더를 엽니다.

- esp32s3_hearing_aid_proto 폴더 내의 platformio.ini 파일을 확인합니다.

현재 프로젝트의 기본 설정은 일반 ESP32 개발보드 기준입니다.

```
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
upload_speed = 460800
upload_port = COM5
monitor_port = COM5

lib_deps =
    adafruit/Adafruit SSD1306
    adafruit/Adafruit GFX Library

```

본인의 PC에서 COM 번호가 다르면 아래 부분을 본인 COM 번호로 바꿔야 합니다.

```
upload_port = COM5
monitor_port = COM5
```

- 업로드가 불안정하면 업로드 속도를 낮출 수 있습니다.
```
upload_speed = 115200
```

**ESP32-S3 보드를 사용하는 경우에는 보드 설정을 사용중인 보드에 맞게 바꿔야 합니다.**

```
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 460800
```

## 6. 하드웨어 연결

현재 main.cpp는 아래 핀 연결을 기준으로 작성되어 있습니다.

OLED 디스플레이 연결은 다음과 같습니다.
```check
OLED VCC -> ESP32 3V3
OLED GND -> ESP32 GND
OLED SDA -> ESP32 GPIO21
OLED SCL -> ESP32 GPIO22
```

마이크 3개 연결은 다음과 같습니다.
```check
LEFT 마이크 AO   -> ESP32 GPIO34
CENTER 마이크 AO -> ESP32 GPIO35
RIGHT 마이크 AO  -> ESP32 GPIO32

모든 마이크 +   -> ESP32 3V3
모든 마이크 GND -> ESP32 GND
```

_ESP32-S3의 +와 GND에 모든 선을 연결할 수 없기 때문에 브레드 보드를 활용해서 +와 GND를 따로 빼 연결합니다._

보드에 적힌 이름이 D21, D22, D34 처럼 되어있다면 다음처럼 보면 됩니다.
```check
D21 = GPIO21
D22 = GPIO22
D34 = GPIO34
D35 = GPIO35
D32 = GPIO32
```

_ESP32-S3 보드를 사용하는 경우에는 위 핀이 그대로 맞지 않을 수 있으므로, 실제 보드의 핀맵을 보고 main.cpp 상단의 핀 번호를 수정해야 합니다._

수정할 부분은 대략 다음 위치입니다.

```
#define OLED_SDA 21
#define OLED_SCL 22

const int MIC_LEFT_PIN = 34;
const int MIC_CENTER_PIN = 35;
const int MIC_RIGHT_PIN = 32;
```

## 7. 빌드 방법
VS Code 왼쪽의 PlatformIO 아이콘을 누른 뒤 PlatformIO에서 프로젝트로 오픈합니다.

```check
QUICK ACCESS > PIO Home > Open > Open Project > 깃허브로 다운받은 프로젝트 폴더 선택
```
![PlatformIO로 프로젝트 오픈](./images/img03.png)

**PlatformIO 내에서 빌드하기**
```check
PROJECT TASKS > esp32dev > General > Build
```
![빌드1](./images/img04.png)

**프로젝트 코드를 보면서 빌드하기**
![빌드2](./images/img05.png)

빌드가 성공하면 마지막에 SUCCESS가 표시됩니다.

## 8. 업로드 방법
보드를 USB로 연결한 상태에서 pload를 실행합니다.

**PlatformIO 내에서 빌드하기**
```check
PROJECT TASKS > esp32dev > General > Build
```
![업로드1](./images/img06.png)

**프로젝트 코드를 보면서 빌드하기**
![업로드2](./images/img07.png)

업로드는 빌드된 펌웨어를 실제 ESP32 보드에 넣는 과정입니다.

## 9. 업로드가 안 될 때
업로드 중 아래 메시지에서 멈추면 보드가 업로드 모드로 자동 진입하지 못한 것입니다.

```
Connecting........_____....._____.....
```

이 경우 다음 방법을 시도합니다.
1. Upload를 다시 실행합니다.
2. 터미널에 Connecting...이 보이면 보드의 BOOT 버튼을 누르고 유지합니다.
3. 업로드가 시작되면 BOOT 버튼에서 손을 뗍니다.

## 10. 시리얼 모니터 확인 방법
업로드가 끝난 뒤 시리얼 모니터를 실행합니다.

**PlatformIO 내에서**
```check
PROJECT TASKS > esp32dev > General > Monitor
```
![시리얼 모니터1](./images/img08.png)

**프로젝트 코드를 보면서**
![시리얼 모니터2](./images/img09.png)

시리얼 모니터는 ESP32가 Serial.print()로 출력하는 값을 PC에서 확인하는 창입니다.
정상적으로 실행되면 아래와 비슷한 출력이 나옵니다.
```check
Mic direction display demo start
Please stay quiet for calibration.
Calibration complete.
centerLeft=1840
centerCenter=1905
centerRight=1780
L=5 C=3 R=4 direction=QUIET
L=180 C=30 R=20 direction=LEFT
```

## 11. OLED 화면에서 확인할 결과
업로드 후 처음에는 OLED에 보정 화면이 표시됩니다.
```check
Mic Direction Demo
Calibrating...
Stay quiet
```

이때 주변을 조용하게 유지해야 합니다.
보정이 끝나면 다음과 같은 결과가 OLED에 표시됩니다.

```
LEFT
CENTER
RIGHT
QUIET
UNKNOWN
```

각 의미는 다음과 같습니다.

1. LEFT: 왼쪽 마이크 쪽 소리가 가장 크게 감지됨
2. CENTER: 가운데 마이크 쪽 소리가 가장 크게 감지됨
3. RIGHT: 오른쪽 마이크 쪽 소리가 가장 크게 감지됨
4. QUIET: 전체 소리 변화가 작아서 조용한 상태로 판단됨
5. UNKNOWN: 여러 마이크 값이 비슷해서 방향 판단이 애매함