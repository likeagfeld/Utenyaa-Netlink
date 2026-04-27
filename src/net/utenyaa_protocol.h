/**
 * utenyaa_protocol.h - Utenyaa NetLink Protocol (SNCP framing)
 *
 * 4-player tank-arena for Sega Saturn NetLink.
 * Framing, auth, and RX state machine are byte-compatible with the
 * Coup/Disasteroids/Flicky protocol family. Opcodes and payloads are
 * Utenyaa-specific.
 *
 * Sync model: server-authoritative (Disasteroids "passthrough" style).
 * - Clients send INPUT_STATE + occasional PLAYER_STATE
 * - Server broadcasts PLAYER_SYNC, entity spawns, damage, match timer
 * - Local movement is client-authoritative (zero-lag); server corrects
 *   remote positions via periodic PLAYER_SYNC with client-side lerp
 */

#ifndef UTENYAA_PROTOCOL_H
#define UTENYAA_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "net_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * SNCP Auth Opcodes (shared with Coup/Disasteroids/Flicky)
 *============================================================================*/

#define SNCP_MSG_CONNECT           0x01
#define SNCP_MSG_SET_USERNAME      0x02
#define SNCP_MSG_HEARTBEAT         0x04
#define SNCP_MSG_DISCONNECT        0x05

#define SNCP_MSG_USERNAME_REQUIRED 0x81
#define SNCP_MSG_WELCOME           0x82
#define SNCP_MSG_WELCOME_BACK      0x83
#define SNCP_MSG_USERNAME_TAKEN    0x84

#define SNCP_UUID_LEN              36

/*============================================================================
 * Utenyaa Client -> Server Opcodes (0x10-0x2F)
 *============================================================================*/

#define UNET_MSG_READY             0x10  /* Toggle ready (no payload) */
#define UNET_MSG_INPUT_STATE       0x11  /* [frame:2 BE][input:1] */
#define UNET_MSG_START_GAME_REQ    0x12  /* No payload (only honored when 2+ ready) */
#define UNET_MSG_PAUSE_REQ         0x13  /* No payload */
#define UNET_MSG_PLAYER_STATE      0x14  /* [x:4][y:4][z:4][dx:4][dy:4][dz:4][angle:2][health:1] */
#define UNET_MSG_CHARACTER_SELECT  0x15  /* [char_id:1] - unique per player, server validates */
#define UNET_MSG_BOT_ADD           0x16  /* No payload */
#define UNET_MSG_BOT_REMOVE        0x17  /* No payload */
#define UNET_MSG_ADD_LOCAL_PLAYER  0x18  /* [name_len:1][name:N] - register P2 co-op */
#define UNET_MSG_REMOVE_LOCAL_PLAYER 0x19 /* No payload */
#define UNET_MSG_INPUT_STATE_P2    0x1A  /* [player_id:1][frame:2 BE][input:1] */
#define UNET_MSG_LEADERBOARD_REQ   0x1B  /* No payload */
#define UNET_MSG_CLIENT_DEATH      0x1C  /* No payload - client reports own HP=0 */
#define UNET_MSG_CLIENT_DEATH_P2   0x1D  /* [player_id:1] */
#define UNET_MSG_CLIENT_FIRE_BULLET 0x1E /* [dx:4][dy:4][dz:4][x:4][y:4][z:4] fxp 16.16 */
#define UNET_MSG_CLIENT_DROP_MINE  0x1F  /* [x:4][y:4][z:4] */
#define UNET_MSG_CLIENT_THROW_BOMB 0x20  /* [dx:4][dy:4][dz:4][x:4][y:4][z:4] */
#define UNET_MSG_CLIENT_PICKUP_CRATE 0x21 /* [slot:1] - request pickup; server rolls type */
#define UNET_MSG_STAGE_VOTE        0x22  /* [stage_id:1] 0=Island 1=Cross 2=Valley 3=Railway */
#define UNET_MSG_STAGE_LOADED_ACK  0x23  /* No payload - all peers ACK before GAME_START */
#define UNET_MSG_CHARACTER_SELECT_P2 0x24 /* [player_id:1][char_id:1] */

/*============================================================================
 * Utenyaa Server -> Client Opcodes (0xA0-0xBF)
 *============================================================================*/

#define UNET_MSG_LOBBY_STATE       0xA0  /* [count:1]{id:1,name:LP,ready:1,char:1,vote:1}... */
#define UNET_MSG_GAME_START        0xA1  /* [seed:4 BE][my_id:1][stage:1][player_count:1][timer:2 BE][crate_count:1]{crate roster}... */
#define UNET_MSG_INPUT_RELAY       0xA2  /* [player_id:1][frame:2 BE][input:1] */
#define UNET_MSG_PLAYER_JOIN       0xA3  /* [id:1][name:LP][char:1] */
#define UNET_MSG_PLAYER_LEAVE      0xA4  /* [id:1] */
#define UNET_MSG_GAME_OVER         0xA5  /* [winner_id:1][sudden_death:1][count:1]{id:1,hp:1,kills:1,deaths:1}... */
#define UNET_MSG_LOG               0xA6  /* [len:1][text:N] */
#define UNET_MSG_PAUSE_ACK         0xA7  /* [paused:1] */
#define UNET_MSG_MATCH_TIMER       0xA8  /* [seconds_remaining:2 BE] broadcast periodically */
#define UNET_MSG_PLAYER_SYNC       0xA9  /* [player_id:1][x:4][y:4][z:4][dx:4][dy:4][dz:4][angle:2][health:1][pickup:1] */
#define UNET_MSG_BULLET_SPAWN      0xAA  /* [entity_id:2 BE][origin_pid:1][x:4][y:4][z:4][dx:4][dy:4][dz:4] */
#define UNET_MSG_MINE_SPAWN        0xAB  /* [entity_id:2 BE][origin_pid:1][x:4][y:4][z:4] */
#define UNET_MSG_BOMB_SPAWN        0xAC  /* [entity_id:2 BE][origin_pid:1][x:4][y:4][z:4][dx:4][dy:4][dz:4] */
#define UNET_MSG_EXPLOSION         0xAD  /* [x:4][y:4][z:4][radius:2][victim_count:1]{pid:1,dmg:1}... */
#define UNET_MSG_CRATE_SPAWN       0xAE  /* [slot:1][x:4][y:4][z:4][flags:1] - flags=bits(Health|Bomb|Mine) */
#define UNET_MSG_CRATE_DESTROY     0xAF  /* [slot:1][picker_id:1][pickup_type:1] 0=Health 1=Bomb 2=Mine */
#define UNET_MSG_DAMAGE            0xB0  /* [victim_id:1][attacker_id:1][amount:1][new_hp:1] */
#define UNET_MSG_PLAYER_KILL       0xB1  /* [victim_id:1][attacker_id:1] */
#define UNET_MSG_SUDDEN_DEATH      0xB2  /* No payload - 120s timer expired, tied on HP */
#define UNET_MSG_SCORE_UPDATE      0xB3  /* [player_id:1][kills:1][deaths:1][best_hp:1] */
#define UNET_MSG_LEADERBOARD_DATA  0xB4  /* [count:1]{name_len:1,name:N,wins:2BE,best_hp:1,kills:2BE,deaths:2BE,games:2BE}... */
#define UNET_MSG_LOCAL_PLAYER_ACK  0xB5  /* [player_id:1] P2 assigned id */
#define UNET_MSG_CHARACTER_TAKEN   0xB6  /* [char_id:1] - reject, try another */
#define UNET_MSG_STAGE_VOTE_TALLY  0xB7  /* [{stage:1,votes:1}×4] live vote display */

/*============================================================================
 * Input Bitmask (fits in 1 byte)
 * Utenyaa controls: D-pad moves tank, A=shoot, B=drop mine, C=throw bomb,
 * START=pause. Shoulder triggers repurposed in lobby for char cycle.
 *============================================================================*/

#define UNET_INPUT_UP      (1 << 0)
#define UNET_INPUT_DOWN    (1 << 1)
#define UNET_INPUT_LEFT    (1 << 2)
#define UNET_INPUT_RIGHT   (1 << 3)
#define UNET_INPUT_SHOOT   (1 << 4)  /* A */
#define UNET_INPUT_MINE    (1 << 5)  /* B */
#define UNET_INPUT_BOMB    (1 << 6)  /* C */
#define UNET_INPUT_START   (1 << 7)  /* START/pause */

/* Lobby-only (reuses button bits; context-dependent decode) */
#define UNET_INPUT_CHAR_PREV  UNET_INPUT_LEFT
#define UNET_INPUT_CHAR_NEXT  UNET_INPUT_RIGHT

/*============================================================================
 * Limits & Buffer Sizes
 *============================================================================*/

#define UNET_MAX_PLAYERS        4
#define UNET_MAX_CHARACTERS    12  /* available skins; uniqueness enforced */
#define UNET_MAX_NAME_LEN      16
#define UNET_MAX_CRATES        16  /* per stage */
#define UNET_STAGE_COUNT        4  /* Island, Cross, Valley, Railway */
#define UNET_RX_FRAME_SIZE    512
#define UNET_TX_FRAME_SIZE    128
/* UART hardware FIFO is 16 bytes. At 14400 baud the modem delivers
 * ~1440 bytes/sec. Utenyaa renders at variable framerate (often 25-30
 * fps under SGL load), so 1440/25 = ~58 bytes incoming per game frame
 * worst case. The previous 48-byte budget could not keep up — FIFO
 * overflowed, bytes were lost, and SNCP frames went out of alignment.
 * Once misaligned the parser stays out of sync since each garbage
 * "length prefix" eats two more random bytes. Symptom: WELCOME never
 * decodes, client stuck in AUTHENTICATING for 5×UNET_AUTH_TIMEOUT
 * (~25-50s depending on framerate), then DISCONNECT → kicked to title.
 * 192 bytes/frame is well above 14.4 kbaud throughput at any plausible
 * framerate — keeps the FIFO drained even at 15 fps. */
#define UNET_RX_MAX_PER_POLL  192

#define UNET_HP_MAX             6  /* matches Player::MaxHealth */
#define UNET_MATCH_SECONDS    120  /* default; overridable in lobby */
#define UNET_MIN_TO_START       2  /* 1 console with co-op counts as 2 */

/* Stage IDs */
#define UNET_STAGE_ISLAND    0
#define UNET_STAGE_CROSS     1
#define UNET_STAGE_VALLEY    2
#define UNET_STAGE_RAILWAY   3

/* Pickup type IDs (match Messages::Pickup::PickupType ordering) */
#define UNET_PICKUP_HEALTH   0
#define UNET_PICKUP_BOMB     1
#define UNET_PICKUP_MINE     2

/*============================================================================
 * SNCP Framing: [LEN_HI][LEN_LO][payload...]
 *============================================================================*/

static inline void unet_send_frame(const net_transport_t* transport,
                                   const uint8_t* payload, int payload_len)
{
    uint8_t hdr[2];
    hdr[0] = (uint8_t)((payload_len >> 8) & 0xFF);
    hdr[1] = (uint8_t)(payload_len & 0xFF);
    net_transport_send(transport, hdr, 2);
    net_transport_send(transport, payload, payload_len);
}

typedef struct {
    uint8_t* buf;
    int      buf_size;
    int      rx_pos;
    int      frame_len;
} unet_rx_state_t;

static inline void unet_rx_init(unet_rx_state_t* st, uint8_t* buf, int buf_size)
{
    st->buf = buf;
    st->buf_size = buf_size;
    st->rx_pos = 0;
    st->frame_len = -1;
}

/**
 * Poll for a complete frame.
 *   >0 = payload length (in st->buf[0..len-1])
 *    0 = incomplete
 *   -1 = error (oversize or zero-length)
 */
/* Forward decl — defined in utenyaa_net.h, but we want unet_rx_poll
 * (an inline in this header) to bump the byte counter without pulling
 * the whole net header in here. The pointer is published below by the
 * net layer's init function. */
extern uint32_t* g_unet_rx_bytes_counter;

static inline int unet_rx_poll(unet_rx_state_t* st,
                               const net_transport_t* transport)
{
    int bytes_read = 0;
    while (bytes_read < UNET_RX_MAX_PER_POLL && net_transport_rx_ready(transport)) {
        uint8_t b = net_transport_rx_byte(transport);
        bytes_read++;
        if (g_unet_rx_bytes_counter) (*g_unet_rx_bytes_counter)++;

        if (st->frame_len < 0) {
            st->buf[st->rx_pos++] = b;
            if (st->rx_pos == 2) {
                st->frame_len = ((int)st->buf[0] << 8) | (int)st->buf[1];
                st->rx_pos = 0;
                if (st->frame_len > st->buf_size || st->frame_len == 0) {
                    st->frame_len = -1;
                    st->rx_pos = 0;
                    return -1;
                }
            }
        } else {
            st->buf[st->rx_pos++] = b;
            if (st->rx_pos >= st->frame_len) {
                int len = st->frame_len;
                st->frame_len = -1;
                st->rx_pos = 0;
                return len;
            }
        }
    }
    return 0;
}

/*============================================================================
 * Big-endian encode/decode helpers (fxp values are raw int32 16.16)
 *============================================================================*/

static inline void unet_w_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void unet_w_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static inline uint16_t unet_r_u16(const uint8_t* p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline int16_t unet_r_i16(const uint8_t* p)
{
    return (int16_t)unet_r_u16(p);
}

static inline uint32_t unet_r_u32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline int32_t unet_r_i32(const uint8_t* p)
{
    return (int32_t)unet_r_u32(p);
}

static inline int unet_read_string(const uint8_t* p, int remaining,
                                    char* dst, int max)
{
    int slen, copy, i;
    if (remaining < 1) { dst[0] = '\0'; return -1; }
    slen = (int)p[0];
    if (remaining < 1 + slen) { dst[0] = '\0'; return -1; }
    copy = (slen < max - 1) ? slen : (max - 1);
    for (i = 0; i < copy; i++) dst[i] = (char)p[1 + i];
    dst[copy] = '\0';
    return 1 + slen;
}

/*============================================================================
 * Core Encoders (client -> server)
 * Additional per-message encoders are defined in utenyaa_net.c
 *============================================================================*/

/** CONNECT (new user, no UUID). Returns total bytes written incl. length prefix. */
static inline int unet_encode_connect(uint8_t* buf)
{
    buf[0] = 0x00;
    buf[1] = 0x01;
    buf[2] = SNCP_MSG_CONNECT;
    return 3;
}

/** CONNECT with UUID (reconnect path). */
static inline int unet_encode_connect_uuid(uint8_t* buf, const char* uuid)
{
    int i;
    buf[0] = 0x00;
    buf[1] = 37;
    buf[2] = SNCP_MSG_CONNECT;
    for (i = 0; i < SNCP_UUID_LEN; i++)
        buf[3 + i] = (uint8_t)uuid[i];
    return 3 + SNCP_UUID_LEN;
}

/** SET_USERNAME. */
static inline int unet_encode_set_username(uint8_t* buf, const char* name)
{
    int nlen = 0;
    int payload_len, i;
    while (name[nlen]) nlen++;
    if (nlen > UNET_MAX_NAME_LEN) nlen = UNET_MAX_NAME_LEN;
    payload_len = 1 + 1 + nlen;
    buf[0] = (uint8_t)((payload_len >> 8) & 0xFF);
    buf[1] = (uint8_t)(payload_len & 0xFF);
    buf[2] = SNCP_MSG_SET_USERNAME;
    buf[3] = (uint8_t)nlen;
    for (i = 0; i < nlen; i++) buf[4 + i] = (uint8_t)name[i];
    return 2 + payload_len;
}

/** HEARTBEAT. */
static inline int unet_encode_heartbeat(uint8_t* buf)
{
    buf[0] = 0x00;
    buf[1] = 0x01;
    buf[2] = SNCP_MSG_HEARTBEAT;
    return 3;
}

/** DISCONNECT. */
static inline int unet_encode_disconnect(uint8_t* buf)
{
    buf[0] = 0x00;
    buf[1] = 0x01;
    buf[2] = SNCP_MSG_DISCONNECT;
    return 3;
}

/** INPUT_STATE. */
static inline int unet_encode_input(uint8_t* buf, uint16_t frame, uint8_t input)
{
    buf[0] = 0x00;
    buf[1] = 0x04;
    buf[2] = UNET_MSG_INPUT_STATE;
    unet_w_u16(&buf[3], frame);
    buf[5] = input;
    return 6;
}

#ifdef __cplusplus
}
#endif

#endif /* UTENYAA_PROTOCOL_H */
