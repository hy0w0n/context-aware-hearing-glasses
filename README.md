### 1. 필수 시스템 프로그램 설치
`winget install ffmpeg`  
음성 처리(m4a-wav 변환)를 위해 컴퓨터에 FFmpeg가 반드시 설치되어 있어야 합니다.   
설치 후 `ffmpeg -version`으로 설치를 확인하세요.

### 2. 파이썬 라이브러리 설치
가상환경을 활성화한 상태에서 진행하세요.   
   1.  **가상환경 활성화:** `.\venv\Scripts\activate`
   2.  **폴더 이동:** `cd fastAPI_server`
   3.  **패키지 설치:** `pip install -r requirements.txt`   
핵심 라이브러리만 넣어둠
 
### 3. 서버 실행
`python -m uvicorn fastAPI:app --reload`