// proto_frame — 통신 프로토콜 v0.1 프레임 빌더 (CLAUDE.md §6.1)
// [0xA5 0x5A][type u8][flags u8][seq u16][len u16][payload...][crc16]
// CRC16-CCITT: magic 제외, type부터 payload 끝까지
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "audio_capture.h"

#define PROTO_TYPE_AUDIO     0x01
#define PROTO_TYPE_IMG_START 0x02
#define PROTO_TYPE_IMG_CHUNK 0x03
#define PROTO_TYPE_IMG_END   0x04
#define PROTO_TYPE_EVENT     0x10
#define PROTO_TYPE_CMD       0x20
#define PROTO_TYPE_PING      0x30
#define PROTO_TYPE_PONG      0x31

uint16_t crc16_ccitt(const uint8_t *data, size_t len);

// 범용 프레임 빌더: [A5 5A][type][flags=0][seq][len][payload][crc16]. 반환: 전체 길이(실패 0)
size_t proto_build(uint8_t *out, size_t outCap, uint8_t type, uint16_t seq, const uint8_t *payload, size_t plen);

// AUDIO 프레임 생성 (fmt=0 PCM16). 반환: 전체 바이트 수(실패 0)
size_t proto_build_audio(uint8_t *out, size_t outCap, uint16_t seq, const audio_block_t &blk, uint8_t flags);
size_t proto_build_audio_raw(uint8_t *out, size_t outCap, uint16_t seq, uint32_t t_ms,
                             const int16_t *samples, uint16_t nsamples, uint8_t flags);
// DOA 메타 포함 버전 (az01=0.1°단위, hist=8방향 히스토그램 — dsp_pipeline 출력용, 2026-09-10)
size_t proto_build_audio_meta(uint8_t *out, size_t outCap, uint16_t seq, uint32_t t_ms,
                              int16_t az01, uint8_t conf, const uint8_t hist[8],
                              const int16_t *samples, uint16_t nsamples, uint8_t flags);
// ADPCM 버전 (fmt=1): 메타 뒤에 adpcm_state{predictor i16, step_idx u8} + 4bit 니블 데이터 (§6.1)
size_t proto_build_audio_adpcm(uint8_t *out, size_t outCap, uint16_t seq, uint32_t t_ms,
                               int16_t az01, uint8_t conf, const uint8_t hist[8],
                               int16_t predictor, uint8_t stepIdx,
                               const uint8_t *nibbles, uint16_t nibBytes, uint8_t flags);
