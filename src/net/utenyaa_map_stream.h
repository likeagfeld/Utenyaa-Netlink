/**
 * utenyaa_map_stream.h - Streamed .UTE map RX state machine.
 *
 * Server pushes a custom-authored .UTE byte stream to clients before
 * GAME_START via three opcodes (BEGIN / CHUNK × N / END). This module
 * owns the per-client receive buffer, chunk reassembly, and CRC
 * validation, and hands the completed buffer off to the World ctor
 * via take_buffer().
 *
 * Single-instance: only one streamed map is ever in flight at a time.
 * Begin → Chunks → End is gated by GAME_START's stage_id == 0xFE; if
 * any opcode arrives out of order or the END CRC fails, the buffer is
 * freed and the receive cycle resets without corrupting the running
 * gameplay. (The CD-baked stages remain a fallback — caller checks
 * `unet_map_stream_is_ready()` before swapping behavior.)
 */

#ifndef UTENYAA_MAP_STREAM_H
#define UTENYAA_MAP_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "utenyaa_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t* buffer;          /* jo_malloc'd on BEGIN. NULL when idle. */
    int      total_size;      /* bytes promised by BEGIN */
    int      received_bytes;  /* sum of CHUNK payload lengths so far */
    uint8_t  num_chunks;      /* count promised by BEGIN */
    uint8_t  chunks_received[(UNET_MAP_MAX_CHUNKS + 7) / 8];  /* bit per chunk */
    uint16_t expected_crc;    /* CCITT-FALSE crc16 promised by BEGIN */
    bool     ready;           /* true when END received and CRC matches */
    bool     active;          /* true between BEGIN and END (or reset) */
} unet_map_stream_t;

extern unet_map_stream_t g_map_stream;

/** Free any held buffer and clear all fields. Safe to call any time. */
void unet_map_stream_reset(void);

/** Handle MAP_BEGIN payload. Allocates buffer, resets bookkeeping.
 *  payload points at op+1 (so BEGIN body = first byte). Returns true
 *  on success. On failure (invalid args, alloc fail) the stream is
 *  reset and false is returned. */
bool unet_map_stream_on_begin(const uint8_t* payload, int len);

/** Handle MAP_CHUNK payload. Copies data into buffer at the right
 *  offset (chunk_idx * UNET_MAP_CHUNK_DATA_MAX). Tolerates duplicate
 *  chunks (overwrites idempotently); rejects out-of-range indices. */
void unet_map_stream_on_chunk(const uint8_t* payload, int len);

/** Handle MAP_END (no payload). Verifies all chunks present + CRC ok.
 *  On success sets g_map_stream.ready = true. On failure resets the
 *  stream silently. Returns final ready state. */
bool unet_map_stream_on_end(void);

/** True iff a complete validated map buffer is held. World ctor checks
 *  this before deciding whether to read from CD vs. consume the buffer. */
bool unet_map_stream_is_ready(void);

/** Transfer ownership of the buffer to the caller. After this, the
 *  module forgets about the buffer (caller is responsible for
 *  jo_free). Returns NULL if not ready. *out_size receives the byte
 *  count. */
uint8_t* unet_map_stream_take_buffer(int* out_size);

/** CCITT-FALSE CRC-16 (poly 0x1021, init 0xFFFF, no xor-out, no
 *  reflection). Matches the Python `crcmod.predefined("crc-ccitt-false")`
 *  used on the server. */
uint16_t unet_map_stream_crc16(const uint8_t* data, int len);

#ifdef __cplusplus
}
#endif

#endif /* UTENYAA_MAP_STREAM_H */
