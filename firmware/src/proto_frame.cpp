#include "proto_frame.h"
#include <string.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline void put_u32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

size_t proto_build(uint8_t *out, size_t outCap, uint8_t type, uint16_t seq, const uint8_t *payload, size_t plen) {
  const size_t total = 2 + 6 + plen + 2;
  if (total > outCap) return 0;
  out[0] = 0xA5; out[1] = 0x5A;
  out[2] = type; out[3] = 0;
  put_u16(out + 4, seq);
  put_u16(out + 6, (uint16_t)plen);
  memcpy(out + 8, payload, plen);
  put_u16(out + 8 + plen, crc16_ccitt(out + 2, 6 + plen));
  return total;
}

size_t proto_build_audio_meta(uint8_t *out, size_t outCap, uint16_t seq, uint32_t t_ms,
                              int16_t az01, uint8_t conf, const uint8_t hist[8],
                              const int16_t *samples, uint16_t nsamples, uint8_t flags) {
  // payload: fmt u8 | t_ms u32 | azimuth i16 | doa_conf u8 | dir_hist u8[8] | samples[]
  const size_t META = 1 + 4 + 2 + 1 + 8;
  const size_t payloadLen = META + (size_t)nsamples * 2;
  const size_t total = 2 + 6 + payloadLen + 2;  // magic + header + payload + crc
  if (total > outCap) return 0;

  out[0] = 0xA5; out[1] = 0x5A;
  out[2] = PROTO_TYPE_AUDIO;
  out[3] = flags;
  put_u16(out + 4, seq);
  put_u16(out + 6, (uint16_t)payloadLen);

  uint8_t *p = out + 8;
  p[0] = 0;                          // fmt=0 PCM16
  put_u32(p + 1, t_ms);
  put_u16(p + 5, (uint16_t)az01);    // azimuth 0.1° (-1800~1800)
  p[7] = conf;                       // doa_conf 0~255
  if (hist != nullptr) memcpy(p + 8, hist, 8);
  else memset(p + 8, 0, 8);
  memcpy(p + META, samples, (size_t)nsamples * 2);

  const uint16_t crc = crc16_ccitt(out + 2, 6 + payloadLen);
  put_u16(out + 8 + payloadLen, crc);
  return total;
}

size_t proto_build_audio_raw(uint8_t *out, size_t outCap, uint16_t seq, uint32_t t_ms,
                             const int16_t *samples, uint16_t nsamples, uint8_t flags) {
  return proto_build_audio_meta(out, outCap, seq, t_ms, 0, 0, nullptr, samples, nsamples, flags);
}

size_t proto_build_audio_adpcm(uint8_t *out, size_t outCap, uint16_t seq, uint32_t t_ms,
                               int16_t az01, uint8_t conf, const uint8_t hist[8],
                               int16_t predictor, uint8_t stepIdx,
                               const uint8_t *nibbles, uint16_t nibBytes, uint8_t flags) {
  const size_t META = 1 + 4 + 2 + 1 + 8;
  const size_t payloadLen = META + 3 + nibBytes;  // +3 = adpcm_state{predictor i16, step_idx u8}
  const size_t total = 2 + 6 + payloadLen + 2;
  if (total > outCap) return 0;

  out[0] = 0xA5; out[1] = 0x5A;
  out[2] = PROTO_TYPE_AUDIO;
  out[3] = flags;
  put_u16(out + 4, seq);
  put_u16(out + 6, (uint16_t)payloadLen);

  uint8_t *p = out + 8;
  p[0] = 1;                          // fmt=1 ADPCM
  put_u32(p + 1, t_ms);
  put_u16(p + 5, (uint16_t)az01);
  p[7] = conf;
  if (hist != nullptr) memcpy(p + 8, hist, 8);
  else memset(p + 8, 0, 8);
  put_u16(p + META, (uint16_t)predictor);   // 패킷 리싱크 상태 (§6.1)
  p[META + 2] = stepIdx;
  memcpy(p + META + 3, nibbles, nibBytes);

  put_u16(out + 8 + payloadLen, crc16_ccitt(out + 2, 6 + payloadLen));
  return total;
}

size_t proto_build_audio(uint8_t *out, size_t outCap, uint16_t seq, const audio_block_t &blk, uint8_t flags) {
  return proto_build_audio_raw(out, outCap, seq, blk.t_ms, blk.samples, blk.nsamples, flags);
}
