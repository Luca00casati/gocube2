#pragma once
// Mods' content: blocks, items, recipes, ores, trees, biomes and sound sets
// that mods register from Lua; numeric block ids; the engine's block
// table and texture atlas; the world generator's tables.

#include "voxel/generator.h"
#include "voxel/mesher.h"

#include "data/data.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace gc {

class Content {
public:
	// Definitions by qualified id ("core:stone"), sorted by id. Each has "id"
	// and "mod" added.
	std::map<std::string, Data> blocks, items, recipes, ores, trees, biomes, sound_sets, creatures;
	std::map<std::string, std::filesystem::path> mod_dirs; // mod id -> folder

	// One entry (mods' registry.register_* calls): `id` gets "mod:" in front
	// unless it has a ":".
	void register_entry(std::map<std::string, Data> &table, const std::string &mod, const std::string &id,
			const Data &def);
	// The table for a section name ("blocks", "items", ...), or nullptr.
	std::map<std::string, Data> *table(const std::string &section);
	// After every mod: adds shaped blocks' variants, assigns block ids.
	void freeze(const std::vector<std::string> &saved_block_ids = {});

	// Numeric block ids: "core:air" first, then a world's saved ids, then the
	// rest sorted, so the numbers depend only on which blocks exist.
	void assign_block_ids(const std::vector<std::string> &saved = {});
	const std::vector<std::string> &block_ids() const { return block_ids_; }
	int block_index(const std::string &id) const; // -1 if unknown

	std::filesystem::path asset_path(const Data &def, const std::string &relative) const;

private:
	std::vector<std::string> block_ids_;
	std::unordered_map<std::string, int> block_index_;
};

// The tables the world generator is configured with.
struct GeneratorConfig {
	gocube::GeneratorIds ids;
	std::vector<gocube::OreKind> ores;
	std::vector<gocube::TreeKind> trees;
	std::vector<gocube::BiomeKind> biomes;
};
GeneratorConfig generator_config(const Content &content);
// The trees the generator knows, by id, in its order (Generator::tree_blocks' kind).
std::vector<std::string> tree_order(const Content &content);
// Sets up `generator` for a world of this seed.
void configure_generator(gocube::Generator &generator, const Content &content, uint32_t seed);

// Block textures packed into one atlas: each 32x32 tile fills a CELL x CELL
// area with copies (faces use the one at FACE_X, FACE_Y), so mipmaps never mix
// neighbouring tiles.
struct Atlas {
	static constexpr int TILE = 32, CELL = 4, FACE_X = 1, FACE_Y = 1;
	gocube::AtlasLayout layout;
	int width = 0, height = 0;
	std::vector<uint8_t> rgba; // width * height * 4, mip level 0
	std::vector<std::vector<uint8_t>> tiles; // TILE * TILE * 4 each

	// A tile's uv rectangle (u0, v0, u1, v1): the copy faces use inside its cell.
	void tile_uv(int tile, float uv[4]) const {
		const float cw = 1.0f / float(layout.cols), ch = 1.0f / float(layout.rows);
		uv[0] = (float(tile % layout.cols) + float(layout.face_x) / float(layout.cell)) * cw;
		uv[1] = (float(tile / layout.cols) + float(layout.face_y) / float(layout.cell)) * ch;
		uv[2] = uv[0] + cw / float(layout.cell);
		uv[3] = uv[1] + ch / float(layout.cell);
	}
};

struct BlockTable {
	std::vector<gocube::BlockInfo> blocks; // index = block id
	Atlas atlas;
	// Atlas tile of every item's and placeable block's icon (its "icon" or
	// "texture" image, a block's side face, else its colour).
	std::map<std::string, int> icons;
	int icon(const std::string &id) const {
		auto it = icons.find(id);
		return it == icons.end() ? 0 : it->second;
	}
};
BlockTable build_block_table(const Content &content);

// What players see an item or block called: its "name", else its id without
// the mod, words capitalised ("core:wood_pickaxe" -> "Wood Pickaxe").
std::string display_name(const Content &content, const std::string &id);

// A string's djb2 hash (the ore salts come from it).
uint32_t string_hash(const std::string &s);

} // namespace gc
