# 소리 인식 테스트 앱 (Capstone Project)
스마트폰 마이크로 주변 소리를 녹음하여 AI 서버로 전송하면, AI가 소리를 분석해(대화소리, 위험소리, 일상소음) 결과를 음성(TTS)으로 알려주는 과정 테스트

대화소리에는 단어소리, 1분가량의 책읽는 소리 60개, 위험소리에는 차 경적소리/ 유리깨지는 소리/ 사이렌소리 데이터 20개씩, 일상소음은 청소기 돌리는 소리/ 바람소리 60개 첨부했습니당

## 서버 세팅 및 실행 (VS Code실행)
### 1. Python 3.10.11버전 설치 후 vscode 터미널로 ai_venv 라는 이름의 파이썬 3.10 가상환경 생성 (각자 Python 3.10 설치 필수)
py -3.10 -m venv ai_venv
### 1-2. 만든 가상환경으로 들어가기
cd SoundLearningTest

.\ai_venv\Scripts\activate

### 2. 필요한 AI 도구 및 서버 도구 한 번에 설치
pip install fastapi uvicorn tensorflow tensorflow-hub librosa python-multipart

### 3. 서버 실행
설치가 완료되면 아래 명령어로 서버 실행

python -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload

Application startup complete. 라는 초록색 글씨가 뜨면 서버가 정상적으로 켜진 거예용 (앱을 테스트하는 동안 이 터미널 창 끄면 안됨)

## 안드로이드 스튜디오 실행
### 1. 내 컴퓨터 IP 주소 찾기
(장소가 바뀌거나 와이파이가 바뀌면 IP도 바뀜)

window cmd(명령 프롬프트)실행

ipconfig 

목록 중에서 무선 LAN 어댑터 Wi-Fi (또는 이더넷) 항목 아래에 있는 IPv4 주소 (예: 192.168.x.x)를 찾아 안드로이드스튜디오 내 MainActivity.java 파일의 110번째 줄 근처
serverUrl 주소 부분에 아까 2단계에서 찾은 본인의 IPv4 주소로 수정

// ↓ 

ex) String serverUrl = "http://192.168.X.X:8000/api/analyze_sound"; 

## 통신 테스트 전 주의사항 
스마트폰과 노트북(서버)이 반드시 같은 Wi-Fi에 연결되어 있어야 됨
스마트폰의 모바일 데이터(LTE/5G)가 켜져 있으면 통신이 안 될 수 있으니, 테스트 중에는 와이파이만 켜두는 걸 권장

## 앱 실행
안드로이드 스튜디오의 (▶️ Run) 버튼을 눌러 앱 실행
[녹음 시작] 버튼을 누르고 소리를 낸 뒤, 버튼을 다시 눌러 AI의 분석 결과가 나오는지 확인

