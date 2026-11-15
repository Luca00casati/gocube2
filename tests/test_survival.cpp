// Survival: the inventory (and its Godot save format), the core mod's mining
// and drop rules, breaking with drops that get picked up, placing that uses
// up blocks, and starting items.

#include "content/content.h"
#include "game/game.h"
#include "game/inventory.h"
#include "mods/mods.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>

using gc::BlockPos;
using gc::Json;
using gc::Stack;

namespace {

// The base game loaded (its mods kept running: the rules are Lua functions)
// and a world (seed 4242) around the origin.
struct Play {
	gc::Content content;
	gc::Mods mods{content};
	gc::BlockTable table;
	std::unique_ptr<gc::BlockRules> rules;
	gocube::Generator generator;
	std::unique_ptr<gc::World> world;
	gc::Player player;
	std::unique_ptr<gc::Game> game;

	explicit Play(bool creative = false) {
		mods.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"});
		REQUIRE(mods.problems().empty());
		content.freeze();
		table = gc::build_block_table(content);
		rules = std::make_unique<gc::BlockRules>(content);
		gc::configure_generator(generator, content, 4242, gc::GENERATOR_VERSION);
		world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator);
		world->set_view_distance(32);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!(world->is_loaded(0, 0, 0) && world->stats().queued == 0)) {
			world->update(0.5f, 10.0f, 0.5f);
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		mods.attach(world.get(), rules.get());
		game = std::make_unique<gc::Game>(content, table, *rules, *world, mods, player, creative);
		game->start_new_player();
	}
	~Play() { mods.attach(nullptr, nullptr); }
	gocube::BlockId id(const char *name) const { return gocube::BlockId(content.block_index(name)); }
	std::string block(int x, int y, int z) const { return content.block_ids()[world->block_at(x, y, z)]; }
};

} // namespace

TEST_CASE("inventory: stacks, limits, removing, mouse moves") {
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"});
	gc::Inventory inv(content);
	CHECK(inv.add("core:dirt", 100) == 0); // 64 + 36
	CHECK(inv.slots[0] == Stack{"core:dirt", 64});
	CHECK(inv.slots[1] == Stack{"core:dirt", 36});
	CHECK(inv.add("core:wood_pickaxe", 2) == 0); // max_stack 1: two slots
	CHECK(inv.slots[2] == Stack{"core:wood_pickaxe", 1});
	CHECK(inv.slots[3] == Stack{"core:wood_pickaxe", 1});
	CHECK(inv.count_of("core:dirt") == 100);
	CHECK_FALSE(inv.remove("core:dirt", 101));
	CHECK(inv.remove("core:dirt", 40)); // from the back first
	CHECK(inv.slots[0].count == 60);
	CHECK(inv.slots[1].empty());
	inv.click(0); // pick up the 60 dirt
	CHECK(inv.cursor == Stack{"core:dirt", 60});
	inv.click(5); // put it down elsewhere
	CHECK(inv.slots[5] == Stack{"core:dirt", 60});
	CHECK(inv.add("core:stone", 64 * 36) > 0); // full
	CHECK_FALSE(inv.can_fit("core:glass"));
}

TEST_CASE("inventory: the Godot version's save format, both ways") {
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"});
	// As the Godot version's Inventory.serialize writes it.
	Json saved = Json::array();
	saved.push_back(Json::array({"core:torch", 5}));
	saved.push_back(nullptr);
	saved.push_back(Json::array({"gone:item", 3})); // its mod isn't here: dropped
	for (int i = 3; i < 36; i++) {
		saved.push_back(nullptr);
	}
	saved.push_back({{"armor", {"core:iron_helmet", "", "", ""}}});
	gc::Inventory inv(content);
	inv.deserialize(saved);
	CHECK(inv.slots[0] == Stack{"core:torch", 5});
	CHECK(inv.slots[2].empty());
	CHECK(inv.armor[0] == Stack{"core:iron_helmet", 1});
	const Json again = inv.serialize();
	CHECK(again.size() == 37);
	CHECK(again[0] == Json::array({"core:torch", 5}));
	CHECK(again[2].is_null());
	CHECK(again[36]["armor"][0] == "core:iron_helmet");
}

TEST_CASE("the core mod's rules: mining time and drops") {
	Play p;
	CHECK(p.game->mine_seconds("core:stone", "") == doctest::Approx(1.5 * 0.75 * 3)); // needs a pickaxe
	CHECK(p.game->mine_seconds("core:stone", "core:wood_pickaxe") == doctest::Approx(1.5 * 0.75 / 2));
	CHECK(p.game->mine_seconds("core:dirt", "") == doctest::Approx(0.5 * 0.75));
	CHECK(p.game->drops_of("core:stone", "").empty());
	CHECK(p.game->drops_of("core:stone", "core:wood_pickaxe") == std::vector<std::string>{"core:cobblestone"});
	CHECK(p.game->drops_of("core:grass", "") == std::vector<std::string>{"core:dirt"});
	CHECK(p.game->drops_of("core:log", "") == std::vector<std::string>{"core:log"});
	CHECK(p.game->drops_of("core:wood_door_n_upper", "") == std::vector<std::string>{"core:wood_door"}); // a variant
	int seeds = 0;
	for (int i = 0; i < 400; i++) { // tall grass: nothing, sometimes a seed (15%)
		for (const std::string &d : p.game->drops_of("core:tall_grass", "")) {
			CHECK(d == "core:seeds");
			seeds++;
		}
	}
	CHECK(seeds > 20);
	CHECK(seeds < 110);
}

TEST_CASE("survival: a new player has the starting items; breaking drops items that get picked up") {
	Play p;
	CHECK(p.game->inventory().slots[0] == Stack{"core:torch", 1});
	// A dirt block next to the player, broken by hand.
	const BlockPos at{2, 60, 0};
	REQUIRE(p.world->set_block(at, p.id("core:dirt")));
	p.player.position[0] = 0.5f;
	p.player.position[1] = 59.0f;
	p.player.position[2] = 0.5f;
	p.player.flying = true;
	p.game->break_block(at, "");
	CHECK(p.block(2, 60, 0) == "core:air");
	REQUIRE(p.game->drops().drops().size() == 1);
	CHECK(p.game->drops().drops()[0].item == "core:dirt");
	for (int i = 0; i < 120; i++) { // two seconds: it pops out, then drifts in
		p.game->physics_step(1.0f / 60.0f);
	}
	CHECK(p.game->drops().drops().empty());
	CHECK(p.game->inventory().count_of("core:dirt") == 1);
}

TEST_CASE("survival: placing uses the block up; creative doesn't, and drops nothing") {
	for (const bool creative : {false, true}) {
		CAPTURE(creative);
		Play p(creative);
		auto &inv = p.game->inventory();
		inv.slots[0] = {"core:planks", 2};
		// Stand on a stone floor and place against it, looking down ahead.
		const int h = 40;
		for (int x = -1; x <= 1; x++) {
			for (int z = -3; z <= 1; z++) {
				REQUIRE(p.world->set_block({x, h, z}, p.id("core:stone")));
				for (int y = h + 1; y <= h + 3; y++) {
					REQUIRE(p.world->set_block({x, y, z}, 0));
				}
			}
		}
		p.player.position[0] = 0.5f;
		p.player.position[1] = float(h + 1);
		p.player.position[2] = 0.5f;
		p.player.yaw = 0;
		p.player.pitch = -std::atan2(1.62f, 2.0f);
		gc::GameInput in;
		in.captured = true;
		in.use_pressed = true;
		p.game->update(0.016f, in);
		CHECK(p.block(0, h + 1, -2) == "core:planks");
		CHECK(inv.slots[0].count == (creative ? 2 : 1));
		// Break it: survival mines over time and drops it, creative breaks at once.
		gc::GameInput hold;
		hold.captured = true;
		hold.break_held = true;
		hold.break_pressed = true;
		p.game->update(0.016f, hold);
		hold.break_pressed = false;
		for (int i = 0; i < 200 && p.block(0, h + 1, -2) != "core:air"; i++) {
			p.game->update(0.016f, hold);
		}
		CHECK(p.block(0, h + 1, -2) == "core:air");
		CHECK(p.game->drops().drops().size() == (creative ? 0u : 1u));
	}
}

TEST_CASE("the player's inventory and hotbar slot are saved in world.json's player") {
	Play p;
	p.game->inventory().slots[4] = {"core:coal", 7};
	gc::GameInput in;
	in.captured = true;
	in.number_key = 5;
	p.game->update(0.016f, in);
	const Json saved = p.game->save_player();
	CHECK(saved["selected"] == 4);
	CHECK(saved["inventory"][4] == Json::array({"core:coal", 7}));
	Play q;
	q.game->load_player(saved);
	CHECK(q.game->selected() == 4);
	CHECK(q.game->inventory().slots[4] == Stack{"core:coal", 7});
	CHECK(q.game->inventory().slots[0] == Stack{"core:torch", 1});
}
