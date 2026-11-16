// A world's folder: world.lua (a Lua data table) and chunk files. A world is
// made, edited and saved, and opens again with its edits and every field it
// had; worlds that can't be read safely (damaged, newer, the old world.json)
// are left alone.

#include "content/content.h"
#include "data/lua_data.h"
#include "helpers.h"
#include "world/save.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace fs = std::filesystem;
using gc::BlockPos;
using gc::Json;

namespace {

fs::path fresh_dir(const char *name) {
	const fs::path dir = fs::temp_directory_path() / ("gocube_test_" + std::to_string(GetCurrentProcessId())) / name;
	fs::remove_all(dir);
	fs::create_directories(dir.parent_path());
	return dir;
}

Json read_world(const fs::path &dir) {
	Json meta;
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
	const Json inventory = Json::array({Json::array({"core:torch", 1}), false, Json::array({"core:dirt", 64}),
			Json::object({{"armor", Json::array({"", "", "", ""})}})});
	const Json chests = {{"1,2,3", {{"type", "chest"}, {"slots", Json::array({false, Json::array({"core:stick", 4})})}}}};
	std::vector<std::string> ids;
	{
		OpenWorld w(dir);
		ids = w.content.block_ids();
		CHECK(w.world->set_block({3, 5, 3}, w.id("core:air")));
		CHECK(w.world->set_block({1, 2, 1}, w.id("core:glass")));
		CHECK(w.world->set_block({-40, 0, 3}, w.id("core:planks"))); // another chunk
		CHECK(w.world->save_modified() == 2);
		w.save.set_field("block_entities", chests);
		w.save.set_field("plants", Json::array({Json::array({1, 41, 1, 12.5})}));
		w.save.set_field("time", 0.375);
		w.save.set_field("from_a_newer_mod", {{"keep", "me"}, {"empty", Json::object()}});
		Json player = Json::object();
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
	const Json meta = read_world(dir);
	CHECK(meta["player"]["position"][0].get<double>() == doctest::Approx(1.5));
	CHECK(meta["player"]["flying"] == true);
	CHECK(meta["player"]["health"] == 17);
	CHECK(meta["player"]["inventory"] == inventory);
	CHECK(nlohmann::json::parse(meta["block_entities"].dump()) == nlohmann::json::parse(chests.dump())); // (order-free)
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
	Json meta = read_world(dir);
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
	const Json meta = read_world(dir);
	CHECK(meta["block_ids"] == Json::array({"core:air", "core:stone"}));
	CHECK(meta["format"] == gc::WorldSave::FORMAT_VERSION);
	CHECK(fs::is_directory(dir / "chunks"));
}

TEST_CASE("Lua data files: written and read back the same; only data") {
	const Json data = {
			{"int", 42}, {"negative", -7}, {"float", 0.1}, {"whole_float", 2.0}, {"big", 4294967295LL},
			{"text", "quote \" backslash \\ newline \n tab \t bell \a café"}, {"yes", true}, {"no", false},
			{"list", Json::array({1, "two", Json::array({3.5, false})})},
			{"needs brackets", 1}, {"end", "a keyword as a key"}, {"9lives", 9},
			{"nested", {{"deeper", {{"deepest", Json::array({"x"})}}}}},
	};
	const std::string text = gc::write_lua_data(data);
	CHECK(text.rfind("return {", 0) == 0);
	Json back;
	std::string why;
	REQUIRE_MESSAGE(gc::parse_lua_data(text, "test", back, why), why);
	// (Order-free: a Lua table's keys come back in its own order.)
	CHECK(nlohmann::json::parse(back.dump()) == nlohmann::json::parse(data.dump()));
	CHECK(back["float"].is_number_float());
	CHECK(back["whole_float"].is_number_float());
	CHECK(back["int"].is_number_integer());
	// A list can't hold nil.
	CHECK_THROWS(gc::write_lua_data(Json::array({1, nullptr, 3})));
	// Not data: functions, code that calls anything, a script that never ends.
	CHECK_FALSE(gc::parse_lua_data("return {f = function() end}", "t", back, why));
	CHECK_FALSE(gc::parse_lua_data("print('hi') return {}", "t", back, why));
	CHECK_FALSE(gc::parse_lua_data("while true do end", "t", back, why));
	CHECK(why.find("too long") != std::string::npos);
	CHECK_FALSE(gc::parse_lua_data("return 5", "t", back, why));
	CHECK_FALSE(gc::parse_lua_data("\x1bLua", "t", back, why)); // bytecode
}
