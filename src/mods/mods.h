#pragma once
// Mods: found in folders, loaded in dependency order (the base game, "core",
// first). A mod can be data only (content as a data table, chat command
// files: nothing of it runs) or have code: a DLL the game loads into itself
// (mods/gocube_api.h, the C boundary; mods/mod_sdk.h for writing one in C++).
//
// A mod is a folder with mod.json, its manifest (JSON, data/json.h):
//   {
//     "id": "mymod",                       required, a-z 0-9 _
//     "name": "My Mod", "version": "1.0.0",
//     "dependencies": ["othermod"],        "core" is implicit
//     "content": "content.json",           content as data (optional): an object
//                                          {"blocks": {id: def}, "items": ...,
//                                          recipes, ores, trees, biomes,
//                                          sound_sets, creatures}
//     "entry": "mymod.dll",                code (optional), see below
//     "api_version": 2                     the modding API it was written for
//   }
// Command files: functions/NAME.txt (folders too), a chat command a line ("#"
// starts a comment); /run mod:NAME runs one, as whoever ran /run. Data and
// command files can't do anything a command doesn't allow, so a player can
// play only with servers whose mods are data (Settings::server_mod_code).
//
// The entry DLL's gocube_mod_load runs once at load. What it can call
// (GcApi::call, by name; arguments and results are data):
//   registry   register(section, id, def) (section: "blocks", "items",
//              "recipes", "ores", "trees", "biomes", "sound_sets",
//              "creatures"; ids without "mod:" get this mod's) -> the full id,
//              get(section, id) -> def or nothing, all(section) -> {[id] = def},
//              with(section, field) -> {[id] = def} of those with that field.
//              A definition is a table of named fields; a biome's "trees"
//              is a list of {id, percent} pairs (their order reaches the
//              world generator)
//   events     (GcApi::on) "tick" (dt), "block_placed" and "block_broken"
//              (x, y, z, block id; by the player), "block_changed" (x, y, z,
//              new block id; any change, told on the next tick),
//              "creature_hit" (id, damage, x, y, z of the attacker, attacker's peer)
//   world      get_block(x, y, z) -> id or nothing, set_block(x, y, z, id) -> bool
//              (a dyed id, "core:stone#ff8800", sets its dye too; both also
//              as GcApi::get_block/set_block),
//              supported(x, y, z, id) -> whether id could stand there,
//              drop_item(x, y, z, item, count), grow_tree(x, y, z, tree id) -> bool,
//              raycast(x, y, z, dx, dy, dz, max) -> {x, y, z, id, distance} or nothing,
//              box_free(x, y, z, width, height, depth) -> bool
//   creatures  spawn(kind, x, y, z [, variant]) -> id or nothing, remove(id),
//              all() -> {id...}, get(id) -> {kind, variant, x, y, z, yaw,
//              heading, speed, on_floor, hit_wall, walking} or nothing,
//              set(id, {heading, speed, variant, yaw, push, hurt, flash, attack});
//              their bodies and physics are the game's (game/creatures.h),
//              what they do is the mods'
//   game       (GcApi::set_rule) rules: the game asks this function when it
//              needs that rule (the last mod to set one wins);
//              rule_before(name, args...) -> what the rule the calling mod
//              replaced gives (to wrap it); random() -> 0..1 while playing
//              (not while loading: what mods register must depend only on
//              their files). Rules the game asks: see Mods::rule.
//              While playing: players() -> {{peer, x, y, z, survival, alive}},
//              day() -> 0..1, damage_player(peer, amount, source [, push xyz]),
//              play_sound(set, kind, x, y, z [, dB]), effect("smoke", x, y, z),
//              command(line) -> {reply lines}: a chat command, run as the
//              world itself (an operator)
//   commands   (GcApi::add_command) a chat command /name (a-z 0-9 _), with
//              {usage=, help=, op=true}; it's called with `who` {peer, name,
//              x, y, z, op} and `args` its words; what it returns (a string)
//              is the answer. op = false: anyone may run it.
//   mod        info() -> {id, version, dir}
//
// Native code isn't sandboxed: a mod can do anything the game can, so the
// player only runs code from servers if they allow it (Settings). A mod
// whose load fails is undone: its registrations are rolled back, its event
// handlers dropped, its DLL unloaded, and mods that depend on it are
// skipped. A function of a mod that fails (an error, an uncaught exception)
// ERRORS_BEFORE_DISABLED times turns the mod off; one that crashes (an access
// violation: caught) turns it off at once.

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

namespace gc {

class BlockRules;

struct ModInfo {
	std::string id, name, version;
	std::filesystem::path dir;
	std::string fingerprint; // of its files: two copies with the same one are the same mod
};

class Mods {
public:
	static constexpr int API_VERSION = 2; // (GOCUBE_API_VERSION)
	static constexpr int ERRORS_BEFORE_DISABLED = 3;

	explicit Mods(Content &content);
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
	// Mods in these folders may not run code (an "entry" DLL): they fail
	// to load, unless they're data only (content and command files).
	void set_code_forbidden_in(std::vector<std::filesystem::path> folders) { code_forbidden_ = std::move(folders); }

	// Mods' commands (commands.register), running one as `who` with these
	// arguments (what it answers, "" for nothing; nullopt if it failed), and their command files (functions/NAME.txt, by
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
	// Starts random() over from `seed` (it starts from a random one): the
	// same seed, the same numbers (tests).
	void set_random_seed(uint64_t seed);
	void block_changed(const BlockPos &p, const std::string &id);
	void tick(double dt);
	void block_placed(const BlockPos &p, const std::string &id);
	void block_broken(const BlockPos &p, const std::string &id);
	// A player hit a creature (peer 0: the local one) standing at `from`.
	void creature_hit(int id, int damage, const float from[3], int attacker);

	// Asks rule `name` (set by a mod, GcApi::set_rule), with these arguments
	// (a list); its result, or nothing if no mod set it or it failed (the
	// caller then uses its own default). Rules:
	//   mine_seconds(block, tool) -> seconds to break block holding tool ("" = hand)
	//   drops(block, tool) -> {item ids} breaking it gives (one each)
	//   starting_items() -> {{id=, count=}} a new survival player gets
	//   hand_recipes() -> {{id=, ingredients={[item]=count}, output=, count=}}
	//   smelting(item) -> {item=, count=} or nil
	//   fuel_seconds(item) -> seconds it burns in a furnace
	//   fall_damage(blocks) -> health a fall of that many blocks takes
	//   food(item) -> health eating it gives (0: not food)
	//   save_plants() -> data for world.json's "plants"; load_plants(data)
	//   use_item(item, x, y, z, nx, ny, nz) -> right click with item on that block's
	//     side (nx, ny, nz): nil if nothing was done (the item is placed), else
	//     {take = how many to use up, placed = {x, y, z} of a block put down}
	std::optional<Data> rule(const std::string &name, const Data &args);

	struct Impl; // (mods.cpp)

private:
	std::unique_ptr<Impl> impl_;
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
