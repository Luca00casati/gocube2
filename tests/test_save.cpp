// A world's folder: world.json (a data table) and chunk files. A world is
// made, edited and saved, and opens again with its edits and every field it
// had; worlds that can't be read safely (damaged, newer, the old world.json)
// are left alone.

#include "content/content.h"
#include "data/json.h"
#include "helpers.h"
#include "world/save.h"
#include "world/world.h"

#include "game/settings.h"

#include "check.h"

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
	REQUIRE_MESSAGE(gc::read_json_file(dir / "world.json", meta, why), why);
	return meta;
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
		gc::configure_generator(generator, content, save.seed());
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
	// Everything the game keeps, including lists with empty slots (false, as
	// worlds have always saved them) and an empty object.
	const Data inventory = Data::array({Data::array({"core:torch", 1}), false, Data::array({"core:dirt", 64}),
			Data::object({{"armor", Data::array({"", "", "", ""})}})});
	const Data chests = {{"1,2,3", {{"type", "chest"}, {"slots", Data::array({false, Data::array({"core:coal", 4})})}}}};
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
	CHECK(fs::exists(dir / "world.json"));
	CHECK(fs::exists(dir / "chunks" / "c.-2.0.0.bin"));

	OpenWorld w(dir);
	CHECK(w.save.seed() == 4242);
	CHECK(w.save.mode() == "survival");
	CHECK(w.content.block_ids() == ids);
	CHECK(w.block(3, 5, 3) == "core:air");
	CHECK(w.block(1, 2, 1) == "core:glass");
	CHECK(w.block(-40, 0, 3) == "core:planks");
	const Data meta = read_world(dir);
	CHECK(meta["player"]["position"][0].get<double>() == check::Approx(1.5));
	CHECK(meta["player"]["flying"] == true);
	CHECK(meta["player"]["health"] == 17);
	CHECK(meta["player"]["inventory"] == inventory);
	CHECK(meta["block_entities"] == chests); // (order-free)
	CHECK(meta["plants"][0][3].get<double>() == check::Approx(12.5));
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
	write(dir / "world.json", gc::write_json(meta));
	gc::WorldSave save;
	CHECK(save.open(dir, 1).find("newer version") != std::string::npos);
	write(dir / "world.json", R"({"seed": 1,)");
	CHECK(save.open(dir, 1).find("damaged") != std::string::npos);
	// A world.json is data: code in it can't run.
	write(dir / "world.json", "os.remove('x') return {seed = 1}");
	CHECK(save.open(dir, 1).find("damaged") != std::string::npos);
	CHECK(fs::exists(dir / "world.json")); // (left as it is)
	// The Godot version's worlds (formats 1 and 2) can't be opened.
	write(dir / "world.json", R"({"seed": 1, "format": 2})");
	CHECK(save.open(dir, 1).find("old version") != std::string::npos);
}

TEST_CASE("a new world gets a seed and the block table") {
	const fs::path dir = fresh_dir("new");
	gc::WorldSave save;
	REQUIRE(save.open(dir, 99) == "");
	CHECK(save.seed() == 99);
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

TEST_CASE("JSON: written and read back the same; only data") {
	const Data data = {
			{"int", 42}, {"negative", -7}, {"float", 0.1}, {"whole_float", 2.0}, {"big", 4294967295LL},
			{"text", "quote \" backslash \\ newline \n tab \t bell \a café"}, {"yes", true}, {"no", false},
			{"nothing", nullptr}, {"list", Data::array({1, "two", nullptr, Data::array({3.5, false})})},
			{"needs quotes", 1}, {"end", "a keyword as a key"}, {"9lives", 9}, {"empty list", Data::array()},
			{"empty object", Data::object()},
			{"nested", {{"deeper", {{"deepest", Data::array({"x"})}}}}},
	};
	for (bool compact : {false, true}) {
		const std::string text = gc::write_json(data, compact);
		CHECK(text.rfind("{", 0) == 0);
		CHECK((text.find('\n') == std::string::npos) == compact);
		Data back;
		std::string why;
		REQUIRE_MESSAGE(gc::parse_json(text, "test", back, why), why);
		CHECK(back == data); // (order-free: written sorted)
		CHECK(back["float"].is_number_float());
		CHECK(back["whole_float"].is_number_float());
		CHECK(back["int"].is_number_integer());
		CHECK(back["empty list"].is_array());
		CHECK(back["empty object"].is_object());
		CHECK(back["list"][2].is_null());
	}
	CHECK(gc::write_json(Data::array({1, 2}), true) == "[1,2]");
	CHECK_THROWS(gc::write_json(Data(std::nan(""))));
	// Not JSON: code, comments, trailing commas, single quotes.
	Data back;
	std::string why;
	CHECK_FALSE(gc::parse_json("return {a = 1}", "t", back, why));
	CHECK_FALSE(gc::parse_json("print('hi') return {}", "t", back, why));
	CHECK_FALSE(gc::parse_json("{\"a\": 1} // note", "t", back, why));
	CHECK_FALSE(gc::parse_json("[1, 2,]", "t", back, why));
	CHECK_FALSE(gc::parse_json("{'a': 1}", "t", back, why));
	CHECK_FALSE(gc::parse_json("{\"a\": x}", "t", back, why));
	CHECK(why.find("t:1:") == 0); // (where)
	CHECK_FALSE(gc::parse_json("{\n\n  \"a\": \"open}", "t", back, why));
	CHECK(why.find("t:3:") == 0);
}

TEST_CASE("JSON: what it allows and refuses") {
	Data d;
	std::string why;
	REQUIRE_MESSAGE(gc::parse_json("\xEF\xBB\xBF" R"( {
	"a": 1, "b": -0.5e1, "c": 1E2, "big": 9223372036854775808,
	"esc": "\"\\\/\b\f\n\r\t\u0041\u00e9\ud83d\ude00",
	"twice": 1, "twice": 2,
	"list": [10, 20, 30], "none": null, "empty": {}, "nothing": []
})", "t", d, why), why);
	CHECK(d["a"] == 1);
	CHECK(d["b"] == -5.0);
	CHECK(d["c"].is_number_float());
	CHECK(d["big"].is_number_float()); // (too large for an integer)
	CHECK(d["esc"] == "\"\\/\b\f\n\r\tA\u00e9\U0001F600");
	CHECK(d["twice"] == 2); // (the last wins)
	CHECK(d["list"] == Data::array({10, 20, 30}));
	CHECK(d.contains("none"));
	CHECK(d["none"].is_null());
	CHECK(d["empty"].is_object());
	CHECK(d["nothing"].is_array());
	// Fields keep the order they're written in.
	CHECK(d.items()[0].first == "a");
	CHECK(d.items()[1].first == "b");
	// Any value at the top.
	CHECK(gc::parse_json(" 5 ", "t", d, why));
	CHECK(d == 5);
	// Not JSON.
	for (const char *bad : {"", "{", "[1 2]", "{\"a\" 1}", "{1: 2}", "01", "1.", ".5", "-", "+1", "1e", "0x10",
				 "\"\\q\"", "\"\\u12\"", "\"\\ud800\"", "\"tab\there\"", "tru", "nul", "1e999", "NaN",
				 "{\"a\": 1} {}"}) {
		CAPTURE(bad);
		CHECK_FALSE(gc::parse_json(bad, "t", d, why));
	}
	// Nested too deeply: refused (no stack overflow, whatever the input).
	CHECK_FALSE(gc::parse_json(std::string(100000, '['), "t", d, why));
	CHECK(why.find("too deeply") != std::string::npos);
	const std::string ok = std::string(size_t(gc::MAX_DATA_DEPTH) + 1, '[') + std::string(size_t(gc::MAX_DATA_DEPTH) + 1, ']');
	CHECK(gc::parse_json(ok, "t", d, why));
}

TEST_CASE("settings: saved and read back; what's missing or wrong is the default") {
	const std::filesystem::path file =
			std::filesystem::temp_directory_path() / ("gocube_settings_" + std::to_string(GetCurrentProcessId()) + ".json");
	gc::Settings s;
	CHECK(s.view_distance == 128);
	CHECK(s.key("move_forward") == 'W');
	CHECK(s.load(file)); // (no file: the defaults)
	s.view_distance = 192;
	s.mouse_sensitivity = 0.0052f;
	s.max_fps = 60;
	s.vsync = false;
	CHECK_FALSE(s.server_mod_code); // (by default: no code from servers)
	s.server_mod_code = true;
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
	CHECK(back.mouse_sensitivity == check::Approx(0.0052f));
	CHECK(back.max_fps == 60);
	CHECK_FALSE(back.vsync);
	CHECK(back.server_mod_code);
	CHECK_FALSE(back.smooth_lighting);
	CHECK(back.shadow_mode == "off");
	CHECK(back.render_scale == check::Approx(0.75f));
	CHECK(back.clock_24h);
	CHECK(back.master_volume == check::Approx(0.5f));
	CHECK(back.effects_volume == check::Approx(1.0f));
	CHECK(back.ambience_volume == check::Approx(0.25f));
	CHECK(back.key("jump") == 'J');
	CHECK(back.key("sneak") == VK_SHIFT);
	CHECK(back.disabled_mods == std::vector<std::string>{"lights", "boats"});
	CHECK(back.player_name == "Ëlla");
	CHECK(back.last_address == "192.168.1.20:24680");
	CHECK(back.actions_using('J') == std::vector<std::string>{"jump"});

	// Out of range, the wrong type, an unknown shadow mode, a key that isn't
	// one: those settings are the defaults.
	std::ofstream(file) << R"({"view_distance": 100000, "mouse_sensitivity": "fast", "shadow_mode": "smooth", )"
						   R"("render_scale": 0.01, "keys": {"jump": -3, "sprint": 82}, "master_volume": 7})" << "\n";
	gc::Settings odd;
	REQUIRE(odd.load(file));
	CHECK(odd.view_distance == 512);
	CHECK(odd.mouse_sensitivity == check::Approx(0.003f));
	CHECK(odd.shadow_mode == "blocks");
	CHECK(odd.render_scale == check::Approx(0.25f));
	CHECK(odd.key("jump") == VK_SPACE);
	CHECK(odd.key("sprint") == 'R');
	CHECK(odd.master_volume == check::Approx(1.0f));
	// Not a settings file at all: the defaults, and it says so.
	std::ofstream(file) << "this isn't JSON";
	gc::Settings bad;
	std::string why;
	CHECK_FALSE(bad.load(file, &why));
	CHECK_FALSE(why.empty());
	CHECK(bad.view_distance == 128);
	std::filesystem::remove(file);

}
