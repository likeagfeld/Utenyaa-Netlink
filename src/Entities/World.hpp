#pragma once

// Engine
#include <jo/Jo.hpp>

// Object definitions
#include "../Objects/Map.hpp"
#include "../Objects/Model.hpp"

// Managers
#include "../Utils/ModelManager.hpp"

// Interfaces
#include "../Interfaces/IRenderable.hpp"
#include "../Objects/Terrain.hpp"

// Spawnable entities
#include "StaticModel.hpp"
#include "Player.hpp"
#include "Crate.hpp"
#include "Explosion.hpp"

#include "../Utils/Debug.hpp"
#include "../Utils/PakTextureLoader.hpp"
#include "../Utils/ponesound/ponesound.hpp"   /* CD::Stop before Map load */

#include "../net/utenyaa_net.h"   /* unet_send_dbg_log for World-ctor checkpoints */


namespace Entities
{
	/** @brief Static 3D detail
	 */
	struct World : public IRenderable, TrackableObject<Entities::World>
	{
	private:
		/** @brief Index of first ground texture
		 */
		int groundTextures = 0;

	public:
		/** @brief Map definition
		 */
		Objects::Map* Map;

		/** @brief Initializes a new instance of the World and populates it with entities
		 * @param name Name of the map file on the CD
		 */
		World(const char* name)
		{
			unet_send_dbg_log("CKPT-W1 World pre-Map");
			/* Stop CD audio (track 2 lobby music is still playing) before
			 * the Map's CD data read. The Saturn CD drive can only do one
			 * thing at a time — competing audio streaming + data seeks on
			 * a marginal lens stalls the data read indefinitely on some
			 * hardware (TOAST playtest: hangs inside Map ctor with no
			 * CKPT-W2 ever emitted, no W2X null-bail either → the
			 * jo_fs_read_file_in_dir call simply doesn't return). Stopping
			 * audio first frees the lens. The gameplay track is started
			 * after World ctor returns. */
			PoneSound::CD::Stop();
			// Load map file
			this->Map = new Objects::Map(name, Objects::Terrain::FirstGroundTextureIndex);
			unet_send_dbg_log("CKPT-W2 World post-Map");
			if (!this->Map || this->Map->EntityDefinitionsCount == 0)
			{
				/* Map allocation failed OR file read returned empty/garbage.
				 * Don't proceed into the entity loop — that would dereference
				 * Map->EntityDefinitions[i] and crash. The black screen is
				 * already happening; this just prevents an undebuggable hang
				 * and surfaces a clear sentinel in the server log. */
				unet_send_dbg_log("CKPT-W2X Map alloc/load FAILED");
				jo_clear_screen();
				return;
			}
			Objects::Terrain::Map = this->Map;
			Objects::Terrain::ClearColliders();
			unet_send_dbg_log("CKPT-W3 Terrain set");

			// Player contorller
			uint8_t controller = 0;

			// Log entity count so we can see if the map data is sane.
			{
				char buf[32]; int n = 0;
				const char* hdr = "CKPT-W4 ents=";
				while (hdr[n]) { buf[n] = hdr[n]; n++; }
				int ec = (int)this->Map->EntityDefinitionsCount;
				if (ec > 99) ec = 99;
				if (ec >= 10) buf[n++] = '0' + (ec / 10);
				buf[n++] = '0' + (ec % 10);
				buf[n] = 0;
				unet_send_dbg_log(buf);
			}

			// Start spawning entities
			for (int i = 0; i < this->Map->EntityDefinitionsCount; i++)
			{
				Objects::Map::EntityCreationDefinition entity = this->Map->EntityDefinitions[i];

				switch (entity.Type)
				{
				case Objects::Map::EntityType::PlayerSpawn:

					// We can spawn 12 players at most
					if (controller < JO_INPUT_MAX_DEVICE && controller < Settings::PlayerCount)
					{
						new Entities::Player(entity.Location, entity.Angle, controller++);
					}

					break;

				case Objects::Map::EntityType::Model:
					new Entities::StaticDetail3D(entity.Location, entity.Angle, (unsigned short)entity.Reserved[1]);
					break;

				case Objects::Map::EntityType::Crate:
					new Entities::Crate(entity.Location, (unsigned char)entity.Reserved[0], (unsigned short)entity.Reserved[1]);
					break;

				default:
					break;
				}
			}
			unet_send_dbg_log("CKPT-W5 entities-spawned");

			jo_clear_screen();
		}

		/** @brief Destroy the World object
		 */
		~World()
		{
			delete this->Map;
		}

		/** @brief Render world
		 */
		void Draw()
		{
			this->Map->Draw();
		}
	};
}
