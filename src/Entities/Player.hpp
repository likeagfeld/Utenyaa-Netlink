#pragma once

#include <jo/Jo.hpp>

#include "../Interfaces/IRenderable.hpp"
#include "../Interfaces/IUpdatable.hpp"
#include "../Interfaces/IColliding.hpp"

#include "../Utils/Geometry/AABB.hpp"
#include "../Utils/Math/Trigonometry.hpp"
#include "../Utils/Math/Plane3D.hpp"

#include "../Objects/Model.hpp"
#include "../Utils/ModelManager.hpp"
#include "../Utils/UI.hpp"

#include "Bullet.hpp"
#include "Mine.hpp"
#include "Bomb.hpp"

#include "../Messages/Damage.hpp"
#include "../Messages/Pickup.hpp"
#include "../Messages/QueryController.hpp"

#include "../Utils/Helpers.hpp"

#include "../net/utenyaa_net.h"
#include "../net/utenyaa_game.h"

namespace Entities
{
	/** @brief Static 3D detail
	 */
	struct Player : public IRenderable, IUpdatable, IColliding, TrackableObject<Entities::Player>
	{
	private:
		/** @brief Player size
		 */
		const Fxp Size = 4.0;

		/** @brief Number of frames controller has
		 */
		const int FramesPerController = 5;

		/** @brief Rotation speed in radians per frame
		 */
		const Fxp RotationSpeed = 4.0;

		/** @brief Movement speed in units per frame
		 */
		const Fxp MovementSpeed = 30.0;

		/** @brief Maximum health player can have
		 */
		const int16_t MaxHealth = 6;

		/** @brief Index of first character sprite
		 */
		inline static int CharacterSpiteStart;

		/** @brief Direction of the player
		 */
		Fxp angle;

		/** @brief Controller ID
		 */
		uint8_t controller;

		/** @brief Player health
		 */
		int16_t health = Player::MaxHealth;

		/** @brief Model of the detail
		 */
		Objects::Model* model;

		/** @brief Position of the detail in the world
		 */
		Vec3 position;

		/** @brief How much time is left before player can shoot again
		 */
		uint8_t shootCoolDownTimeLeft;

		/** @brief What pickup player has
		 */
		Messages::Pickup::PickupType hasPickup = Messages::Pickup::PickupType::None;

		/** @brief Handle actions like shooting or others
		 */
		void HandleActions()
		{
			ANGLE angle = Trigonometry::RadiansToSgl(this->angle);
			Vec3 movementDir = Vec3(
				Fxp::BuildRaw(slCos(angle)),
				Fxp::BuildRaw(slSin(angle)),
				0.0);

			// Local-only: drive weapon fire for controllers this Saturn owns.
			// In online mode, remote players get their projectiles via
			// server BULLET_SPAWN/MINE_SPAWN/BOMB_SPAWN broadcasts (see
			// utenyaa_online_bridge.cxx), so skip input-triggered fire for
			// non-local controllers here to avoid double-spawning.
			const bool isLocalCtrl = (!g_Game.isOnlineMode) ||
				((uint8_t)this->controller == g_Game.myPlayerID) ||
				(g_Game.hasSecondLocal && (uint8_t)this->controller == g_Game.myPlayerID2);

			// Do the shooting
			if (isLocalCtrl &&
				Helpers::IsControllerButtonDown(this->controller, JO_KEY_A) &&
				this->shootCoolDownTimeLeft == 0)
			{
				PoneSound::Sound::Play(1, PoneSound::PlayMode::Semi, 5);
				this->shootCoolDownTimeLeft = 0x1b;
				new Entities::Bullet(this->controller, movementDir, this->position);
				if (g_Game.isOnlineMode)
				{
					unet_send_fire_bullet(
						this->position.x.Value(), this->position.y.Value(), this->position.z.Value(),
						movementDir.x.Value(), movementDir.y.Value(), movementDir.z.Value());
				}
			}

			// Use pickup
			if (isLocalCtrl && Helpers::IsControllerButtonDown(this->controller, JO_KEY_B))
			{
				switch (this->hasPickup)
				{
				case Messages::Pickup::PickupType::Mine:
					new Mine(this->controller, this->position);
					if (g_Game.isOnlineMode)
						unet_send_drop_mine(this->position.x.Value(),
											this->position.y.Value(),
											this->position.z.Value());
					break;

				case Messages::Pickup::PickupType::Bomb:
					new Bomb(movementDir, this->position);
					if (g_Game.isOnlineMode)
						unet_send_throw_bomb(
							this->position.x.Value(), this->position.y.Value(), this->position.z.Value(),
							movementDir.x.Value(), movementDir.y.Value(), movementDir.z.Value());
					break;

				default:
					break;
				}

				// We have used the pickup
				this->hasPickup = Messages::Pickup::PickupType::None;
			}
		}

		/** @brief Handle player movement
		 *
		 * 8-directional D-pad-relative scheme:
		 *   - UP/DOWN/LEFT/RIGHT map directly to world-space movement
		 *     along +Y/-Y/-X/+X. Diagonals (UP+RIGHT etc.) move at the
		 *     correct speed (normalized by 1/√2) so diagonal isn't
		 *     √2× faster than cardinal.
		 *   - The character auto-rotates to face the movement direction
		 *     each frame, so HandleActions() (which derives bullet
		 *     direction from this->angle via cos/sin) keeps working —
		 *     bullets fire in the direction you're moving.
		 *   - When no D-pad input, the character holds its last facing
		 *     and stands still, so you can fire in your last-moved
		 *     direction without re-pressing the stick.
		 *
		 * The boundary / ground-slope / dynamic-collider / static-
		 * collider passes below are unchanged from the upstream tank-
		 * controls implementation.
		 */
		void HandleMovement()
		{
			Fxp inX = Fxp(0.0);
			Fxp inY = Fxp(0.0);

			// Resolve physical Saturn pad port for this Player's controller.
			// In offline multi-local play this is the Nth available pad. In
			// online play, our owned pids map to ports 0 and 1 (handled
			// inside IsControllerButtonPressed for digital input; mirror
			// the same routing here for raw analog access).
			int physPort = -1;
			if (g_Game.isOnlineMode && g_Game.gameState == UGAME_STATE_GAMEPLAY)
			{
				if ((uint8_t)this->controller == g_Game.myPlayerID)
					physPort = Helpers::GetNthAvailableController(0);
				else if (g_Game.hasSecondLocal && (uint8_t)this->controller == g_Game.myPlayerID2)
					physPort = Helpers::GetNthAvailableController(1);
			}
			else
			{
				physPort = Helpers::GetNthAvailableController(this->controller);
			}

			// 3D Control Pad in analog mode reports peripheral ID 0x16 with
			// PerAnalog layout (x at byte 8, y at byte 9; 0x80 = centered;
			// X: 0x00=left, 0xFF=right; Y: 0x00=up, 0xFF=down). Read it if
			// available; on standard pad (id=0x02) or 3D pad in DIGITAL
			// mode this branch is skipped and we fall back to the D-pad.
			bool analogUsed = false;
			if (physPort >= 0 && physPort < JO_INPUT_MAX_DEVICE && Smpc_Peripheral)
			{
				PerDigital* p = &Smpc_Peripheral[physPort];
				if (p->id == 0x16)
				{
					PerAnalog* a = (PerAnalog*)p;
					int rawX = (int)a->x - 128;   /* -128 .. +127 */
					int rawY = (int)a->y - 128;
					const int DEADZONE = 32;       /* ~25 % of full deflection */
					if (rawX > DEADZONE || rawX < -DEADZONE ||
					    rawY > DEADZONE || rawY < -DEADZONE)
					{
						/* Map -128..+127 to ~-1.0..+1.0 fxp. Y is inverted
						 * because Saturn analog Y reads HIGHER for stick-down,
						 * but we want positive inY = screen up. */
						const Fxp INV_127 = Fxp(1.0 / 127.0);
						inX =  Fxp::FromInt(rawX) * INV_127;
						inY = -Fxp::FromInt(rawY) * INV_127;
						analogUsed = true;
					}
				}
			}

			// Digital D-pad fallback (works for standard Saturn pad AND for
			// the 3D Control Pad in digital mode AND when the analog stick
			// is inside the deadzone). Independent UP/DOWN/LEFT/RIGHT bits
			// give 8-direction diagonals naturally.
			if (!analogUsed)
			{
				if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_RIGHT)) inX = inX + Fxp(1.0);
				if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_LEFT))  inX = inX - Fxp(1.0);
				if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_UP))    inY = inY + Fxp(1.0);
				if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_DOWN))  inY = inY - Fxp(1.0);
			}

			// No input → don't move and don't rotate (preserve last facing).
			if (inX == Fxp(0.0) && inY == Fxp(0.0)) return;

			// Normalize diagonals so diagonal speed matches cardinal speed
			// (1/√2 ≈ 0.7071). Pure axis presses keep magnitude 1.
			if (inX != Fxp(0.0) && inY != Fxp(0.0))
			{
				const Fxp INV_SQRT2 = Fxp(0.7071);
				inX = inX * INV_SQRT2;
				inY = inY * INV_SQRT2;
			}

			// Auto-rotate to face the movement direction. With a digital
			// D-pad there are exactly 8 possible result angles — snap to
			// the right one. If smooth rotation is wanted later, lerp
			// this->angle toward targetAngle here instead of assigning.
			{
				Fxp targetAngle;
				if (inY > Fxp(0.0))
				{
					if (inX > Fxp(0.0))      targetAngle = Fxp(Trigonometry::RadPi * 0.25);  // NE
					else if (inX < Fxp(0.0)) targetAngle = Fxp(Trigonometry::RadPi * 0.75);  // NW
					else                     targetAngle = Fxp(Trigonometry::RadPi * 0.5);   // N
				}
				else if (inY < Fxp(0.0))
				{
					if (inX > Fxp(0.0))      targetAngle = Fxp(Trigonometry::RadPi * 1.75);  // SE
					else if (inX < Fxp(0.0)) targetAngle = Fxp(Trigonometry::RadPi * 1.25);  // SW
					else                     targetAngle = Fxp(Trigonometry::RadPi * 1.5);   // S
				}
				else
				{
					// inY == 0, so inX is non-zero (we returned early on all-zero).
					if (inX > Fxp(0.0)) targetAngle = Fxp(0.0);                      // E
					else                targetAngle = Fxp(Trigonometry::RadPi);      // W
				}
				this->angle = targetAngle;
			}

			// Movement step
			{
				// Get current ground tile (for material check + below)
				Objects::Terrain::Ground ground;
				ground.Height = 0.0;
				Objects::Terrain::GetGround(this->position, &ground);

				// Waterlogged tiles slow movement (preserved upstream rule).
				Fxp moveBy = Player::MovementSpeed;
				if (ground.Material == 4 || ground.Material == 6)
				{
					moveBy *= Fxp(0.6);
				}

				moveBy = moveBy * Fxp::BuildRaw(delta_time);
				// Movement vector is the D-pad intent scaled by speed×dt,
				// not cos/sin of facing — that's the whole point of the
				// new control scheme.
				Vec3 movementDir = Vec3(inX * moveBy, inY * moveBy, Fxp(0.0));

				Fxp boundry = Fxp::BuildRaw(Objects::Map::MapDimensionSize << 19);

				if ((this->position.x + movementDir.x - Player::Size < 0.0) ||
					(this->position.x + movementDir.x + Player::Size > boundry))
				{
					movementDir.x = 0.0;
				}

				if ((this->position.y + movementDir.y - Player::Size < 0.0) ||
					(this->position.y + movementDir.y + Player::Size > boundry))
				{
					movementDir.y = 0.0;
				}

				this->position += movementDir;

				// Get new ground tile
				Objects::Terrain::GetGround(this->position, &ground);

				// Get ground height
				Vec3 location = Vec3(
					((this->position.x >> 3).TruncateFraction() + 0.5) << 3,
					((this->position.y >> 3).TruncateFraction() + 0.5) << 3,
					ground.Height + 1.0);

				Plane3D plane = Plane3D(ground.Normal, location);
				Fxp distance = plane.Distance(Vec3(this->position.x, this->position.y, 0.0));
				movementDir.z = distance - this->position.z;
				this->position.z = distance;

				// Check for collision
				// Find first moving collider, not neccesary the closest
				IColliding* collidesWith = TrackableObject<IColliding>::FirstOrDefault([this](IColliding* item) { return item != this && item->Collide(this); });

				// We have collided with something dynamic, move back
				if (collidesWith != nullptr)
				{
					// Move back
					this->position -= movementDir;

					// Find max axis to limit
					AABB colliderBox;
					collidesWith->GetBounds(&colliderBox);
					Vec3 center = colliderBox.GetCenter();
					Vec3 axies = this->position - center;
					int maxAxis = 0;

					for (int axis = Y; axis < XYZ; axis++)
					{
						if (((Fxp*)&axies)[axis].Abs() > ((Fxp*)&axies)[maxAxis].Abs())
						{
							maxAxis = axis;
						}
					}

					// Limit axis
					((Fxp*)&movementDir)[maxAxis] = 0.0;
				
					// Move player by new vector
					this->position += movementDir >> 1;
				}
				
				// Find static colliders
				AABB* staticCollision = Objects::Terrain::FindCollision(this->position, 2, this);

				// On static collision just move back
				if (staticCollision != nullptr)
				{
					// Do not move player
					this->position -= movementDir;
				}
			}
		}

	public:
		/** @brief Set the texture ID of first character sprite
		 * @param Texture ID
		 */
		static void SetTextureId(uint16_t id)
		{
			Player::CharacterSpiteStart = id;
		}

		/** @brief Initializes a new instance of the StaticDetail3D class
		 * @param position Position in the scene
		 * @param model render model of the static detail
		 * @param controller Controller ID
		 */
		Player(const Vec3& position, Fxp angle, uint8_t controller) : position(position), angle(angle), controller(controller)
		{
			this->shootCoolDownTimeLeft = 0;
			this->model = ModelManager::GetModel(1);
		}

		/** @brief Get the Health
		 */
		int GetHealth()
		{
			return this->health;
		}

		/** @brief Get player controller
		 */
		int GetController()
		{
			return this->controller;
		}

		/** @brief Get player world position (for network PLAYER_STATE sync)
		 */
		const Vec3& GetPosition() const { return this->position; }

		/** @brief Get player facing angle (radians)
		 */
		const Fxp& GetAngle() const { return this->angle; }

		/** @brief Get object bounds
		 * @param result Axis aligned bounding box
		 */
		void GetBounds(AABB* result) override
		{
			// Online mode: expand hitbox by NET_COLLISION_PAD (3 units in
			// Fxp 16.16 = 3.0 world units) to compensate for remote
			// position desync. Matches the Disasteroids fix: otherwise a
			// bullet can visually pass through a remote tank when their
			// snapshot is 2-3 frames stale compared to the shooter.
			if (g_Game.isOnlineMode)
			{
				Fxp paddedSize = Player::Size + Fxp::FromInt(3);
				*result = AABB(this->position, Vec3(paddedSize, paddedSize, paddedSize));
			}
			else
			{
				*result = AABB(this->position, Vec3(Player::Size, Player::Size, Player::Size));
			}
		}

		/** @brief Apply a server-authoritative snapshot (remote players only).
		 *  Called from OnlineBridge::ApplyRemoteSnapshots each frame after
		 *  PLAYER_SYNC decode. Performs 50% lerp toward server position and
		 *  +3-frame extrapolation along the server velocity to reduce
		 *  visible jitter from network latency (Disasteroids pattern). */
		void ApplyNetworkSnapshot(const Vec3& serverPos, const Vec3& serverVel,
								  const Fxp& serverAngle, int16_t serverHp)
		{
			// 50% lerp — halfway toward server pos.
			Vec3 diff = serverPos - this->position;
			this->position = this->position + (diff * Fxp(0.5));
			// Extrapolate +3 frames along server velocity.
			this->position = this->position + (serverVel * Fxp::FromInt(3));
			this->angle = serverAngle;
			this->health = serverHp;
		}

		/** @brief Handle messages sent to this entity
		 * @param message Recieved message
		 */
		void HandleMessages(Message& message) override
		{
			switch (message.typeId)
			{
			case Messages::Damage::Type:
				if (this->health > 0)
				{
					this->health -= ((Messages::Damage*)&message)->DamageValue;
					this->health = JO_MAX(this->health, 0);
					this->health = JO_MIN(this->health, Player::MaxHealth);
				}
				break;
			
			case Messages::QueryController::Type:
				((Messages::QueryController*)&message)->Handled = true;
				((Messages::QueryController*)&message)->Controller = this->controller;
				break;

			case Messages::Pickup::Type:
				this->hasPickup = ((Messages::Pickup*)&message)->Identifier;
				break;

			default:
				break;
			}
		}

		/** @brief Handle messages sent to this entity
		 * @param message Recieved message
		 */
		void HandleMessages(const Message& message) override
		{
			this->HandleMessages(const_cast<Message&>(message));
		}

		/** @brief Indicates whether collider is enabled
		 * @return Returns true
		 */
		bool IsColliderEnabled() override
		{
			return true;
		}

		/** @brief Make entity think
		 */
		void Update() override
		{
			// Handle shoot cool down
			if (this->shootCoolDownTimeLeft > 0)
			{
				this->shootCoolDownTimeLeft--;
			}

			// In online mode, remote players receive their position via
			// OnlineBridge::ApplyRemoteSnapshots (server PLAYER_SYNC with
			// lerp+extrapolate). Don't let local HandleMovement drift
			// their position from relay-buffer inputs — true passthrough.
			const bool isRemoteOnline = g_Game.isOnlineMode &&
				((uint8_t)this->controller != g_Game.myPlayerID) &&
				!(g_Game.hasSecondLocal && (uint8_t)this->controller == g_Game.myPlayerID2);

			// Allow player to do actions only if alive and controller is connected
			if (this->IsColliderEnabled() && this->health > 0)
			{
				if (!isRemoteOnline) this->HandleMovement();
				this->HandleActions();
			}

			UI::Messages::UpdatePlayer playerUpdate;

			playerUpdate.health = this->health;
			playerUpdate.index = controller;
			playerUpdate.powerupType = (int)this->hasPickup;

			UI::HudHandler.HandleMessages(playerUpdate);
		}

		/** @brief Draw detail
		 */
		void Draw() override
		{
			// Draw body
			jo_3d_push_matrix();
			jo_3d_translate_matrix_fixed(this->position.x.Value(), this->position.y.Value(), (this->position.z + 1.0).Value());
			slRotZ(Trigonometry::RadiansToSgl(this->angle));

			this->model->Draw(1);

			if (this->shootCoolDownTimeLeft > 0x10)
			{
				this->model->Draw(0);
			}

			jo_3d_pop_matrix();

			// Draw head
			jo_3d_push_matrix();
			jo_3d_translate_matrix_fixed(this->position.x.Value(), (this->position.y - 1.0).Value(), (this->position.z + 4.0).Value());
			Fxp mirror = 0.4;

			int index = (this->health > 0) ? this->controller : 4;
			int ang = Trigonometry::RadiansToDeg(this->angle);
			int frame = 0;

			if (ang <= 291 && ang >= 256)
			{
				mirror = -mirror;
				frame = 4;
			}
			else if (ang <= 256 && ang >= 201)
			{
				mirror = -mirror;
				frame = 3;
			}
			else if (ang <= 201 && ang >= 156)
			{
				mirror = -mirror;
				frame = 2;
			}
			else if (ang <= 156 && ang >= 111)
			{
				mirror = -mirror;
				frame = 1;
			}
			else if (ang <= 111 && ang >= 66)
			{
				mirror = -mirror;
				frame = 0;
			}
			else if (ang <= 66 && ang >= 21)
			{
				frame = 1;
			}
			else if (ang <= 336 && ang >= 291)
			{
				frame = 3;
			}
			else if (ang <= 21 || ang >= 336)
			{
				frame = 2;
			}
			
			jo_3d_set_scale_fixed(mirror.Value(), Fxp(0.4).Value(), Fxp(0.4).Value());

			Helpers::DrawSprite(this->CharacterSpiteStart + (index * Player::FramesPerController) + frame);
			
			jo_3d_pop_matrix();
		}
	};
}
