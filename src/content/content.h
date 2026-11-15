#pragma once
// Mods' content: blocks, items, recipes, ores, trees, biomes and sound sets
// from each mod's JSON content files; numeric block ids; the engine's block
// table and texture atlas; the world generator's tables.
//
// Mirrors the Godot version (Registry, BlockShapes, BlockAtlas, World's
// tables) closely: block ids, the block table and the generator tables must
// come out the same, or worlds would generate differently.

#include "voxel/generator.h"
#include "voxel/mesher.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace gc {

// Object keys keep the order they have in the file (biomes list their trees
// and plants as objects, and that order reaches the generator).
using Json = nlohmann::ordered_json;

constexpr int GENERATOR_VERSION = 3; // the newest; older worlds keep theirs

class Content {
public:
	// Definitions by qualified id ("core:stone"), sorted by id. Each has "id"
	// and "mod" added, as the Godot registry does.
	std::map<std::string, Json> blocks, items, recipes, ores, trees, biomes, sound_sets;
	std::map<std::string, std::filesystem::path> mod_dirs; // mod id -> folder

	// Reads a mod folder (mod.json and its content files). "" or what's wrong.
	std::string load_mod(const std::filesystem::path &dir);
	// After every mod: adds shaped blocks' variants, assigns block ids.
	void freeze(const std::vector<std::string> &saved_block_ids = {});

	// Numeric block ids: "core:air" first, then a world's saved ids, then the
	// rest sorted, so the numbers depend only on which blocks exist.
	void assign_block_ids(const std::vector<std::string> &saved = {});
	const std::vector<std::string> &block_ids() const { return block_ids_; }
	int block_index(const std::string &id) const; // -1 if unknown

	std::filesystem::path asset_path(const Json &def, const std::string &relative) const;

private:
	void register_entry(std::map<std::string, Json> &table, const std::string &mod, const std::string &id,
			const Json &def);

	std::vector<std::string> block_ids_;
	std::unordered_map<std::string, int> block_index_;
};

// The tables the world generator is configured with.
struct GeneratorConfig {
	gocube::GeneratorIds ids;
	std::vector<gocube::OreKind> ores;
	std::vector<gocube::TreeKind> trees;
	std::vector<gocube::BiomeKind> biomes; // empty for worlds older than generator 2
};
GeneratorConfig generator_config(const Content &content, int generator_version);
// Sets up `generator` for a world (seed and generator version from its meta).
void configure_generator(gocube::Generator &generator, const Content &content, uint32_t seed,
		int generator_version);

// Block textures packed into one atlas: each 32x32 tile fills a CELL x CELL
// area with copies (faces use the one at FACE_X, FACE_Y), so mipmaps never mix
// neighbouring tiles.
struct Atlas {
	static constexpr int TILE = 32, CELL = 4, FACE_X = 1, FACE_Y = 1;
	gocube::AtlasLayout layout;
	int width = 0, height = 0;
	std::vector<uint8_t> rgba; // width * height * 4, mip level 0
	std::vector<std::vector<uint8_t>> tiles; // TILE * TILE * 4 each
};

struct BlockTable {
	std::vector<gocube::BlockInfo> blocks; // index = block id
	Atlas atlas;
};
BlockTable build_block_table(const Content &content);

// Godot's String::hash (djb2): the ore salts in the Godot version came from it.
uint32_t godot_string_hash(const std::string &s);

} // namespace gc
