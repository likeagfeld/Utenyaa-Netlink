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

	/** @brief Three-explosion sequencer — for player death + bomb hits.
	 *
	 * A single regular Explosion is too easy to miss in a busy match
	 * (one 0.6 s sprite cycle at scale 0.25). For dramatic destruction
	 * events (tank obliterated, bomb shot) we want a fireball CLUSTER:
	 * three big explosions spawned at staggered times AND staggered
	 * world offsets so the silhouette covers the entity for ~1 s.
	 *
	 * Stages:
	 *   t = 0.00 s  →  Explosion at center,           scale 2.5
	 *   t = 0.18 s  →  Explosion at +X,-Y,+Z offset,  scale 2.0
	 *   t = 0.36 s  →  Explosion at -X,+Y,+Z offset,  scale 2.5
	 * After stage 2 spawns, this sequencer self-deletes; the spawned
	 * Explosion entities continue their own frame cycle independently
	 * (each lives 0.6 s = 6 frames × FrameTime). Total visible cluster
	 * = ~1.0 s (last spawn + last frame's lifetime).
	 *
	 * Not renderable itself — only updates a timer and spawns. Lives
	 * for ~0.36 s before deleting itself.
	 */
	struct BigExplosion : public IUpdatable, TrackableObject<Entities::BigExplosion>
	{
	private:
		Vec3   center;
		Fxp    timer;
		int    stage;   /* 0 = pending stage 1 spawn, 1 = pending stage 2, 2 = done */

		void spawnStage(int n)
		{
			Vec3 pos = this->center;
			Fxp  scale = Fxp(2.5);
			if (n == 0) {
				pos.z = pos.z + Fxp(2.0);
				scale = Fxp(2.5);
			} else if (n == 1) {
				pos.x = pos.x + Fxp(2.5);
				pos.y = pos.y - Fxp(1.5);
				pos.z = pos.z + Fxp(3.5);
				scale = Fxp(2.0);
			} else if (n == 2) {
				pos.x = pos.x - Fxp(2.0);
				pos.y = pos.y + Fxp(2.5);
				pos.z = pos.z + Fxp(3.0);
				scale = Fxp(2.5);
			} else {
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
			this->stage = 0;
			/* Stage 0 fires immediately on construction so the boom
			 * starts on the same frame as the death event. */
			this->spawnStage(0);
			this->stage = 1;
		}

		void Update() override
		{
			this->timer += Fxp::BuildRaw(delta_time);
			if (this->stage == 1 && this->timer >= Fxp(0.18))
			{
				this->spawnStage(1);
				this->stage = 2;
			}
			else if (this->stage == 2 && this->timer >= Fxp(0.36))
			{
				this->spawnStage(2);
				delete this;
				return;
			}
		}
	};
}
