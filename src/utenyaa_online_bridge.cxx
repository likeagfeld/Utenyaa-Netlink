/**
 * utenyaa_online_bridge.cxx - C++ ↔ net layer bridge implementation
 */

#include "utenyaa_online_bridge.hpp"
#include "Entities/Player.hpp"
#include "Entities/Bullet.hpp"
#include "Entities/Mine.hpp"
#include "Entities/Bomb.hpp"
#include "Entities/Explosion.hpp"
#include "Entities/Crate.hpp"
#include "Utils/Math/Vec3.hpp"
#include "Utils/TrackableObject.hpp"
#include "Utils/Message.hpp"
#include "Messages/Damage.hpp"
#include "Messages/Pickup.hpp"

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
    uint8_t bit = key_to_bit(key);
    if (!bit) return false;
    return (s_last_remote_bits[controller] & bit) != 0;
}

bool OnlineBridge::RemoteKeyPressed(int controller, jo_gamepad_keys key)
{
    uint8_t bit = key_to_bit(key);
    if (!bit) return false;
    return (s_last_remote_bits[controller] & bit) != 0;
}

/*============================================================================
 * Server → client event callbacks
 *============================================================================*/

static Entities::Player* find_player_by_pid(uint8_t pid)
{
    for (auto* p : TrackableObject<Entities::Player>::objects)
    {
        if (p && p->controller == pid) return p;
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
    Vec3 dir = fxp_vec(dx, dy, dz);
    Vec3 pos = fxp_vec(x, y, z);
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
    Vec3 dir = fxp_vec(dx, dy, dz);
    Vec3 pos = fxp_vec(x, y, z);
    new Entities::Bomb(dir, pos);
}

static void cb_explosion(int32_t x, int32_t y, int32_t z, uint16_t /*radius*/,
                         const uint8_t* victims, int victim_count)
{
    Vec3 pos = fxp_vec(x, y, z);
    new Entities::Explosion(pos, 1.0);

    /* Apply server-resolved damage to named victims. */
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

static void cb_match_timer(uint16_t /*seconds_remaining*/) {}
static void cb_sudden_death(void) {}
static void cb_game_over(uint8_t /*winner_id*/, bool /*sudden*/)
{
    /* Flip gameState back to lobby; the server already demoted us. */
    g_Game.gameState = UGAME_STATE_LOBBY;
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

static int s_player_state_cooldown = 0;
static int s_player_state_cooldown_p2 = 0;

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

    int32_t x  = p->position.x.Value();
    int32_t y  = p->position.y.Value();
    int32_t z  = p->position.z.Value();
    int32_t dx = 0, dy = 0, dz = 0;  /* Utenyaa Player has no vel field */
    int16_t angle = (int16_t)(p->angle.Value() >> 8);
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

void OnlineBridge::TickLocalPlayers()
{
    if (!g_Game.isOnlineMode) return;
    if (g_Game.gameState != UGAME_STATE_GAMEPLAY) return;

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
}
