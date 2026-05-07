
#include <jo/Jo.hpp>
#include "Utils/ponesound/ponesound.hpp"

#include "Utils/Math/Vec3.hpp"
#include "Utils/TrackableObject.hpp"

#include "Utils/Message.hpp"
#include "Entities/World.hpp"
#include "Utils/Menu.hpp"
#include "Utils/Helpers.hpp"

#include "Utils/Debug.hpp"

#include "net/utenyaa_main_glue.h"
#include "net/utenyaa_net.h"
#include "net/utenyaa_map_stream.h"
#include "net/utenyaa_protocol.h"   /* UNET_STAGE_STREAMED */
#include "map_pick.h"
#include "cc_download.h"
#include "lobby.h"                  /* lobby_reset_edge_state for post-match return */
#include "utenyaa_online_bridge.hpp"
#include "Entities/Player.hpp"
#include "Entities/Bullet.hpp"
#include "Entities/Mine.hpp"
#include "Entities/Bomb.hpp"
#include "Entities/Explosion.hpp"
#include <stdio.h>                 /* snprintf for VDP_PEAK diagnostic */

/* === C-linkage shims so the C lobby/screen code can read C++ engine
 *     state (sprite slots, character counts) without including C++. === */
extern "C" int unet_glue_num_characters(void)
{
	/* CHARS.PAK ships 25 textures × 5 frames per character = 5 chars
	 * built-in. Phase D extends the selectable range to also include
	 * any custom characters the operator has downloaded this session
	 * via the Title-screen "Download Characters" flow. The lobby
	 * picker (next_available_character) auto-extends; Player::Draw
	 * clamps to the built-in range with a fallback render for
	 * character_id ≥ 5 since custom-sprite VDP1 slot loading isn't
	 * shipped yet. */
	return 5 + cc_download_count();
}

extern "C" int unet_glue_character_sprite_for(uint8_t character_id)
{
	const int frames_per_char = 5;
	int n = unet_glue_num_characters();
	int idx = (character_id < (uint8_t)n) ? (int)character_id : 0;
	return Entities::Player::GetCharacterSpriteStart() + idx * frames_per_char;
}

jo_camera camera;
int logo;

static jo_palette titleScreen;

/* Palette creation handler
 * @return Pointer to created palette
 */
jo_palette* TgaPaletteHandling(void)
{
	jo_create_palette(&titleScreen);
	return (&titleScreen);
}

// JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC is bumped in the makefile to give
// jo_core_init + LOGO.TGA (123 KB) + sound buffers + PAK textures +
// backup-RAM I/O enough contiguous pool headroom.
//
// Earlier attempts used jo_add_memory_zone(LWRAM, 256KB) — a pattern
// that works for Flicky/Disasteroids but NOT for Utenyaa because SGL
// (used extensively here, not in the other two forks) reserves LWRAM
// for its DMA/matrix workspace. jo_add_memory_zone memsets the region
// to zero as part of registration, which corrupts SGL state and
// black-screens the boot after the SEGA logo. Don't touch LWRAM.

int main()
{
	jo_core_init(JO_COLOR_Black);
	slDynamicFrame(1);

	// Hide NBG1 (the title-logo plane) until LOGO.TGA has been
	// decoded into VRAM. Without this, the operator sees ~3 seconds
	// of uninitialized VRAM as garbled pixelated noise on the title
	// graphic while the boot sequence loads sounds + PAKs from CD —
	// jo_core_init enables NBG1ON by default but NBG1 VRAM still
	// holds the garbage left by the BIOS at this point. ShowLogo
	// is called again right after jo_set_background_8bits_sprite
	// finishes the decode so the visible flicker window collapses
	// to one frame instead of three seconds.
	Helpers::HideLogo();
	Objects::Terrain::InitColliders();
	IMessageHandler::Init();

	// Random seed
	jo_random_seed = jo_time_get_frc();

	// This magic number fixes the 8bits images on VDP2 not working
	*(volatile unsigned char*)0x060FFCD8 = 0x1F;

	// This will set the screen order
	jo_core_set_screens_order(JO_NBG1_SCREEN, JO_SPRITE_SCREEN);

	PoneSound::Driver::Initialize(PoneSound::ADXMode::ADX2304);

	jo_3d_camera_init(&camera);
	jo_3d_camera_set_z_angle(&camera, 0);
	jo_3d_camera_set_viewpoint(&camera, 0, 20, 120);
	jo_3d_camera_set_target(&camera, 0, 30, 0);

	// Load title screen, we can just leave this on NBG2 and turn that layer off when we do not want to see it
	jo_img_8bits bg;
	bg.data = NULL;
	jo_set_tga_palette_handling(TgaPaletteHandling);
	jo_tga_8bits_loader(&bg, JO_ROOT_DIR, "LOGO.TGA", 1); // Thx to jo's wisdom the colors in the image palette start with 1
	jo_set_tga_palette_handling(nullptr);
	jo_set_background_8bits_sprite(&bg, titleScreen.id, false);
	jo_free_img(&bg);

	// LOGO.TGA is now in VRAM; safe to re-enable NBG1.
	Helpers::ShowLogo();

	// Load sounds
	PoneSound::Sound::LoadPcm((char*)"BOMB.PCM", PoneSound::PCMBitDepth::PCM8, 15360);
	PoneSound::Sound::LoadPcm((char*)"CANNON.PCM", PoneSound::PCMBitDepth::PCM8, 15360);

	PoneSound::Sound::LoadPcm((char*)"CRFALL.PCM", PoneSound::PCMBitDepth::PCM8, 15360);
	PoneSound::Sound::LoadPcm((char*)"CROPEN.PCM", PoneSound::PCMBitDepth::PCM8, 15360);
	PoneSound::Sound::LoadPcm((char*)"CRSPAWN.PCM", PoneSound::PCMBitDepth::PCM8, 15360);

	PoneSound::Sound::LoadPcm((char*)"MHOV.PCM", PoneSound::PCMBitDepth::PCM8, 15360);
	PoneSound::Sound::LoadPcm((char*)"MSEL.PCM", PoneSound::PCMBitDepth::PCM8, 15360);

	PakTextureLoader::LoadTextures("HUD.PAK");

	Objects::Terrain::FirstGroundTextureIndex = PakTextureLoader::LoadTextures("TERRAIN.PAK");
	Entities::Player::SetTextureId(PakTextureLoader::LoadTextures("CHARS.PAK"));
	Entities::Explosion::SetTextureId(PakTextureLoader::LoadTextures("EXP.PAK"));

	// Load additional textures
	int first = PakTextureLoader::LoadTextures("WEAP.PAK");
	Entities::Bullet::SetTextureId(first);
	Entities::Mine::SetTextureId(first + 1);

	// Load models
	ModelManager::LoadModel((char*)"CRATE.NYA");
	ModelManager::LoadModel((char*)"PLAYER.NYA");
	ModelManager::LoadModel((char*)"TREE.NYA");
	ModelManager::LoadModel((char*)"WALL.NYA");
	ModelManager::LoadModel((char*)"WALL2.NYA");
	ModelManager::LoadModel((char*)"WALL3.NYA");
	ModelManager::LoadModel((char*)"BOMB.NYA");

	PoneSound::CD::Play(2, 2, true);

	// Online layer init (modem detect, net state machine, font, globals)
	unet_glue_init();
	OnlineBridge::Install();

	Entities::World* worldPtr = nullptr;
	Fxp startTime = 0.0;

	while (1)
	{
		jo_fixed_point_time();

		// Set up the SGL camera matrix BEFORE the network tick. The
		// online lobby's character-preview sprite draw uses
		// jo_sprite_draw3D which internally calls slDispSprite — that
		// requires a valid camera/look-at matrix from the current frame.
		// Without this call the lobby preview projects to undefined
		// coordinates and the sprite never appears (operator-reported:
		// "not seeing a sprite preview of character of select"). The
		// gameplay path further down also calls jo_3d_camera_look_at,
		// so this is just a one-frame-earlier setup that has no effect
		// on the gameplay matrix.
		jo_3d_camera_look_at(&camera);

		// Online screens (name entry / connecting / lobby) short-circuit
		// the offline menu and world. Keep the network state machine
		// ticking every frame regardless.
		unet_glue_tick_frame();

		// Online match ended — TickMatchEndPause flipped gameState to
		// LOBBY. THIS BLOCK MUST RUN BEFORE the `continue` below: once
		// gameState is LOBBY, unet_glue_is_online_screen_active()
		// returns true and the rest of the loop is skipped — any
		// post-match teardown / ready-state reset placed below the
		// continue NEVER FIRES. That was the actual root cause of the
		// "press A+START after 1st game removes ready status" bug:
		// stale Settings::IsActive=true + stale g_net.my_ready=true
		// from the previous match.
		if (g_Game.isOnlineMode && g_Game.gameState == UGAME_STATE_LOBBY
			&& Settings::IsActive)
		{
			Settings::IsActive = false;
			Settings::GameEnded = false;
			if (worldPtr)
			{
				for (auto* object : IRenderable::objects) delete object;
				worldPtr = nullptr;
			}
			PoneSound::CD::Play(2, 2, true);
			unet_reset_ready_state();
			g_Game.input.pressedABC = false;
			g_Game.input.pressedStart = false;
			g_Game.input.pressedLT = false;
			g_Game.input.pressedRT = false;
			/* Reset lobby's per-screen edge-detect flags too — operator-
			 * reported: "L/R triggers do not cycle through character
			 * sprites" on post-match return. Without this, a held L
			 * carrying over from gameplay leaves the lobby's
			 * g_ltrig_pressed sticky and the inner CHARACTER_SELECT
			 * send never fires. */
			lobby_reset_edge_state();
			/* Pull fresh leaderboard so wins/kills/deaths from the
			 * just-ended match show up when the user holds Z. lobby_init
			 * only requests leaderboard ONCE on the first CONNECTING →
			 * LOBBY transition; subsequent match-end → LOBBY returns
			 * would otherwise show stale data (often all zeros). */
			unet_request_leaderboard();
			jo_clear_screen();
		}

		if (unet_glue_is_online_screen_active())
		{
			// The offline title/menu path leaves the NBG1 logo layer
			// visible; hide it so online screen text isn't shown on
			// top of the title graphic.
			Helpers::HideLogo();
			slSynch();
			continue;
		}

		// menu.Update has side effects beyond rendering (jo_clear_screen,
		// state transitions on the GameEnd/Pause screens) that gameplay
		// relies on for a clean NBG0. Skipping it entirely produces a
		// black screen during online gameplay. Instead, the START→pause
		// logic inside menu.Update is gated on !isOnlineMode (see
		// Menu.hpp:204) so the user's still-held START from the lobby
		// can no longer pause the freshly-started online match.
		// Skip menu.Update entirely during online gameplay — its
		// jo_clear_screen calls happen mid-display and wipe NBG0
		// cells while VDP2 is scanning them out, causing the visible
		// strobe on top-of-screen text reported by the user. World
		// ctor calls jo_clear_screen once at gameplay-start so any
		// residue from lobby is gone; subsequent frames just need
		// to write the fresh HUD cells, no clear required.
		static UI::Menu menu;
		if (!(g_Game.isOnlineMode && g_Game.gameState == UGAME_STATE_GAMEPLAY))
		{
			menu.Update();
		}

		if (Settings::Quit && worldPtr)
		{
			PoneSound::CD::Play(2, 2, true);
			for (auto* object : IRenderable::objects) delete object;
			worldPtr = nullptr;
			Settings::Quit = false;
			Settings::GameEnded = false;
		}

		// (match-end teardown moved above the continue — see top of loop)

		// Online lobby transitioned us into gameplay — sync Settings so
		// the existing offline code path spins up World correctly, and
		// seed RNG from the server's match seed for deterministic crate
		// pickup rolls / cosmetic random state across peers.
		if (g_Game.isOnlineMode && g_Game.gameState == UGAME_STATE_GAMEPLAY
			&& !Settings::IsActive)
		{
			const unet_state_data_t* nd = unet_get_data();
			/* DIAG: server-bound checkpoint trace. Server journal logs
			 * each CLIENT_DBG receive with the sender's username so we
			 * can see exactly how far a black-screening client got. */
			unet_send_dbg_log("CKPT-A: gameplay-state-entered");
			Settings::SelectedStage = nd->stage_id;
			Settings::TotalSeconds = nd->match_seconds_total;
			Settings::PlayerCount = nd->opponent_count;
			{
				char buf[40]; int n = 0;
				const char* hdr = "CKPT-B stage=";
				while (hdr[n]) { buf[n] = hdr[n]; n++; }
				buf[n++] = '0' + ((int)nd->stage_id % 10);
				buf[n++] = ' '; buf[n++] = 'P'; buf[n++] = 'C'; buf[n++] = '=';
				if (nd->opponent_count >= 10) buf[n++] = '0' + ((nd->opponent_count / 10) % 10);
				buf[n++] = '0' + (nd->opponent_count % 10);
				buf[n++] = ' '; buf[n++] = 'P'; buf[n++] = '1'; buf[n++] = '=';
				buf[n++] = '0' + ((g_Game.myPlayerID < 10) ? g_Game.myPlayerID : 9);
				buf[n] = 0;
				unet_send_dbg_log(buf);
			}
			jo_random_seed = nd->game_seed ? nd->game_seed : 1;
			Settings::IsActive = true;
		}

		if (Settings::IsActive)
		{

			if (worldPtr == nullptr)
			{
				unet_send_dbg_log("CKPT-C pre-World-ctor");
				Settings::GameEnded = false;
				startTime = Fxp::FromInt(Settings::TotalSeconds);
				/* If the server flagged this match as STREAMED (stage 0xFE)
				 * AND a complete validated map buffer was received before
				 * GAME_START, load the World from the streamed buffer.
				 * Otherwise fall through to the CD path (the four baked
				 * stages indexed 0..3). The streamed-buffer take transfers
				 * ownership; Map ctor jo_free()s after parse. */
				if ((uint8_t)Settings::SelectedStage == UNET_STAGE_STREAMED &&
				    unet_map_stream_is_ready())
				{
					int sz = 0;
					uint8_t* buf = unet_map_stream_take_buffer(&sz);
					if (buf)
					{
						unet_send_dbg_log("CKPT-C-S using streamed map");
						worldPtr = new Entities::World(buf, sz);
					}
					else
					{
						/* Should not happen: is_ready true but take returned
						 * NULL. Fall back to stage 0 so we don't black-screen. */
						unet_send_dbg_log("CKPT-CX streamed take NULL fallback ISLAND");
						worldPtr = new Entities::World(Settings::StageFiles[0]);
					}
				}
				else
				{
					if ((uint8_t)Settings::SelectedStage == UNET_STAGE_STREAMED)
					{
						/* Server promised streamed but RX state isn't ready —
						 * either CRC failed or chunks didn't all arrive. Fall
						 * back to ISLAND rather than dereferencing a null
						 * StageFiles[0xFE] index. The dbg-log makes the
						 * fallback visible in server journal so the operator
						 * knows to retry. */
						unet_send_dbg_log("CKPT-CX streamed not-ready fallback ISLAND");
						Settings::SelectedStage = 0;
					}
					worldPtr = new Entities::World(Settings::StageFiles[Settings::SelectedStage]);
				}
				unet_send_dbg_log("CKPT-D post-World-ctor");
				PoneSound::CD::Play(3, 3, true);
				unet_send_dbg_log("CKPT-E post-CD-Play (gameplay live)");
			}
			slUnitMatrix(0);

			// Send local pad input + player state to server during online play
			OnlineBridge::TickLocalPlayers();

			// Update entities
			for (auto* object : IUpdatable::objects) object->Update();

			/* VDP1 redline-artifact diagnostic. Operator reports random
			 * red lines / garbage flashing during play. Hardware suspects:
			 *   (a) VDP1 cmd-table pressure when many simultaneous
			 *       polygon-bearing entities overlap on screen
			 *       (BigExplosion = 5 child Explosions × N players +
			 *       bullets + mines + bombs + map quads + HUD)
			 *   (b) Heap fragmentation from new/delete during render
			 *       corrupting the cmd table buffer
			 *   (c) Polygon vertex coords clipping near INT16 boundaries
			 *
			 * Per-frame counter tracks the peak concurrent IRenderable
			 * count and per-type peaks for the high-pressure entities.
			 * Logs ONLY when a new high-water mark is hit (one line per
			 * peak — quiet otherwise). When the operator next reports
			 * a red-line event, the journal will show the entity
			 * watermarks at the time, conclusively pointing at the
			 * cause (or eliminating cmd-table pressure if peaks are
			 * low when artifacts appear).
			 *
			 * Resets at match start via the worldPtr==nullptr block
			 * above (which also resets Settings::IsActive). */
			{
				static int s_peak_total = 0;
				static int s_peak_update = 0;
				static int s_peak_expl = 0;
				static int s_peak_big = 0;
				static int s_peak_bul = 0;
				static int s_peak_mine = 0;
				static int s_peak_bomb = 0;
				int n_total = (int)IRenderable::objects.size();
				int n_update = (int)IUpdatable::objects.size();
				int n_expl = (int)TrackableObject<Entities::Explosion>::objects.size();
				int n_big = (int)TrackableObject<Entities::BigExplosion>::objects.size();
				int n_bul = (int)TrackableObject<Entities::Bullet>::objects.size();
				int n_mine = (int)TrackableObject<Entities::Mine>::objects.size();
				int n_bomb = (int)TrackableObject<Entities::Bomb>::objects.size();
				bool new_peak = false;
				if (n_total > s_peak_total) { s_peak_total = n_total; new_peak = true; }
				if (n_update > s_peak_update) { s_peak_update = n_update; new_peak = true; }
				if (n_expl  > s_peak_expl)   { s_peak_expl  = n_expl;  new_peak = true; }
				if (n_big   > s_peak_big)    { s_peak_big   = n_big;   new_peak = true; }
				if (n_bul   > s_peak_bul)    { s_peak_bul   = n_bul;   new_peak = true; }
				if (n_mine  > s_peak_mine)   { s_peak_mine  = n_mine;  new_peak = true; }
				if (n_bomb  > s_peak_bomb)   { s_peak_bomb  = n_bomb;  new_peak = true; }
				if (new_peak) {
					char buf[96];
					snprintf(buf, sizeof(buf),
					         "VDP_PEAK r=%d u=%d ex=%d bg=%d bul=%d mn=%d bm=%d",
					         n_total, n_update, n_expl, n_big,
					         n_bul, n_mine, n_bomb);
					unet_send_dbg_log(buf);
				}
			}

			// Apply server PLAYER_SYNC snapshots to remote players AFTER their
			// Update() so the overwrite is authoritative this frame (lerp+extrap).
			OnlineBridge::ApplyRemoteSnapshots();

			// Match-end countdown. Also: follow-winner spectator aim.
			OnlineBridge::TickMatchEndPause();

			/* Spectator-follow camera removed per user directive: when
			 * a player dies, leave the camera at the default whole-
			 * map view instead of tracking the leader. The default
			 * jo_3d_camera_set_target(0, 30, 0) (set once in main()
			 * init) shows the whole arena, which the user wants for
			 * watching the rest of the round play out from a fixed
			 * angle. */

			jo_3d_camera_look_at(&camera);
			jo_3d_push_matrix();
			{
				jo_3d_rotate_matrix_rad_x(0.5f);
				jo_3d_translate_matrix_fixed(-10 << 19, -10 << 19, 0);

				// Draw entities onto the world
				for (auto* object : IRenderable::objects) object->Draw();
			}
			jo_3d_pop_matrix();

			Helpers::HideLogo();

			// Online HUD overlay (match timer, sudden death, spectator, results)
			OnlineBridge::DrawGameplayOverlay();

			int alive = 0;

			for (auto* object : TrackableObject<Entities::Player>::objects)
			{
				if (object->GetHealth() > 0)
				{
					alive++;
				}
			}

			// In online mode the server decides match-over, not local
			// HP/timer — skip the offline end-of-match branch entirely.
			if (!g_Game.isOnlineMode && (startTime <= 0.0 || alive <= 1))
			{
				// Show match results
				Settings::IsActive = false;
				Settings::GameEnded = true;

				int winner = 0;
				int maxHealth = 0;

				for (auto* object : TrackableObject<Entities::Player>::objects)
				{
					int health = object->GetHealth();

					if (health > maxHealth)
					{
						maxHealth = health;
						winner = object->GetController();
					}
				}

				Settings::PlayerWon = winner + 1;
				PoneSound::CD::Play(4, 4, false);
			}
			else if (g_Game.isOnlineMode)
			{
				/* Online: HUD timer is server-authoritative — pull
				 * nd->match_seconds_left (uint16, server clamps to 0).
				 * Local startTime is NOT decremented (avoids the
				 * size_t-cast underflow that displayed "560 minutes"
				 * after expiry). DrawGameplayOverlay also renders a
				 * "TIME mm:ss" line at row 1 from the same server
				 * source — kept consistent. */
				const unet_state_data_t* nd = unet_get_data();
				UI::HudHandler.HandleMessages(
					UI::Messages::UpdateTime((size_t)nd->match_seconds_left));
			}
			else
			{
				/* Offline path — local countdown clamped to 0 to avoid
				 * size_t-cast underflow if a frame slips past zero. */
				int t = (startTime > Fxp(0.0))
					? (int)((startTime >> 16).Value())
					: 0;
				UI::HudHandler.HandleMessages(
					UI::Messages::UpdateTime((size_t)t));
				startTime -= Fxp::BuildRaw(delta_time);
				if (startTime < Fxp(0.0)) startTime = Fxp(0.0);
			}
		}
		else
		{
			Helpers::ShowLogo();
		}


		slSynch();
	}
	return 0;
}
