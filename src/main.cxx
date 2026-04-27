
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
#include "utenyaa_online_bridge.hpp"
#include "Entities/Player.hpp"

/* === C-linkage shims so the C lobby/screen code can read C++ engine
 *     state (sprite slots, character counts) without including C++. === */
extern "C" int unet_glue_num_characters(void)
{
	/* CHARS.PAK ships 25 textures × 5 frames per character = 5 chars.
	 * Hardcoded constant matches Player::FramesPerController. */
	return 5;
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

		// Online screens (name entry / connecting / lobby) short-circuit
		// the offline menu and world. Keep the network state machine
		// ticking every frame regardless.
		unet_glue_tick_frame();

		if (unet_glue_is_online_screen_active())
		{
			// The offline title/menu path leaves the NBG1 logo layer
			// visible; hide it so online screen text isn't shown on
			// top of the title graphic.
			Helpers::HideLogo();
			slSynch();
			continue;
		}

		static UI::Menu menu;
		menu.Update();

		if (Settings::Quit && worldPtr)
		{
			PoneSound::CD::Play(2, 2, true);
			for (auto* object : IRenderable::objects) delete object;
			worldPtr = nullptr;
			Settings::Quit = false;
			Settings::GameEnded = false;
		}

		// Online match ended — TickMatchEndPause flipped gameState to LOBBY.
		// Tear down the active world so main re-enters the online screen
		// dispatch path (lobby_draw) on the next iteration.
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
		}

		// Online lobby transitioned us into gameplay — sync Settings so
		// the existing offline code path spins up World correctly, and
		// seed RNG from the server's match seed for deterministic crate
		// pickup rolls / cosmetic random state across peers.
		if (g_Game.isOnlineMode && g_Game.gameState == UGAME_STATE_GAMEPLAY
			&& !Settings::IsActive)
		{
			const unet_state_data_t* nd = unet_get_data();
			Settings::SelectedStage = nd->stage_id;
			Settings::TotalSeconds = nd->match_seconds_total;
			Settings::PlayerCount = nd->opponent_count;
			jo_random_seed = nd->game_seed ? nd->game_seed : 1;
			Settings::IsActive = true;
		}

		if (Settings::IsActive)
		{

			if (worldPtr == nullptr)
			{
				Settings::GameEnded = false;
				startTime = Fxp::FromInt(Settings::TotalSeconds);
				worldPtr = new Entities::World(Settings::StageFiles[Settings::SelectedStage]);
				PoneSound::CD::Play(3, 3, true);
			}
			slUnitMatrix(0);

			// Send local pad input + player state to server during online play
			OnlineBridge::TickLocalPlayers();

			// Update entities
			for (auto* object : IUpdatable::objects) object->Update();

			// Apply server PLAYER_SYNC snapshots to remote players AFTER their
			// Update() so the overwrite is authoritative this frame (lerp+extrap).
			OnlineBridge::ApplyRemoteSnapshots();

			// Match-end countdown. Also: follow-winner spectator aim.
			OnlineBridge::TickMatchEndPause();

			if (g_Game.isOnlineMode && OnlineBridge::LocalIsDeadSpectator())
			{
				uint8_t tgt = OnlineBridge::GetSpectatorTargetPid();
				if (tgt != 0xFF)
				{
					for (auto* obj : TrackableObject<Entities::Player>::objects)
					{
						if ((uint8_t)obj->GetController() == tgt)
						{
							const Vec3& tp = obj->GetPosition();
							jo_3d_camera_set_target(&camera,
								tp.x.Value() >> 16, tp.y.Value() >> 16, 0);
							break;
						}
					}
				}
			}

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
			else
			{
				UI::HudHandler.HandleMessages(UI::Messages::UpdateTime((startTime >> 16).Value()));
				startTime -= Fxp::BuildRaw(delta_time);
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
