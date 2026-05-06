#pragma once

#include <jo/Jo.hpp>

#include "../Utils/Math/Vec3.hpp"
#include "../Interfaces/IRenderable.hpp"
#include "../Interfaces/IUpdatable.hpp"
#include "../Utils/Helpers.hpp"

namespace Entities
{
	/** @brief Explosion particle
	 */
	struct Explosion : public IRenderable, IUpdatable, TrackableObject<Entities::Explosion>
	{
	private:

		/** @brief Time in seconds between frames
		 */
		inline const static Fxp FrameTime = 0.1;

		/** @brief Total number of frames
		 */
		inline const static unsigned char TotalFrames = 6;

		/** @brief Explosion sprite
		 */
		inline static int texture;

		/** @brief Sprite position
		 */
		Vec3 position;

		/** @brief Sprite scale
		 */
		Fxp scale;

		/** @brief Current frame
		 */
		unsigned char frame;

		/** @brief Currently passed time
		 */
		Fxp passedTime;

	public:
		/** @brief Set the explosion Texture ID
		 * @param Texture ID
		 */
		static void SetTextureId(uint16_t id)
		{
			Explosion::texture = id;
		}

		/** @brief Initializes a new instance of the Bomb class
		 * @param position Where to spawn explosion
		 * @param scale Sprite scale
		 */
		Explosion(Vec3& position, Fxp scale) : position(position), scale(scale)
		{
			this->frame = 0;
			this->passedTime = 0.0;
		}

		/** @brief Update bullet
		 */
		void Update() override
		{
			if (Explosion::FrameTime <= this->passedTime)
			{
				this->frame++;
				this->passedTime = 0.0;
			}

			if (Explosion::TotalFrames == this->frame)
			{
				delete this;
				return;
			}

			this->passedTime += Fxp::BuildRaw(delta_time);
		}
		
		/** @brief Draw bullet on screen
		 */
		void Draw() override
		{
			jo_3d_push_matrix();
			jo_3d_translate_matrix_fixed(this->position.x.Value(), this->position.y.Value(), this->position.z.Value());
			jo_3d_set_scale_fixed(this->scale.Value(), this->scale.Value(), this->scale.Value());

			Helpers::DrawSprite(Explosion::texture + this->frame);

			jo_3d_pop_matrix();
		}
	};

	/** @brief Multi-stage HUGE explosion sequencer — for player death
	 *  + bomb hits. Operator directive: "I want a HUGE BIG BANG
	 *  EXPLOSION, fully obvious what happened."
	 *
	 * A single regular Explosion is too easy to miss in a busy match
	 * (one 0.6 s sprite cycle at scale 0.25). For dramatic destruction
	 * events (tank obliterated, bomb shot) we spawn a fireball CLUSTER
	 * of FIVE explosions at scaled-up sizes and staggered world
	 * offsets in all four cardinal directions plus center, so the
	 * silhouette completely engulfs the entity for ~1.1 s.
	 *
	 * Stages (each Explosion has its own 0.6 s sprite-cycle lifetime):
	 *   t = 0.00 s  →  CENTER,             scale 3.5  (+0.5 z)
	 *   t = 0.12 s  →  +X / -Y offset,     scale 3.0  (+1.5 z)
	 *   t = 0.24 s  →  -X / +Y offset,     scale 3.5  (+2.5 z)
	 *   t = 0.36 s  →  -X / -Y offset,     scale 3.0  (+1.0 z)
	 *   t = 0.48 s  →  +X / +Y offset,     scale 3.5  (+2.0 z)
	 *
	 * After stage 4 spawns, this sequencer self-deletes; the spawned
	 * Explosion entities continue their own frame cycle independently
	 * (each lives 0.6 s = 6 frames × FrameTime). Total visible cluster
	 * = ~1.08 s (last spawn at 0.48 s + 0.6 s lifetime).
	 *
	 * The earlier 3-stage / 2.5-max-scale version was still being
	 * reported as "not obvious enough" — bumped to 5 stages and
	 * 3.5 max scale for unmistakable destruction feedback.
	 *
	 * Not renderable itself — only updates a timer and spawns. Lives
	 * for ~0.48 s before deleting itself.
	 */
	struct BigExplosion : public IUpdatable, TrackableObject<Entities::BigExplosion>
	{
	private:
		Vec3   center;
		Fxp    timer;
		int    stage;   /* next stage index to spawn (1..4); 5 = done */

		void spawnStage(int n)
		{
			Vec3 pos = this->center;
			Fxp  scale = Fxp(3.5);
			switch (n) {
				case 0:  /* center */
					pos.z = pos.z + Fxp(0.5);
					scale = Fxp(3.5);
					break;
				case 1:  /* +X -Y */
					pos.x = pos.x + Fxp(3.5);
					pos.y = pos.y - Fxp(3.0);
					pos.z = pos.z + Fxp(1.5);
					scale = Fxp(3.0);
					break;
				case 2:  /* -X +Y */
					pos.x = pos.x - Fxp(3.0);
					pos.y = pos.y + Fxp(3.5);
					pos.z = pos.z + Fxp(2.5);
					scale = Fxp(3.5);
					break;
				case 3:  /* -X -Y */
					pos.x = pos.x - Fxp(3.5);
					pos.y = pos.y - Fxp(3.5);
					pos.z = pos.z + Fxp(1.0);
					scale = Fxp(3.0);
					break;
				case 4:  /* +X +Y */
					pos.x = pos.x + Fxp(3.0);
					pos.y = pos.y + Fxp(3.0);
					pos.z = pos.z + Fxp(2.0);
					scale = Fxp(3.5);
					break;
				default:
					return;
			}
			new Entities::Explosion(pos, scale);
		}

	public:
		/* All members initialized in the body to sidestep a GCC
		 * complaint about Fxp's literal ctor in the member-init
		 * list. Functionally identical, just formally body-init. */
		BigExplosion(Vec3& centerRef) : center(centerRef)
		{
			this->timer = Fxp(0.0);
			/* Stage 0 fires immediately on construction so the boom
			 * starts on the same frame as the death event. */
			this->spawnStage(0);
			this->stage = 1;  /* next stage to spawn */
		}

		void Update() override
		{
			this->timer += Fxp::BuildRaw(delta_time);
			/* Spawn one stage per ~0.12 s tick. Each stage check is
			 * triggered when the timer crosses the cumulative threshold
			 * (0.12, 0.24, 0.36, 0.48) AND we're still pending that
			 * stage. Self-delete after stage 4. */
			if (this->stage == 1 && this->timer >= Fxp(0.12))
			{
				this->spawnStage(1);
				this->stage = 2;
			}
			else if (this->stage == 2 && this->timer >= Fxp(0.24))
			{
				this->spawnStage(2);
				this->stage = 3;
			}
			else if (this->stage == 3 && this->timer >= Fxp(0.36))
			{
				this->spawnStage(3);
				this->stage = 4;
			}
			else if (this->stage == 4 && this->timer >= Fxp(0.48))
			{
				this->spawnStage(4);
				delete this;
				return;
			}
		}
	};
}
