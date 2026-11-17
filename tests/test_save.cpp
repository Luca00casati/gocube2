// A world's folder: world.lua (a Lua data table) and chunk files. A world is
// made, edited and saved, and opens again with its edits and every field it
// had; worlds that can't be read safely (damaged, newer, the old world.json)
// are left alone.

#include "content/content.h"
#include "data/lua_data.h"
#include "helpers.h"
#include "world/save.h"
#include "world/world.h"

#include "game/settings.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace fs = std::filesystem;
using gc::BlockPos;
using gc::Data;

namespace {

fs::path fresh_dir(const char *name) {
	const fs::path dir = fs::temp_directory_path() / ("gocube_test_" + std::to_string(GetCurrentProcessId())) / name;
	fs::remove_all(dir);
	fs::create_directories(dir.parent_path());
	return dir;
}

Data read_world(const fs::path &dir) {
	Data meta;
	std::string why;
	REQUIRE_MESSAGE(gc::read_lua_data(dir / "world.lua", meta, why), why);
	return meta;
}

void write(const fs::path &p, const std::string &text) {
	std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::string utf8(const fs::path &p) {
	const std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

// A world opened from a folder (made if new, seed 4242), with its chunks
// around the origin loaded.
struct OpenWorld {
	gc::WorldSave save;
	gc::Content content;
	gc::BlockTable table;
	gocube::Generator generator;
	std::unique_ptr<gc::World> world;

	explicit OpenWorld(const fs::path &dir) {
		load_base_game(content);
		REQUIRE(save.open(dir, 4242, "survival") == "");
		content.freeze(save.block_ids());
		save.set_block_ids(content.block_ids());
		table = gc::build_block_table(content);
		gc::configure_generator(generator, content, save.seed(), save.generator());
		world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator, utf8(save.chunks_dir()));
		world->set_view_distance(32);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!(world->is_loaded(-40, 0, 3) && world->is_loaded(6, 0, 3) && world->stats().queued == 0)) {
			world->update(0.5f, 10.0f, 0.5f);
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	std::string block(int x, int y, int z) const {
		return content.block_ids()[world->block_at(x, y, z)];
	}
	gocube::BlockId id(const char *name) const {
		return gocube::BlockId(content.block_index(name));
	}
};

} // namespace

TEST_CASE("a world is made, edited and saved, and opens again with its edits and every field") {
	const fs::path dir = fresh_dir("edit");
	// Everything the game keeps, including lists with empty slots (false: a Lua
	// list can't hold nil) and an empty table.
	const Data inventory = Data::array({Data::array({"core:torch", 1}), false, Data::array({"core:dirt", 64}),
			Data::object({{"armor", Data::array({"", "", "", ""})}})});
	const Data chests = {{"1,2,3", {{"type", "chest"}, {"slots", Data::array({false, Data::array({"core:stick", 4})})}}}};
	std::vector<std::string> ids;
	{
		OpenWorld w(dir);
		ids = w.content.block_ids();
		CHECK(w.world->set_block({3, 5, 3}, w.id("core:air")));
		CHECK(w.world->set_block({1, 2, 1}, w.id("core:glass")));
		CHECK(w.world->set_block({-40, 0, 3}, w.id("core:planks"))); // another chunk
		CHECK(w.world->save_modified() == 2);
		w.save.set_field("block_entities", chests);
		w.save.set_field("plants", Data::array({Data::array({1, 41, 1, 12.5})}));
		w.save.set_field("time", 0.375);
		w.save.set_field("from_a_newer_mod", {{"keep", "me"}, {"empty", Data::object()}});
		Data player = Data::object();
		player["position"] = {1.5, 3.0, 1.5};
		player["rotation"] = {0.1, -1.0};
		player["flying"] = true;
		player["inventory"] = inventory;
		player["health"] = 17;
		CHECK(w.save.save(player));
	}
	CHECK(fs::exists(dir / "world.lua"));
	CHECK_FALSE(fs::exists(dir / "world.json"));
	CHECK(fs::exists(dir / "chunks" / "c.-2.0.0.bin"));

	OpenWorld w(dir);
	CHECK(w.save.seed() == 4242);
	CHECK(w.save.mode() == "survival");
	CHECK(w.content.block_ids() == ids);
	CHECK(w.block(3, 5, 3) == "core:air");
	CHECK(w.block(1, 2, 1) == "core:glass");
	CHECK(w.block(-40, 0, 3) == "core:planks");
	const Data meta = read_world(dir);
	CHECK(meta["player"]["position"][0].get<double>() == doctest::Approx(1.5));
	CHECK(meta["player"]["flying"] == true);
	CHECK(meta["player"]["health"] == 17);
	CHECK(meta["player"]["inventory"] == inventory);
	CHECK(meta["block_entities"] == chests); // (order-free)
	CHECK(meta["plants"][0][3].get<double>() == doctest::Approx(12.5));
	CHECK(meta["time"].is_number_float());
	CHECK(meta["from_a_newer_mod"]["keep"] == "me");
	CHECK(meta["format"] == gc::WorldSave::FORMAT_VERSION);
}

TEST_CASE("worlds that can't be read safely are left alone") {
	const fs::path dir = fresh_dir("refused");
	{
		gc::WorldSave save;
		REQUIRE(save.open(dir, 7) == "");
		REQUIRE(save.save());
	}
	Data meta = read_world(dir);
	meta["format"] = gc::WorldSave::FORMAT_VERSION + 1;
	write(dir / "world.lua", gc::write_lua_data(meta));
	gc::WorldSave save;
	CHECK(save.open(dir, 1).find("newer version") != std::string::npos);
	write(dir / "world.lua", "return { broken");
	CHECK(save.open(dir, 1).find("damaged") != std::string::npos);
	// A world.lua is data: code in it can't run (there's nothing to call).
	write(dir / "world.lua", "os.remove('x') return {seed = 1}");
	CHECK(save.open(dir, 1).find("damaged") != std::string::npos);
	CHECK(fs::exists(dir / "world.lua")); // (left as it is)

	// The old format (the Godot version's, and this one's before): not read.
	const fs::path old = fresh_dir("old_json");
	fs::create_directories(old);
	write(old / "world.json", R"({"seed": 1, "format": 2})");
	CHECK(save.open(old, 1).find("world.json") != std::string::npos);
	CHECK_FALSE(fs::exists(old / "world.lua"));
}

TEST_CASE("a new world gets a seed, the newest generator and the block table") {
	const fs::path dir = fresh_dir("new");
	gc::WorldSave save;
	REQUIRE(save.open(dir, 99) == "");
	CHECK(save.seed() == 99);
	CHECK(save.generator() == gc::GENERATOR_VERSION);
	CHECK(save.block_ids().empty());
	save.set_block_ids({"core:air", "core:stone"});
	const Data meta = read_world(dir);
	CHECK(meta["block_ids"] == Data::array({"core:air", "core:stone"}));
	CHECK(meta["format"] == gc::WorldSave::FORMAT_VERSION);
	CHECK(fs::is_directory(dir / "chunks"));
}

TEST_CASE("data values: brace lists, fields in order, equality, fallbacks") {
	// Pairs with a string first make an object; anything else a list.
	const Data obj = {{"b", 1}, {"a", "x"}};
	CHECK(obj.is_object());
	CHECK(Data{"core:torch", 5}.is_array());
	CHECK(Data::array({{"a", 1}})[0].is_array());
	CHECK(Data{}.is_null());
	// Fields keep the order they were added in; equality ignores it.
	REQUIRE(obj.items().size() == 2);
	CHECK(obj.items()[0].first == "b");
	CHECK(obj == Data{{"a", "x"}, {"b", 1}});
	CHECK(obj != Data{{"a", "x"}, {"b", 2}});
	CHECK(Data(1) == Data(1.0));
	CHECK(Data::array({1, 2}) != Data::array({2, 1}));
	// Reading: a missing field, the wrong type or not an object gives the fallback.
	CHECK(obj.value("b", 0) == 1);
	CHECK(obj.value("b", 0.5) == 1.0);
	CHECK(obj.value("a", 3) == 3);
	CHECK(obj.value("missing", "none") == "none");
	CHECK(Data(5).value("b", 7) == 7);
	CHECK(obj["missing"].is_null());
	CHECK(obj.contains("a"));
	CHECK_FALSE(obj.contains("c"));
	CHECK_THROWS_AS(obj.at("c"), std::out_of_range);
	CHECK_THROWS(obj["a"].get<int>());
	CHECK(Data(2.9).get<int>() == 2);
	// Changing: null becomes what's asked of it; update overwrites fields.
	Data d;
	d["x"]["y"] = 1;
	d["list"].push_back("first");
	d["list"][2] = true; // (grows, with null between)
	CHECK(d["list"].size() == 3);
	CHECK(d["list"][1].is_null());
	d.update({{"x", 2}, {"z", false}});
	CHECK(d["x"] == 2);
	CHECK(d.erase("z") == 1);
	CHECK(d.erase("z") == 0);
	CHECK(d.size() == 2);
	// Iterating goes through values; a number has none.
	int n = 0;
	for (const Data &v : Data::array({1, 2, 3})) {
		n += v.get<int>();
	}
	CHECK(n == 6);
	const Data five = 5;
	CHECK(five.begin() == five.end());
	CHECK(Data(std::vector<std::string>{"a", "b"}) == Data::array({"a", "b"}));
}

TEST_CASE("Lua data files: written and read back the same; only data") {
	const Data data = {
			{"int", 42}, {"negative", -7}, {"float", 0.1}, {"whole_float", 2.0}, {"big", 4294967295LL},
			{"text", "quote \" backslash \\ newline \n tab \t bell \a café"}, {"yes", true}, {"no", false},
			{"list", Data::array({1, "two", Data::array({3.5, false})})},
			{"needs brackets", 1}, {"end", "a keyword as a key"}, {"9lives", 9},
			{"nested", {{"deeper", {{"deepest", Data::array({"x"})}}}}},
	};
	const std::string text = gc::write_lua_data(data);
	CHECK(text.rfind("return {", 0) == 0);
	Data back;
	std::string why;
	REQUIRE_MESSAGE(gc::parse_lua_data(text, "test", back, why), why);
	// (Order-free: a Lua table's keys come back in its own order.)
	CHECK(back == data);
	CHECK(back["float"].is_number_float());
	CHECK(back["whole_float"].is_number_float());
	CHECK(back["int"].is_number_integer());
	// A list can't hold nil.
	CHECK_THROWS(gc::write_lua_data(Data::array({1, nullptr, 3})));
	// Not data: functions, code that calls anything, a script that never ends.
	CHECK_FALSE(gc::parse_lua_data("return {f = function() end}", "t", back, why));
	CHECK_FALSE(gc::parse_lua_data("print('hi') return {}", "t", back, why));
	CHECK_FALSE(gc::parse_lua_data("while true do end", "t", back, why));
	CHECK(why.find("too long") != std::string::npos);
	CHECK_FALSE(gc::parse_lua_data("return 5", "t", back, why));
	CHECK_FALSE(gc::parse_lua_data("\x1bLua", "t", back, why)); // bytecode
}

TEST_CASE("settings: saved and read back; what's missing or wrong is the default") {
	const std::filesystem::path file =
			std::filesystem::temp_directory_path() / ("gocube_settings_" + std::to_string(GetCurrentProcessId()) + ".lua");
	gc::Settings s;
	CHECK(s.view_distance == 128);
	CHECK(s.key("move_forward") == 'W');
	CHECK(s.load(file)); // (no file: the defaults)
	s.view_distance = 192;
	s.mouse_sensitivity = 0.0052f;
	s.max_fps = 60;
	s.vsync = false;
	s.smooth_lighting = false;
	s.shadow_mode = "off";
	s.render_scale = 0.75f;
	s.clock_24h = true;
	s.master_volume = 0.5f;
	s.ambience_volume = 0.25f;
	s.keys["jump"] = 'J';
	s.disabled_mods = {"lights", "boats"};
	s.player_name = "Ëlla";
	s.last_address = "192.168.1.20:24680";
	REQUIRE(s.save(file));
	gc::Settings back;
	REQUIRE(back.load(file));
	CHECK(back.view_distance == 192);
	CHECK(back.mouse_sensitivity == doctest::Approx(0.0052f));
	CHECK(back.max_fps == 60);
	CHECK_FALSE(back.vsync);
	CHECK_FALSE(back.smooth_lighting);
	CHECK(back.shadow_mode == "off");
	CHECK(back.render_scale == doctest::Approx(0.75f));
	CHECK(back.clock_24h);
	CHECK(back.master_volume == doctest::Approx(0.5f));
	CHECK(back.effects_volume == doctest::Approx(1.0f));
	CHECK(back.ambience_volume == doctest::Approx(0.25f));
	CHECK(back.key("jump") == 'J');
	CHECK(back.key("sneak") == VK_SHIFT);
	CHECK(back.disabled_mods == std::vector<std::string>{"lights", "boats"});
	CHECK(back.player_name == "Ëlla");
	CHECK(back.last_address == "192.168.1.20:24680");
	CHECK(back.actions_using('J') == std::vector<std::string>{"jump"});

	// Out of range, the wrong type, an unknown shadow mode (the Godot version's
	// "smooth"), a key that isn't one: those settings are the defaults.
	std::ofstream(file) << "return {view_distance = 100000, mouse_sensitivity = \"fast\", shadow_mode = \"smooth\", "
						   "render_scale = 0.01, keys = {jump = -3, sprint = 82}, master_volume = 7}\n";
	gc::Settings odd;
	REQUIRE(odd.load(file));
	CHECK(odd.view_distance == 512);
	CHECK(odd.mouse_sensitivity == doctest::Approx(0.003f));
	CHECK(odd.shadow_mode == "blocks");
	CHECK(odd.render_scale == doctest::Approx(0.25f));
	CHECK(odd.key("jump") == VK_SPACE);
	CHECK(odd.key("sprint") == 'R');
	CHECK(odd.master_volume == doctest::Approx(1.0f));
	// Not a settings file at all: the defaults, and it says so.
	std::ofstream(file) << "this isn't Lua";
	gc::Settings bad;
	std::string why;
	CHECK_FALSE(bad.load(file, &why));
	CHECK_FALSE(why.empty());
	CHECK(bad.view_distance == 128);
	std::filesystem::remove(file);
}
