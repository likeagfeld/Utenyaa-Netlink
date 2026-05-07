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
#include "../net/utenyaa_main_glue.h"   /* unet_glue_num_characters */

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

		/** @brief Latched fire request — set on A rising edge, cleared
		 *  when fire actually executes after cooldown ends. Without
		 *  this, an A press DURING cooldown was lost: rising edge fires
		 *  on `push` register clear, but the fire was blocked by
		 *  cooldown > 0; by the time cooldown reached 0, A was still
		 *  held but there was no new rising edge to detect → no fire
		 *  even though the operator clearly pressed A. Operator
		 *  reported "sometimes pressing A to fire does nothing."
		 *  The latch queues a single shot for after cooldown so a
		 *  press that arrives mid-cooldown still produces a shot.
		 */
		bool pendingFire = false;

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

			// Fire logic with pending-fire latch only. Operator:
			// "bullets are firing now even before cooldown ends" —
			// the held-state backstop I'd added was firing on every
			// frame A is held, which combined with the latch let an
			// in-progress release-and-quickly-repress cycle slip a
			// shot in faster than 0x1b frames if the rebuild-pending
			// state survived the cooldown decrement. Reverting to
			// edge-only with the pending-fire latch — the latch
			// alone solves "press during cooldown lost" without
			// touching the cooldown gate.
			if (isLocalCtrl &&
				Helpers::IsControllerButtonDown(this->controller, JO_KEY_A))
			{
				this->pendingFire = true;
			}
			if (isLocalCtrl &&
				this->pendingFire &&
				this->shootCoolDownTimeLeft == 0)
			{
				this->pendingFire = false;
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
		 * Two schemes selected at runtime by peripheral type:
		 *
		 *   1. Default (Saturn standard pad, OR 3D Control Pad in
		 *      DIGITAL mode):
		 *      - Tank controls verbatim from upstream:
		 *        UP/DOWN move forward/backward along current facing,
		 *        LEFT/RIGHT rotate the character.
		 *
		 *   2. 3D Control Pad in ANALOG mode (peripheral ID 0x16,
		 *      i.e. the user has flipped the switch on the controller
		 *      to the "3D" position):
		 *      - 8-direction screen-relative movement: UP = world +Y
		 *        (screen up), RIGHT = world +X (screen right), etc.
		 *        D-pad gives 8 directions; analog stick gives smooth
		 *        magnitude with a 32-unit deadzone.
		 *      - Character auto-rotates to face movement direction each
		 *        frame so HandleActions() bullet/bomb fire (cos/sin of
		 *        this->angle) goes the way you're moving.
		 *
		 * Downstream boundary / ground-slope / dynamic-collider /
		 * static-collider passes are common to both schemes — only the
		 * way `movementDir` is computed and `this->angle` is updated
		 * differs.
		 */
		void HandleMovement()
		{
			// Resolve physical Saturn pad port for this Player's controller.
			// Mirrors Helpers::IsControllerButtonPressed routing exactly:
			//  offline → Nth available pad; online local → port 0/1.
			int physPort = -1;
			if (g_Game.isOnlineMode && g_Game.gameState == UGAME_STATE_GAMEPLAY)
			{
				if ((uint8_t)this->controller == g_Game.myPlayerID)
					physPort = Helpers::GetLocalP1Port();
				else if (g_Game.hasSecondLocal && (uint8_t)this->controller == g_Game.myPlayerID2)
					physPort = Helpers::GetNthAvailableController(1);
			}
			else
			{
				physPort = Helpers::GetNthAvailableController(this->controller);
			}

			// Detect 3D Control Pad in analog mode — peripheral ID 0x16.
			// Standard pad (0x02) and 3D pad in DIGITAL mode (also 0x02)
			// take the legacy tank-controls path below.
			bool useAnalogScheme = false;
			if (physPort >= 0 && physPort < JO_INPUT_MAX_DEVICE && Smpc_Peripheral)
			{
				if (Smpc_Peripheral[physPort].id == 0x16)
					useAnalogScheme = true;
			}

			// We compute `movementDir` (Vec3 in world space) and update
			// this->angle differently per scheme, then run the shared
			// downstream logic below.
			Vec3 movementDir = Vec3(Fxp(0.0), Fxp(0.0), Fxp(0.0));
			bool willMove = false;

			if (useAnalogScheme)
			{
				/* === 3D Control Pad analog scheme === */

				// Read analog stick first; deadzone means small drift around
				// center is ignored. Falls back to D-pad if stick centered.
				Fxp inX = Fxp(0.0);
				Fxp inY = Fxp(0.0);
				bool analogActive = false;
				PerAnalog* a = (PerAnalog*)&Smpc_Peripheral[physPort];
				int rawX = (int)a->x - 128;
				int rawY = (int)a->y - 128;
				const int DEADZONE = 32;
				if (rawX > DEADZONE || rawX < -DEADZONE ||
				    rawY > DEADZONE || rawY < -DEADZONE)
				{
					/* X is inverted relative to screen (world +X → screen LEFT
					 * because of the engine's camera + rotate_x(0.5) +
					 * translate(-10,-10,0) chain) so we negate rawX.
					 * Y on the Saturn analog stick is already 0=top/255=bot,
					 * matching world +Y = screen DOWN — so DON'T negate
					 * rawY. The previous build negated it (matching the
					 * inverted D-pad branch convention) which made stick
					 * UP move the tank screen-DOWN; user-confirmed bug. */
					const Fxp INV_127 = Fxp(1.0 / 127.0);
					inX = -Fxp::FromInt(rawX) * INV_127;
					inY =  Fxp::FromInt(rawY) * INV_127;
					analogActive = true;
				}
				if (!analogActive)
				{
					/* D-pad fallback. Same inversion reason as the analog
					 * branch above — pad RIGHT must produce inX < 0 (which,
					 * after the world-space negation that the targetAngle
					 * snap encodes via Vec3(inX,inY) below, manifests as
					 * tank movement to screen-RIGHT). Same for UP→inY<0. */
					if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_RIGHT)) inX = inX - Fxp(1.0);
					if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_LEFT))  inX = inX + Fxp(1.0);
					if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_UP))    inY = inY - Fxp(1.0);
					if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_DOWN))  inY = inY + Fxp(1.0);
					if (inX != Fxp(0.0) && inY != Fxp(0.0))
					{
						const Fxp INV_SQRT2 = Fxp(0.7071);
						inX = inX * INV_SQRT2;
						inY = inY * INV_SQRT2;
					}
				}

				if (inX == Fxp(0.0) && inY == Fxp(0.0))
					return;  // no input — no rotation, no movement

				/* Auto-rotate to face the movement direction (snap to 8).
				 *
				 * Previous version compared inX/inY to exactly 0 to gate
				 * the cardinal branches — fine for D-pad (inX/inY land
				 * on exact ±1.0 or 0.0) but broken for analog stick
				 * input. Even after the 32-unit deadzone, a stick pushed
				 * "straight up" almost always reads a few raw units of
				 * X bleed, making `inX == 0.0` virtually never true.
				 * Tank could only ever face the four diagonals.
				 *
				 * New scheme: snap by the ratio of |inX| to |inY|. The
				 * 8-way sector boundaries lie at 22.5° / 67.5° from
				 * each axis, where tan(22.5°) ≈ 0.414 and tan(67.5°) ≈
				 * 2.414. Use 0.414 as the small-axis threshold relative
				 * to the dominant axis: if the smaller component is
				 * less than 41.4% of the larger, snap to cardinal
				 * (dominant axis only); else diagonal. Comparison done
				 * via |small| * 1000 vs |large| * 414 so it stays in
				 * fxp without a divide. */
				{
					Fxp ax = inX < Fxp(0.0) ? -inX : inX;
					Fxp ay = inY < Fxp(0.0) ? -inY : inY;
					bool dominant_x = ax > ay;
					Fxp small = dominant_x ? ay : ax;
					Fxp large = dominant_x ? ax : ay;
					/* small * 1000 < large * 414  →  small/large < 0.414  →  cardinal */
					bool cardinal = (small * Fxp(1000.0)) < (large * Fxp(414.0));

					Fxp targetAngle;
					if (cardinal) {
						if (dominant_x) {
							targetAngle = (inX > Fxp(0.0)) ? Fxp(0.0)
							                               : Fxp(Trigonometry::RadPi);
						} else {
							targetAngle = (inY > Fxp(0.0))
							                ? Fxp(Trigonometry::RadPi * 0.5)
							                : Fxp(Trigonometry::RadPi * 1.5);
						}
					} else {
						/* Diagonal — pick by quadrant of (inX, inY). */
						if (inY > Fxp(0.0)) {
							targetAngle = (inX > Fxp(0.0))
							                ? Fxp(Trigonometry::RadPi * 0.25)   // DR
							                : Fxp(Trigonometry::RadPi * 0.75);  // DL
						} else {
							targetAngle = (inX > Fxp(0.0))
							                ? Fxp(Trigonometry::RadPi * 1.75)   // UR
							                : Fxp(Trigonometry::RadPi * 1.25);  // UL
						}
					}
					this->angle = targetAngle;
				}

				// Build movement vector from intent × speed × dt.
				Objects::Terrain::Ground ground;
				ground.Height = 0.0;
				Objects::Terrain::GetGround(this->position, &ground);
				Fxp moveBy = Player::MovementSpeed;
				if (ground.Material == 4 || ground.Material == 6)
					moveBy *= Fxp(0.6);
				moveBy = moveBy * Fxp::BuildRaw(delta_time);
				movementDir = Vec3(inX * moveBy, inY * moveBy, Fxp(0.0));
				willMove = true;
			}
			else
			{
				/* === Default tank controls (verbatim upstream behavior) === */
				Fxp moveBy = Fxp(0.0);
				Fxp rotateBy = Fxp(0.0);

				if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_LEFT))
					rotateBy = Player::RotationSpeed;
				else if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_RIGHT))
					rotateBy = -Player::RotationSpeed;

				if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_UP))
					moveBy = Player::MovementSpeed;
				else if (Helpers::IsControllerButtonPressed(this->controller, JO_KEY_DOWN))
					moveBy = -Player::MovementSpeed;

				if (rotateBy != Fxp(0.0))
				{
					this->angle += rotateBy * Fxp::BuildRaw(delta_time);
					if (this->angle < Fxp(0.0))
						this->angle += Fxp(Trigonometry::RadPi * 2.0);
					else if (this->angle >= Fxp(Trigonometry::RadPi * 2.0))
						this->angle -= Fxp(Trigonometry::RadPi * 2.0);
				}

				if (moveBy == Fxp(0.0))
					return;  // rotation only; nothing left to do this frame

				Objects::Terrain::Ground ground;
				ground.Height = 0.0;
				Objects::Terrain::GetGround(this->position, &ground);
				if (ground.Material == 4 || ground.Material == 6)
					moveBy *= Fxp(0.6);

				ANGLE angle = Trigonometry::RadiansToSgl(this->angle);
				moveBy = moveBy * Fxp::BuildRaw(delta_time);
				movementDir = Vec3(
					Fxp::BuildRaw(slCos(angle)) * moveBy,
					Fxp::BuildRaw(slSin(angle)) * moveBy,
					Fxp(0.0));
				willMove = true;
			}

			if (!willMove) return;

			// === Common downstream pass: boundary + slope + collisions ===
			{
				Objects::Terrain::Ground ground;
				ground.Height = 0.0;
				Objects::Terrain::GetGround(this->position, &ground);

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

		/** @brief Read-only accessor for the first character sprite slot.
		 *  Used by the C-linkage glue so lobby.c can render a player's
		 *  selected character in the roster (instead of just a number). */
		static int GetCharacterSpriteStart()
		{
			return Player::CharacterSpiteStart;
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
			/* Online padding policy: ONLY remote players get the +6
			 * pad. Padding both sides (local AND remote) made the
			 * effective player-vs-player blocking distance 4+10+4+10
			 * → AABB-overlap fires at center-to-center 20 units,
			 * vs offline's 8 units. Symptom: tanks "stuck and can't
			 * move" / "stuck behind invisible walls" — the local
			 * player's own padded body locked them up against any
			 * remote tank 14+ visible units away. By padding only
			 * the remote side, bullet hit detection still benefits
			 * (point-vs-AABB uses the remote's bounds → +6), while
			 * the local player's own physics body stays at the
			 * native 4-unit half-size. Effective local-vs-remote
			 * blocking distance falls to 14 (4 local + 10 remote),
			 * close enough to offline feel for tanks to navigate. */
			bool isRemoteOnline = g_Game.isOnlineMode &&
				((uint8_t)this->controller != g_Game.myPlayerID) &&
				!(g_Game.hasSecondLocal && (uint8_t)this->controller == g_Game.myPlayerID2);
			Fxp halfSize = isRemoteOnline
				? (Player::Size + Fxp::FromInt(6))
				: Player::Size;
			*result = AABB(this->position, Vec3(halfSize, halfSize, halfSize));
		}

		/** @brief Apply an already-interpolated snapshot (remote players only).
		 *  Called from OnlineBridge::ApplyRemoteSnapshots each frame.
		 *  Under UNETv2 the bridge does the interpolation between bracketing
		 *  PLAYER_SYNC snapshots before calling here, so serverPos IS the
		 *  desired render position — no extra 50%-lerp / +3-frame extrap.
		 *  Doubling the smoothing made remote tanks feel rubbery and
		 *  arrive at server-truth always slightly late. Snap to serverPos.
		 *  Velocity is kept on the entity for any consumers that read it. */
		void ApplyNetworkSnapshot(const Vec3& serverPos, const Vec3& serverVel,
								  const Fxp& serverAngle, int16_t serverHp)
		{
			this->position = serverPos;
			this->angle    = serverAngle;
			this->health   = serverHp;
			(void)serverVel;
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
			/* Dead tanks must NOT block live tanks. Online mode pads
			 * the AABB by +6 units (Player::Size+6 per GetBounds), so
			 * a dead corpse left behind creates a ~20-unit-wide
			 * invisible wall that other players can't pass through.
			 * User-reported as "invisible wall" / "dead zone around
			 * player I can't get past". Only collide when alive.
			 * The tick loop also guards HandleMovement on health > 0
			 * so dead tanks are static — they remain at the position
			 * they died, just no longer block motion. */
			return this->health > 0;
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
			/* In online mode we never see the remote player's reload
			 * timer (only their position via PLAYER_SYNC) — flag with
			 * -1 so the HUD skips rendering "RDY" misleadingly. Local
			 * P1 and local P2 (co-op) get the real countdown. */
			playerUpdate.cooldownFrames = isRemoteOnline ? -1 : (int)this->shootCoolDownTimeLeft;

			UI::HudHandler.HandleMessages(playerUpdate);
		}

		/** @brief Draw detail
		 */
		void Draw() override
		{
			// Draw body
			jo_3d_push_matrix();
			jo_3d_translate_matrix_fixed(this->position.x.Value(), this->position.y.Value(), (this->position.z + 1.0).Value());

			if (this->health > 0)
			{
				/* Alive — render upright body with current facing. */
				slRotZ(Trigonometry::RadiansToSgl(this->angle));
				this->model->Draw(1);

				/* Muzzle flash overlay during the bright firing impact —
				 * upstream's ~12-frame window. */
				if (this->shootCoolDownTimeLeft > 0x10)
				{
					this->model->Draw(0);
				}
			}
			else
			{
				/* Dead — render the body tipped on its side (rotate 70°
				 * around Y so the tank lays flat) and offset DOWN so the
				 * silhouette reads as wreckage instead of a still-alive
				 * tank just facing oddly. Also lower z so the body sinks
				 * into the ground a bit, and skip muzzle flash. This
				 * gives a clear, visible "this tank is destroyed" cue
				 * without needing a separate wreckage model. */
				jo_3d_translate_matrix_fixed(0, 0, -((Fxp)1.5).Value());
				slRotY(jo_DEGtoANG_int(72));
				slRotZ(Trigonometry::RadiansToSgl(this->angle));
				this->model->Draw(1);
			}

			jo_3d_pop_matrix();

			// Draw head
			jo_3d_push_matrix();
			jo_3d_translate_matrix_fixed(this->position.x.Value(), (this->position.y - 1.0).Value(), (this->position.z + 4.0).Value());
			Fxp mirror = 0.4;

			/* Pick which CHARS.PAK character to render. Offline / dead
			 * tile fall back to the spawn-order controller index — the
			 * upstream behavior. Online + alive: look up the server's
			 * character_id for this pid in the roster so the cat
			 * stays stable per username (server's stable-hash fix
			 * takes effect here). Without this, the head sprite was
			 * keyed only by ready-order pid and changed every match. */
			int charIdx = (int)this->controller;
			if (g_Game.isOnlineMode && this->health > 0)
			{
				const unet_state_data_t* nd = unet_get_data();
				const int nchars = unet_glue_num_characters();
				for (int i = 0; i < nd->game_roster_count; i++)
				{
					if (nd->game_roster[i].active &&
						nd->game_roster[i].id == (uint8_t)this->controller)
					{
						uint8_t cid = nd->game_roster[i].character_id;
						if (cid < (uint8_t)nchars) charIdx = (int)cid;
						break;
					}
				}
			}
			/* Phase D minimum: custom-character VDP1 slot loading is
			 * deferred. CHARS.PAK is loaded with 5 built-in characters
			 * (indices 0..4 → sprites 0..24); indexing past that for a
			 * downloaded custom (character_id ≥ 5) would draw past the
			 * loaded textures and render garbage. Fallback: clamp the
			 * draw index to a safe built-in. character_id mod 5 keeps
			 * variety so different remote players picking different
			 * customs render as DIFFERENT default characters rather
			 * than all collapsing to char 0. The "random catgirl"
			 * spec in the design doc is approximated by deterministic
			 * mod — same character every render for stability across
			 * frames so the player doesn't visually thrash. */
			const int kBuiltinChars = 5;
			if (charIdx >= kBuiltinChars) {
				charIdx = charIdx % kBuiltinChars;
			}
			int index = (this->health > 0) ? charIdx : 4;
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
