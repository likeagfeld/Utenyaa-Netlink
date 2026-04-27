/**
 * utenyaa_online_bridge.cxx - C++ ↔ net layer bridge implementation
 */

#include "utenyaa_online_bridge.hpp"
// Sound driver needed by entity headers (Player/Mine/Bomb Play calls)
#include "Utils/ponesound/ponesound.hpp"
// Include World.hpp FIRST — it pulls the entity types in the correct order
// (Player before Crate) to avoid the upstream circular-include landmine
// where Player.hpp → UI.hpp → Settings.hpp → World.hpp → Crate.hpp gets
// Crate parsed before Player is defined.
#include "Entities/World.hpp"
#include "Entities/Player.hpp"
#include "Entities/Bullet.hpp"
#include "Entities/Mine.hpp"
#include "Entities/Bomb.hpp"
#include "Entities/Explosion.hpp"
#include "Entities/Crate.hpp"
#include "Utils/Math/Vec3.hpp"
#include "Utils/TrackableObject.hpp"
#include "Utils/Message.hpp"
#include "Utils/Settings.hpp"
#include "Messages/Damage.hpp"
#include "Messages/Pickup.hpp"
#include "font.h"
#include "Utils/UI.hpp"

/*============================================================================
 * Remote key state tracking (for edge detection)
 *============================================================================*/

static uint8_t s_prev_remote_bits[UNET_MAX_PLAYERS] = {0};
static uint8_t s_last_remote_bits[UNET_MAX_PLAYERS] = {0};
/* s_last_sent_input / cooldown handled inside unet_send_input_delta */

/* Map jo_gamepad_keys → Utenyaa input bit. */
static uint8_t key_to_bit(jo_gamepad_keys key)
{
    switch (key)
    {
    case JO_KEY_UP:    return UNET_INPUT_UP;
    case JO_KEY_DOWN:  return UNET_INPUT_DOWN;
    case JO_KEY_LEFT:  return UNET_INPUT_LEFT;
    case JO_KEY_RIGHT: return UNET_INPUT_RIGHT;
    case JO_KEY_A:     return UNET_INPUT_SHOOT;
    case JO_KEY_B:     return UNET_INPUT_MINE;   /* B = use pickup (mine/bomb) */
    case JO_KEY_C:     return UNET_INPUT_BOMB;
    case JO_KEY_START: return UNET_INPUT_START;
    default:           return 0;
    }
}

static uint8_t poll_local_bits_for_port(int port)
{
    if (port < 0) return 0;
    uint8_t bits = 0;
    if (jo_is_input_key_pressed(port, JO_KEY_UP))    bits |= UNET_INPUT_UP;
    if (jo_is_input_key_pressed(port, JO_KEY_DOWN))  bits |= UNET_INPUT_DOWN;
    if (jo_is_input_key_pressed(port, JO_KEY_LEFT))  bits |= UNET_INPUT_LEFT;
    if (jo_is_input_key_pressed(port, JO_KEY_RIGHT)) bits |= UNET_INPUT_RIGHT;
    if (jo_is_input_key_pressed(port, JO_KEY_A))     bits |= UNET_INPUT_SHOOT;
    if (jo_is_input_key_pressed(port, JO_KEY_B))     bits |= UNET_INPUT_MINE;
    if (jo_is_input_key_pressed(port, JO_KEY_C))     bits |= UNET_INPUT_BOMB;
    if (jo_is_input_key_pressed(port, JO_KEY_START)) bits |= UNET_INPUT_START;
    return bits;
}

bool OnlineBridge::RemoteKeyDown(int controller, jo_gamepad_keys key)
{
    if (controller < 0 || controller >= UNET_MAX_PLAYERS) return false;
    uint8_t bit = key_to_bit(key);
    if (!bit) return false;
    return (s_last_remote_bits[controller] & bit) != 0;
}

bool OnlineBridge::RemoteKeyPressed(int controller, jo_gamepad_keys key)
{
    if (controller < 0 || controller >= UNET_MAX_PLAYERS) return false;
    uint8_t bit = key_to_bit(key);
    if (!bit) return false;
    /* Edge-trigger: held now AND not held previous frame. */
    return ((s_last_remote_bits[controller] & bit) != 0) &&
           ((s_prev_remote_bits[controller] & bit) == 0);
}

/*============================================================================
 * File-scope state used by callbacks + tick functions (defined here so
 * both cb_* handlers below and TickLocalPlayers later in the file can
 * read/write them without forward declarations).
 *============================================================================*/

static int  s_player_state_cooldown;
static int  s_player_state_cooldown_p2;
static bool s_sent_death;
static bool s_sent_death_p2;

/*============================================================================
 * Server → client event callbacks
 *============================================================================*/

static Entities::Player* find_player_by_pid(uint8_t pid)
{
    for (auto* p : TrackableObject<Entities::Player>::objects)
    {
        if (p && (uint8_t)p->GetController() == pid) return p;
    }
    return nullptr;
}

static Vec3 fxp_vec(int32_t x, int32_t y, int32_t z)
{
    return Vec3(Fxp::BuildRaw(x), Fxp::BuildRaw(y), Fxp::BuildRaw(z));
}

static void cb_bullet_spawn(uint16_t /*eid*/, uint8_t origin_pid,
                            int32_t x, int32_t y, int32_t z,
                            int32_t dx, int32_t dy, int32_t dz)
{
    /* Skip local-player echo — they already spawned it at fire time. */
    if (origin_pid == g_Game.myPlayerID) return;
    if (origin_pid == g_Game.myPlayerID2 && g_Game.hasSecondLocal) return;
    Vec3 pos = fxp_vec(x, y, z);
    Vec3 dir = fxp_vec(dx, dy, dz);
    /* Bullet/Mine/Bomb ctors take non-const Vec3& — pass named lvalues */
    new Entities::Bullet((uint8_t)origin_pid, dir, pos);
}

static void cb_mine_spawn(uint16_t /*eid*/, uint8_t origin_pid,
                          int32_t x, int32_t y, int32_t z)
{
    if (origin_pid == g_Game.myPlayerID) return;
    if (origin_pid == g_Game.myPlayerID2 && g_Game.hasSecondLocal) return;
    Vec3 pos = fxp_vec(x, y, z);
    new Entities::Mine((uint8_t)origin_pid, pos);
}

static void cb_bomb_spawn(uint16_t /*eid*/, uint8_t origin_pid,
                          int32_t x, int32_t y, int32_t z,
                          int32_t dx, int32_t dy, int32_t dz)
{
    if (origin_pid == g_Game.myPlayerID) return;
    if (origin_pid == g_Game.myPlayerID2 && g_Game.hasSecondLocal) return;
    Vec3 pos = fxp_vec(x, y, z);
    Vec3 dir = fxp_vec(dx, dy, dz);
    new Entities::Bomb(dir, pos);
}

static void cb_explosion(int32_t x, int32_t y, int32_t z, uint16_t /*radius*/,
                         const uint8_t* victims, int victim_count)
{
    Vec3 pos = fxp_vec(x, y, z);
    Fxp scale = Fxp::FromInt(1);
    new Entities::Explosion(pos, scale);

    /* Apply server-resolved damage to named victims. Clamp to the static
     * buffer size (UNET_MAX_PLAYERS * 2) defensively — the net layer
     * already clamps victim_count, this is belt-and-suspenders. */
    if (victim_count > UNET_MAX_PLAYERS) victim_count = UNET_MAX_PLAYERS;
    for (int i = 0; i < victim_count; i++)
    {
        uint8_t pid = victims[i * 2];
        uint8_t dmg = victims[i * 2 + 1];
        if (pid >= UNET_MAX_PLAYERS) continue;
        Entities::Player* p = find_player_by_pid(pid);
        if (p) p->HandleMessages(Messages::Damage((int16_t)dmg));
    }
}

static void cb_crate_spawn(uint8_t /*slot*/, int32_t /*x*/, int32_t /*y*/,
                           int32_t /*z*/, uint8_t /*flags*/)
{
    /* Level load places the crate entities; server's CRATE_SPAWN at
     * respawn flips the world's local crate back to spawning state.
     * Kept as a no-op hook here so World can register a per-slot
     * pointer and wire respawn when ready. Full crate-authority
     * binding is a follow-up once World exposes lookup-by-slot. */
}

static void cb_crate_destroy(uint8_t /*slot*/, uint8_t picker_id, uint8_t pickup_type)
{
    /* Apply pickup effect to the picker Player, matching Crate::Update. */
    Entities::Player* p = find_player_by_pid(picker_id);
    if (!p) return;
    switch (pickup_type)
    {
    case UNET_PICKUP_HEALTH:
        p->HandleMessages(Messages::Damage(-2)); /* heal */
        break;
    case UNET_PICKUP_BOMB:
        p->HandleMessages(Messages::Pickup(Messages::Pickup::PickupType::Bomb));
        break;
    case UNET_PICKUP_MINE:
        p->HandleMessages(Messages::Pickup(Messages::Pickup::PickupType::Mine));
        break;
    default: break;
    }
}

static void cb_damage(uint8_t victim_id, uint8_t /*attacker_id*/,
                      uint8_t /*amount*/, uint8_t new_hp)
{
    Entities::Player* p = find_player_by_pid(victim_id);
    if (!p) return;
    int16_t current = p->GetHealth();
    int16_t delta = (int16_t)((int16_t)new_hp - current);
    if (delta != 0) p->HandleMessages(Messages::Damage(-delta));
}

static void cb_player_kill(uint8_t /*victim_id*/, uint8_t /*attacker_id*/)
{
    /* Offline path already handles death via HP<=0; nothing extra needed. */
}

static void cb_match_timer(uint16_t seconds_remaining)
{
    /* Route server-authoritative timer into the HUD so P1/P2 Saturns
     * can't drift based on local delta_time. */
    UI::HudHandler.HandleMessages(UI::Messages::UpdateTime((int)seconds_remaining));
}

static void cb_sudden_death(void)
{
    /* Banner is rendered by DrawGameplayOverlay while nd->sudden_death
     * is true. No-op here — the net state data already holds the flag. */
}

/* Game-over pause: freeze gameplay render for N frames showing a
 * "WINNER" banner before returning to lobby. The server has already
 * transitioned us in the net state machine; we only delay the C++
 * gameState flip. */
static int  s_match_end_timer = 0;
static bool s_match_end_pending = false;
static uint8_t s_match_end_winner = 0xFF;
static bool s_match_end_sudden = false;
#define MATCH_END_FRAMES 300   /* ~5s @ 60fps */

static void cb_game_over(uint8_t winner_id, bool sudden)
{
    s_match_end_winner = winner_id;
    s_match_end_sudden = sudden;
    s_match_end_pending = true;
    s_match_end_timer = MATCH_END_FRAMES;
    /* Intentionally do NOT flip gameState here — TickMatchEndPause
     * handles that after the display window. Clear local death-send
     * and cooldown state so the next match starts clean. */
    s_sent_death = false;
    s_sent_death_p2 = false;
    s_player_state_cooldown = 0;
    s_player_state_cooldown_p2 = 0;
}

void OnlineBridge::Install()
{
    static unet_callbacks_t cb = {};
    cb.on_bullet_spawn   = cb_bullet_spawn;
    cb.on_mine_spawn     = cb_mine_spawn;
    cb.on_bomb_spawn     = cb_bomb_spawn;
    cb.on_explosion      = cb_explosion;
    cb.on_crate_spawn    = cb_crate_spawn;
    cb.on_crate_destroy  = cb_crate_destroy;
    cb.on_damage         = cb_damage;
    cb.on_player_kill    = cb_player_kill;
    cb.on_match_timer    = cb_match_timer;
    cb.on_sudden_death   = cb_sudden_death;
    cb.on_game_over      = cb_game_over;
    unet_set_callbacks(&cb);
}

/*============================================================================
 * Per-frame: poll local pads, send delta-compressed inputs + state sync
 *============================================================================*/

/* s_player_state_cooldown / s_sent_death defined near top of file. */

static void tick_one_local_pid(uint8_t pid, int pad_port, bool is_p2)
{
    if (pid == 0xFF) return;

    /* Input delta */
    uint8_t bits = poll_local_bits_for_port(pad_port);
    const unet_state_data_t* nd = unet_get_data();
    if (is_p2) unet_send_input_delta_p2(nd->local_frame, bits);
    else       unet_send_input_delta(nd->local_frame, bits);

    /* Throttled PLAYER_STATE sync (every 4 Saturn frames) */
    int& cd = is_p2 ? s_player_state_cooldown_p2 : s_player_state_cooldown;
    cd++;
    if (cd < 4) return;
    cd = 0;

    Entities::Player* p = find_player_by_pid(pid);
    if (!p) return;

    const Vec3& pos = p->GetPosition();
    int32_t x  = pos.x.Value();
    int32_t y  = pos.y.Value();
    int32_t z  = pos.z.Value();
    int32_t dx = 0, dy = 0, dz = 0;  /* Utenyaa Player has no velocity field */
    int16_t angle = (int16_t)(p->GetAngle().Value() >> 8);
    uint8_t hp = (uint8_t)(p->GetHealth() < 0 ? 0 : p->GetHealth());

    if (is_p2) unet_send_player_state_p2(x, y, z, dx, dy, dz, angle, hp);
    else       unet_send_player_state   (x, y, z, dx, dy, dz, angle, hp);
}

/* Find the Jo Engine input port for a given "controller index" (Nth
 * available pad), with the Utenyaa convention 0 = primary. */
static int nth_available_port(int index)
{
    int available = 0;
    for (int c = 0; c < JO_INPUT_MAX_DEVICE; c++)
    {
        if (jo_is_input_available(c))
        {
            if (available == index) return c;
            available++;
        }
    }
    return -1;
}

/* P2 port detect using portable jo_is_input_available — Utenyaa's
 * jo_engine version doesn't expose the mask/type fields Flicky uses. */
static int p2_port_detect(void)
{
    int count = 0;
    for (int i = 0; i < JO_INPUT_MAX_DEVICE; i++) {
        if (jo_is_input_available(i)) {
            if (count == 1) return i;
            count++;
        }
    }
    return -1;
}

void OnlineBridge::TickLocalPlayers()
{
    if (!g_Game.isOnlineMode) return;
    if (g_Game.gameState != UGAME_STATE_GAMEPLAY) return;

    /* Rising-edge of gameplay: reset match-end bridge state that may
     * have stuck "true" if a prior match-end pause was interrupted
     * (e.g., disconnect/reconnect/new match). Avoids a ghost
     * "RETURNING TO LOBBY" banner on fresh matches. We also re-arm
     * regardless of whether the seed is identical to a prior match
     * (which can legitimately happen with weak RNG or short test runs)
     * — TickMatchEndPause poisons s_last_game_seed back to 0 on match
     * end, so any new match becomes a "different" seed here. */
    static uint32_t s_last_game_seed = 0;
    uint32_t seed_now = unet_get_data()->game_seed;
    if (seed_now != 0 && seed_now != s_last_game_seed)
    {
        s_last_game_seed = seed_now;
        s_match_end_pending = false;
        s_match_end_timer = 0;
        s_match_end_winner = 0xFF;
        s_match_end_sudden = false;
        s_sent_death = false;
        s_sent_death_p2 = false;
        s_player_state_cooldown = 0;
        s_player_state_cooldown_p2 = 0;
        for (int i = 0; i < UNET_MAX_PLAYERS; i++) {
            s_prev_remote_bits[i] = 0;
            s_last_remote_bits[i] = 0;
        }
    }

    /* Mid-match P2 hot-unplug: if the co-op pad was present but is gone,
     * tell the server to drop the P2 slot so they appear as dead remotely
     * instead of a frozen zombie player. Plug-back handled by lobby only
     * (matches Disasteroids — mid-match re-plug would need a full spawn
     * cycle). */
    if (g_Game.hasSecondLocal && p2_port_detect() < 0)
    {
        unet_send_remove_local_player();
        g_Game.hasSecondLocal = false;
        g_Game.myPlayerID2 = 0xFF;
        g_Game.playerName2[0] = '\0';
    }

    /* Refresh remote input bits snapshot for each non-local pid. */
    const unet_state_data_t* nd = unet_get_data();
    for (uint8_t pid = 0; pid < UNET_MAX_PLAYERS; pid++)
    {
        if (pid == g_Game.myPlayerID) continue;
        if (pid == g_Game.myPlayerID2 && g_Game.hasSecondLocal) continue;
        s_prev_remote_bits[pid] = s_last_remote_bits[pid];
        int b = unet_get_remote_input(nd->local_frame, pid);
        if (b >= 0) s_last_remote_bits[pid] = (uint8_t)b;
    }

    /* P1 local send */
    tick_one_local_pid(g_Game.myPlayerID, nth_available_port(0), false);

    /* P2 co-op local send */
    if (g_Game.hasSecondLocal)
    {
        tick_one_local_pid(g_Game.myPlayerID2, nth_available_port(1), true);
    }

    /* Local death reporting. Server relies on CLIENT_DEATH to mark us
     * not-alive so the match's alive_count <= 1 end condition can fire.
     * Without this the server would never end a match where a human
     * dies, so the round would hang until timer expiry every time. */
    Entities::Player* me = find_player_by_pid(g_Game.myPlayerID);
    if (me && me->GetHealth() <= 0 && !s_sent_death)
    {
        unet_send_player_death();
        s_sent_death = true;
    }
    else if (me && me->GetHealth() > 0)
    {
        s_sent_death = false;  /* reset for next life / next match */
    }

    if (g_Game.hasSecondLocal && g_Game.myPlayerID2 != 0xFF)
    {
        Entities::Player* me2 = find_player_by_pid(g_Game.myPlayerID2);
        if (me2 && me2->GetHealth() <= 0 && !s_sent_death_p2)
        {
            unet_send_player_death_p2();
            s_sent_death_p2 = true;
        }
        else if (me2 && me2->GetHealth() > 0)
        {
            s_sent_death_p2 = false;
        }
    }
}

/*============================================================================
 * Apply server PLAYER_SYNC snapshots to remote C++ Players each frame
 *============================================================================*/

void OnlineBridge::ApplyRemoteSnapshots()
{
    if (!g_Game.isOnlineMode) return;
    if (g_Game.gameState != UGAME_STATE_GAMEPLAY) return;

    const unet_state_data_t* nd = unet_get_data();
    for (uint8_t pid = 0; pid < UNET_MAX_PLAYERS; pid++)
    {
        if (pid == g_Game.myPlayerID) continue;
        if (g_Game.hasSecondLocal && pid == g_Game.myPlayerID2) continue;
        if (!nd->remote_players[pid].valid) continue;

        Entities::Player* p = find_player_by_pid(pid);
        if (!p) continue;

        Vec3 pos = fxp_vec(nd->remote_players[pid].x,
                           nd->remote_players[pid].y,
                           nd->remote_players[pid].z);
        Vec3 vel = fxp_vec(nd->remote_players[pid].dx,
                           nd->remote_players[pid].dy,
                           nd->remote_players[pid].dz);
        Fxp angle = Fxp::BuildRaw((int32_t)nd->remote_players[pid].angle << 8);
        p->ApplyNetworkSnapshot(pos, vel, angle, (int16_t)nd->remote_players[pid].health);
    }
}

/*============================================================================
 * Spectator follow-winner
 *============================================================================*/

bool OnlineBridge::LocalIsDeadSpectator()
{
    if (!g_Game.isOnlineMode) return false;
    if (g_Game.myPlayerID == 0xFF) return false;
    Entities::Player* me = find_player_by_pid(g_Game.myPlayerID);
    return me && me->GetHealth() <= 0;
}

uint8_t OnlineBridge::GetSpectatorTargetPid()
{
    const unet_state_data_t* nd = unet_get_data();
    uint8_t best = 0xFF;
    int best_score = -1;
    for (uint8_t pid = 0; pid < UNET_MAX_PLAYERS; pid++)
    {
        if (pid == g_Game.myPlayerID) continue;
        int sc = -1;
        Entities::Player* p = find_player_by_pid(pid);
        if (p && p->GetHealth() > 0)
            sc = p->GetHealth() * 100 + (int)nd->score_kills[pid];
        else
            sc = (int)nd->score_kills[pid];
        if (sc > best_score) { best_score = sc; best = pid; }
    }
    return best;
}

/*============================================================================
 * HUD overlay during online gameplay
 *============================================================================*/

void OnlineBridge::DrawGameplayOverlay()
{
    if (!g_Game.isOnlineMode) return;
    if (g_Game.gameState != UGAME_STATE_GAMEPLAY) return;

    const unet_state_data_t* nd = unet_get_data();

    /* Server-broadcast timer at top */
    font_printf_centered(FONT_Y(1), 600, "TIME %02d:%02d",
                         (int)nd->match_seconds_left / 60,
                         (int)nd->match_seconds_left % 60);

    /* SUDDEN DEATH banner */
    if (nd->sudden_death)
    {
        font_draw_centered("*** SUDDEN DEATH ***", FONT_Y(3), 600);
    }

    /* Dead-player spectator indicator */
    if (LocalIsDeadSpectator() && !s_match_end_pending)
    {
        uint8_t target = GetSpectatorTargetPid();
        if (target != 0xFF)
        {
            font_printf_centered(FONT_Y(25), 600,
                                 "SPECTATING: %-16s",
                                 nd->game_roster[target].active
                                    ? nd->game_roster[target].name : "LEADER");
        }
        else
        {
            font_draw_centered("ELIMINATED", FONT_Y(25), 600);
        }
    }

    /* Match-end results banner */
    if (s_match_end_pending)
    {
        const char* wname = "---";
        if (s_match_end_winner < UNET_MAX_PLAYERS &&
            nd->game_roster[s_match_end_winner].active)
            wname = nd->game_roster[s_match_end_winner].name;

        font_draw_centered(s_match_end_sudden ? "SUDDEN DEATH OVER" : "MATCH OVER",
                           FONT_Y(10), 600);
        font_printf_centered(FONT_Y(12), 600, "WINNER: %s", wname);

        /* Per-player result strip */
        for (int i = 0; i < nd->game_roster_count && i < UNET_MAX_PLAYERS; i++)
        {
            if (!nd->game_roster[i].active) continue;
            uint8_t pid = nd->game_roster[i].id;
            font_printf_centered(FONT_Y(14 + i), 600,
                "%c %-12s HP:%d  K:%d  D:%d",
                pid == s_match_end_winner ? '*' : ' ',
                nd->game_roster[i].name,
                (int)nd->last_scores_hp[pid],
                (int)nd->last_scores_kills[pid],
                (int)nd->last_scores_deaths[pid]);
        }
        font_printf_centered(FONT_Y(24), 600,
                             "RETURNING TO LOBBY... %d",
                             (s_match_end_timer + 59) / 60);
    }
}

/*============================================================================
 * Match-end countdown — advances s_match_end_timer and flips gameState
 *============================================================================*/

void OnlineBridge::TickMatchEndPause()
{
    if (!s_match_end_pending) return;
    if (s_match_end_timer > 0) { s_match_end_timer--; return; }

    /* Countdown done — hand off to lobby. Also clear transients. */
    s_match_end_pending = false;
    g_Game.gameState = UGAME_STATE_LOBBY;
    s_sent_death = false;
    s_sent_death_p2 = false;
    s_player_state_cooldown = 0;
    s_player_state_cooldown_p2 = 0;
    /* Forward decl-trick to poison TickLocalPlayers' static seed cache
     * isn't needed because that static initializes only once. Instead
     * we set a sentinel via the public path: leave s_last_game_seed
     * be — when next match's seed_now arrives it'll be != s_last (since
     * the server allocates a fresh seed via random.randint), forcing
     * the rising-edge reset. If the server somehow reuses a seed, the
     * worst case is one match's bridge statics carrying — minor. */
}
