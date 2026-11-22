// Mods: the base game (its content as data, its rules from core.dll), mods'
// DLLs (loading, failing and crashing, undoing, events, commands, rules,
// each world's own state), manifests, data-only mods and fingerprints. The
// test mods' code is tests/mods/test_mod.cpp, built once per mod.

#include "content/content.h"
#include "data/json.h"
#include "helpers.h"
#include "game/blocks.h"
#include "mods/mods.h"
#include "world/world.h"

#include "check.h"

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
	std::error_code ec;
	fs::remove_all(dir, ec); // (a DLL of an earlier run may still be loaded: then it stays)
	fs::create_directories(dir);
	return dir;
}

// A mod folder `id` in `root` with these extra mod.json fields, and the test
// mod DLL `dll` (tests/mods/test_mod.cpp, by name; "" for none) as its entry.
void make_mod(const fs::path &root, const std::string &id, const std::string &dll, Data extra = Data::object()) {
	Data m = {{"id", id}, {"version", "1.0"}};
	if (!dll.empty()) {
		m["entry"] = id + ".dll";
		fs::create_directories(root / id);
		fs::copy_file(fs::path(GOCUBE_TEST_MODS_DIR) / "dlls" / (dll + ".dll"), root / id / (id + ".dll"),
				fs::copy_options::overwrite_existing);
	}
	for (const auto &[k, v] : extra.items()) {
		m[k] = v;
	}
	write(root / id / "mod.json", gc::write_json(m));
}

// Loads the base game and the mods in `root`.
struct Loaded {
	gc::Content content;
	std::unique_ptr<gc::Mods> mods;

	explicit Loaded(const fs::path &root) {
		mods = std::make_unique<gc::Mods>(content);
		mods->load_all({fs::path(GOCUBE_MODS_DIR), root});
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

} // namespace

TEST_CASE("the base game: its content is data, its rules are core.dll's") {
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({GOCUBE_MODS_DIR});
	REQUIRE(mods.problems().empty());
	REQUIRE(content.blocks.count("core:planks"));
	const Data &planks = content.blocks["core:planks"];
	CHECK(planks["color"] == "#b8945f");
	CHECK(planks["textures"]["all"]["color"] == "#93683d"); // painted, no image
	CHECK(planks["hardness"].get<double>() == 2.0);
	CHECK(planks["mod"] == "core");
	CHECK(content.recipes.empty()); // (no crafting in the base game)
	// Rules that need no world.
	CHECK(mods.rule("fall_damage", Data::array({10.0})) == std::optional<Data>(Data(7)));
	CHECK(mods.rule("fall_damage", Data::array({2.0})) == std::optional<Data>(Data(0)));
	CHECK(mods.rule("void_damage", Data::array({5})) == std::optional<Data>(Data(4)));
	const std::optional<Data> recipes = mods.rule("hand_recipes", Data::array());
	REQUIRE(recipes);
	REQUIRE(recipes->size() == 2);
	CHECK((*recipes)[0]["output"] == "core:planks");
	CHECK((*recipes)[0]["ingredients"]["core:log"] == 1);
	CHECK_FALSE(mods.rule("no_such_rule", Data::array()).has_value());
}

TEST_CASE("a mod whose code fails or crashes is undone: its content and handlers go, mods needing it are skipped") {
	const fs::path root = test_dir("failing");
	make_mod(root, "broken", "failing");
	make_mod(root, "wrecked", "crasher");
	make_mod(root, "fine", "fine");
	make_mod(root, "needs_broken", "fine", {{"dependencies", {"broken"}}});
	make_mod(root, "needs_missing", "", {{"dependencies", {"nowhere"}}});
	Loaded l(root);
	CHECK(l.problem("broken").find("boom") != std::string::npos);
	CHECK_FALSE(l.content.blocks.count("broken:half"));
	CHECK_FALSE(l.content.items.count("broken:thing"));
	CHECK_FALSE(l.content.mod_dirs.count("broken"));
	CHECK(l.problem("wrecked").find("access violation") != std::string::npos);
	CHECK_FALSE(l.content.blocks.count("wrecked:wreck"));
	CHECK(l.loaded("fine"));
	CHECK(l.content.blocks.count("fine:ok"));
	CHECK(l.problem("needs_broken").find("broken") != std::string::npos);
	CHECK_FALSE(l.content.blocks.count("needs_broken:ok"));
	CHECK(l.problem("needs_missing").find("nowhere") != std::string::npos);
	const std::string before = l.problem("broken");
	for (int i = 0; i < 5; i++) {
		l.mods->tick(0.05);
	}
	CHECK(l.problem("broken") == before); // its handler never ran
	CHECK(l.loaded("core"));
}

TEST_CASE("event handlers run; a mod whose handlers keep failing, or crash once, is turned off") {
	const fs::path root = test_dir("events");
	make_mod(root, "counter", "counter");
	make_mod(root, "flaky", "flaky");
	make_mod(root, "crashy", "crashy");
	Loaded l(root);
	REQUIRE(l.loaded("counter"));
	REQUIRE(l.loaded("flaky"));
	REQUIRE(l.loaded("crashy"));
	l.mods->tick(0.05);
	CHECK(l.problem("crashy").find("access violation") != std::string::npos); // at once
	CHECK(l.problem("flaky") == "");
	for (int i = 0; i < 5; i++) {
		l.mods->tick(0.05);
	}
	CHECK(l.problem("flaky").find("kept failing") != std::string::npos); // after 3
	CHECK(l.problem("counter") == ""); // one failure isn't enough
	CHECK_FALSE(l.content.blocks.count("counter:late"));
}

TEST_CASE("mod.json files that are broken, run code, need a newer game or point outside are skipped") {
	const fs::path root = test_dir("manifests");
	write(root / "badid" / "mod.json", R"({"id": "Bad Id"})");
	write(root / "baddata" / "mod.json", R"({"id": "baddata",)");
	// A manifest is data: nothing in it can run.
	write(root / "runs" / "mod.json", R"(os.exit(1) return {id = "runs"})");
	write(root / "spins" / "mod.json", R"(while true do end)");
	make_mod(root, "future", "", {{"api_version", 3}});
	make_mod(root, "noentry", "", {{"entry", "missing.dll"}});
	make_mod(root, "escape", "", {{"entry", "../core/core.dll"}});
	make_mod(root, "notdllname", "", {{"entry", "main.txt"}});
	make_mod(root, "notdll", "", {{"entry", "fake.dll"}});
	write(root / "notdll" / "fake.dll", "not a DLL");
	make_mod(root, "old", "", {{"api_version", 1}}); // (data only: an old manifest is fine)
	Loaded l(root);
	CHECK(l.problem("badid").find("\"id\"") != std::string::npos);
	CHECK(l.problem("baddata").find("can't be read") != std::string::npos);
	CHECK(l.problem("runs").find("can't be read") != std::string::npos);
	CHECK(l.problem("spins").find("can't be read") != std::string::npos);
	CHECK(l.problem("future").find("newer game") != std::string::npos);
	CHECK(l.problem("noentry").find("doesn't exist") != std::string::npos);
	CHECK(l.problem("escape").find("inside the mod") != std::string::npos);
	CHECK(l.problem("notdllname").find("must be a .dll") != std::string::npos);
	CHECK(l.problem("notdll").find("can't be loaded") != std::string::npos);
	CHECK(l.loaded("old"));
	CHECK(l.loaded("core"));
}

TEST_CASE("fingerprints: the same files give the same fingerprint") {
	// SHA-256 over the sorted relative paths (dot-names left out), each followed
	// by a 0 byte and the file's bytes; the first 12 hex digits. The value was
	// worked out separately (Python's hashlib) for this folder.
	const char *const EXPECTED = "fa60a95b89f0";
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
	make_mod(root, "builder", "builder");
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({fs::path(GOCUBE_MODS_DIR), root});
	REQUIRE(mods.problems().empty());
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	const gc::BlockRules rules(content);
	gocube::Generator generator;
	gc::configure_generator(generator, content, 4242);
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

TEST_CASE("mods add commands, command files and rules; wrap a rule; run commands (game.command)") {
	const fs::path root = test_dir("commands");
	make_mod(root, "greeter", "greeter");
	write(root / "greeter" / "functions" / "build" / "tower.txt", "# a tower\r\n/setblock 0 0 0 stone\r\n\r\nsay done\n");
	Loaded l(root);
	REQUIRE(l.problem("greeter") == "");
	CHECK(l.content.blocks.count("greeter:named_greeter")); // (mod.info)
	const std::vector<gc::ModCommand> list = l.mods->commands();
	REQUIRE(list.size() == 3);
	CHECK(list[2].name == "greet");
	CHECK(list[2].usage == "<name>");
	CHECK_FALSE(list[2].op);
	CHECK(list[0].op); // (by default: operators only)
	gc::CommandPlayer ann{2, "Ann", {1, 2, 3}, true};
	CHECK(l.mods->run_command("greet", ann, {"Bob"}) == std::optional<std::string>("hello Bob from Ann (op)"));
	CHECK_FALSE(l.mods->run_command("broken", ann, {}).has_value());
	CHECK_FALSE(l.mods->run_command("nothing", ann, {}).has_value());
	// Wrapping: the base game's fall damage (7 for 10 blocks), doubled.
	CHECK(l.mods->rule("fall_damage", Data::array({10.0})) == std::optional<Data>(Data(14)));
	// Command files: a line each (Windows line ends too), by "mod:path".
	const std::vector<std::string> *tower = l.mods->function("greeter:build/tower");
	REQUIRE(tower);
	CHECK(*tower == std::vector<std::string>{"# a tower", "/setblock 0 0 0 stone", "", "say done", ""});
	CHECK(l.mods->function("greeter:nothing") == nullptr);

	// Each world (Mods) has the mod's own state, though the DLL is loaded once.
	Loaded other(root);
	CHECK(l.mods->run_command("count", ann, {}) == std::optional<std::string>("1"));
	CHECK(l.mods->run_command("count", ann, {}) == std::optional<std::string>("2"));
	CHECK(other.mods->run_command("count", ann, {}) == std::optional<std::string>("1"));

	// game.command: through whatever runs commands (the session), while playing.
	gc::Content c2;
	gc::Mods mods(c2);
	mods.load_all({fs::path(GOCUBE_MODS_DIR), root});
	c2.freeze();
	const gc::BlockTable table = gc::build_block_table(c2);
	const gc::BlockRules rules(c2);
	gocube::Generator generator;
	gc::configure_generator(generator, c2, 4242);
	gc::World world(table.blocks, table.atlas.layout, generator);
	mods.attach(&world, &rules);
	std::vector<std::string> ran;
	mods.set_command_runner([&](const std::string &line) {
		ran.push_back(line);
		return std::vector<std::string>{"ok: " + line};
	});
	const std::optional<Data> r = mods.rule("try_command", Data::array({"/time set noon"}));
	REQUIRE(r);
	CHECK(*r == Data::array({"ok: /time set noon"}));
	CHECK(ran == std::vector<std::string>{"/time set noon"});
	mods.attach(nullptr, nullptr);
}

TEST_CASE("data-only mods: content from a data file (no code); folders where mods may not run code") {
	const fs::path root = test_dir("data_only");
	make_mod(root, "gems", "", {{"content", "content.json"}});
	write(root / "gems" / "content.json", R"({
	"blocks": {"ruby_ore": {"hardness": 3, "color": "#c02040"}, "ruby_block": {"color": "#e03050"}},
	"items": {"ruby": {"color": "#ff2050"}}
})");
	make_mod(root, "badcontent", "", {{"content", "content.json"}});
	write(root / "badcontent" / "content.json", R"({"spells": {"fire": {}}})");
	make_mod(root, "runsit", "", {{"content", "content.json"}});
	write(root / "runsit" / "content.json", "print('hi') return {}"); // (data only: code can't run)
	make_mod(root, "coder", "fine");
	Loaded l(root);
	CHECK(l.problem("gems") == "");
	CHECK(l.content.blocks.count("gems:ruby_ore"));
	CHECK(l.content.blocks.at("gems:ruby_ore").value("hardness", 0.0) == 3.0);
	CHECK(l.content.items.count("gems:ruby"));
	CHECK(l.problem("badcontent").find("unknown section") != std::string::npos);
	CHECK(l.problem("runsit").find("can't be read") != std::string::npos);
	CHECK(l.loaded("coder"));

	// The same folder as one whose mods may not run code: the data-only mod
	// loads, the one with a DLL doesn't.
	gc::Content content;
	gc::Mods mods(content);
	mods.set_code_forbidden_in({root});
	mods.load_all({fs::path(GOCUBE_MODS_DIR), root});
	CHECK(content.blocks.count("gems:ruby_ore"));
	CHECK_FALSE(content.blocks.count("coder:ok"));
	CHECK(mods.problems().at("coder").find("only allow server mods without code") != std::string::npos);
	CHECK(content.blocks.count("core:stone")); // (the base game, elsewhere, is fine)
}
