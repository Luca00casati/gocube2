// A world saved by the Godot version opens, keeps its edits, can be edited and
// saved again, and keeps what this version doesn't use yet (inventory, block
// entities, plants). The fixture tests/data/godot_world was made by the Godot
// version (tests/data/make_godot_world.gd): its edits are in expected.json.
//
// GOCUBE_TEST_WORLD_OUT=<dir>: also leaves the world edited here in <dir>,
// to check that the Godot version opens it.

#include "content/content.h"
#include "world/save.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace fs = std::filesystem;
using gc::BlockPos;
using gc::Json;

namespace {

const fs::path FIXTURE = fs::path(GOCUBE_SOURCE_DIR) / "tests" / "data" / "godot_world";

fs::path fresh_copy(const char *name) {
	const fs::path dir = fs::temp_directory_path() / ("gocube_test_" + std::to_string(GetCurrentProcessId())) / name;
	fs::remove_all(dir);
	fs::create_directories(dir.parent_path());
	fs::copy(FIXTURE, dir, fs::copy_options::recursive);
	fs::remove(dir / "expected.json");
	return dir;
}

Json read_json(const fs::path &p) {
	std::ifstream f(p, std::ios::binary);
	return Json::parse(f);
}

std::string utf8(const fs::path &p) {
	const std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

// A world opened from a folder, with its chunks around the origin loaded.
struct OpenWorld {
	gc::WorldSave save;
	gc::Content content;
	gc::BlockTable table;
	gocube::Generator generator;
	std::unique_ptr<gc::World> world;

	explicit OpenWorld(const fs::path &dir) {
		REQUIRE(content.load_mod(fs::path(GOCUBE_SOURCE_DIR) / "mods" / "core") == "");
		REQUIRE(save.open(dir, 1) == "");
		content.freeze(save.block_ids());
		save.set_block_ids(content.block_ids());
		table = gc::build_block_table(content);
		gc::configure_generator(generator, content, save.seed(), save.generator());
		world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator, utf8(save.chunks_dir()));
		world->set_view_distance(32);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!(world->is_loaded(-21, 0, 9) && world->is_loaded(6, 0, 3) && world->stats().queued == 0)) {
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

TEST_CASE("a world saved by the Godot version opens with its edits") {
	const fs::path dir = fresh_copy("open");
	OpenWorld w(dir);
	CHECK(w.save.seed() == 4242);
	CHECK(w.save.generator() == 3);
	CHECK(w.content.block_ids() == read_json(FIXTURE / "world.json")["block_ids"].get<std::vector<std::string>>());
	const Json expected = read_json(FIXTURE / "expected.json");
	REQUIRE(expected["edits"].size() == 9);
	for (const Json &e : expected["edits"]) {
		const int x = e["pos"][0], y = e["pos"][1], z = e["pos"][2];
		INFO("at ", x, ", ", y, ", ", z);
		CHECK(w.block(x, y, z) == e["id"].get<std::string>());
	}
	// The saved player: feet position, [pitch, yaw].
	CHECK(w.save.player()["rotation"][1].get<double>() == doctest::Approx(0.75));
}

TEST_CASE("it can be edited and saved again, and opens with both its edits and ours") {
	const fs::path dir = fresh_copy("edit");
	{
		OpenWorld w(dir);
		CHECK(w.world->set_block({3, 5, 3}, w.id("core:air")));      // the top of the Godot pillar
		CHECK(w.world->set_block({1, 2, 1}, w.id("core:glass")));    // in the same chunk
		CHECK(w.world->set_block({-40, 0, 3}, w.id("core:planks"))); // in a chunk the Godot version didn't edit
		CHECK(w.world->stats().dirty == 2);
		CHECK(w.world->save_modified() == 2);
		Json player = Json::object();
		player["position"] = {1.5, 3.0, 1.5};
		player["rotation"] = {0.1, -1.0};
		player["flying"] = true;
		CHECK(w.save.save(player));
	}
	CHECK(fs::exists(dir / "chunks" / "c.-2.0.0.bin"));

	OpenWorld w(dir);
	CHECK(w.block(3, 5, 3) == "core:air");
	CHECK(w.block(3, 4, 3) == "core:stone"); // the Godot version's edits are still there
	CHECK(w.block(1, 2, 1) == "core:glass");
	CHECK(w.block(-40, 0, 3) == "core:planks");
	CHECK(w.block(-20, 0, 9) == "core:glass");
	CHECK(w.block(6, 4, 3) == "core:wood_door_n_upper");

	// world.json: our player fields replaced, the rest kept as the Godot version wrote it.
	const Json meta = read_json(dir / "world.json");
	const Json original = read_json(FIXTURE / "world.json");
	CHECK(meta["player"]["position"][0].get<double>() == doctest::Approx(1.5));
	CHECK(meta["player"]["flying"] == true);
	CHECK(meta["player"]["inventory"] == original["player"]["inventory"]);
	CHECK(meta["player"]["selected"] == original["player"]["selected"]);
	for (const char *key : {"block_ids", "block_entities", "plants", "seed", "created", "first_night", "mode", "generator"}) {
		INFO(key);
		CHECK(meta[key] == original[key]);
	}
	CHECK(meta["format"] == 2);

	if (const char *out = std::getenv("GOCUBE_TEST_WORLD_OUT")) {
		w.world->save_modified();
		fs::remove_all(out);
		fs::copy(dir, out, fs::copy_options::recursive);
	}
}

TEST_CASE("worlds this version can't read safely are left alone") {
	const fs::path dir = fresh_copy("refused");
	gc::WorldSave save;
	Json meta = read_json(dir / "world.json");
	meta["format"] = 3;
	std::ofstream(dir / "world.json", std::ios::binary | std::ios::trunc) << meta.dump();
	CHECK(save.open(dir, 1).find("newer version") != std::string::npos);
	std::ofstream(dir / "world.json", std::ios::binary | std::ios::trunc) << "{ broken";
	CHECK(save.open(dir, 1).find("damaged") != std::string::npos);
	CHECK(read_json(FIXTURE / "world.json")["format"] == 2); // (the fixture itself untouched)
}

TEST_CASE("a new world gets a seed, the newest generator and the block table") {
	const fs::path dir = fs::temp_directory_path() / ("gocube_test_" + std::to_string(GetCurrentProcessId())) / "new";
	fs::remove_all(dir);
	gc::WorldSave save;
	REQUIRE(save.open(dir, 99) == "");
	CHECK(save.seed() == 99);
	CHECK(save.generator() == gc::GENERATOR_VERSION);
	CHECK(save.block_ids().empty());
	save.set_block_ids({"core:air", "core:stone"});
	const Json meta = read_json(dir / "world.json");
	CHECK(meta["block_ids"].size() == 2);
	CHECK(meta["format"] == 2);
	CHECK(fs::is_directory(dir / "chunks"));
}
