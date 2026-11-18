#pragma once
// Mods: found in folders, loaded in dependency order (the base game, "core",
// first). A mod can be data only (content as a data table, chat command
// files: nothing of it runs) or have a Lua entry script, run in a sandbox.
//
// A mod is a folder with mod.lua, its manifest: a Lua data table, read in an
// empty Lua state (data/lua_data.h: nothing in it can run):
//   return {
//     id = "mymod",                        required, a-z 0-9 _
//     name = "My Mod", version = "1.0.0",
//     dependencies = {"othermod"},         "core" is implicit
//     content = "content.lua",             content as data (optional): a table
//                                          {blocks = {[id] = def}, items = ...,
//                                          recipes, ores, trees, biomes,
//                                          sound_sets, creatures}, read like this one
//     entry = "main.lua",                  code (optional), see below
//     api_version = 1,                     the modding API it was written for
//   }
// Command files: functions/NAME.txt (folders too), a chat command a line ("#"
// starts a comment); /run mod:NAME runs one, as whoever ran /run. Data and
// command files can't do anything a command doesn't allow, so a player can
// play only with servers whose mods are data (Settings::server_mod_code).
// The entry script runs once at load, in the mod's own environment: Lua's
// string (without dump), table, math (without random) and utf8 libraries,
// the safe basic functions, and
//   registry   register_block/item/recipe/ore/tree/biome/sound_set/creature(id, def),
//              get_block(id), get_item(id), recipes() -> {[id] = def},
//              all(section) -> {[id] = def} ("blocks", "trees", ...);
//              ids without "mod:" get this mod's. A definition is a
//              table of named fields (lists 1..n inside it stay lists); a
//              biome's "trees" is a list of {id, percent} pairs
//              (their order reaches the world generator)
//   events     on(name, function): "tick" (dt), "block_placed" and
//              "block_broken" (x, y, z, block id; by the player),
//              "block_changed" (x, y, z, new block id; any change, told on
//              the next tick), "creature_hit" (id, damage, x, y, z of the
//              attacker, attacker's peer)
//   world      get_block(x, y, z) -> id or nil, set_block(x, y, z, id) -> bool,
//              supported(x, y, z, id) -> whether id could stand there,
//              drop_item(x, y, z, item, count), grow_tree(x, y, z, tree id) -> bool,
//              raycast(x, y, z, dx, dy, dz, max) -> {x, y, z, id, distance} or nil,
//              box_free(x, y, z, width, height, depth) -> bool
//   creatures  spawn(kind, x, y, z [, variant]) -> id or nil, remove(id),
//              all() -> {id...}, get(id) -> {kind, variant, x, y, z, yaw,
//              heading, speed, on_floor, hit_wall, walking} or nil,
//              set(id, {heading, speed, variant, yaw, push, hurt, flash, attack});
//              their bodies and physics are the game's (game/creatures.h),
//              what they do is the mods'
//   game       set_rule(name, function): the game asks this function when it
//              needs that rule (the last mod to set one wins; game.rule(name)
//              gets the one before, to wrap it); random() -> 0..1 while playing
//              (not while loading: what mods register must depend only on
//              their files). Rules the game asks: see Mods::rule.
//              While playing: players() -> {{peer, x, y, z, survival, alive}},
//              day() -> 0..1, damage_player(peer, amount, source [, push xyz]),
//              play_sound(set, kind, x, y, z [, dB]), effect("smoke", x, y, z),
//              command(line) -> {reply lines}: a chat command, run as the
//              world itself (an operator)
//   commands   register(name, {usage=, help=, op=true}, function(who, args)):
//              a chat command /name (a-z 0-9 _); `who` {peer, name, x, y, z,
//              op}, `args` its words; what it returns (a string) is the
//              answer. op = false: anyone may run it.
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
#include "game/commands.h"
#include "world/world.h"

#include <filesystem>
#include <functional>
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
	// freeze it next. `only` (multiplayer: the host's mods): load exactly these
	// entries (mod_entry), skipping every other copy found.
	void load_all(const std::vector<std::filesystem::path> &dirs, const std::vector<std::string> *only = nullptr);
	// Mods the player turned off (not the base game): skipped by load_all
	// unless they're the host's (`only`).
	void set_disabled(std::vector<std::string> ids) { disabled_ = std::move(ids); }
	// Mods in these folders may not run code (an "entry" script): they fail
	// to load, unless they're data only (content and command files).
	void set_code_forbidden_in(std::vector<std::filesystem::path> folders) { code_forbidden_ = std::move(folders); }

	// Mods' commands (commands.register), running one as `who` with these
	// arguments (what it answers, "" for nothing; nullopt if it failed or
	// ran too long), and their command files (functions/NAME.txt, by
	// "mod:NAME"; null if none).
	std::vector<ModCommand> commands() const;
	std::optional<std::string> run_command(const std::string &name, const CommandPlayer &who,
			const std::vector<std::string> &args);
	const std::vector<std::string> *function(const std::string &name) const;
	const std::vector<ModInfo> &loaded() const { return loaded_; }
	// Why mods didn't load, or were turned off: mod id (or folder) -> reason.
	const std::map<std::string, std::string> &problems() const { return problems_; }

	// While playing: the world mods see (nullptr: none), and what dropping
	// items and growing trees do there.
	void attach(World *world, const BlockRules *rules);
	struct WorldHooks {
		std::function<void(const BlockPos &, const std::string &, int)> drop_item;
		std::function<bool(const BlockPos &, const std::string &)> grow_tree;
		// Creatures (game/creatures.h): spawn (id or 0), remove, every id,
		// one as data (null if none), changing one (false if none).
		std::function<int(const std::string &kind, const float at[3], const std::string &variant)> creature_spawn;
		std::function<bool(int)> creature_remove;
		std::function<std::vector<int>()> creature_ids;
		std::function<Data(int)> creature_get;
		std::function<bool(int, const Data &)> creature_set;
		// Players: [{peer, x, y, z (feet), survival, alive}].
		std::function<Data()> players;
		std::function<float()> day; // 0 at night .. 1 in full day
		std::function<void(int peer, int amount, const std::string &source, const float push[3])> damage_player;
		std::function<void(const std::string &set, const std::string &kind, const float at[3], float volume_db)>
				play_sound;
		std::function<void(const std::string &name, const float at[3])> effect; // "smoke"
	};
	void set_world_hooks(WorldHooks hooks);
	// game.command(line): runs a command as the world itself (an operator);
	// what it says. Set by whoever runs commands (the session).
	void set_command_runner(std::function<std::vector<std::string>(const std::string &line)> run);
	void block_changed(const BlockPos &p, const std::string &id);
	void tick(double dt);
	void block_placed(const BlockPos &p, const std::string &id);
	void block_broken(const BlockPos &p, const std::string &id);
	// A player hit a creature (peer 0: the local one) standing at `from`.
	void creature_hit(int id, int damage, const float from[3], int attacker);

	// Asks rule `name` (set by a mod with game.set_rule), with these arguments
	// (a list); its first result, or nothing if no mod set it, it failed
	// or ran too long (the caller then uses its own default). Rules:
	//   mine_seconds(block, tool) -> seconds to break block holding tool ("" = hand)
	//   drops(block, tool) -> {item ids} breaking it gives (one each)
	//   starting_items() -> {{id=, count=}} a new survival player gets
	//   hand_recipes() -> {{id=, ingredients={[item]=count}, output=, count=}}
	//   smelting(item) -> {item=, count=} or nil
	//   fuel_seconds(item) -> seconds it burns in a furnace
	//   fall_damage(blocks) -> health a fall of that many blocks takes
	//   food(item) -> health eating it gives (0: not food)
	//   save_plants() -> data for world.lua's "plants"; load_plants(data)
	//   use_item(item, x, y, z, nx, ny, nz) -> right click with item on that block's
	//     side (nx, ny, nz): nil if nothing was done (the item is placed), else
	//     {take = how many to use up, placed = {x, y, z} of a block put down}
	std::optional<Data> rule(const std::string &name, const Data &args);

	// The sandbox's state, for the tests.
	size_t memory_in_use() const;
	struct Lua; // (mods.cpp)

private:
	std::unique_ptr<Lua> lua_;
	std::vector<ModInfo> loaded_;
	std::map<std::string, std::string> problems_;
	std::vector<std::string> disabled_;
	std::vector<std::filesystem::path> code_forbidden_;
	std::string load_data_content(const std::filesystem::path &dir, const Data &manifest, const std::string &id,
			Content &content);
	void load_functions(const std::filesystem::path &dir, const std::string &id);
};

// A short hash of every file in a mod folder (names and contents, dot-names
// left out): players can tell
// whether two copies of a mod with the same version really match.
std::string mod_fingerprint(const std::filesystem::path &root);
// The files a mod's fingerprint covers ("/"-separated paths under root, sorted).
std::vector<std::string> mod_files(const std::filesystem::path &root);
// "id@version#fingerprint": a mod exactly (players must have the same ones).
std::string mod_entry(const ModInfo &m);
// Every mod folder in these folders (each holding mod folders), by entry.
std::map<std::string, std::filesystem::path> mod_copies(const std::vector<std::filesystem::path> &dirs);

} // namespace gc
