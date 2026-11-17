// Mods: the base game's main.lua, the sandbox (time, memory, what mods can
// reach, bytecode), undoing a failing mod, events, determinism and fingerprints.

#include "content/content.h"
#include "data/lua_data.h"
#include "helpers.h"
#include "game/blocks.h"
#include "mods/mods.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace fs = std::filesystem;
using gc::Data;

namespace {

fs::path test_dir(const char *name) {
	const fs::path dir = fs::temp_directory_path() / ("gocube_test_" + std::to_string(GetCurrentProcessId())) / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

void write(const fs::path &p, const std::string &text) {
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

// A mod folder `id` in `root` with main.lua (unless empty) and these extra mod.lua fields.
void make_mod(const fs::path &root, const std::string &id, const std::string &main_lua, Data extra = Data::object()) {
	Data m = {{"id", id}, {"version", "1.0"}};
	if (!main_lua.empty()) {
		m["entry"] = "main.lua";
		write(root / id / "main.lua", main_lua);
	}
	for (const auto &[k, v] : extra.items()) {
		m[k] = v;
	}
	write(root / id / "mod.lua", gc::write_lua_data(m));
}

// Loads the base game and the mods in `root`.
struct Loaded {
	gc::Content content;
	std::unique_ptr<gc::Mods> mods;
	double ms = 0;

	Loaded(const fs::path &root, gc::ModLimits limits = {}) {
		mods = std::make_unique<gc::Mods>(content, limits);
		const auto t0 = std::chrono::steady_clock::now();
		mods->load_all({fs::path(GOCUBE_SOURCE_DIR) / "mods", root});
		ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
	bool loaded(const std::string &id) const {
		for (const gc::ModInfo &m : mods->loaded()) {
			if (m.id == id) {
				return true;
			}
		}
		return false;
	}
	std::string problem(const std::string &id) const {
		auto it = mods->problems().find(id);
		return it == mods->problems().end() ? "" : it->second;
	}
};

gc::ModLimits quick() {
	gc::ModLimits l;
	l.load_ms = 200;
	l.callback_ms = 20;
	l.memory = 32u << 20;
	return l;
}

} // namespace

TEST_CASE("the base game's main.lua registers its planks (and no recipes)") {
	gc::Content content;
	load_base_game(content);
	REQUIRE(content.blocks.count("core:planks"));
	const Data &planks = content.blocks["core:planks"];
	CHECK(planks["color"] == "#b8945f");
	CHECK(planks["textures"]["all"]["color"] == "#93683d"); // painted (content/looks.lua), no image
	CHECK(planks["hardness"].get<double>() == 2.0);
	CHECK(planks["mod"] == "core");
	CHECK(content.recipes.empty()); // (no crafting in the base game)
}

TEST_CASE("an endless loop is stopped and the mod undone; others load") {
	const fs::path root = test_dir("loop");
	make_mod(root, "loop", "registry.register_block('thing', {color = '#ff0000'})\nwhile true do end\n");
	make_mod(root, "fine", "registry.register_block('ok', {color = '#00ff00'})\n");
	Loaded l(root, quick());
	CHECK(l.problem("loop").find("ran too long") != std::string::npos);
	CHECK_FALSE(l.loaded("loop"));
	CHECK_FALSE(l.content.blocks.count("loop:thing")); // undone
	CHECK(l.loaded("fine"));
	CHECK(l.content.blocks.count("fine:ok"));
	CHECK(l.ms < 3000);
}

TEST_CASE("pcall can't catch the sandbox stopping a mod") {
	const fs::path root = test_dir("pcall");
	make_mod(root, "sneaky", R"(
while true do
	pcall(function() while true do end end)
	xpcall(function() while true do end end, function(e) return e end)
end
)");
	Loaded l(root, quick());
	CHECK(l.problem("sneaky").find("ran too long") != std::string::npos);
	CHECK(l.ms < 3000);
}

TEST_CASE("a mod asking for too much memory is stopped and undone, and its memory freed") {
	const fs::path root = test_dir("memory");
	make_mod(root, "huge", "registry.register_block('a', {})\nlocal s = string.rep('x', 1 << 28) -- (256 MB: Lua itself refuses strings past 2 GB)\n");
	make_mod(root, "greedy", R"(
local t = {}
for i = 1, 100000000 do t[i] = string.rep('y', 100000) .. i end -- (100 KB a step: past the cap quickly)
)");
	make_mod(root, "catcher", R"(
for i = 1, 100 do pcall(function() local s = string.rep('z', 1 << 28) end) end
)");
	gc::ModLimits limits = quick();
	limits.load_ms = 5000; // (only memory may stop them here, even in a slow debug build)
	Loaded l(root, limits);
	CHECK(l.problem("huge").find("too much memory") != std::string::npos);
	CHECK(l.problem("greedy").find("too much memory") != std::string::npos);
	CHECK(l.problem("catcher").find("too much memory") != std::string::npos);
	CHECK_FALSE(l.content.blocks.count("huge:a"));
	CHECK(l.mods->memory_in_use() < (16u << 20)); // what they held was collected
}

TEST_CASE("mods reach only what the sandbox gives them") {
	const fs::path root = test_dir("reach");
	// The mod checks it all itself: any failed check is an error, and the mod fails.
	make_mod(root, "prober", R"(
for _, name in ipairs({'io', 'os', 'debug', 'package', 'coroutine', 'load', 'loadstring', 'dofile',
		'loadfile', 'collectgarbage'}) do
	assert(_G[name] == nil, name .. ' is reachable')
end
assert(string.dump == nil, 'string.dump is reachable')
assert(math.random == nil and math.randomseed == nil, 'math.random is reachable')
assert(getmetatable('') == 'string', "strings' metatable is reachable")
assert(not pcall(require, '../escape'), 'require left the mod')
assert(not pcall(require, 'missing'), 'require found a missing file')
assert(not pcall(setmetatable, {}, {__gc = function() end}), '__gc was allowed')
assert(not pcall(world.get_block, 0, 0, 0), 'world before there is one')
-- Creatures and the playing-only game functions: there, but only with a world.
for _, f in ipairs({creatures.spawn, creatures.remove, creatures.all, creatures.get, creatures.set,
		world.raycast, world.box_free, game.players, game.day, game.first_night, game.damage_player, game.play_sound,
		game.effect}) do
	assert(type(f) == 'function')
	assert(not pcall(f, 'core:block_eater', 0, 0, 0), 'a playing-only function before there is a world')
end
registry.register_creature('probe', {model = 'block_eater', box = {1, 1, 1}})
string.upper = nil -- (its own copy)
assert(require('helper').value == 42)
assert(require('lib.deep') == true)
registry.register_block('probed', {color = '#123456'})
)");
	write(root / "prober" / "helper.lua", "return {value = 42}\n");
	write(root / "prober" / "lib" / "deep.lua", "-- returns nothing: require gives true\n");
	make_mod(root, "after", "assert(('x'):upper() == 'X' and string.upper('y') == 'Y', 'another mod changed string')\n",
			{{"dependencies", {"prober"}}});
	Loaded l(root, quick());
	CHECK(l.problem("prober") == "");
	CHECK(l.content.blocks.count("prober:probed"));
	CHECK(l.content.creatures.count("prober:probe"));
	CHECK(l.problem("after") == "");
}

TEST_CASE("compiled Lua (bytecode) is refused") {
	const fs::path root = test_dir("bytecode");
	make_mod(root, "compiled", std::string("\x1bLua\x54\x00 fake bytecode", 20));
	make_mod(root, "requires", "require('bin')\n");
	write(root / "requires" / "bin.lua", std::string("\x1bLua\x54\x00", 6));
	Loaded l(root, quick());
	CHECK(l.problem("compiled").find("bytecode") != std::string::npos);
	CHECK(l.problem("requires").find("bytecode") != std::string::npos);
}

TEST_CASE("a failing mod is undone: its content and handlers go, mods needing it are skipped") {
	const fs::path root = test_dir("failing");
	make_mod(root, "broken", R"(
registry.register_block('half', {color = '#ffffff'})
registry.register_item('thing', {})
events.on('tick', function() error('a handler of an undone mod ran') end)
error('boom')
)");
	make_mod(root, "needs_broken", "registry.register_block('x', {})\n", {{"dependencies", {"broken"}}});
	make_mod(root, "needs_missing", "", {{"dependencies", {"nowhere"}}});
	Loaded l(root, quick());
	CHECK(l.problem("broken").find("boom") != std::string::npos);
	CHECK_FALSE(l.content.blocks.count("broken:half"));
	CHECK_FALSE(l.content.items.count("broken:thing"));
	CHECK_FALSE(l.content.mod_dirs.count("broken"));
	CHECK(l.problem("needs_broken").find("broken") != std::string::npos);
	CHECK_FALSE(l.content.blocks.count("needs_broken:x"));
	CHECK(l.problem("needs_missing").find("nowhere") != std::string::npos);
	const std::string before = l.problem("broken");
	for (int i = 0; i < 5; i++) {
		l.mods->tick(0.05);
	}
	CHECK(l.problem("broken") == before); // its handler never ran
	CHECK(l.loaded("core"));
}

TEST_CASE("event handlers run; one that keeps failing or runs too long turns its mod off") {
	const fs::path root = test_dir("events");
	make_mod(root, "counter", R"(
local ticks = 0
events.on('tick', function(dt)
	ticks = ticks + 1
	if ticks == 3 then registry.register_block('late', {}) end -- (too late: an error)
end)
)");
	make_mod(root, "slow", "events.on('tick', function() while true do end end)\n");
	Loaded l(root, quick());
	REQUIRE(l.loaded("counter"));
	REQUIRE(l.loaded("slow"));
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 6; i++) {
		l.mods->tick(0.05);
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	CHECK(l.problem("slow").find("ran too long") != std::string::npos); // turned off after 3
	CHECK(ms < 1000); // 3 x 20 ms, then never again
	CHECK(l.problem("counter") == ""); // one failure isn't enough
	CHECK_FALSE(l.content.blocks.count("counter:late"));
}

TEST_CASE("what mods register is the same every time (pairs order)") {
	const fs::path root = test_dir("order");
	make_mod(root, "orderly", R"(
local t = {}
for i = 1, 200 do t['key' .. i] = i end
local order = {}
for k in pairs(t) do order[#order + 1] = k end
registry.register_block('walk', {order = table.concat(order, ','), data = t})
)");
	std::string first;
	for (int run = 0; run < 3; run++) {
		Loaded l(root, quick());
		REQUIRE(l.content.blocks.count("orderly:walk"));
		const Data &walk = l.content.blocks["orderly:walk"];
		std::string dump = walk["order"].str() + ";"; // pairs' order, then the fields' order as read
		for (const auto &[k, v] : walk["data"].items()) {
			dump += k + "=" + std::to_string(v.get<int64_t>()) + ",";
		}
		if (run == 0) {
			first = dump;
		}
		CHECK(dump == first);
	}
}

TEST_CASE("broken mod.lua files and newer mods are skipped") {
	const fs::path root = test_dir("manifests");
	write(root / "badid" / "mod.lua", R"(return {id = "Bad Id"})");
	write(root / "badlua" / "mod.lua", "return { nope");
	// A manifest is data: nothing in it can run (there's nothing to call).
	write(root / "runs" / "mod.lua", R"(os.exit(1) return {id = "runs"})");
	write(root / "spins" / "mod.lua", R"(while true do end)");
	write(root / "oldjson" / "mod.json", R"({"id": "oldjson"})");
	make_mod(root, "future", "", {{"api_version", 2}});
	make_mod(root, "noentry", "", {{"entry", "missing.lua"}});
	make_mod(root, "escape", "", {{"entry", "../core/main.lua"}});
	make_mod(root, "jsoncontent", "", {{"content", {"content/blocks.json"}}});
	Loaded l(root, quick());
	CHECK(l.problem("badid").find("\"id\"") != std::string::npos);
	CHECK(l.problem("badlua").find("can't be read") != std::string::npos);
	CHECK(l.problem("runs").find("can't be read") != std::string::npos);
	CHECK(l.problem("spins").find("too long") != std::string::npos);
	CHECK(l.problem("oldjson").find("mod.lua now") != std::string::npos);
	CHECK(l.problem("future").find("newer game") != std::string::npos);
	CHECK(l.problem("noentry").find("doesn't exist") != std::string::npos);
	CHECK(l.problem("escape").find("inside the mod") != std::string::npos);
	// Content comes from Lua only: a mod still listing content files is told so.
	CHECK(l.problem("jsoncontent").find("from Lua") != std::string::npos);
	CHECK(l.loaded("core"));
}

TEST_CASE("fingerprints: the Godot version's algorithm (same files, same fingerprint)") {
	// SHA-256 over the sorted relative paths (dot-names left out), each followed
	// by a 0 byte and the file's bytes; the first 12 hex digits. The value was
	// worked out separately (Python's hashlib) for this folder.
	const char *const EXPECTED = "fb9aa5253cc6";
	CHECK(gc::mod_fingerprint(fs::path(GOCUBE_SOURCE_DIR) / "tests" / "data" / "fingerprint_mod") == EXPECTED);
	const fs::path copy = test_dir("fingerprint") / "copy";
	fs::copy(fs::path(GOCUBE_SOURCE_DIR) / "tests" / "data" / "fingerprint_mod", copy, fs::copy_options::recursive);
	CHECK(gc::mod_fingerprint(copy) == EXPECTED);
	write(copy / ".another_hidden", "dot-names don't count");
	CHECK(gc::mod_fingerprint(copy) == EXPECTED);
	write(copy / "textures" / "a.bin", "changed");
	CHECK(gc::mod_fingerprint(copy) != EXPECTED);
}

TEST_CASE("event handlers see the world: block_placed and block_broken, get_block and set_block") {
	const fs::path root = test_dir("world");
	make_mod(root, "builder", R"(
events.on('block_placed', function(x, y, z, id)
	if id == 'core:stone' and world.get_block(x, y + 1, z) == 'core:air' then
		world.set_block(x, y + 1, z, 'core:glass')
	end
end)
events.on('block_broken', function(x, y, z, id)
	world.set_block(x, y, z, 'core:planks') -- grows back as planks
end)
)");
	gc::Content content;
	gc::Mods mods(content, quick());
	mods.load_all({fs::path(GOCUBE_SOURCE_DIR) / "mods", root});
	REQUIRE(mods.problems().empty());
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	const gc::BlockRules rules(content);
	gocube::Generator generator;
	gc::configure_generator(generator, content, 4242, gc::GENERATOR_VERSION);
	gc::World world(table.blocks, table.atlas.layout, generator);
	world.set_view_distance(32);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!(world.is_loaded(0, 60, 0) && world.stats().queued == 0)) {
		world.update(0.5f, 10.0f, 0.5f);
		REQUIRE(std::chrono::steady_clock::now() < deadline);
	}
	mods.attach(&world, &rules);
	const gc::BlockPos p{0, 60, 0}; // in the air
	REQUIRE(world.set_block(p, gocube::BlockId(content.block_index("core:stone"))));
	mods.block_placed(p, "core:stone");
	CHECK(content.block_ids()[world.block_at(0, 61, 0)] == "core:glass");
	REQUIRE(world.set_block(p, 0));
	mods.block_broken(p, "core:stone");
	CHECK(content.block_ids()[world.block_at(0, 60, 0)] == "core:planks");
	CHECK(mods.problems().empty());
	mods.attach(nullptr, nullptr);
}
