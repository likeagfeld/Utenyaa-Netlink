/**
 * utenyaa_map_stream.c - Streamed .UTE map RX state machine.
 * See utenyaa_map_stream.h for full design notes.
 *
 * Logging convention: every state transition emits a CKPT-S* trace
 * via unet_send_dbg_log so a single hardware test run captures the
 * full RX timeline in the server journal. Tracepoints:
 *   S1   BEGIN received (size, chunks, crc)
 *   S2   alloc ok
 *   S2X  alloc fail
 *   S3   first chunk received
 *   S4   last chunk received (count == promised)
 *   S5   END received
 *   S6   CRC ok, buffer ready
 *   S6X  CRC mismatch / chunks missing
 *   S7   buffer taken by World ctor
 *   SR   stream reset (any reason)
 */

#include "utenyaa_map_stream.h"
#include "utenyaa_net.h"
#include <jo/jo.h>
#include <string.h>

unet_map_stream_t g_map_stream;

/* Compact decimal/hex format helper for CKPT logs. CHANGED-WITH-CARE:
 * mirrors the buffer-format style already used in main.cxx CKPT-B. */
static void s_log_kv(const char* tag, int v1, int v2, int v3)
{
    char buf[48];
    int n = 0;
    while (tag[n] && n < 16) { buf[n] = tag[n]; n++; }
    buf[n++] = ' ';
    /* three int fields, signed, up to 5 digits each */
    int values[3] = { v1, v2, v3 };
    for (int i = 0; i < 3; i++) {
        if (i) buf[n++] = ',';
        int v = values[i];
        if (v < 0) { buf[n++] = '-'; v = -v; }
        if (v >= 10000) { buf[n++] = '0' + (v / 10000); v %= 10000; }
        if (v >= 1000)  { buf[n++] = '0' + (v / 1000);  v %= 1000;  }
        if (v >= 100)   { buf[n++] = '0' + (v / 100);   v %= 100;   }
        if (v >= 10)    { buf[n++] = '0' + (v / 10);    v %= 10;    }
        buf[n++] = '0' + v;
    }
    buf[n] = 0;
    unet_send_dbg_log(buf);
}

static void s_log_msg(const char* tag)
{
    unet_send_dbg_log(tag);
}

static void s_clear_mask(void)
{
    for (int i = 0; i < (int)sizeof(g_map_stream.chunks_received); i++)
        g_map_stream.chunks_received[i] = 0;
}

static bool s_chunk_seen(uint8_t idx)
{
    return (g_map_stream.chunks_received[idx >> 3] & (1u << (idx & 7))) != 0;
}

static void s_mark_chunk(uint8_t idx)
{
    g_map_stream.chunks_received[idx >> 3] |= (1u << (idx & 7));
}

void unet_map_stream_reset(void)
{
    if (g_map_stream.buffer) {
        jo_free(g_map_stream.buffer);
        g_map_stream.buffer = 0;
    }
    g_map_stream.total_size = 0;
    g_map_stream.received_bytes = 0;
    g_map_stream.num_chunks = 0;
    g_map_stream.expected_crc = 0;
    g_map_stream.ready = false;
    g_map_stream.active = false;
    s_clear_mask();
    s_log_msg("CKPT-SR map-stream reset");
}

bool unet_map_stream_on_begin(const uint8_t* payload, int len)
{
    /* payload[0] = opcode (already dispatched on); body starts at [1].
     * Body: [total_size:2 BE][num_chunks:1][crc16:2 BE] = 5 bytes. */
    if (len < 1 + 5) {
        s_log_msg("CKPT-S1X BEGIN payload short");
        return false;
    }
    int total = ((int)payload[1] << 8) | (int)payload[2];
    int chunks = (int)payload[3];
    uint16_t crc = ((uint16_t)payload[4] << 8) | (uint16_t)payload[5];

    if (total <= 0 || total > UNET_MAP_MAX_SIZE ||
        chunks <= 0 || chunks > UNET_MAP_MAX_CHUNKS) {
        s_log_kv("CKPT-S1X bad", total, chunks, (int)crc);
        return false;
    }

    /* Restart cleanly if a prior stream was mid-flight. */
    unet_map_stream_reset();

    g_map_stream.buffer = (uint8_t*)jo_malloc(total);
    if (!g_map_stream.buffer) {
        s_log_kv("CKPT-S2X alloc-fail", total, 0, 0);
        return false;
    }
    /* Zero the buffer so missing-chunk corruption fails loudly at parse
     * time rather than presenting as plausible garbage. */
    memset(g_map_stream.buffer, 0, (size_t)total);

    g_map_stream.total_size = total;
    g_map_stream.num_chunks = (uint8_t)chunks;
    g_map_stream.expected_crc = crc;
    g_map_stream.received_bytes = 0;
    g_map_stream.ready = false;
    g_map_stream.active = true;
    s_clear_mask();

    s_log_kv("CKPT-S1 BEGIN", total, chunks, (int)crc);
    s_log_kv("CKPT-S2 alloc", (int)((uintptr_t)g_map_stream.buffer & 0xFFFF), 0, 0);
    return true;
}

void unet_map_stream_on_chunk(const uint8_t* payload, int len)
{
    /* Body: [chunk_idx:1][len:1][data:N] starting at [1]. */
    if (!g_map_stream.active || !g_map_stream.buffer) {
        /* Chunk arriving without an active BEGIN — drop silently. Don't
         * spam logs here; could be tail of a previous match's stream
         * that crossed the GAME_START boundary. */
        return;
    }
    if (len < 1 + 2) {
        s_log_msg("CKPT-S3X CHUNK header short");
        return;
    }
    uint8_t idx = payload[1];
    uint8_t dlen = payload[2];
    if (idx >= g_map_stream.num_chunks) {
        s_log_kv("CKPT-S3X idx-oor", (int)idx, (int)g_map_stream.num_chunks, 0);
        return;
    }
    if (dlen > UNET_MAP_CHUNK_DATA_MAX) {
        s_log_kv("CKPT-S3X dlen-oor", (int)dlen, UNET_MAP_CHUNK_DATA_MAX, 0);
        return;
    }
    if (1 + 2 + (int)dlen > len) {
        s_log_kv("CKPT-S3X frame-short", (int)dlen, len, (int)idx);
        return;
    }

    int offset = (int)idx * UNET_MAP_CHUNK_DATA_MAX;
    if (offset + (int)dlen > g_map_stream.total_size) {
        s_log_kv("CKPT-S3X past-end", offset, (int)dlen, g_map_stream.total_size);
        return;
    }

    bool was_first = (g_map_stream.received_bytes == 0);
    bool seen_before = s_chunk_seen(idx);

    memcpy(g_map_stream.buffer + offset, payload + 3, dlen);
    if (!seen_before) {
        s_mark_chunk(idx);
        g_map_stream.received_bytes += (int)dlen;
    }

    if (was_first) s_log_kv("CKPT-S3 first", (int)idx, (int)dlen, g_map_stream.received_bytes);

    /* Last-chunk-of-set notice (count match, not the highest idx — chunks
     * may arrive out of order on a flaky modem). */
    int seen_count = 0;
    for (int i = 0; i < (int)sizeof(g_map_stream.chunks_received); i++) {
        uint8_t b = g_map_stream.chunks_received[i];
        while (b) { seen_count += b & 1; b >>= 1; }
    }
    /* Sparse progress log every 16th chunk so the server journal shows
     * the stream is alive during the ~7-8s wire transfer. Without this
     * there's silence between CKPT-S3 (first chunk) and CKPT-S4 (last)
     * which makes a stuck-mid-stream failure indistinguishable from a
     * working-but-slow stream. Only log when seen_count is on a 16
     * boundary to avoid flooding (chunks arrive at ~12/sec). */
    if (!was_first && (seen_count & 0xF) == 0 && seen_count != (int)g_map_stream.num_chunks) {
        s_log_kv("CKPT-S3p prog", seen_count, (int)g_map_stream.num_chunks, g_map_stream.received_bytes);
    }
    if (seen_count == (int)g_map_stream.num_chunks) {
        s_log_kv("CKPT-S4 all-chunks", seen_count, g_map_stream.received_bytes, g_map_stream.total_size);
    }
}

bool unet_map_stream_on_end(void)
{
    s_log_msg("CKPT-S5 END");
    if (!g_map_stream.active || !g_map_stream.buffer) {
        s_log_msg("CKPT-S5X END without active stream");
        return false;
    }

    /* All chunks must be present. */
    int seen = 0;
    for (int i = 0; i < (int)sizeof(g_map_stream.chunks_received); i++) {
        uint8_t b = g_map_stream.chunks_received[i];
        while (b) { seen += b & 1; b >>= 1; }
    }
    if (seen != (int)g_map_stream.num_chunks) {
        s_log_kv("CKPT-S6X missing", seen, (int)g_map_stream.num_chunks, 0);
        unet_map_stream_reset();
        return false;
    }

    if (g_map_stream.received_bytes != g_map_stream.total_size) {
        s_log_kv("CKPT-S6X size-mismatch",
                 g_map_stream.received_bytes, g_map_stream.total_size, 0);
        unet_map_stream_reset();
        return false;
    }

    /* CRC verify */
    uint16_t got = unet_map_stream_crc16(g_map_stream.buffer, g_map_stream.total_size);
    if (got != g_map_stream.expected_crc) {
        s_log_kv("CKPT-S6X crc", (int)got, (int)g_map_stream.expected_crc, 0);
        unet_map_stream_reset();
        return false;
    }

    g_map_stream.ready = true;
    g_map_stream.active = false;
    s_log_kv("CKPT-S6 ready", g_map_stream.total_size, (int)g_map_stream.num_chunks, (int)got);
    return true;
}

bool unet_map_stream_is_ready(void)
{
    return g_map_stream.ready && g_map_stream.buffer != 0;
}

uint8_t* unet_map_stream_take_buffer(int* out_size)
{
    if (!g_map_stream.ready || !g_map_stream.buffer) return 0;
    uint8_t* p = g_map_stream.buffer;
    int size = g_map_stream.total_size;
    /* Forget about the buffer — caller now owns it. Caller must
     * jo_free it (the existing Map ctor does this on its `streamStart`
     * pointer at the end of parse). */
    g_map_stream.buffer = 0;
    g_map_stream.ready = false;
    g_map_stream.active = false;
    g_map_stream.total_size = 0;
    g_map_stream.received_bytes = 0;
    g_map_stream.num_chunks = 0;
    g_map_stream.expected_crc = 0;
    s_clear_mask();
    if (out_size) *out_size = size;
    s_log_kv("CKPT-S7 taken", size, 0, 0);
    return p;
}

uint16_t unet_map_stream_crc16(const uint8_t* data, int len)
{
    /* CCITT-FALSE: poly 0x1021, init 0xFFFF, no xor-out, no reflection.
     * Bit-by-bit, runs once per map (~11 KB × 8 bits = 88K iterations).
     * At ~10 cycles/iteration on SH-2 that's ~880 K cycles ≈ 30 ms.
     * Acceptable one-time cost just before World ctor. */
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}
