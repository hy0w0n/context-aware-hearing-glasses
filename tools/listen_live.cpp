// listen_live — ESP32 AUDIO 프레임 실시간 청취 (Windows, C++/waveOut 저지연)
// 용도: 마이크/빔포밍 출력을 PC 스피커로 거의 실시간 모니터링 (지연 ≈ 네트워크 지터 + 재생 큐 수십 ms)
// 참고: receiver.py는 WAV 저장+검증용(지연 무관), 본 도구는 청취 전용. 프로토콜은 CLAUDE.md §6.1.
//
// 빌드:  g++ -O2 -std=c++17 -static -s -o listen_live.exe listen_live.cpp -lws2_32 -lwinmm
// 사용:  listen_live.exe [ESP32_IP|auto] [gain(기본 2)] [port(기본 5005)] [채널(1~3, 기본 1)]
//        인자 없이 실행하면 auto — 브로드캐스트로 ESP32 자동 탐색
//        3ch 스트림에서 4번째 인자로 들을 마이크 선택: 예) listen_live.exe auto 2 5005 3 → mic3
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static const int META_LEN = 1 + 4 + 2 + 1 + 8;  // fmt, t_ms, azimuth, conf, dir_hist[8]
static const int MAX_SAMPLES = 1024;
static const int POOL = 32;  // 재생 버퍼 풀 (10ms 블록 × 32 = 최대 320ms 버퍼링 허용)

// IMA ADPCM 디코더 (fmt=1, §6.1) — 펌웨어 adpcm.cpp와 대칭. 패킷 헤더 상태로 매 패킷 리싱크
static const int16_t ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t ADPCM_IDX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static void adpcm_decode(const uint8_t *nib, int nbytes, int pred, int idx, int16_t *out) {
  int o = 0;
  for (int i = 0; i < nbytes; i++) {
    for (int half = 0; half < 2; half++) {
      const int n = half ? (nib[i] >> 4) : (nib[i] & 0xF);
      const int step = ADPCM_STEP[idx];
      int delta = step >> 3;
      if (n & 4) delta += step;
      if (n & 2) delta += step >> 1;
      if (n & 1) delta += step >> 2;
      pred += (n & 8) ? -delta : delta;
      if (pred > 32767) pred = 32767;
      if (pred < -32768) pred = -32768;
      idx += ADPCM_IDX[n];
      if (idx < 0) idx = 0;
      if (idx > 88) idx = 88;
      out[o++] = (int16_t)pred;
    }
  }
}

struct Block {
  WAVEHDR hdr;
  int16_t pcm[MAX_SAMPLES];
  bool inUse;
};

int main(int argc, char **argv) {
  SetConsoleOutputCP(65001);  // 소스가 UTF-8 한글 — cmd 기본 CP949에서 깨짐 방지 (2026-09-10)
  const char *ipArg = (argc > 1) ? argv[1] : "auto";
  const int gain = (argc > 2) ? atoi(argv[2]) : 2;
  const int port = (argc > 3) ? atoi(argv[3]) : 5005;
  int pickCh = (argc > 4) ? atoi(argv[4]) - 1 : 0;  // 3ch 스트림에서 재생할 채널 (0-base)
  if (pickCh < 0) pickCh = 0;
  if (pickCh > 2) pickCh = 2;

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup 실패\n"); return 1; }

  SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = INADDR_ANY;
  local.sin_port = 0;
  bind(sock, (sockaddr *)&local, sizeof(local));
  DWORD tmo = 500;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
  BOOL bc = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&bc, sizeof(bc));

  // auto: 전역 + 로컬 IPv4별 /24 지향 브로드캐스트로 SUB를 뿌려 ESP32 탐색 (receiver.py와 동일 기법)
  std::vector<sockaddr_in> targets;
  bool discovered = false;
  sockaddr_in esp = {};
  if (strcmp(ipArg, "auto") == 0) {
    auto add = [&](uint32_t addrNbo) {
      sockaddr_in t = {};
      t.sin_family = AF_INET;
      t.sin_addr.s_addr = addrNbo;
      t.sin_port = htons((u_short)port);
      targets.push_back(t);
    };
    add(INADDR_BROADCAST);
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
      addrinfo hints = {};
      hints.ai_family = AF_INET;
      addrinfo *res = nullptr;
      if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
        for (addrinfo *p = res; p; p = p->ai_next) {
          const uint32_t a = ((sockaddr_in *)p->ai_addr)->sin_addr.s_addr;  // network order
          if ((a & 0xFF) == 127) continue;
          add(a | 0xFF000000u);  // 마지막 옥텟 .255
        }
        freeaddrinfo(res);
      }
    }
    printf("[listen] ESP32 자동 탐색 중 (브로드캐스트, gain=x%d)... Ctrl+C 종료\n", gain);
  } else {
    esp.sin_family = AF_INET;
    esp.sin_addr.s_addr = inet_addr(ipArg);
    esp.sin_port = htons((u_short)port);
    discovered = true;
    printf("[listen] %s:%d 에 SUB 전송, 스트림 대기... (Ctrl+C 종료, gain=x%d)\n", ipArg, port, gain);
  }

  HWAVEOUT wo = nullptr;
  static Block pool[POOL] = {};
  int rate = 0;
  int nch = 1;  // 1=mono, 2=stereo 재생, 3=ch0만 모노 재생 (모니터 용도)

  uint32_t pkts = 0, seqDrops = 0, lateDrops = 0, crcSkip = 0;
  int lastSeq = -1;
  ULONGLONG lastSub = 0, lastStat = 0, firstRx = 0;
  uint8_t buf[2048];

  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now - lastSub > 2000) {
      if (discovered) {
        sendto(sock, "SUB", 3, 0, (sockaddr *)&esp, sizeof(esp));
      } else {
        for (auto &t : targets) sendto(sock, "SUB", 3, 0, (sockaddr *)&t, sizeof(t));
      }
      lastSub = now;
    }

    sockaddr_in from = {};
    int fromLen = sizeof(from);
    const int n = recvfrom(sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fromLen);
    if (n <= 0) {
      if (firstRx == 0) printf("[listen] 수신 없음 — ESP32 IP/네트워크/방화벽 확인\n");
      continue;
    }
    if (n < 10 || buf[0] != 0xA5 || buf[1] != 0x5A || buf[2] != 0x01) continue;

    const uint8_t flags = buf[3];
    const uint16_t seq = (uint16_t)(buf[4] | (buf[5] << 8));
    const uint16_t plen = (uint16_t)(buf[6] | (buf[7] << 8));
    if (n < 8 + plen + 2 || plen <= META_LEN) continue;

    if (firstRx == 0) {
      firstRx = now;
      if (!discovered) {
        esp = from;  // 탐색 완료 → 이후 keepalive는 유니캐스트
        discovered = true;
        printf("[listen] ESP32 발견: %s\n", inet_ntoa(from.sin_addr));
      }
      rate = (flags & 0x01) ? 48000 : 16000;
      nch = ((flags >> 4) & 0x3) + 1;
      const WORD outCh = (nch == 2) ? 2 : 1;
      WAVEFORMATEX wf = {};
      wf.wFormatTag = WAVE_FORMAT_PCM;
      wf.nChannels = outCh;
      wf.nSamplesPerSec = (DWORD)rate;
      wf.wBitsPerSample = 16;
      wf.nBlockAlign = (WORD)(2 * outCh);
      wf.nAvgBytesPerSec = (DWORD)rate * 2 * outCh;
      if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[listen] waveOutOpen 실패\n");
        return 1;
      }
      if (nch == 3)
        printf("[listen] 첫 패킷 수신! %dHz 3ch — mic%d 채널 모노 재생 (4번째 인자로 변경)\n", rate, pickCh + 1);
      else
        printf("[listen] 첫 패킷 수신! %dHz %dch 재생 시작\n", rate, nch);
    }

    if (lastSeq >= 0) {
      const uint16_t gap = (uint16_t)(seq - (uint16_t)lastSeq - 1);
      if (gap && gap < 0x8000) seqDrops += gap;
    }
    lastSeq = seq;
    pkts++;

    // fmt(payload[0]): 0=PCM16 그대로, 1=ADPCM → 디코드 후 재생
    static int16_t decoded[MAX_SAMPLES];
    const uint8_t fmt = buf[8];
    int nsamp;
    const int16_t *src;
    if (fmt == 1) {
      const uint8_t *pp = buf + 8 + META_LEN;  // adpcm_state{predictor i16, step_idx u8} + 니블
      const int nbytes = plen - META_LEN - 3;
      nsamp = nbytes * 2;
      if (nbytes <= 0 || nsamp > MAX_SAMPLES || pp[2] > 88) continue;
      adpcm_decode(pp + 3, nbytes, (int16_t)(pp[0] | (pp[1] << 8)), pp[2], decoded);
      src = decoded;
    } else {
      nsamp = (plen - META_LEN) / 2;
      if (nsamp > MAX_SAMPLES) continue;
      src = (const int16_t *)(buf + 8 + META_LEN);
    }

    // 빈 재생 블록 찾기 (재생 완료된 것 회수)
    Block *blk = nullptr;
    int queued = 0;
    for (int i = 0; i < POOL; i++) {
      if (pool[i].inUse && (pool[i].hdr.dwFlags & WHDR_DONE)) {
        waveOutUnprepareHeader(wo, &pool[i].hdr, sizeof(WAVEHDR));
        pool[i].inUse = false;
      }
      if (pool[i].inUse) queued++;
      else if (!blk) blk = &pool[i];
    }
    if (!blk) { lateDrops++; continue; }  // 재생이 밀림 → 패킷 버림 (지연 누적 방지)

    int outCount = 0;
    if (nch == 3) {
      for (int i = 0; i < nsamp / 3; i++) {  // 선택 채널만 추출 (4번째 인자, 기본 mic1)
        int v = (int)src[3 * i + pickCh] * gain;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        blk->pcm[outCount++] = (int16_t)v;
      }
    } else {
      for (int i = 0; i < nsamp; i++) {  // mono/stereo는 그대로 (인터리브 유지)
        int v = (int)src[i] * gain;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        blk->pcm[outCount++] = (int16_t)v;
      }
    }
    memset(&blk->hdr, 0, sizeof(WAVEHDR));
    blk->hdr.lpData = (LPSTR)blk->pcm;
    blk->hdr.dwBufferLength = (DWORD)(outCount * 2);
    waveOutPrepareHeader(wo, &blk->hdr, sizeof(WAVEHDR));
    waveOutWrite(wo, &blk->hdr, sizeof(WAVEHDR));
    blk->inUse = true;

    if (now - lastStat > 1000) {
      lastStat = now;
      printf("\r[listen] pkts=%u drops=%u late=%u queue=%d블록   ", pkts, seqDrops, lateDrops, queued);
      fflush(stdout);
    }
    (void)crcSkip;
  }
}
