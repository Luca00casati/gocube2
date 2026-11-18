// World generation: terrain from a 3D density field (hills, cliffs,
// overhangs, now and then a floating bit), seas with sand and gravel beaches
// and sea floors, bare stone where the dirt runs out, biomes, caves, lava deep
// down, ore veins and trees. All methods are const and thread-safe once
// configured; the same seed always gives the same world.
#pragma once

#include "chunk.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gocube {

struct Noise3Cell; // noise.h

// Terrain blocks the base shape is made of.
struct GeneratorIds {
	BlockId stone = 0, dirt = 0, grass = 0, sand = 0, water = 0, lava = 0, ice = 0, gravel = 0;
};

// One kind of ore (defined by mods): veins are random walks through `replaces`.
struct OreKind {
	BlockId id;
	BlockId replaces;        // usually stone
	int min_y, max_y;        // vein starting points lie in this range
	int veins_per_chunk;     // attempts per 32^3 chunk
	int min_size, max_size;  // blocks per vein (before clipping)
	uint32_t salt;           // from the ore's id, so adding mods doesn't move veins
};

// Tree shapes.
enum TreeStyle : uint8_t { TREE_ROUND_OAK, TREE_BIG_OAK, TREE_CACTUS };

// One block of a tree, in world coordinates (only_air: leaves, which don't
// replace anything).
struct TreeBlock {
	int x, y, z;
	BlockId id;
	bool only_air;
};

// One kind of tree (defined by mods).
struct TreeKind {
	BlockId log, leaves;     // leaves may be 0 (cactus)
	int min_trunk, max_trunk;
	uint8_t style = TREE_ROUND_OAK;
};

// One biome (defined by mods): where it is in climate space, its ground and
// what grows there. The nearest biome to a column's climate wins; the land's
// height is blended between biomes so borders don't make cliffs.
struct BiomeKind {
	float temperature = 0.0f, humidity = 0.0f; // roughly -1..1
	float height = 20.0f;                      // hill height (blocks)
	float base = 0.0f;                         // raises or lowers the land
	BlockId surface = 0, filler = 0;           // top block and the ones under it
	std::vector<std::pair<int, int>> trees;    // tree kind, percent of tree cells
	bool frozen = false;                       // ice on water
	float mountains = 0.0f;                    // height of mountain ranges on top of the hills (0: none)
	BlockId rock = 0;                          // bare top (and filler) from rock_above up
	int rock_above = 1 << 20;
	BlockId cap = 0;                           // top of the highest peaks, from cap_above up
	int cap_above = 1 << 20;
	BlockId cover = 0;                         // a thin layer on top of the ground (snow), from cover_above up
	int cover_above = 1 << 20;
};

struct Tree {
	int x, z, ground, trunk;
	int kind;                // index into the tree kinds
};

class Generator {
public:
	static constexpr int SEA_LEVEL = -4;
	static constexpr float CAVE_WIDTH = 0.11f;
	static constexpr float CAVE_PERIOD = 48.0f;
	static constexpr float CAVE_Y_SQUASH = 1.6f;
	static constexpr int CAVE_MIN_Y = -92;
	static constexpr int LAVA_LEVEL = -76;       // caves below this are full of lava
	static constexpr float CAVERN_PERIOD = 40.0f;
	static constexpr int CAVERN_MAX_Y = -18;     // big caverns only deep down
	static constexpr int TREE_CELL = 8;          // at most one tree in each TREE_CELL x TREE_CELL
	static constexpr int TREE_REACH = 5;         // how far a tree's leaves spread
	// The density lattice: a value every LX, LY, LZ blocks (interpolated
	// between), from BOTTOM_Y up LY_COUNT points; solid where above 0.
	static constexpr int LX = 4, LY = 8, LZ = 4, BOTTOM_Y = -96, LY_COUNT = 30;
	static constexpr int TOP_LIMIT = 100;        // terrain stays under it (room for trees below the world's top, 127)

	void configure(uint32_t seed, const GeneratorIds &ids, const std::vector<OreKind> &ores,
			const std::vector<TreeKind> &trees);
	void set_biomes(const std::vector<BiomeKind> &biomes);

	// The biome of a column (index into the biomes; 0 if none are configured).
	int biome_at(int x, int z) const;
	// The topmost solid block of the terrain at a column (before trees).
	int height_at(int x, int z) const;
	bool is_cave(int x, int y, int z) const;
	// The same, keeping each noise's lattice cell in `cells` (3) between calls.
	bool is_cave(int x, int y, int z, Noise3Cell *cells) const;
	// Trees whose leaves may reach columns [x0, x1) x [z0, z1).
	void trees_near(int x0, int z0, int x1, int z1, std::vector<Tree> &out) const;
	// The blocks of a tree of `kind` standing on the block at (x, ground, z)
	// (one growing by itself). Its height and shape come from the position.
	void tree_blocks(int kind, int x, int ground, int z, std::vector<TreeBlock> &out) const;
	int tree_kind_count() const { return int(_trees.size()); }

	// Fills a chunk whose minimum corner is `origin` (in blocks).
	void generate(ChunkData &chunk, int ox, int oy, int oz) const;

private:
	// A chunk column: the density at the lattice points over its 32 x 32
	// columns (the whole height of the world), each column's topmost solid
	// block and biome. Shared by the chunks stacked in it (the latest
	// COLUMN_CACHE kept).
	struct Columns {
		int heights[CHUNK_AREA];
		int biomes[CHUNK_AREA];
		int max_h;
		std::vector<float> density; // ((LY_COUNT x NZ) x NX)
	};
	static constexpr size_t COLUMN_CACHE = 256;
	std::shared_ptr<const Columns> columns(int ox, int oz) const;
	void fill_columns(Columns &c, int ox, int oz) const;
	void forget_cached();

	void climate(int x, int z, float &temperature, float &humidity) const;
	const BiomeKind &biome(int index) const;
	float mountain_shape(int x, int z) const; // 0 (no mountain) .. 1 (a ridge in a range)
	void blend(int x, int z, float &amp, float &base, float &mountains) const;
	float density(int x, int y, int z) const; // at a lattice point (world coordinates)
	// The ground's top block at a column whose topmost solid block is at h
	// (beaches, sea floor, bare stone, the biome's surface), and the filler
	// under it and how deep it goes (0: stone right under).
	BlockId top_block(int x, int h, int z, const BiomeKind &b, int &filler_depth, BlockId &filler) const;
	void place_tree(ChunkData &chunk, int ox, int oy, int oz, const Tree &t) const;
	// Calls put(x, y, z, id, only_air) for every block of the tree.
	template <typename Put>
	void build_tree(const Tree &t, Put &&put) const;
	void place_ores(ChunkData &chunk, int ox, int oy, int oz) const;

	uint32_t _seed = 0;
	GeneratorIds _ids;
	std::vector<OreKind> _ores;
	std::vector<TreeKind> _trees;
	std::vector<BiomeKind> _biomes;
	mutable std::mutex _columns_mutex;
	mutable std::unordered_map<int64_t, std::shared_ptr<const Columns>> _columns;
	mutable std::deque<int64_t> _columns_order;
};

} // namespace gocube
