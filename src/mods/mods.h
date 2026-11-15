#pragma once
// Mods: found in folders, loaded in dependency order (the base game, "core",
// first), each with its JSON content files and a Lua entry script, run in a
// sandbox.
//
// A mod is a folder with mod.json:
//   {
//     "id": "mymod",                       required, a-z 0-9 _
//     "name": "My Mod", "version": "1.0.0",
//     "dependencies": ["othermod"],        "core" is implicit
//     "content": ["content/blocks.json"],  data-only content (optional)
//     "entry": "main.lua",                 code (optional), see below
//     "api_version": 1                     the modding API it was written for
//   }
//
// The entry script runs once at load, in the mod's own environment: Lua's
// string (without dump), table, math (without random) and utf8 libraries,
// the safe basic functions, and
//   registry   register_block/item/recipe/ore/tree/biome/sound_set(id, def),
//              get_block(id), get_item(id), recipes() -> {[id] = def};
//              ids without "mod:" get this mod's
//   events     on(name, function): "tick" (dt), "block_placed" and
//              "block_broken" (x, y, z, block id)
//   world      get_block(x, y, z) -> id or nil, set_block(x, y, z, id) -> bool
//   game       set_rule(name, function): the game asks this function when it
//              needs that rule (the last mod to set one wins; game.rule(name)
//              gets the one before, to wrap it); random() -> 0..1 while playing
//              (not while loading: what mods register must depend only on
//              their files). Rules the game asks: see Mods::rule.
//   mod        {id, version}
//   print      to the game's log
//   require    another .lua file of this mod (text only)
// No io, os, debug, package, coroutines, load or loading bytecode.
//
// Limits: a mod's code is stopped when it runs too long (Limits) or asks for
// more memory than it may have; pcall can't catch that. A mod whose entry
// fails is undone: its registrations are rolled back, its event handlers
// dropped, and mods that depend on it are skipped. An event handler that
// fails ERRORS_BEFORE_DISABLED times turns its mod's handlers off.
//
// Lua is built with a fixed string-hash seed (thirdparty/lua), so pairs()
// walks a table in the same order on every run: what a mod registers depends
// only on its files.

#include "content/content.h"
#include "world/world.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace gc {

class BlockRules;

struct ModInfo {
	std::string id, name, version;
	std::filesystem::path dir;
	std::string fingerprint; // of its files: two copies with the same one are the same mod
};

struct ModLimits {
	double load_ms = 2000;       // an entry script's whole run
	double callback_ms = 20;     // one event handler call
	size_t memory = 256u << 20;  // more than in use when the mod's code started
	size_t total_memory = 1024u << 20; // the whole VM
};

class Mods {
public:
	static constexpr int API_VERSION = 1;
	static constexpr int ERRORS_BEFORE_DISABLED = 3;

	explicit Mods(Content &content, ModLimits limits = {});
	~Mods();
	Mods(const Mods &) = delete;
	Mods &operator=(const Mods &) = delete;

	// Finds the mods in these folders (each holds mod folders; an id found
	// twice keeps the first) and loads them. The content is then complete:
	// freeze it next.
	void load_all(const std::vector<std::filesystem::path> &dirs);
	const std::vector<ModInfo> &loaded() const { return loaded_; }
	// Why mods didn't load, or were turned off: mod id (or folder) -> reason.
	const std::map<std::string, std::string> &problems() const { return problems_; }

	// While playing: the world mods see (nullptr: none).
	void attach(World *world, const BlockRules *rules);
	void tick(double dt);
	void block_placed(const BlockPos &p, const std::string &id);
	void block_broken(const BlockPos &p, const std::string &id);

	// Asks rule `name` (set by a mod with game.set_rule), with these arguments
	// (a JSON array); its first result, or nothing if no mod set it, it failed
	// or ran too long (the caller then uses its own default). Rules:
	//   mine_seconds(block, tool) -> seconds to break block holding tool ("" = hand)
	//   drops(block, tool) -> {item ids} breaking it gives (one each)
	//   starting_items() -> {{id=, count=}} a new survival player gets
	//   hand_recipes() -> {{id=, ingredients={[item]=count}, output=, count=}}
	//   smelting(item) -> {item=, count=} or nil
	//   fuel_seconds(item) -> seconds it burns in a furnace
	std::optional<Json> rule(const std::string &name, const Json &args);

	// The sandbox's state, for the tests.
	size_t memory_in_use() const;
	struct Lua; // (mods.cpp)

private:
	std::unique_ptr<Lua> lua_;
	std::vector<ModInfo> loaded_;
	std::map<std::string, std::string> problems_;
};

// A short hash of every file in a mod folder (names and contents, dot-names
// left out), as the Godot version's ModLoader.fingerprint: players can tell
// whether two copies of a mod with the same version really match.
std::string mod_fingerprint(const std::filesystem::path &root);

} // namespace gc
