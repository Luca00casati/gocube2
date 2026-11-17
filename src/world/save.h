#pragma once
// A world's folder:
//   world.lua    a Lua data table (data/lua_data.h): seed, generator version,
//                game mode, the block id table, the player and everything else
//                the game keeps (block entities, growing plants...)
//   chunks/      one file per edited chunk (unedited chunks are generated again;
//                the Godot version's chunk file format)
// Fields this version doesn't use are kept as they are, so a world opened and
// saved loses nothing. Worlds saved as world.json (the Godot version's) aren't
// read.

#include "content/content.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gc {

class WorldSave {
public:
	static constexpr int FORMAT_VERSION = 3; // 3: world.lua (2 was world.json)

	// Opens (or, if it has no world.lua, starts) the world in `dir`. New
	// worlds get `new_seed` and the newest generator. "" or why it can't be
	// opened (a damaged world.lua, one from a newer version, a world.json).
	std::string open(const std::filesystem::path &dir, uint32_t new_seed, const std::string &new_mode = "creative");

	const std::filesystem::path &dir() const { return dir_; }
	std::filesystem::path chunks_dir() const { return dir_ / "chunks"; }
	uint32_t seed() const;
	int generator() const;
	std::string mode() const;
	// The world's own block id table (empty for a new world): numeric ids in its
	// chunk files mean these blocks.
	std::vector<std::string> block_ids() const;
	// After the content is frozen with block_ids(): the table now in use (the
	// saved one plus any new blocks), written with the next save.
	void set_block_ids(const std::vector<std::string> &ids);

	// The saved player ("position" is the feet; "rotation" is [pitch, yaw]), or an empty object.
	const Data &player() const;
	// Another field of world.lua (null if it has none), and setting one
	// (written with the next save): "block_entities", "plants", "time"...
	const Data &field(const std::string &key) const;
	void set_field(const std::string &key, const Data &value) { meta_[key] = value; }
	// Writes world.lua (atomically); `player` replaces the saved player's
	// fields it has, keeping the others (inventory, health...).
	bool save(const Data &player = Data::object());

private:
	bool write_meta();

	std::filesystem::path dir_;
	Data meta_ = Data::object();
};

} // namespace gc
