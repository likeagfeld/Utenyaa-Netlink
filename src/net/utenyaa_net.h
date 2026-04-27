/**
 * utenyaa_net.h - Utenyaa Networking State Machine
 *
 * Full network lifecycle: modem detection, connection, authentication,
 * lobby, and in-game relay. Adapted from flock_net.h with Utenyaa-specific
 * state (4-player cap, character uniqueness, stage vote, health-based
 * combat, crate/bullet/mine/bomb authority, match timer + sudden death).
 *
 * Saturn + NetLink --phone--> USB Modem --serial--> Bridge --TCP--> Server
 */

#ifndef UTENYAA_NET_H
#define UTENYAA_NET_H

#include <stdint.h>
#include <stdbool.h>
#include "net_transport.h"
#include "utenyaa_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define UNET_MAX_NAME              16
#define UNET_HEARTBEAT_INTERVAL   600   /* frames (~10s at 60fps) */
#define UNET_AUTH_TIMEOUT         300   /* frames (~5s) */
#define UNET_AUTH_MAX_RETRIES       5
/* With UNET_RX_MAX_PER_POLL bumped to 192 bytes/frame, at average ~6
 * bytes/frame for typical opcodes we could see ~32 frames/poll worst
 * case. Bump dispatch budget to match so we don't leave parsed-but-
 * unprocessed frames sitting in the buffer. */
#define UNET_MAX_PACKETS_FRAME     48
#define UNET_INPUT_BUFFER_PER_PLAYER 8
#define UNET_LEADERBOARD_MAX       10

/* Delta compression: resend input at least every N frames as keepalive */
#define UNET_INPUT_KEEPALIVE_FRAMES 15

/* Death retry window (client-authoritative death) */
#define UNET_DEATH_RETRY_FRAMES    90

/*============================================================================
 * Network State Machine
 *============================================================================*/

typedef enum {
    UNET_STATE_OFFLINE = 0,
    UNET_STATE_CONNECTING,
    UNET_STATE_AUTHENTICATING,
    UNET_STATE_USERNAME,
    UNET_STATE_LOBBY,
    UNET_STATE_PLAYING,
    UNET_STATE_DISCONNECTED
} unet_state_t;

/*============================================================================
 * Lobby / Game Data
 *============================================================================*/

typedef struct {
    uint8_t id;
    char    name[UNET_MAX_NAME + 1];
    bool    ready;
    bool    active;
    uint8_t character_id;       /* Unique per player, 0..UNET_MAX_CHARACTERS-1 */
    uint8_t stage_vote;         /* 0xFF = no vote, else 0..UNET_STAGE_COUNT-1 */
} unet_lobby_player_t;

typedef struct {
    uint8_t id;
    char    name[UNET_MAX_NAME + 1];
    bool    active;
    uint8_t character_id;
} unet_roster_entry_t;

typedef struct {
    char     name[UNET_MAX_NAME + 1];
    uint16_t wins;
    uint8_t  best_hp;
    uint16_t kills;
    uint16_t deaths;
    uint16_t games_played;
} unet_leaderboard_entry_t;

typedef struct {
    uint16_t frame_num;
    uint8_t  input_bits;
    uint8_t  player_id;
    bool     valid;
} unet_input_entry_t;

/* Server-broadcast snapshot of a remote player (one slot of PLAYER_SYNC). */
typedef struct {
    int32_t  x, y, z;          /* Fxp raw (16.16) */
    int32_t  dx, dy, dz;       /* Fxp raw */
    int16_t  angle;            /* Trigonometry raw */
    uint8_t  health;
    uint8_t  pickup;           /* UNET_PICKUP_* */
    uint16_t last_update_frame;
    bool     valid;
} unet_player_sync_t;

/* Crate roster entry (sent in GAME_START + updates via CRATE_SPAWN/DESTROY). */
typedef struct {
    uint8_t  slot;
    uint8_t  flags;            /* bits(Health|Bomb|Mine) */
    int32_t  x, y, z;
    bool     active;
} unet_crate_entry_t;

/*============================================================================
 * Network State (single global; one per Saturn)
 *============================================================================*/

typedef struct {
    /* Connection */
    unet_state_t state;
    bool modem_available;
    const net_transport_t* transport;

    /* RX framing */
    unet_rx_state_t rx;
    uint8_t rx_buf[UNET_RX_FRAME_SIZE];
    uint8_t tx_buf[UNET_TX_FRAME_SIZE];

    /* Auth */
    char my_uuid[SNCP_UUID_LEN + 4];
    bool has_uuid;
    uint8_t my_player_id;
    uint8_t my_player_id2;     /* 0xFF if no P2 co-op */
    bool has_second_local;
    int auth_timer;
    int auth_retries;
    int username_retry;

    /* Identity */
    char my_name[UNET_MAX_NAME + 1];
    char my_name2[UNET_MAX_NAME + 1];

    /* Lobby */
    unet_lobby_player_t lobby_players[UNET_MAX_PLAYERS];
    int  lobby_count;
    bool my_ready;
    uint8_t my_character;
    uint8_t my_character2;
    uint8_t my_stage_vote;
    uint8_t stage_vote_tally[UNET_STAGE_COUNT];

    /* Game roster (survives lobby→playing for results screen) */
    unet_roster_entry_t game_roster[UNET_MAX_PLAYERS];
    int game_roster_count;

    /* Game config (from GAME_START) */
    uint32_t game_seed;
    uint8_t  stage_id;
    uint8_t  opponent_count;
    uint16_t match_seconds_total;
    uint16_t match_seconds_left;
    bool     sudden_death;

    /* Crate roster */
    unet_crate_entry_t crates[UNET_MAX_CRATES];
    int crate_count;

    /* Remote player snapshots (indexed by player_id) */
    unet_player_sync_t remote_players[UNET_MAX_PLAYERS];

    /* In-game input relay -- per-player ring buffers */
    unet_input_entry_t remote_inputs[UNET_MAX_PLAYERS][UNET_INPUT_BUFFER_PER_PLAYER];
    int remote_input_head[UNET_MAX_PLAYERS];
    uint16_t local_frame;

    /* Delta compression (P1 + P2) */
    uint8_t  last_sent_input;
    uint16_t send_cooldown;
    int      player_state_cooldown;
    uint8_t  last_sent_input_p2;
    uint16_t send_cooldown_p2;
    int      player_state_cooldown_p2;

    /* Death retry (client-authoritative local death) */
    int death_retry_timer;
    int death_retry_timer_p2;

    /* Score tracking (broadcast by server, shown in HUD) */
    uint8_t  score_kills[UNET_MAX_PLAYERS];
    uint8_t  score_deaths[UNET_MAX_PLAYERS];
    uint8_t  score_best_hp[UNET_MAX_PLAYERS];

    /* Timers */
    int heartbeat_counter;
    int frame_count;

    /* Status / UI */
    const char* status_msg;
    int connect_stage;
    char log_lines[4][40];
    int  log_count;

    /* Last game result */
    uint8_t last_winner_id;
    bool    last_sudden_death;
    bool    has_last_results;
    uint8_t last_scores_hp[UNET_MAX_PLAYERS];
    uint8_t last_scores_kills[UNET_MAX_PLAYERS];
    uint8_t last_scores_deaths[UNET_MAX_PLAYERS];

    /* Persistent leaderboard (from server) */
    unet_leaderboard_entry_t leaderboard[UNET_LEADERBOARD_MAX];
    int leaderboard_count;
} unet_state_data_t;

/*============================================================================
 * App-level event callbacks (set by game code; invoked from unet_tick)
 *============================================================================*/

typedef struct {
    /* Spawn a bullet as directed by the server. */
    void (*on_bullet_spawn)(uint16_t entity_id, uint8_t origin_pid,
                            int32_t x, int32_t y, int32_t z,
                            int32_t dx, int32_t dy, int32_t dz);
    void (*on_mine_spawn)(uint16_t entity_id, uint8_t origin_pid,
                          int32_t x, int32_t y, int32_t z);
    void (*on_bomb_spawn)(uint16_t entity_id, uint8_t origin_pid,
                          int32_t x, int32_t y, int32_t z,
                          int32_t dx, int32_t dy, int32_t dz);
    /* Server-resolved explosion with per-victim damage. */
    void (*on_explosion)(int32_t x, int32_t y, int32_t z, uint16_t radius,
                         const uint8_t* victims, int victim_count);
    /* Crate events (spawn on match start + respawn; destroy on pickup). */
    void (*on_crate_spawn)(uint8_t slot, int32_t x, int32_t y, int32_t z, uint8_t flags);
    void (*on_crate_destroy)(uint8_t slot, uint8_t picker_id, uint8_t pickup_type);
    /* Damage applied to a player (server-authoritative). */
    void (*on_damage)(uint8_t victim_id, uint8_t attacker_id,
                      uint8_t amount, uint8_t new_hp);
    void (*on_player_kill)(uint8_t victim_id, uint8_t attacker_id);
    void (*on_match_timer)(uint16_t seconds_remaining);
    void (*on_sudden_death)(void);
    void (*on_game_over)(uint8_t winner_id, bool sudden_death);
} unet_callbacks_t;

/*============================================================================
 * Public API
 *============================================================================*/

void unet_init(void);
void unet_set_callbacks(const unet_callbacks_t* cb);
void unet_set_modem_available(bool available);
void unet_set_transport(const net_transport_t* transport);
void unet_set_username(const char* name);
void unet_set_username_p2(const char* name);

unet_state_t unet_get_state(void);
const unet_state_data_t* unet_get_data(void);

void unet_on_connected(void);      /* Modem dial succeeded */
void unet_tick(void);              /* Call every frame */
void unet_enter_offline(void);

/* Lobby */
void unet_send_ready(void);
bool unet_is_ready(void);
void unet_send_start_game(void);
void unet_send_character_select(uint8_t char_id);
void unet_send_character_select_p2(uint8_t char_id);
void unet_send_stage_vote(uint8_t stage_id);
void unet_send_bot_add(void);
void unet_send_bot_remove(void);
void unet_send_stage_loaded_ack(void);

/* In-game */
void unet_send_input_delta(uint16_t frame_num, uint8_t input_bits);
void unet_send_input_delta_p2(uint16_t frame_num, uint8_t input_bits);
void unet_send_player_state(int32_t x, int32_t y, int32_t z,
                            int32_t dx, int32_t dy, int32_t dz,
                            int16_t angle, uint8_t health);
void unet_send_player_state_p2(int32_t x, int32_t y, int32_t z,
                               int32_t dx, int32_t dy, int32_t dz,
                               int16_t angle, uint8_t health);
void unet_send_fire_bullet(int32_t x, int32_t y, int32_t z,
                           int32_t dx, int32_t dy, int32_t dz);
void unet_send_drop_mine(int32_t x, int32_t y, int32_t z);
void unet_send_throw_bomb(int32_t x, int32_t y, int32_t z,
                          int32_t dx, int32_t dy, int32_t dz);
void unet_send_pickup_crate(uint8_t slot);
void unet_send_player_death(void);
void unet_send_player_death_p2(void);
void unet_send_pause(void);

/* Co-op */
void unet_send_add_local_player(const char* name);
void unet_send_remove_local_player(void);

/* Lifecycle */
void unet_send_disconnect(void);
void unet_request_leaderboard(void);
void unet_log(const char* msg);
void unet_clear_log(void);

/* Remote input lookup (returns -1 if none for that frame) */
int unet_get_remote_input(uint16_t frame_num, uint8_t player_id);

#ifdef __cplusplus
}
#endif

#endif /* UTENYAA_NET_H */
