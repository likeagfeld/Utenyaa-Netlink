/**
 * utenyaa_net.c - Utenyaa Networking State Machine
 *
 * Core FSM + RX dispatcher. All game-side effects are routed through the
 * callback table (unet_callbacks_t) set by the game code at startup —
 * this file has no knowledge of Utenyaa entity types, so the same
 * implementation works with the C++ engine layer cleanly.
 */

#include "utenyaa_net.h"
#include "utenyaa_protocol.h"
#include "net_transport.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*============================================================================
 * Globals
 *============================================================================*/

static unet_state_data_t g_net;
static unet_callbacks_t  g_cb;

/*============================================================================
 * Small helpers
 *============================================================================*/

static void clear_remote_inputs(void)
{
    int p, i;
    for (p = 0; p < UNET_MAX_PLAYERS; p++) {
        g_net.remote_input_head[p] = 0;
        for (i = 0; i < UNET_INPUT_BUFFER_PER_PLAYER; i++)
            g_net.remote_inputs[p][i].valid = false;
    }
}

static void clear_remote_players(void)
{
    int i;
    for (i = 0; i < UNET_MAX_PLAYERS; i++) g_net.remote_players[i].valid = false;
}

static void clear_crates(void)
{
    int i;
    for (i = 0; i < UNET_MAX_CRATES; i++) g_net.crates[i].active = false;
    g_net.crate_count = 0;
}

static void tx(const uint8_t* buf, int len)
{
    if (g_net.transport) net_transport_send(g_net.transport, buf, len);
}

/*============================================================================
 * Init / configuration
 *============================================================================*/

void unet_init(void)
{
    memset(&g_net, 0, sizeof(g_net));
    memset(&g_cb, 0, sizeof(g_cb));
    g_net.state = UNET_STATE_OFFLINE;
    g_net.my_player_id = 0xFF;
    g_net.my_player_id2 = 0xFF;
    g_net.my_character = 0xFF;
    g_net.my_character2 = 0xFF;
    g_net.my_stage_vote = 0xFF;
    clear_remote_inputs();
    clear_remote_players();
    clear_crates();
    unet_rx_init(&g_net.rx, g_net.rx_buf, sizeof(g_net.rx_buf));
}

void unet_set_callbacks(const unet_callbacks_t* cb)
{
    if (cb) g_cb = *cb;
    else memset(&g_cb, 0, sizeof(g_cb));
}

void unet_set_modem_available(bool available) { g_net.modem_available = available; }
void unet_set_transport(const net_transport_t* t) { g_net.transport = t; }

void unet_set_username(const char* name)
{
    int i;
    for (i = 0; i < UNET_MAX_NAME && name[i]; i++) g_net.my_name[i] = name[i];
    g_net.my_name[i] = '\0';
}

void unet_set_username_p2(const char* name)
{
    int i;
    for (i = 0; i < UNET_MAX_NAME && name[i]; i++) g_net.my_name2[i] = name[i];
    g_net.my_name2[i] = '\0';
}

unet_state_t unet_get_state(void) { return g_net.state; }
const unet_state_data_t* unet_get_data(void) { return &g_net; }

void unet_log(const char* msg)
{
    int i, n;
    if (!msg) return;
    if (g_net.log_count >= 4) {
        for (i = 0; i < 3; i++) memcpy(g_net.log_lines[i], g_net.log_lines[i+1], 40);
        g_net.log_count = 3;
    }
    n = 0;
    while (msg[n] && n < 39) { g_net.log_lines[g_net.log_count][n] = msg[n]; n++; }
    g_net.log_lines[g_net.log_count][n] = '\0';
    g_net.log_count++;
}

void unet_clear_log(void)
{
    int i;
    for (i = 0; i < 4; i++) g_net.log_lines[i][0] = '\0';
    g_net.log_count = 0;
}

void unet_enter_offline(void)
{
    g_net.state = UNET_STATE_OFFLINE;
    g_net.status_msg = "Offline";
}

/*============================================================================
 * Connect / auth
 *============================================================================*/

void unet_on_connected(void)
{
    int len;
    unet_rx_init(&g_net.rx, g_net.rx_buf, sizeof(g_net.rx_buf));
    g_net.state = UNET_STATE_AUTHENTICATING;
    g_net.status_msg = "Authenticating...";
    g_net.auth_timer = 0;
    g_net.auth_retries = 0;
    g_net.heartbeat_counter = 0;
    if (g_net.has_uuid)
        len = unet_encode_connect_uuid(g_net.tx_buf, g_net.my_uuid);
    else
        len = unet_encode_connect(g_net.tx_buf);
    tx(g_net.tx_buf, len);
    unet_log("Sent CONNECT");
}

/*============================================================================
 * Incoming message handlers
 *============================================================================*/

static void on_welcome(const uint8_t* p, int len)
{
    int off, i;
    if (len < 2) return;
    g_net.my_player_id = p[1];
    off = 2;
    if (off + SNCP_UUID_LEN <= len) {
        for (i = 0; i < SNCP_UUID_LEN; i++) g_net.my_uuid[i] = (char)p[off + i];
        g_net.my_uuid[SNCP_UUID_LEN] = '\0';
        g_net.has_uuid = true;
        off += SNCP_UUID_LEN;
    }
    if (off < len) {
        unet_read_string(&p[off], len - off, g_net.my_name, UNET_MAX_NAME + 1);
    }
    g_net.state = UNET_STATE_LOBBY;
    g_net.status_msg = "In Lobby";
    unet_log("Welcome!");
    if (g_net.has_second_local && g_net.my_name2[0] != '\0') {
        unet_send_add_local_player(g_net.my_name2);
    }
}

static void on_username_required(void)
{
    int len;
    g_net.state = UNET_STATE_USERNAME;
    g_net.status_msg = "Enter username";
    if (g_net.my_name[0] != '\0') {
        len = unet_encode_set_username(g_net.tx_buf, g_net.my_name);
        tx(g_net.tx_buf, len);
        g_net.state = UNET_STATE_AUTHENTICATING;
        g_net.status_msg = "Authenticating...";
    }
}

static void on_username_taken(void)
{
    g_net.username_retry++;
    g_net.status_msg = "Username taken";
}

static void on_lobby_state(const uint8_t* p, int len)
{
    int off, i, consumed, k;
    if (len < 2) return;
    g_net.lobby_count = p[1];
    if (g_net.lobby_count > UNET_MAX_PLAYERS) g_net.lobby_count = UNET_MAX_PLAYERS;
    off = 2;
    for (i = 0; i < g_net.lobby_count && off < len; i++) {
        g_net.lobby_players[i].id = p[off++];
        consumed = unet_read_string(&p[off], len - off,
                                    g_net.lobby_players[i].name, UNET_MAX_NAME + 1);
        if (consumed < 0) break;
        off += consumed;
        g_net.lobby_players[i].ready        = (off < len) ? (p[off++] != 0) : false;
        g_net.lobby_players[i].character_id = (off < len) ? p[off++] : 0xFF;
        g_net.lobby_players[i].stage_vote   = (off < len) ? p[off++] : 0xFF;
        g_net.lobby_players[i].active = true;
    }
    for (; i < UNET_MAX_PLAYERS; i++) g_net.lobby_players[i].active = false;

    for (k = 0; k < g_net.lobby_count; k++) {
        if (g_net.lobby_players[k].id == g_net.my_player_id) {
            g_net.my_ready      = g_net.lobby_players[k].ready;
            g_net.my_character  = g_net.lobby_players[k].character_id;
            g_net.my_stage_vote = g_net.lobby_players[k].stage_vote;
            break;
        }
    }
}

static void on_stage_vote_tally(const uint8_t* p, int len)
{
    int i, off = 1;
    uint8_t s, v;
    for (i = 0; i < UNET_STAGE_COUNT; i++) {
        if (off + 1 >= len) break;
        s = p[off++];
        v = p[off++];
        if (s < UNET_STAGE_COUNT) g_net.stage_vote_tally[s] = v;
    }
}

static void on_game_start(const uint8_t* p, int len)
{
    int off, i, cc;
    if (len < 11) return;
    off = 1;
    g_net.game_seed           = unet_r_u32(&p[off]); off += 4;
    g_net.my_player_id        = p[off++];
    g_net.stage_id            = p[off++];
    g_net.opponent_count      = p[off++];
    g_net.match_seconds_total = unet_r_u16(&p[off]); off += 2;
    g_net.match_seconds_left  = g_net.match_seconds_total;
    g_net.sudden_death = false;
    clear_crates();
    if (off < len) {
        cc = p[off++];
        if (cc > UNET_MAX_CRATES) cc = UNET_MAX_CRATES;
        for (i = 0; i < cc && off + 13 <= len; i++) {
            g_net.crates[i].slot  = p[off++];
            g_net.crates[i].x     = unet_r_i32(&p[off]); off += 4;
            g_net.crates[i].y     = unet_r_i32(&p[off]); off += 4;
            g_net.crates[i].z     = unet_r_i32(&p[off]); off += 4;
            g_net.crates[i].flags = p[off++];
            g_net.crates[i].active = true;
            if (g_cb.on_crate_spawn)
                g_cb.on_crate_spawn(g_net.crates[i].slot,
                                    g_net.crates[i].x, g_net.crates[i].y,
                                    g_net.crates[i].z, g_net.crates[i].flags);
        }
        g_net.crate_count = cc;
    }
    g_net.game_roster_count = g_net.lobby_count;
    for (i = 0; i < g_net.lobby_count; i++) {
        g_net.game_roster[i].id = g_net.lobby_players[i].id;
        memcpy(g_net.game_roster[i].name, g_net.lobby_players[i].name, UNET_MAX_NAME + 1);
        g_net.game_roster[i].active = g_net.lobby_players[i].active;
        g_net.game_roster[i].character_id = g_net.lobby_players[i].character_id;
    }
    clear_remote_players();
    g_net.local_frame = 0;
    g_net.state = UNET_STATE_PLAYING;
    g_net.status_msg = "Playing";
}

static void on_player_sync(const uint8_t* p, int len)
{
    uint8_t pid;
    if (len < 32) return;
    pid = p[1];
    if (pid >= UNET_MAX_PLAYERS) return;
    g_net.remote_players[pid].x      = unet_r_i32(&p[2]);
    g_net.remote_players[pid].y      = unet_r_i32(&p[6]);
    g_net.remote_players[pid].z      = unet_r_i32(&p[10]);
    g_net.remote_players[pid].dx     = unet_r_i32(&p[14]);
    g_net.remote_players[pid].dy     = unet_r_i32(&p[18]);
    g_net.remote_players[pid].dz     = unet_r_i32(&p[22]);
    g_net.remote_players[pid].angle  = unet_r_i16(&p[26]);
    g_net.remote_players[pid].health = p[28];
    g_net.remote_players[pid].pickup = p[29];
    g_net.remote_players[pid].last_update_frame = g_net.local_frame;
    g_net.remote_players[pid].valid  = true;
}

static void on_input_relay(const uint8_t* p, int len)
{
    uint8_t pid;
    int head;
    if (len < 5) return;
    pid = p[1];
    if (pid >= UNET_MAX_PLAYERS) return;
    head = g_net.remote_input_head[pid];
    g_net.remote_inputs[pid][head].frame_num = unet_r_u16(&p[2]);
    g_net.remote_inputs[pid][head].input_bits = p[4];
    g_net.remote_inputs[pid][head].player_id = pid;
    g_net.remote_inputs[pid][head].valid = true;
    g_net.remote_input_head[pid] = (head + 1) % UNET_INPUT_BUFFER_PER_PLAYER;
}

static void on_bullet_spawn(const uint8_t* p, int len)
{
    if (len < 28) return;
    if (g_cb.on_bullet_spawn)
        g_cb.on_bullet_spawn(unet_r_u16(&p[1]), p[3],
                             unet_r_i32(&p[4]),  unet_r_i32(&p[8]),  unet_r_i32(&p[12]),
                             unet_r_i32(&p[16]), unet_r_i32(&p[20]), unet_r_i32(&p[24]));
}

static void on_mine_spawn(const uint8_t* p, int len)
{
    if (len < 16) return;
    if (g_cb.on_mine_spawn)
        g_cb.on_mine_spawn(unet_r_u16(&p[1]), p[3],
                           unet_r_i32(&p[4]), unet_r_i32(&p[8]), unet_r_i32(&p[12]));
}

static void on_bomb_spawn(const uint8_t* p, int len)
{
    if (len < 28) return;
    if (g_cb.on_bomb_spawn)
        g_cb.on_bomb_spawn(unet_r_u16(&p[1]), p[3],
                           unet_r_i32(&p[4]),  unet_r_i32(&p[8]),  unet_r_i32(&p[12]),
                           unet_r_i32(&p[16]), unet_r_i32(&p[20]), unet_r_i32(&p[24]));
}

static void on_explosion(const uint8_t* p, int len)
{
    static uint8_t victims[UNET_MAX_PLAYERS * 2];
    int32_t x, y, z;
    uint16_t r;
    uint8_t n;
    int i;
    if (len < 16) return;
    x = unet_r_i32(&p[1]); y = unet_r_i32(&p[5]); z = unet_r_i32(&p[9]);
    r = unet_r_u16(&p[13]);
    n = p[15];
    if (n > UNET_MAX_PLAYERS) n = UNET_MAX_PLAYERS;
    for (i = 0; i < n && 16 + i*2 + 1 < len; i++) {
        victims[i*2]     = p[16 + i*2];
        victims[i*2 + 1] = p[17 + i*2];
    }
    if (g_cb.on_explosion) g_cb.on_explosion(x, y, z, r, victims, n);
}

static void on_crate_spawn(const uint8_t* p, int len)
{
    uint8_t slot, flags;
    int32_t x, y, z;
    if (len < 15) return;
    slot = p[1];
    x = unet_r_i32(&p[2]); y = unet_r_i32(&p[6]); z = unet_r_i32(&p[10]);
    flags = p[14];
    if (slot < UNET_MAX_CRATES) {
        g_net.crates[slot].slot = slot;
        g_net.crates[slot].x = x; g_net.crates[slot].y = y; g_net.crates[slot].z = z;
        g_net.crates[slot].flags = flags;
        g_net.crates[slot].active = true;
    }
    if (g_cb.on_crate_spawn) g_cb.on_crate_spawn(slot, x, y, z, flags);
}

static void on_crate_destroy(const uint8_t* p, int len)
{
    if (len < 4) return;
    if (p[1] < UNET_MAX_CRATES) g_net.crates[p[1]].active = false;
    if (g_cb.on_crate_destroy) g_cb.on_crate_destroy(p[1], p[2], p[3]);
}

static void on_damage(const uint8_t* p, int len)
{
    if (len < 5) return;
    if (p[1] < UNET_MAX_PLAYERS) g_net.remote_players[p[1]].health = p[4];
    if (g_cb.on_damage) g_cb.on_damage(p[1], p[2], p[3], p[4]);
}

static void on_player_kill(const uint8_t* p, int len)
{
    if (len < 3) return;
    if (g_cb.on_player_kill) g_cb.on_player_kill(p[1], p[2]);
}

static void on_match_timer(const uint8_t* p, int len)
{
    if (len < 3) return;
    g_net.match_seconds_left = unet_r_u16(&p[1]);
    if (g_cb.on_match_timer) g_cb.on_match_timer(g_net.match_seconds_left);
}

static void on_sudden_death(void)
{
    g_net.sudden_death = true;
    if (g_cb.on_sudden_death) g_cb.on_sudden_death();
}

static void on_score_update(const uint8_t* p, int len)
{
    if (len < 5) return;
    if (p[1] < UNET_MAX_PLAYERS) {
        g_net.score_kills[p[1]]   = p[2];
        g_net.score_deaths[p[1]]  = p[3];
        g_net.score_best_hp[p[1]] = p[4];
    }
}

static void on_game_over(const uint8_t* p, int len)
{
    int i, off, n;
    uint8_t pid, hp, kk, dd;
    if (len < 4) return;
    g_net.last_winner_id    = p[1];
    g_net.last_sudden_death = (p[2] != 0);
    n = p[3];
    if (n > UNET_MAX_PLAYERS) n = UNET_MAX_PLAYERS;
    off = 4;
    memset(g_net.last_scores_hp, 0, sizeof(g_net.last_scores_hp));
    memset(g_net.last_scores_kills, 0, sizeof(g_net.last_scores_kills));
    memset(g_net.last_scores_deaths, 0, sizeof(g_net.last_scores_deaths));
    for (i = 0; i < n && off + 4 <= len; i++) {
        pid = p[off++]; hp = p[off++]; kk = p[off++]; dd = p[off++];
        if (pid < UNET_MAX_PLAYERS) {
            g_net.last_scores_hp[pid]     = hp;
            g_net.last_scores_kills[pid]  = kk;
            g_net.last_scores_deaths[pid] = dd;
        }
    }
    g_net.has_last_results = true;
    g_net.state = UNET_STATE_LOBBY;
    if (g_cb.on_game_over) g_cb.on_game_over(p[1], g_net.last_sudden_death);
}

static void on_leaderboard_data(const uint8_t* p, int len)
{
    int off, i, consumed, n;
    if (len < 2) return;
    n = p[1];
    if (n > UNET_LEADERBOARD_MAX) n = UNET_LEADERBOARD_MAX;
    off = 2;
    for (i = 0; i < n; i++) {
        consumed = unet_read_string(&p[off], len - off,
                                    g_net.leaderboard[i].name, UNET_MAX_NAME + 1);
        if (consumed < 0) break;
        off += consumed;
        if (off + 9 > len) break;
        g_net.leaderboard[i].wins         = unet_r_u16(&p[off]); off += 2;
        g_net.leaderboard[i].best_hp      = p[off++];
        g_net.leaderboard[i].kills        = unet_r_u16(&p[off]); off += 2;
        g_net.leaderboard[i].deaths       = unet_r_u16(&p[off]); off += 2;
        g_net.leaderboard[i].games_played = unet_r_u16(&p[off]); off += 2;
    }
    g_net.leaderboard_count = i;
}

static void on_local_player_ack(const uint8_t* p, int len)
{
    if (len < 2) return;
    if (p[1] != 0xFF) {
        g_net.my_player_id2 = p[1];
        g_net.has_second_local = true;
    }
}

static void on_character_taken(const uint8_t* p, int len)
{
    (void)p; (void)len;
    g_net.status_msg = "Character taken";
}

static void on_log(const uint8_t* p, int len)
{
    char line[40];
    int n, i;
    if (len < 2) return;
    n = p[1];
    if (n > 38) n = 38;
    if (len < 2 + n) return;
    for (i = 0; i < n; i++) line[i] = (char)p[2 + i];
    line[n] = '\0';
    unet_log(line);
}

/*============================================================================
 * RX dispatch
 *============================================================================*/

static void dispatch(const uint8_t* p, int len)
{
    if (len < 1) return;
    switch (p[0]) {
    case SNCP_MSG_WELCOME:           on_welcome(p, len); break;
    case SNCP_MSG_WELCOME_BACK:      on_welcome(p, len); break;
    case SNCP_MSG_USERNAME_REQUIRED: on_username_required(); break;
    case SNCP_MSG_USERNAME_TAKEN:    on_username_taken(); break;

    case UNET_MSG_LOBBY_STATE:       on_lobby_state(p, len); break;
    case UNET_MSG_STAGE_VOTE_TALLY:  on_stage_vote_tally(p, len); break;
    case UNET_MSG_GAME_START:        on_game_start(p, len); break;
    case UNET_MSG_INPUT_RELAY:       on_input_relay(p, len); break;
    case UNET_MSG_PLAYER_SYNC:       on_player_sync(p, len); break;
    case UNET_MSG_BULLET_SPAWN:      on_bullet_spawn(p, len); break;
    case UNET_MSG_MINE_SPAWN:        on_mine_spawn(p, len); break;
    case UNET_MSG_BOMB_SPAWN:        on_bomb_spawn(p, len); break;
    case UNET_MSG_EXPLOSION:         on_explosion(p, len); break;
    case UNET_MSG_CRATE_SPAWN:       on_crate_spawn(p, len); break;
    case UNET_MSG_CRATE_DESTROY:     on_crate_destroy(p, len); break;
    case UNET_MSG_DAMAGE:            on_damage(p, len); break;
    case UNET_MSG_PLAYER_KILL:       on_player_kill(p, len); break;
    case UNET_MSG_MATCH_TIMER:       on_match_timer(p, len); break;
    case UNET_MSG_SUDDEN_DEATH:      on_sudden_death(); break;
    case UNET_MSG_SCORE_UPDATE:      on_score_update(p, len); break;
    case UNET_MSG_GAME_OVER:         on_game_over(p, len); break;
    case UNET_MSG_LEADERBOARD_DATA:  on_leaderboard_data(p, len); break;
    case UNET_MSG_LOCAL_PLAYER_ACK:  on_local_player_ack(p, len); break;
    case UNET_MSG_CHARACTER_TAKEN:   on_character_taken(p, len); break;
    case UNET_MSG_LOG:               on_log(p, len); break;
    case UNET_MSG_PLAYER_JOIN:       /* handled via LOBBY_STATE */ break;
    case UNET_MSG_PLAYER_LEAVE:      /* handled via LOBBY_STATE */ break;
    case UNET_MSG_PAUSE_ACK:         /* UI-only */ break;
    default: break;
    }
}

/*============================================================================
 * Main tick
 *============================================================================*/

void unet_tick(void)
{
    int pkts, len, n;
    if (!g_net.transport) return;
    if (g_net.state == UNET_STATE_OFFLINE) return;

    for (pkts = 0; pkts < UNET_MAX_PACKETS_FRAME; pkts++) {
        len = unet_rx_poll(&g_net.rx, g_net.transport);
        if (len <= 0) break;
        dispatch(g_net.rx_buf, len);
    }

    g_net.heartbeat_counter++;
    if (g_net.heartbeat_counter >= UNET_HEARTBEAT_INTERVAL) {
        g_net.heartbeat_counter = 0;
        if (g_net.state == UNET_STATE_LOBBY || g_net.state == UNET_STATE_PLAYING) {
            n = unet_encode_heartbeat(g_net.tx_buf);
            tx(g_net.tx_buf, n);
        }
    }

    if (g_net.state == UNET_STATE_AUTHENTICATING) {
        g_net.auth_timer++;
        if (g_net.auth_timer >= UNET_AUTH_TIMEOUT) {
            g_net.auth_timer = 0;
            g_net.auth_retries++;
            if (g_net.auth_retries >= UNET_AUTH_MAX_RETRIES) {
                g_net.state = UNET_STATE_DISCONNECTED;
                g_net.status_msg = "Auth timed out";
            } else {
                n = g_net.has_uuid
                    ? unet_encode_connect_uuid(g_net.tx_buf, g_net.my_uuid)
                    : unet_encode_connect(g_net.tx_buf);
                tx(g_net.tx_buf, n);
            }
        }
    }

    g_net.frame_count++;
    if (g_net.state == UNET_STATE_PLAYING) g_net.local_frame++;

    if (g_net.death_retry_timer > 0)   g_net.death_retry_timer--;
    if (g_net.death_retry_timer_p2 > 0) g_net.death_retry_timer_p2--;
    if (g_net.send_cooldown < 0xFFFF)    g_net.send_cooldown++;
    if (g_net.send_cooldown_p2 < 0xFFFF) g_net.send_cooldown_p2++;
}

/*============================================================================
 * Client -> Server send helpers
 *============================================================================*/

static int simple_frame(uint8_t* buf, uint8_t op, int extra_len)
{
    int payload = 1 + extra_len;
    buf[0] = (uint8_t)((payload >> 8) & 0xFF);
    buf[1] = (uint8_t)(payload & 0xFF);
    buf[2] = op;
    return 2 + payload;
}

void unet_send_ready(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_READY, 0);
    tx(g_net.tx_buf, n);
    g_net.my_ready = !g_net.my_ready;
}

bool unet_is_ready(void) { return g_net.my_ready; }

void unet_send_start_game(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_START_GAME_REQ, 0);
    tx(g_net.tx_buf, n);
}

void unet_send_character_select(uint8_t char_id)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_CHARACTER_SELECT, 1);
    g_net.tx_buf[3] = char_id;
    tx(g_net.tx_buf, n);
}

void unet_send_character_select_p2(uint8_t char_id)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_CHARACTER_SELECT_P2, 2);
    g_net.tx_buf[3] = g_net.my_player_id2;
    g_net.tx_buf[4] = char_id;
    tx(g_net.tx_buf, n);
}

void unet_send_stage_vote(uint8_t stage_id)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_STAGE_VOTE, 1);
    g_net.tx_buf[3] = stage_id;
    tx(g_net.tx_buf, n);
    g_net.my_stage_vote = stage_id;
}

void unet_send_bot_add(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_BOT_ADD, 0);
    tx(g_net.tx_buf, n);
}

void unet_send_bot_remove(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_BOT_REMOVE, 0);
    tx(g_net.tx_buf, n);
}

void unet_send_stage_loaded_ack(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_STAGE_LOADED_ACK, 0);
    tx(g_net.tx_buf, n);
}

void unet_send_input_delta(uint16_t frame_num, uint8_t input_bits)
{
    int n;
    bool force = (g_net.send_cooldown >= UNET_INPUT_KEEPALIVE_FRAMES);
    if (!force && input_bits == g_net.last_sent_input) return;
    n = unet_encode_input(g_net.tx_buf, frame_num, input_bits);
    tx(g_net.tx_buf, n);
    g_net.last_sent_input = input_bits;
    g_net.send_cooldown = 0;
}

void unet_send_input_delta_p2(uint16_t frame_num, uint8_t input_bits)
{
    int payload;
    bool force = (g_net.send_cooldown_p2 >= UNET_INPUT_KEEPALIVE_FRAMES);
    if (!force && input_bits == g_net.last_sent_input_p2) return;
    payload = 1 + 1 + 2 + 1;
    g_net.tx_buf[0] = 0x00;
    g_net.tx_buf[1] = (uint8_t)payload;
    g_net.tx_buf[2] = UNET_MSG_INPUT_STATE_P2;
    g_net.tx_buf[3] = g_net.my_player_id2;
    unet_w_u16(&g_net.tx_buf[4], frame_num);
    g_net.tx_buf[6] = input_bits;
    tx(g_net.tx_buf, 2 + payload);
    g_net.last_sent_input_p2 = input_bits;
    g_net.send_cooldown_p2 = 0;
}

static void send_player_state_core(uint8_t pid_or_ff,
                                   int32_t x, int32_t y, int32_t z,
                                   int32_t dx, int32_t dy, int32_t dz,
                                   int16_t angle, uint8_t health)
{
    bool is_p2 = (pid_or_ff != 0xFF);
    int payload = 1 + (is_p2 ? 1 : 0) + 4*6 + 2 + 1;
    int off = 0;
    g_net.tx_buf[off++] = (uint8_t)((payload >> 8) & 0xFF);
    g_net.tx_buf[off++] = (uint8_t)(payload & 0xFF);
    g_net.tx_buf[off++] = UNET_MSG_PLAYER_STATE;
    if (is_p2) g_net.tx_buf[off++] = pid_or_ff;
    unet_w_u32(&g_net.tx_buf[off], (uint32_t)x);  off += 4;
    unet_w_u32(&g_net.tx_buf[off], (uint32_t)y);  off += 4;
    unet_w_u32(&g_net.tx_buf[off], (uint32_t)z);  off += 4;
    unet_w_u32(&g_net.tx_buf[off], (uint32_t)dx); off += 4;
    unet_w_u32(&g_net.tx_buf[off], (uint32_t)dy); off += 4;
    unet_w_u32(&g_net.tx_buf[off], (uint32_t)dz); off += 4;
    unet_w_u16(&g_net.tx_buf[off], (uint16_t)angle); off += 2;
    g_net.tx_buf[off++] = health;
    tx(g_net.tx_buf, off);
}

void unet_send_player_state(int32_t x, int32_t y, int32_t z,
                            int32_t dx, int32_t dy, int32_t dz,
                            int16_t angle, uint8_t health)
{
    send_player_state_core(0xFF, x, y, z, dx, dy, dz, angle, health);
}

void unet_send_player_state_p2(int32_t x, int32_t y, int32_t z,
                               int32_t dx, int32_t dy, int32_t dz,
                               int16_t angle, uint8_t health)
{
    send_player_state_core(g_net.my_player_id2, x, y, z, dx, dy, dz, angle, health);
}

static void send_pos_vel(uint8_t op, int32_t x, int32_t y, int32_t z,
                         int32_t dx, int32_t dy, int32_t dz)
{
    int payload = 1 + 4*6;
    g_net.tx_buf[0] = (uint8_t)((payload >> 8) & 0xFF);
    g_net.tx_buf[1] = (uint8_t)(payload & 0xFF);
    g_net.tx_buf[2] = op;
    unet_w_u32(&g_net.tx_buf[3],  (uint32_t)x);
    unet_w_u32(&g_net.tx_buf[7],  (uint32_t)y);
    unet_w_u32(&g_net.tx_buf[11], (uint32_t)z);
    unet_w_u32(&g_net.tx_buf[15], (uint32_t)dx);
    unet_w_u32(&g_net.tx_buf[19], (uint32_t)dy);
    unet_w_u32(&g_net.tx_buf[23], (uint32_t)dz);
    tx(g_net.tx_buf, 2 + payload);
}

void unet_send_fire_bullet(int32_t x, int32_t y, int32_t z,
                           int32_t dx, int32_t dy, int32_t dz)
{
    send_pos_vel(UNET_MSG_CLIENT_FIRE_BULLET, x, y, z, dx, dy, dz);
}

void unet_send_throw_bomb(int32_t x, int32_t y, int32_t z,
                          int32_t dx, int32_t dy, int32_t dz)
{
    send_pos_vel(UNET_MSG_CLIENT_THROW_BOMB, x, y, z, dx, dy, dz);
}

void unet_send_drop_mine(int32_t x, int32_t y, int32_t z)
{
    int payload = 1 + 4*3;
    g_net.tx_buf[0] = (uint8_t)((payload >> 8) & 0xFF);
    g_net.tx_buf[1] = (uint8_t)(payload & 0xFF);
    g_net.tx_buf[2] = UNET_MSG_CLIENT_DROP_MINE;
    unet_w_u32(&g_net.tx_buf[3],  (uint32_t)x);
    unet_w_u32(&g_net.tx_buf[7],  (uint32_t)y);
    unet_w_u32(&g_net.tx_buf[11], (uint32_t)z);
    tx(g_net.tx_buf, 2 + payload);
}

void unet_send_pickup_crate(uint8_t slot)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_CLIENT_PICKUP_CRATE, 1);
    g_net.tx_buf[3] = slot;
    tx(g_net.tx_buf, n);
}

void unet_send_player_death(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_CLIENT_DEATH, 0);
    tx(g_net.tx_buf, n);
    g_net.death_retry_timer = UNET_DEATH_RETRY_FRAMES;
}

void unet_send_player_death_p2(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_CLIENT_DEATH_P2, 1);
    g_net.tx_buf[3] = g_net.my_player_id2;
    tx(g_net.tx_buf, n);
    g_net.death_retry_timer_p2 = UNET_DEATH_RETRY_FRAMES;
}

void unet_send_pause(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_PAUSE_REQ, 0);
    tx(g_net.tx_buf, n);
}

void unet_send_add_local_player(const char* name)
{
    int nlen, i, payload;
    nlen = 0;
    while (name[nlen] && nlen < UNET_MAX_NAME) nlen++;
    payload = 1 + 1 + nlen;
    g_net.tx_buf[0] = (uint8_t)((payload >> 8) & 0xFF);
    g_net.tx_buf[1] = (uint8_t)(payload & 0xFF);
    g_net.tx_buf[2] = UNET_MSG_ADD_LOCAL_PLAYER;
    g_net.tx_buf[3] = (uint8_t)nlen;
    for (i = 0; i < nlen; i++) g_net.tx_buf[4 + i] = (uint8_t)name[i];
    tx(g_net.tx_buf, 2 + payload);
    unet_set_username_p2(name);
    g_net.has_second_local = true;
}

void unet_send_remove_local_player(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_REMOVE_LOCAL_PLAYER, 0);
    tx(g_net.tx_buf, n);
    g_net.has_second_local = false;
    g_net.my_player_id2 = 0xFF;
}

void unet_send_disconnect(void)
{
    int n = unet_encode_disconnect(g_net.tx_buf);
    tx(g_net.tx_buf, n);
    g_net.state = UNET_STATE_DISCONNECTED;
}

void unet_request_leaderboard(void)
{
    int n = simple_frame(g_net.tx_buf, UNET_MSG_LEADERBOARD_REQ, 0);
    tx(g_net.tx_buf, n);
}

int unet_get_remote_input(uint16_t frame_num, uint8_t player_id)
{
    int i, newest;
    uint16_t newest_frame, f;
    if (player_id >= UNET_MAX_PLAYERS) return -1;
    newest = -1;
    newest_frame = 0;
    for (i = 0; i < UNET_INPUT_BUFFER_PER_PLAYER; i++) {
        if (!g_net.remote_inputs[player_id][i].valid) continue;
        f = g_net.remote_inputs[player_id][i].frame_num;
        if (f <= frame_num && (newest < 0 || f > newest_frame)) {
            newest = i;
            newest_frame = f;
        }
    }
    if (newest < 0) return -1;
    return (int)g_net.remote_inputs[player_id][newest].input_bits;
}
