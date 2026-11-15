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

namespace {

const gc::Recipe *find_recipe(gc::Game &game, const std::string &id) {
	for (const gc::Recipe &r : game.hand_recipes()) {
		if (r.id == id) {
			return &r;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("hand crafting: from logs to a pickaxe and a chest, as the core mod's recipes say") {
	Play p;
	gc::Game &g = *p.game;
	CHECK(g.hand_recipes().size() > 20);
	const gc::Recipe *planks = find_recipe(g, "core:log_to_planks");
	const gc::Recipe *sticks = find_recipe(g, "core:planks_to_sticks"); // shaped, from main.lua
	const gc::Recipe *pickaxe = find_recipe(g, "core:wood_pickaxe");
	const gc::Recipe *chest = find_recipe(g, "core:chest");
	REQUIRE(planks);
	REQUIRE(sticks);
	REQUIRE(pickaxe);
	REQUIRE(chest);
	CHECK(sticks->ingredients == std::vector<std::pair<std::string, int>>{{"core:planks", 2}});
	CHECK(sticks->count == 4);
	auto &inv = g.inventory();
	CHECK_FALSE(g.craft(*planks)); // no logs yet
	inv.add("core:log", 4);
	for (int i = 0; i < 4; i++) {
		CHECK(g.craft(*planks));
	}
	CHECK(inv.count_of("core:log") == 0);
	CHECK(inv.count_of("core:planks") == 16);
	CHECK(g.craft(*sticks));
	CHECK(inv.count_of("core:stick") == 4);
	CHECK(g.craft(*pickaxe));
	CHECK(inv.count_of("core:wood_pickaxe") == 1);
	CHECK(inv.count_of("core:planks") == 16 - 2 - 3);
	CHECK(inv.count_of("core:planks") == 11);
	CHECK(g.craft(*chest)); // 8 planks
	CHECK(inv.count_of("core:chest") == 1);
	CHECK(inv.count_of("core:planks") == 3);
	CHECK_FALSE(g.can_craft(*chest));
}

TEST_CASE("crafting into a full inventory changes nothing") {
	Play p;
	gc::Game &g = *p.game;
	auto &inv = g.inventory();
	for (gc::Stack &s : inv.slots) {
		s = {"core:dirt", 64};
	}
	inv.slots[0] = {"core:log", 1};
	const gc::Recipe *planks = find_recipe(g, "core:log_to_planks");
	REQUIRE(planks);
	// Taking the log frees its slot, so the planks fit: allowed.
	CHECK(g.craft(*planks));
	CHECK(inv.slots[0] == Stack{"core:planks", 4});
	// Now a truly full inventory: two logs (one stays in its slot), and room
	// for just one more plank of the four: refused, nothing lost.
	inv.slots[0] = {"core:dirt", 64};
	inv.slots[1] = {"core:log", 2};
	inv.slots[2] = {"core:planks", 63};
	const gc::Inventory before = inv;
	CHECK_FALSE(g.craft(*planks));
	CHECK(inv.slots == before.slots);
}

TEST_CASE("a chest: made with its block, holds items, saved in the Godot format, spills when broken") {
	Play p;
	const BlockPos at{3, 60, 3};
	REQUIRE(p.world->set_block(at, p.id("core:chest")));
	gc::BlockEntities &be = p.game->block_entities();
	REQUIRE(be.at(at));
	CHECK(be.at(at)->type == "chest");
	CHECK(be.at(at)->slots.size() == 27);
	CHECK(be.click(at, 0, {"core:coal", 5}).empty()); // put in
	CHECK(be.click(at, 0, {"core:coal", 3}).empty()); // stacked
	CHECK(be.at(at)->slots[0] == Stack{"core:coal", 8});
	CHECK(be.click(at, 4, {"core:dirt", 2}).empty());

	const Json saved = be.serialize();
	CHECK(saved["3,60,3"]["type"] == "chest");
	CHECK(saved["3,60,3"]["slots"][0] == Json::array({"core:coal", 8}));
	CHECK(saved["3,60,3"]["slots"][1].is_null());
	gc::BlockEntities copy(p.content, p.mods);
	copy.deserialize(saved);
	REQUIRE(copy.at(at));
	CHECK(copy.at(at)->slots[4] == Stack{"core:dirt", 2});

	p.game->break_block(at, "core:wood_axe");
	CHECK_FALSE(be.at(at));
	int coal = 0, dirt = 0, chests = 0;
	for (const auto &d : p.game->drops().drops()) {
		coal += d.item == "core:coal" ? d.count : 0;
		dirt += d.item == "core:dirt" ? d.count : 0;
		chests += d.item == "core:chest" ? d.count : 0;
	}
	CHECK(coal == 8);
	CHECK(dirt == 2);
	CHECK(chests == 1);
}

TEST_CASE("a furnace smelts with fuel, as the core mod's recipes say") {
	Play p;
	const BlockPos at{3, 60, 3};
	REQUIRE(p.world->set_block(at, p.id("core:furnace")));
	gc::BlockEntities &be = p.game->block_entities();
	using B = gc::BlockEntities;
	CHECK(be.fuel_seconds("core:coal") == 40);
	CHECK(be.smelting("core:iron_ore") == Stack{"core:iron_ingot", 1});
	CHECK_FALSE(be.smelting("core:dirt"));
	CHECK(be.click(at, B::FURNACE_FUEL, {"core:dirt", 1}) == Stack{"core:dirt", 1}); // not fuel: refused
	CHECK(be.click(at, B::FURNACE_OUTPUT, {"core:iron_ore", 1}) == Stack{"core:iron_ore", 1}); // output: take only
	CHECK(be.click(at, B::FURNACE_INPUT, {"core:iron_ore", 2}).empty());
	CHECK(be.click(at, B::FURNACE_FUEL, {"core:coal", 1}).empty());
	for (int i = 0; i < 44; i++) { // 11 s: two ingots (5 s each)
		be.update(0.25);
	}
	CHECK(be.at(at)->slots[B::FURNACE_OUTPUT] == Stack{"core:iron_ingot", 2});
	CHECK(be.at(at)->slots[B::FURNACE_INPUT].empty());
	CHECK(be.at(at)->slots[B::FURNACE_FUEL].empty()); // the coal is burning
	float burn, progress;
	B::furnace_status(*be.at(at), burn, progress);
	CHECK(burn > 0.5f);
	CHECK(progress == 0.0f);
}

TEST_CASE("right-clicking a container opens it; sneaking places against it") {
	Play p;
	const int h = 40;
	for (int z = -3; z <= 1; z++) {
		REQUIRE(p.world->set_block({0, h, z}, p.id("core:stone")));
		for (int y = h + 1; y <= h + 3; y++) {
			REQUIRE(p.world->set_block({0, y, z}, 0));
		}
	}
	REQUIRE(p.world->set_block({0, h + 1, -2}, p.id("core:chest")));
	p.player.position[0] = 0.5f;
	p.player.position[1] = float(h + 1);
	p.player.position[2] = 0.5f;
	p.player.yaw = 0;
	p.player.pitch = -0.42f; // at the chest's front face (just under its top edge)
	gc::GameInput in;
	in.captured = true;
	in.use_pressed = true;
	p.game->update(0.016f, in);
	const auto opened = p.game->take_opened();
	REQUIRE(opened);
	CHECK(*opened == BlockPos{0, h + 1, -2});
	p.game->inventory().slots[0] = {"core:dirt", 1};
	in.sneak = true;
	p.game->update(0.016f, in);
	CHECK_FALSE(p.game->take_opened());
	CHECK(p.block(0, h + 1, -1) == "core:dirt"); // placed against it
}

namespace {

// A pool of `fluid` (or air) 3 deep around x 0..2, z 0..2 at y 40..42, on stone.
void pool(Play &p, const char *fluid) {
	for (int x = -1; x <= 3; x++) {
		for (int z = -1; z <= 3; z++) {
			for (int y = 39; y <= 46; y++) {
				const bool wall = x < 0 || x > 2 || z < 0 || z > 2 || y == 39;
				REQUIRE(p.world->set_block({x, y, z}, wall ? p.id("core:stone") : (y <= 42 ? p.id(fluid) : 0)));
			}
		}
	}
}

void live(Play &p, float seconds) {
	gc::PlayerInput none;
	for (int i = 0; i < int(seconds * 60); i++) {
		if (!p.game->frozen()) {
			p.player.step(none, *p.world, *p.rules);
			p.game->after_move(gc::Player::STEP);
		}
	}
}

} // namespace

TEST_CASE("falls hurt past 3 blocks; healing comes back after a while") {
	Play p;
	pool(p, "core:air");
	p.player.position[0] = 1.5f;
	p.player.position[1] = 40.0f + 10.5f; // 10.5 blocks above the floor (y 40): floor(7.5) = 7
	// (A fall of exactly 10 measures 9.999: the player rests a hair above the floor.)
	p.player.position[2] = 1.5f;
	live(p, 0.05f); // (start falling from there)
	live(p, 3);
	CHECK(p.player.on_floor);
	CHECK(p.game->health() == gc::Game::MAX_HEALTH - 7); // floor(10.5 - 3)
	live(p, 4.0f + 2.5f * 3 + 0.1f); // 4 s, then a point every 2.5 s
	CHECK(p.game->health() == gc::Game::MAX_HEALTH - 4);
}

TEST_CASE("drowning starts after 10 seconds of breath; lava burns") {
	Play water;
	pool(water, "core:water");
	water.player.position[0] = 1.5f;
	water.player.position[1] = 40.0f;
	water.player.position[2] = 1.5f;
	live(water, 9);
	CHECK(water.game->health() == gc::Game::MAX_HEALTH);
	CHECK(water.game->breath() < 2);
	live(water, 4); // 3 s of no breath: 2 a second
	CHECK(water.game->health() < gc::Game::MAX_HEALTH - 2);

	Play lava;
	pool(lava, "core:lava");
	lava.player.position[0] = 1.5f;
	lava.player.position[1] = 40.0f;
	lava.player.position[2] = 1.5f;
	live(lava, 1.1f);
	CHECK(lava.game->health() < gc::Game::MAX_HEALTH - 5);
}

TEST_CASE("dying keeps the inventory; respawning is at the bed, or the world spawn without it") {
	Play p;
	pool(p, "core:air");
	p.game->inventory().slots[3] = {"core:coal", 9};
	p.game->set_world_spawn(Json::array({1.5, 40.0, 1.5}));
	// A bed on the pool's floor, set as the respawn point by right-clicking it.
	REQUIRE(p.world->set_block({1, 40, 0}, p.id("core:bed")));          // foot (facing north: head at z - 1)
	REQUIRE(p.world->set_block({1, 40, -1}, p.id("core:bed_n_head")));
	p.player.position[0] = 1.5f;
	p.player.position[1] = 41.0f;
	p.player.position[2] = 2.5f;
	p.player.yaw = 0;
	p.player.pitch = -std::atan2(1.62f + 0.45f, 2.0f); // at the bed's foot
	gc::GameInput in;
	in.captured = true;
	in.use_pressed = true;
	p.game->update(0.016f, in);
	CHECK(p.game->notice() == "Respawn point set");
	p.game->damage(100);
	CHECK(p.game->dead());
	CHECK(p.game->frozen());
	p.game->respawn();
	CHECK_FALSE(p.game->dead());
	CHECK(p.game->health() == gc::Game::MAX_HEALTH);
	CHECK(p.player.position[2] == doctest::Approx(-0.5)); // on the bed's head
	CHECK(p.game->inventory().slots[3] == Stack{"core:coal", 9});
	// The bed gone: back to the world spawn.
	REQUIRE(p.world->set_block({1, 40, -1}, 0));
	p.game->damage(100);
	p.game->respawn();
	CHECK(p.player.position[0] == doctest::Approx(1.5));
	CHECK(p.player.position[2] == doctest::Approx(1.5));
	CHECK(p.game->notice().find("bed was missing") != std::string::npos);
}

TEST_CASE("eating heals; food isn't eaten at full health") {
	Play p;
	p.game->inventory().slots[0] = {"core:bread", 2};
	gc::GameInput in;
	in.captured = true;
	in.use_pressed = true;
	p.player.pitch = 1.5f; // at the sky: nothing to place against
	p.game->update(0.016f, in);
	CHECK(p.game->inventory().slots[0].count == 2); // full health
	p.game->damage(10);
	p.game->update(0.016f, in);
	CHECK(p.game->inventory().slots[0].count == 1);
	CHECK(p.game->health() > gc::Game::MAX_HEALTH - 10);
}

TEST_CASE("a new player is put on dry ground (a biome's surface) and that becomes the world spawn") {
	Play p;
	p.player.position[0] = 0.5f;
	p.player.position[1] = 100.0f;
	p.player.position[2] = 0.5f;
	p.game->spawn_on_ground();
	CHECK(p.game->frozen());
	p.game->tick(0.016);
	CHECK_FALSE(p.game->frozen());
	REQUIRE(p.game->world_spawn());
	const int x = int(std::floor(p.player.position[0])), y = int(std::floor(p.player.position[1])), z = int(std::floor(p.player.position[2]));
	CHECK(p.block(x, y - 1, z) == "core:grass"); // (the vanilla biome's surface; seed 4242's origin is a cave)
	CHECK(p.block(x, y, z) != "core:water");
}
