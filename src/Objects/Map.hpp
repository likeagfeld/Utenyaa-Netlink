#pragma once

#include <jo/Jo.hpp>
#include <stdio.h>           /* snprintf for tile-rotation diagnostics */
#include "Mesh3D.hpp"
#include "../Utils/LoaderUtil.hpp"
#include "../Utils/std/vector.h"
#include "../Interfaces/IColliding.hpp"
#include "../net/utenyaa_net.h"   /* unet_send_dbg_log for Map-ctor checkpoints */

/** @brief Game objects
 */
namespace Objects
{
	/** @brief Map object
	 */
	class Map
	{
	public:

		/** @brief Size of the map
		 */
		static const int MapDimensionSize = 20;

	private:
		/** @brief Index of first color in gouraud table
		 */
		static const int GouraudTableStart = JO_VDP1_VRAM + 0x70000;

		/** @brief Level tile — MUST be exactly 4 bytes to match the
		 *  .UTE on-disk layout. The packed attribute disables
		 *  alignment padding; without it some GCC configurations
		 *  back the bitfield with a 32-bit storage unit, making
		 *  sizeof(Tile) = 8 and offsetting every tile beyond the
		 *  first to a wrong .UTE byte. The explicit-byte rotation
		 *  + depth reads elsewhere in this file ALSO defend against
		 *  bitfield endianness mismatches (operator-reported
		 *  "rotations not taking effect on Saturn" was caused by
		 *  one or both of these issues).
		 */
		struct __attribute__((packed)) Tile
		{
			/** @brief Depth and rotation are present in a single byte
			 */
			unsigned char Rotation : 2;

			/** @brief Depth and rotation are present in a single byte
			 */
			unsigned char Depth : 6;

			/** @brief Index of a texture to use
			 */
			unsigned char Texture;

			/** @brief Unused space
			 */
			unsigned short Dummy;
		};

		/** @brief Gouraud table entry
		 */
		struct GouraudColor
		{
			/** @brief Vertex color
			 */
			jo_color Colors[4];
		};

		/** @brief Base entity definition as saved within a file
		 */
		struct EntityDefinition
		{
			/** @brief Entity type to spawn
			 */
			int Type;

			/** @brief X coordinate of tile to spawn entity on
			 */
			unsigned short TileX;

			/** @brief Y coordinate of tile to spawn entity on
			 */
			unsigned short TileY;

			/** @brief Direction in radians
			 */
			Fxp Direction;

			/** @brief Unused space
			 */
			unsigned char Dummy[16];
		};

		/** @brief Light data
		 */
		struct Light
		{
			/** @brief Light direction
			 */
			Vec3 Direction;

			/** @brief Light color
			 */
			jo_color Color;

			/** @brief Unused space
			 */
			short Dummy;
		};

		/** @brief Level file data
		 */
		struct LevelData
		{
			/** @brief File identifier (should read 'UTE' and 4th byte indicates version)
			 */
			unsigned char Identifier[4];

			/** @brief Map tiles
			 */
			Tile TileData[Map::MapDimensionSize * Map::MapDimensionSize];

			/** @brief Level sun
			 */
			Light Sun;

			/** @brief Precalculated gouraud table
			 */
			GouraudColor Gouraud[Map::MapDimensionSize * Map::MapDimensionSize];

			/** @brief Precalculated face normals
			 */
			Vec3 Normals[Map::MapDimensionSize * Map::MapDimensionSize];

			/** @brief Number of entities in the following block
			 */
			size_t EntityCount;
		};

		/** @brief 3D mesh of the map
		 */
		Mesh3D mapMesh;

		/** @brief List of tile heights for each tile
		 */
		jo_fixed tileHeights[Map::MapDimensionSize * Map::MapDimensionSize];

		/** @brief Get vertex index from location
		 * @param x X location
		 * @param y Y location
		 * @return Vertex index
		 */
		constexpr static size_t GetVertexIndex(const size_t& x, const size_t& y)
		{
			return x + (y * (Map::MapDimensionSize + 1));
		}

		/** @brief Get the Tile vertex heights
		 * @param data Level data
		 * @param x Tile X coordinate
		 * @param y Tile Y coordinate
		 * @param result array of 4 depths
		 */
		static void GetTileHeights(const LevelData* data, const int x, const int y, int* result);

	public:

		/** @brief Entity types map will try to spawn
		 */
		enum class EntityType : unsigned int
		{
			/** @brief Empty entity, does nothing
			 */
			Empty = 0,

			/** @brief Player spawn location and direction
			 */
			PlayerSpawn,

			/** @brief 3D model
			 */
			Model,

			/** @brief Item crate
			 */
			Crate
		};

		/** @brief Defines and entity to be created on map
		 */
		struct EntityCreationDefinition
		{
			/** @brief Type of the entity to be created
			 */
			EntityType Type;

			/** @brief Location of the entity in 3D space
			 */
			Vec3 Location;

			/** @brief Angle of the entity in radians
			 */
			Fxp Angle;
			
			/** @brief Reserved space
			 */
			unsigned char Reserved[2];
		};

		/** @brief Level sun light definition
		 */
		struct Sun
		{
			/** @brief Light direction
			 */
			Vec3 Direction;

			/** @brief Light color
			 */
			jo_color Color;
		};

		int EntityDefinitionsCount;

		/** @brief Entities to be created on the map
		 */
		EntityCreationDefinition* EntityDefinitions;

		/** @brief Light definition
		 */
		Sun Light;

		/** @brief Get tile index from location
		 * @param x X location
		 * @param y Y location
		 * @return Tile index
		 */
		constexpr static size_t GetTileIndex(const size_t& x, const size_t& y)
		{
			return x + (y * Map::MapDimensionSize);
		}

		/* file mode: load .UTE bytes from CD via jo_fs_read_file_in_dir.
		 * preload mode: caller passes an already-loaded buffer (from
		 * the streamed-map RX path); Map ctor parses in place and
		 * jo_free()s it on completion (matches CD-mode lifecycle). */
		Map(const char* file, int firstTerrainTexture);
		Map(uint8_t* preloadBuf, int preloadLen, int firstTerrainTexture);

		/** @brief Frees all resources and destroys the isntance
		 */
		~Map();

	private:
		/* Shared parse path used by both ctors. Takes ownership of the
		 * buffer (caller transfers it). On exit the buffer is jo_free()d.
		 * On parse failure (NULL stream), sets empty EntityDefinitions
		 * so World ctor's defensive check can abort cleanly. */
		void ParseLoadedStream(char* streamStart, int firstTerrainTexture);

	public:
		/** @brief Draw map
		 */
		void Draw();

		/** @brief Get information about specific tile
		 * @param x Tile X location
		 * @param y Tile Y location
		 * @param normal Ground plane normal
		 * @param height Ground height
		 * @param material Ground material
		 * @param collider Tile collider
		 * @param Used tile index
		 */
		int GetTile(int x, int y, Vec3* normal, Fxp* height, uint16_t* material)
		{
			int index = Map::GetTileIndex(x, y);
			*height = Fxp::BuildRaw(this->tileHeights[index]);
			*material = this->mapMesh.attbl[index].texno;

			*normal = Vec3(
				Fxp::BuildRaw(this->mapMesh.pltbl[index].norm[X]),
				Fxp::BuildRaw(this->mapMesh.pltbl[index].norm[Y]),
				Fxp::BuildRaw(this->mapMesh.pltbl[index].norm[Z]));

			return index;
		}
	};

	/** @brief Initializes a new instance of the Map class from CD .UTE
	 * @param file Map file name
	 * @param firstTerrainTexture Index of first terrain texture
	 */
	inline Map::Map(const char* file, int firstTerrainTexture)
	{
		unet_send_dbg_log("CKPT-M1 Map pre-fs-read");
		// Load level data
		char* stream = jo_fs_read_file_in_dir(file, JO_ROOT_DIR, NULL);
		unet_send_dbg_log("CKPT-M2 Map post-fs-read");
		this->ParseLoadedStream(stream, firstTerrainTexture);
	}

	/** @brief Initializes a new instance of the Map class from a streamed
	 *  buffer (received over NetLink via the MAP_BEGIN/CHUNK/END opcodes).
	 *  The caller must have allocated `preloadBuf` via jo_malloc; this
	 *  ctor takes ownership and jo_free()s it after parse completes.
	 *  preloadLen is informational only (parse trusts the .UTE header).
	 */
	inline Map::Map(uint8_t* preloadBuf, int preloadLen, int firstTerrainTexture)
	{
		(void)preloadLen;
		unet_send_dbg_log("CKPT-M1S Map streamed-ctor");
		this->ParseLoadedStream(reinterpret_cast<char*>(preloadBuf), firstTerrainTexture);
	}

	inline void Map::ParseLoadedStream(char* stream, int firstTerrainTexture)
	{
		if (!stream)
		{
			/* CD read failed (worn lens, bad sector, emulator quirk, OOM in
			 * the file-system bounce buffer) OR streamed buffer was null.
			 * Caller's defensive check on EntityDefinitionsCount will catch
			 * this and abort World ctor cleanly instead of dereferencing
			 * into garbage. */
			unet_send_dbg_log("CKPT-M2X Map stream NULL");
			this->EntityDefinitionsCount = 0;
			this->EntityDefinitions = nullptr;
			return;
		}
		char* streamStart = stream;
		LevelData* level = GetAndIterate<LevelData>(stream);

		// Initialize mesh
		this->mapMesh = Mesh3D((Map::MapDimensionSize + 1) * (Map::MapDimensionSize + 1), Map::MapDimensionSize * Map::MapDimensionSize);
		unet_send_dbg_log("CKPT-M3 Map mesh-init");

		/* Full-map diagnostic. Histograms how many of the 400 tiles
		 * land in each rotation bucket as decoded by the bitfield
		 * (level->TileData[i].Rotation, the path the parser actually
		 * uses) AND by explicit-byte read (tileBytePtr[0] >> 6).
		 * Sampled-tile output (the previous version) missed the bug
		 * because tiles 0 and 5 happened to be unrotated grass —
		 * the rotated tiles live deeper in the array.
		 *
		 * On the server, audit-script results show the .UTE files
		 * DO contain rotation values (e.g., dansfield.UTE: 354 r=0,
		 * 18 r=1, 13 r=2, 15 r=3). If the engine's histogram disagrees
		 * — especially if BITFIELD differs from EXPLICIT — we've
		 * proven the bitfield ordering is the active bug. Stride is
		 * also reported so we know if struct packing is right.
		 */
		{
			const int stride = (int)(reinterpret_cast<const uint8_t*>(&level->TileData[1])
			                       - reinterpret_cast<const uint8_t*>(&level->TileData[0]));
			int bf_hist[4]  = {0,0,0,0};
			int exp_hist[4] = {0,0,0,0};
			for (int i = 0; i < Map::MapDimensionSize * Map::MapDimensionSize; i++) {
				bf_hist[level->TileData[i].Rotation & 3]++;
				const uint8_t* tb = reinterpret_cast<const uint8_t*>(&level->TileData[i]);
				exp_hist[(tb[0] >> 6) & 3]++;
			}
			char buf[80];
			snprintf(buf, sizeof(buf),
			         "TILE_HIST sz=%d bf=%d/%d/%d/%d exp=%d/%d/%d/%d",
			         stride,
			         bf_hist[0], bf_hist[1], bf_hist[2], bf_hist[3],
			         exp_hist[0], exp_hist[1], exp_hist[2], exp_hist[3]);
			unet_send_dbg_log(buf);
		}

		this->Light.Direction = level->Sun.Direction;
		this->Light.Color = level->Sun.Color;

		Vec3 vector = -Light.Direction;
		slLight((FIXED*)&vector);

		// Load gouraud
		for (uint32_t color = 0; color < this->mapMesh.nbPolygon; color++)
		{
			jo_color* ptr = (jo_color*)(JO_VDP1_VRAM + 0x70000 + JO_MULT_BY_8(color));
			*ptr = level->Gouraud[color].Colors[2];
			*(ptr + 1) = level->Gouraud[color].Colors[1];
			*(ptr + 2) = level->Gouraud[color].Colors[0];
			*(ptr + 3) = level->Gouraud[color].Colors[3];
		}

		// Reset point data
		for (size_t x = 0; x <= Map::MapDimensionSize; x++)
		{
			for (size_t y = 0; y <= Map::MapDimensionSize; y++)
			{
				int index = Map::GetVertexIndex(x, y);
				this->mapMesh.pntbl[index][X] = ((x) << 19);
				this->mapMesh.pntbl[index][Y] = ((y) << 19);
				this->mapMesh.pntbl[index][Z] = 0;
			}
		}

		// Load tile geometry
		for (size_t tileY = 0; tileY < Map::MapDimensionSize; tileY++)
		{
			for (size_t tileX = 0; tileX < Map::MapDimensionSize; tileX++)
			{
				// Get tile depths
				int depths[4];
				Map::GetTileHeights(level, tileX, tileY, depths);

				// Get tile location in array
				size_t currentTile = Map::GetTileIndex(tileX, tileY);

				// // Set polygon
				(Vec3&)mapMesh.pltbl[currentTile].norm = level->Normals[currentTile];

				// Set vertex indicies
				size_t vertices[4] = {
					Map::GetVertexIndex(tileX + 1, tileY),
					Map::GetVertexIndex(tileX + 1, tileY + 1),
					Map::GetVertexIndex(tileX, tileY + 1),
					Map::GetVertexIndex(tileX, tileY),
				};

				// Calculate depth
				int depthsRotated[4] = {
					depths[2],
					depths[3],
					depths[1],
					depths[0]
				};

				// Average the 4 vertex depths to get real smooth depth
				int depth = (depths[0] + depths[1] + depths[2] + depths[3]) / 4;
				this->tileHeights[currentTile] = depth;

				/* Read rotation explicitly from the raw byte rather than
				 * via the C bitfield. The struct declares
				 *    unsigned char Rotation : 2;  // intended bits 7-6
				 *    unsigned char Depth    : 6;  // intended bits 5-0
				 * but GCC's bitfield-packing order for big-endian SH-2
				 * is implementation-defined and produced LSB-first
				 * decoding here — Rotation was being read from bits
				 * 1-0 (always 0 for tiles with depth < 4), so editor
				 * rotation changes had ZERO visible effect on Saturn.
				 * Editor packs the byte as `(rot<<6) | (mir<<4) |
				 * depth_low4`, so reading bits 7-6 directly is
				 * compiler-order-independent and matches the editor. */
				const uint8_t* tileBytePtr =
					reinterpret_cast<const uint8_t*>(&level->TileData[currentTile]);
				const uint8_t rotation_explicit = (tileBytePtr[0] >> 6) & 0x03;
				int baseIndex = 3 - rotation_explicit;

				for (size_t vertex = 0; vertex < 4; vertex++)
				{
					if (baseIndex >= 4)
					{
						baseIndex = 0;
					}

					this->mapMesh.pntbl[vertices[vertex]][Z] = depthsRotated[vertex];
					this->mapMesh.pltbl[currentTile].Vertices[baseIndex] = vertices[vertex];
					baseIndex++;
				}

				// Set attribute
				ATTR attribute = ATTRIBUTE(
					Dual_Plane,
					SORT_MAX,
					(uint16_t)(firstTerrainTexture + level->TileData[currentTile].Texture),
					JO_COLOR_White,
					CL32KRGB | No_Gouraud,
					CL32KRGB | MESHoff,
					sprNoflip,           /* ReyeMe (original author) confirmed: rotation
					                      * is achieved via vertex-array reorder, not
					                      * sprite-side H/V flip. The previous sprHVflip
					                      * here was inherited from the very first code
					                      * upload (49f4773) — incidental, never required
					                      * for the rotation logic. Switching to
					                      * sprNoflip eliminates ambiguity over whether
					                      * SGL Dual_Plane honors the flip bits, and
					                      * makes editor↔Saturn UV calibration trivial:
					                      * each polygon vertex slot V[k] samples image
					                      * corner k (V[0]=TL, V[1]=TR, V[2]=BR, V[3]=BL),
					                      * matching the default VDP1 sprite UV layout. */
					No_Option);

				attribute.gstb = 0xe000 + currentTile;
				JO_ADD_FLAG(attribute.atrb, CL_Gouraud);
				this->mapMesh.attbl[currentTile] = attribute;
			}
		}

		/* Diagnostic: dump the final SGL vertex assignment for one
		 * sample tile per stored-rotation value (0..3). Combined with
		 * the editor's known UV computation we can determine, byte-
		 * by-byte, where the editor preview and the Saturn render
		 * disagree — and prove whether the divergence is universal
		 * across maps or map-specific.
		 *
		 * Format per line:
		 *   TILE_ROT_DBG (x,y) rot=R tex=T V0=L0 V1=L1 V2=L2 V3=L3
		 * where each L is one of TR/BR/BL/TL — the screen-corner
		 * label of that vertex slot. SGL Dual_Plane assigns standard
		 * UVs (0,0)/(1,0)/(1,1)/(0,1) to V[0]/V[1]/V[2]/V[3] in
		 * order, so V[0]'s corner label IS the texture origin —
		 * that's the rotation visualised on screen.
		 *
		 * Sampling: walk row-major and pick the FIRST tile we see
		 * with each rot value 0..3. Emits at most 4 lines per match
		 * — compact enough to read at a glance, complete enough to
		 * fully characterise the rotation algorithm. */
		{
			int sampled[4] = { -1, -1, -1, -1 };  /* tile indices, -1 = none */
			for (int i = 0; i < Map::MapDimensionSize * Map::MapDimensionSize; i++) {
				const uint8_t* tb = reinterpret_cast<const uint8_t*>(&level->TileData[i]);
				int r = (tb[0] >> 6) & 3;
				if (sampled[r] < 0) sampled[r] = i;
				if (sampled[0] >= 0 && sampled[1] >= 0 &&
					sampled[2] >= 0 && sampled[3] >= 0) break;
			}
			for (int r = 0; r < 4; r++) {
				if (sampled[r] < 0) continue;
				int idx = sampled[r];
				int tx = idx % Map::MapDimensionSize;
				int ty = idx / Map::MapDimensionSize;
				int tr_v = (int)Map::GetVertexIndex(tx + 1, ty);
				int br_v = (int)Map::GetVertexIndex(tx + 1, ty + 1);
				int bl_v = (int)Map::GetVertexIndex(tx,     ty + 1);
				int tl_v = (int)Map::GetVertexIndex(tx,     ty);
				const char* lab[4] = { "??", "??", "??", "??" };
				for (int s = 0; s < 4; s++) {
					int v = (int)this->mapMesh.pltbl[idx].Vertices[s];
					if      (v == tr_v) lab[s] = "TR";
					else if (v == br_v) lab[s] = "BR";
					else if (v == bl_v) lab[s] = "BL";
					else if (v == tl_v) lab[s] = "TL";
				}
				char buf[100];
				snprintf(buf, sizeof(buf),
					"TILE_ROT_DBG (%d,%d) rot=%d tex=%u V0=%s V1=%s V2=%s V3=%s",
					tx, ty, r,
					(unsigned)level->TileData[idx].Texture,
					lab[0], lab[1], lab[2], lab[3]);
				unet_send_dbg_log(buf);
			}
		}

		/* Targeted Dansfield 2x2 calibration cluster diagnostic.
		 * Emits effective image-UV per world corner for the 4 tiles
		 * at (13,9)..(14,10) — the all-4-rotations test cluster on
		 * tex=1 (4-piece quarter-circle pattern).
		 *
		 * Format (one line per tile):
		 *   SAT_UV (x,y) rot=R  TL=(u,v) TR=(u,v) BR=(u,v) BL=(u,v)
		 *
		 * (u,v) is in three.js / image-TL-origin convention:
		 *   (0,0) = image TL pixel sampled at this world corner
		 *   (1,1) = image BR pixel sampled at this world corner
		 *
		 * Derivation: for each of the 4 world corners (TL/TR/BR/BL
		 * of the tile), find which polygon slot s holds that
		 * vertex (Vertices[s] == corner_v), then under sprHVflip
		 * the slot samples this image corner:
		 *   s=0 (CMD UV (0,0)) → image BR → (1,1)
		 *   s=1 (CMD UV (1,0)) → image BL → (0,1)
		 *   s=2 (CMD UV (1,1)) → image TL → (0,0)
		 *   s=3 (CMD UV (0,1)) → image TR → (1,0)
		 *
		 * Pair this output with editor3d.js EDIT_UV log for the
		 * same 4 tiles. Per-corner numerical match → editor and
		 * Saturn agree pixel-for-pixel, mismatch must be in the
		 * texture/render pipeline, NOT the UV math. Per-corner
		 * mismatch → divergence in the UV algorithm itself,
		 * directing where to look. */
		{
			const struct { int x, y; } target[4] = {
				{13, 9}, {14, 9}, {13, 10}, {14, 10}
			};
			/* Actual SGL Dual_Plane sampling — calibrated against
			 * operator hardware test on Dansfield. Cyclically shifted
			 * one position from VDP1 raw-sprite default:
			 *   slot 0 → image TR → (1, 0)
			 *   slot 1 → image BR → (1, 1)
			 *   slot 2 → image BL → (0, 1)
			 *   slot 3 → image TL → (0, 0)
			 * Editor's slotUVs in editor3d.js is updated in lockstep
			 * so EDIT_UV continues to match SAT_UV byte-for-byte. */
			static const float SLOT_UV[4][2] = {
				{1.0f, 0.0f},  /* slot 0 → image TR */
				{1.0f, 1.0f},  /* slot 1 → image BR */
				{0.0f, 1.0f},  /* slot 2 → image BL */
				{0.0f, 0.0f},  /* slot 3 → image TL */
			};
			for (int t = 0; t < 4; t++) {
				int tx = target[t].x;
				int ty = target[t].y;
				if (tx < 0 || tx >= (int)Map::MapDimensionSize ||
				    ty < 0 || ty >= (int)Map::MapDimensionSize) continue;
				int idx = tx + ty * (int)Map::MapDimensionSize;
				const uint8_t* tb = reinterpret_cast<const uint8_t*>(&level->TileData[idx]);
				int rot = (tb[0] >> 6) & 3;
				int corner_v[4] = {
					(int)Map::GetVertexIndex(tx,     ty),     /* TL */
					(int)Map::GetVertexIndex(tx + 1, ty),     /* TR */
					(int)Map::GetVertexIndex(tx + 1, ty + 1), /* BR */
					(int)Map::GetVertexIndex(tx,     ty + 1), /* BL */
				};
				float corner_uv[4][2];
				for (int c = 0; c < 4; c++) {
					corner_uv[c][0] = -1.0f;
					corner_uv[c][1] = -1.0f;
					for (int s = 0; s < 4; s++) {
						int v = (int)this->mapMesh.pltbl[idx].Vertices[s];
						if (v == corner_v[c]) {
							corner_uv[c][0] = SLOT_UV[s][0];
							corner_uv[c][1] = SLOT_UV[s][1];
							break;
						}
					}
				}
				/* newlib snprintf on this target lacks %f support
				 * (cost-saving build option; common on embedded
				 * libcs). Cast each component to int — guaranteed
				 * exact since SLOT_UV is only 0.0f or 1.0f. */
				char buf[160];
				snprintf(buf, sizeof(buf),
					"SAT_UV (%d,%d) rot=%d  TL=(%d,%d) TR=(%d,%d) BR=(%d,%d) BL=(%d,%d)",
					tx, ty, rot,
					(int)corner_uv[0][0], (int)corner_uv[0][1],
					(int)corner_uv[1][0], (int)corner_uv[1][1],
					(int)corner_uv[2][0], (int)corner_uv[2][1],
					(int)corner_uv[3][0], (int)corner_uv[3][1]);
				unet_send_dbg_log(buf);
			}
		}

		/* Hypothesis A — runtime sprHVflip plumbing check.
		 * Read back the .dir field of the polygon attribute Map.hpp
		 * actually wrote. Per SGL ATTRIBUTE macro (sl_def.h:143):
		 *   ATTR.dir = sprHVflip & 0x3f = 0x40032 & 0x3f = 0x32
		 * Bits 4 and 5 (mask 0x30) are H-flip and V-flip on the VDP1
		 * sprite control word.
		 * - dir == 0x32 → sprHVflip is fully encoded in source. If the
		 *   visual mismatch persists, SGL Dual_Plane's slPutPolygon
		 *   path is dropping the flip bits at draw time (rasterizer-
		 *   level issue, not an authoring issue).
		 * - dir == 0x02 (or anything missing 0x30) → ATTRIBUTE macro
		 *   path stripped the flip. Source-level fix needed. */
		{
			int idx_calib = 13 + 9 * (int)Map::MapDimensionSize;
			char buf[120];
			snprintf(buf, sizeof(buf),
				"SAT_DIR idx=(13,9) attbl.dir=0x%X attbl.atrb=0x%X attbl.flag=0x%X",
				(unsigned)this->mapMesh.attbl[idx_calib].dir,
				(unsigned)this->mapMesh.attbl[idx_calib].atrb,
				(unsigned)this->mapMesh.attbl[idx_calib].flag);
			unet_send_dbg_log(buf);
		}

		/* Hypothesis B — texture-upload orientation check.
		 * jo_sprite_add → __internal_jo_sprite_add at jo_engine/
		 * sprites.c:220 does a straight jo_dma_copy from PAK source
		 * data into VDP1 VRAM. Pixel order *should* be byte-identical:
		 *   - PAK file byte 0..1   = top-left pixel (16-bit ARGB1555)
		 *   - PAK file byte 510..511 = bottom-right pixel
		 * Read the first and last 16-bit words at the VRAM address
		 * jo_engine assigned to texture (firstTerrainTexture+1) — the
		 * tex=1 quarter-circle our calibration tiles use. User can
		 * compare these two words against the corresponding bytes in
		 * TERRAIN.PAK (texture 1's data starts after texture 0; each
		 * texture is 16x16 = 512 bytes after a small header).
		 * - First word ≠ PAK first 2 bytes → upload reorders pixels
		 *   (V-flip or row-major→col-major).
		 * - Equal → upload is straight; orientation is preserved. */
		{
			int sprite_id_tex1 = firstTerrainTexture + 1;
			const volatile uint16_t* p =
				(const volatile uint16_t*)jo_sprite_get_raw_data(sprite_id_tex1);
			uint16_t first_px = p ? p[0] : 0;
			uint16_t second_px = p ? p[1] : 0;
			uint16_t last_px = p ? p[255] : 0;       /* 16x16=256 px, last index 255 */
			uint16_t last_minus_1 = p ? p[254] : 0;
			/* Split: unet_send_dbg_log caps payload at 64 bytes
			 * (utenyaa_net.c:1253). One header line + one pixel
			 * line keeps each under the cap. */
			char buf[80];
			snprintf(buf, sizeof(buf),
				"SAT_TEX1H sprid=%d vram=0x%X",
				sprite_id_tex1, (unsigned)(uintptr_t)p);
			unet_send_dbg_log(buf);
			snprintf(buf, sizeof(buf),
				"SAT_TEX1P p0=%04X p1=%04X p254=%04X p255=%04X",
				(unsigned)first_px, (unsigned)second_px,
				(unsigned)last_minus_1, (unsigned)last_px);
			unet_send_dbg_log(buf);
		}

		// Load entities to spawn
		EntityDefinitionsCount = level->EntityCount;
		this->EntityDefinitions = new EntityCreationDefinition[level->EntityCount];
		for (size_t entity = 0; entity < level->EntityCount; entity++)
		{
			EntityDefinition* entityPtr = GetAndIterate<EntityDefinition>(stream);
			jo_fixed depth = this->tileHeights[Map::GetTileIndex(entityPtr->TileX, entityPtr->TileY)];

			this->EntityDefinitions[entity] = 
				EntityCreationDefinition
				(
					(EntityType)entityPtr->Type,
					Vec3(
						(Fxp::FromInt(entityPtr->TileX) + 0.5) << 3,
						(Fxp::FromInt(entityPtr->TileY) + 0.5) << 3,
						Fxp::BuildRaw(depth)
					),
					entityPtr->Direction,
					{ entityPtr->Dummy[0], entityPtr->Dummy[1] }
				);
		}

		jo_free(streamStart);
	}

	/** @brief Frees all resources and destroys the isntance
	 */
	inline Map::~Map() {}

	/** @brief Draw map
	 */
	inline void Map::Draw()
	{
		jo_3d_mesh_draw(this->mapMesh.JoPtr());
	}


	/** @brief Get the Tile vertex heights
	 * @param data Level data
	 * @param x Tile X coordinate
	 * @param y Tile Y coordinate
	 * @param result
	 */
	inline void Map::GetTileHeights(const LevelData* data, const int x, const int y, int* result)
	{
		int dim = Map::MapDimensionSize - 1;

		for (size_t py = 0; py < 2; py++)
		{
			int py1 = y + py - 1;
			int py1Min = JO_MIN(py1, dim);
			int tileY1 = JO_MAX(py1Min, 0);

			int py2 = py1 + 1;
			int py2Min = JO_MIN(py2Min, dim);
			int tileY2 = JO_MAX(py2Min, 0);

			for (size_t px = 0; px < 2; px++)
			{
				int px1 = x + px - 1;
				int px1Min = JO_MIN(px1, dim);
				int tileX1 = JO_MAX(px1Min, 0);

				int px2 = tileX1 + 1;
				int px2Min = JO_MIN(px2, dim);
				int tileX2 = JO_MAX(px2Min, 0);

				/* Same explicit-bit-read pattern as the rotation fix
				 * elsewhere in this file. Editor packs the byte as
				 * `(rot<<6) | (mir<<4) | (depth & 0x0F)` so the
				 * authoritative depth bits are 0-3 (4 bits). The
				 * struct's `Depth : 6` bitfield can read them via
				 * GCC's compiler-dependent ordering — which has been
				 * decoding incorrectly here on sh2eb-elf-gcc. Using
				 * explicit bit math eliminates the ambiguity. */
				const uint8_t* tb0 = reinterpret_cast<const uint8_t*>(
					&data->TileData[Map::GetTileIndex(tileX1, tileY1)]);
				const uint8_t* tb1 = reinterpret_cast<const uint8_t*>(
					&data->TileData[Map::GetTileIndex(tileX1, tileY2)]);
				const uint8_t* tb2 = reinterpret_cast<const uint8_t*>(
					&data->TileData[Map::GetTileIndex(tileX2, tileY2)]);
				const uint8_t* tb3 = reinterpret_cast<const uint8_t*>(
					&data->TileData[Map::GetTileIndex(tileX2, tileY1)]);
				result[0] = ((int32_t)(tb0[0] & 0x0F)) << 14;
				result[1] = ((int32_t)(tb1[0] & 0x0F)) << 14;
				result[2] = ((int32_t)(tb2[0] & 0x0F)) << 14;
				result[3] = ((int32_t)(tb3[0] & 0x0F)) << 14;
			}
		}
	}
}
