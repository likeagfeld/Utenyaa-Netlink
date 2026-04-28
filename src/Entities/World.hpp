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

extern "C" void unet_send_dbg_log(const char *text);


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
			unet_send_dbg_log("WORLD_enter");

			// Load map file
			this->Map = new Objects::Map(name, Objects::Terrain::FirstGroundTextureIndex);
			unet_send_dbg_log("WORLD_map_made");
			Objects::Terrain::Map = this->Map;
			Objects::Terrain::ClearColliders();
			unet_send_dbg_log("WORLD_terrain_set");

			// Player contorller
			uint8_t controller = 0;

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

			unet_send_dbg_log("WORLD_entity_loop_done");
			jo_clear_screen();
			unet_send_dbg_log("WORLD_ctor_done");
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
