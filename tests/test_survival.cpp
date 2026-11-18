// Survival: the inventory, the core mod's mining
// and drop rules, breaking with drops that get picked up, placing that uses
// up blocks, and starting items.

#include "content/content.h"
#include "game/game.h"
#include "game/inventory.h"
#include "mods/mods.h"
#include "world/world.h"
#include "play.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>

using gc::BlockPos;
using gc::Data;
using gc::Stack;

namespace {

// The kit test mod (tests/data/kit_mods): crafting, smelting, a tool and
// armour, which the base game no longer has, after the base game.
const std::filesystem::path KIT_MODS = std::filesystem::path(GOCUBE_SOURCE_DIR) / "tests" / "data" / "kit_mods";

Play::Setup with_kit() {
	Play::Setup s;
	s.more_mods = {KIT_MODS};
	return s;
}

} // namespace

TEST_CASE("inventory: stacks, limits, removing, mouse moves") {
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"});
	gc::Inventory inv(content);
	CHECK(inv.add("core:dirt", 100) == 0); // 64 + 36
	CHECK(inv.slots[0] == Stack{"core:dirt", 64});
	CHECK(inv.slots[1] == Stack{"core:dirt", 36});
	CHECK(inv.add("core:snowball", 20) == 0); // max_stack 16: two slots
	CHECK(inv.slots[2] == Stack{"core:snowball", 16});
	CHECK(inv.slots[3] == Stack{"core:snowball", 4});
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

TEST_CASE("inventory: its save format, both ways (false for an empty slot)") {
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods", KIT_MODS}); // (armour: the kit's)
	// As Inventory.serialize writes it (false for an empty slot: a Lua list
	// can't hold nil).
	Data saved = Data::array();
	saved.push_back(Data::array({"core:torch", 5}));
	saved.push_back(false);
	saved.push_back(Data::array({"gone:item", 3})); // its mod isn't here: dropped
	for (int i = 3; i < 36; i++) {
		saved.push_back(false);
	}
	saved.push_back({{"armor", {"kit:iron_helmet", "", "", ""}}});
	gc::Inventory inv(content);
	inv.deserialize(saved);
	CHECK(inv.slots[0] == Stack{"core:torch", 5});
	CHECK(inv.slots[2].empty());
	CHECK(inv.armor[0] == Stack{"kit:iron_helmet", 1});
	const Data again = inv.serialize();
	CHECK(again.size() == 37);
	CHECK(again[0] == Data::array({"core:torch", 5}));
	CHECK(again[2] == false);
	CHECK(again[36]["armor"][0] == "kit:iron_helmet");
}

TEST_CASE("the core mod's rules: mining time and drops (by hand: there are no tools)") {
	Play p;
	CHECK(p.game->mine_seconds("core:stone", "") == doctest::Approx(1.5 * 0.375)); // hardness x 0.375 s
	CHECK(p.game->mine_seconds("core:dirt", "") == doctest::Approx(0.5 * 0.375));
	CHECK(p.game->mine_seconds("core:stone", "core:torch") == doctest::Approx(1.5 * 0.375)); // whatever is held
	CHECK(p.game->mine_seconds("core:torch", "") == doctest::Approx(0.1)); // the least
	CHECK(p.game->drops_of("core:stone", "") == std::vector<std::string>{"core:cobblestone"});
	CHECK(p.game->drops_of("core:iron_ore", "") == std::vector<std::string>{"core:iron_ore"});
	CHECK(p.game->drops_of("core:coal_ore", "") == std::vector<std::string>{"core:coal"});
	CHECK(p.game->drops_of("core:furnace", "") == std::vector<std::string>{"core:furnace"});
	CHECK(p.game->drops_of("core:glass", "").empty());
	CHECK(p.game->drops_of("core:grass", "") == std::vector<std::string>{"core:dirt"});
	CHECK(p.game->drops_of("core:log", "") == std::vector<std::string>{"core:log"});
	CHECK(p.game->drops_of("core:wood_door_n_upper", "") == std::vector<std::string>{"core:wood_door"}); // a variant
	for (int i = 0; i < 100; i++) { // leaves: themselves only (no saplings)
		CHECK(p.game->drops_of("core:leaves", "") == std::vector<std::string>{"core:leaves"});
	}
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
	const Data saved = p.game->save_player();
	CHECK(saved["selected"] == 4);
	CHECK(saved["inventory"][4] == Data::array({"core:coal", 7}));
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

TEST_CASE("the base game's only crafting is planks and the dye vat; no smelting, tools, weapons or armour") {
	Play p;
	REQUIRE(p.game->hand_recipes().size() == 2);
	CHECK(p.game->hand_recipes()[0].output == "core:planks");
	CHECK(p.game->hand_recipes()[1].output == "core:dye_vat");
	CHECK(p.content.recipes.empty());
	CHECK_FALSE(p.game->block_entities().smelting("core:iron_ore"));
	CHECK(p.game->block_entities().fuel_seconds("core:coal") == 0);
	for (const auto &[id, def] : p.content.items) {
		CAPTURE(id);
		CHECK_FALSE(def.contains("tool"));
		CHECK_FALSE(def.contains("armor"));
		CHECK_FALSE(def.contains("damage"));
	}
	// A furnace is a plain block now; a chest still holds things.
	const BlockPos at{3, 60, 3};
	REQUIRE(p.world->set_block(at, p.id("core:furnace")));
	CHECK_FALSE(p.game->block_entities().at(at));
	REQUIRE(p.world->set_block(at, p.id("core:chest")));
	CHECK(p.game->block_entities().at(at));
}

TEST_CASE("hand crafting (a mod's recipes): from logs to a pickaxe and a chest") {
	Play p(with_kit());
	gc::Game &g = *p.game;
	CHECK(g.hand_recipes().size() == 4);
	const gc::Recipe *planks = find_recipe(g, "kit:log_to_planks");
	const gc::Recipe *sticks = find_recipe(g, "kit:planks_to_sticks"); // shaped
	const gc::Recipe *pickaxe = find_recipe(g, "kit:wood_pickaxe");
	const gc::Recipe *chest = find_recipe(g, "kit:chest");
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
	CHECK(inv.count_of("kit:stick") == 4);
	CHECK(g.craft(*pickaxe));
	CHECK(inv.count_of("kit:wood_pickaxe") == 1);
	CHECK(inv.count_of("core:planks") == 16 - 2 - 3);
	CHECK(inv.count_of("core:planks") == 11);
	CHECK(g.craft(*chest)); // 8 planks
	CHECK(inv.count_of("core:chest") == 1);
	CHECK(inv.count_of("core:planks") == 3);
	CHECK_FALSE(g.can_craft(*chest));
}

TEST_CASE("crafting (a mod's recipes) into a full inventory changes nothing") {
	Play p(with_kit());
	gc::Game &g = *p.game;
	auto &inv = g.inventory();
	for (gc::Stack &s : inv.slots) {
		s = {"core:dirt", 64};
	}
	inv.slots[0] = {"core:log", 1};
	const gc::Recipe *planks = find_recipe(g, "kit:log_to_planks");
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

TEST_CASE("a chest: made with its block, holds items, saved (false for an empty slot), spills when broken") {
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

	const Data saved = be.serialize();
	CHECK(saved["3,60,3"]["type"] == "chest");
	CHECK(saved["3,60,3"]["slots"][0] == Data::array({"core:coal", 8}));
	CHECK(saved["3,60,3"]["slots"][1] == false);
	gc::BlockEntities copy(p.content, p.mods);
	copy.deserialize(saved);
	REQUIRE(copy.at(at));
	CHECK(copy.at(at)->slots[4] == Stack{"core:dirt", 2});

	p.game->break_block(at, "");
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

TEST_CASE("a furnace (a mod's) smelts with fuel, as the mod's recipes say") {
	Play p(with_kit());
	const BlockPos at{3, 60, 3};
	REQUIRE(p.world->set_block(at, p.id("kit:kiln")));
	gc::BlockEntities &be = p.game->block_entities();
	REQUIRE(be.at(at));
	using B = gc::BlockEntities;
	CHECK(be.fuel_seconds("core:coal") == 40);
	CHECK(be.smelting("core:iron_ore") == Stack{"kit:iron_ingot", 1});
	CHECK_FALSE(be.smelting("core:dirt"));
	CHECK(be.click(at, B::FURNACE_FUEL, {"core:dirt", 1}) == Stack{"core:dirt", 1}); // not fuel: refused
	CHECK(be.click(at, B::FURNACE_OUTPUT, {"core:iron_ore", 1}) == Stack{"core:iron_ore", 1}); // output: take only
	CHECK(be.click(at, B::FURNACE_INPUT, {"core:iron_ore", 2}).empty());
	CHECK(be.click(at, B::FURNACE_FUEL, {"core:coal", 1}).empty());
	for (int i = 0; i < 44; i++) { // 11 s: two ingots (5 s each)
		be.update(0.25);
	}
	CHECK(be.at(at)->slots[B::FURNACE_OUTPUT] == Stack{"kit:iron_ingot", 2});
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
	p.game->set_world_spawn(Data::array({1.5, 40.0, 1.5}));
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

TEST_CASE("eating (a mod's food: the base game has none) heals; food isn't eaten at full health") {
	Play p(with_kit());
	p.game->inventory().slots[0] = {"kit:bread", 2};
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

#include "game/day_cycle.h"

TEST_CASE("the day: sun, daylight and the clock") {
	gc::DayCycle d;
	d.time = 0.5; // noon
	CHECK(d.sun_height() == doctest::Approx(1.0f));
	CHECK(d.day_amount() == doctest::Approx(1.0f));
	CHECK(d.daylight() == doctest::Approx(1.0f));
	float sun[3];
	d.sun_dir(sun);
	CHECK(sun[1] == doctest::Approx(1.0f));
	d.time = 0.0; // midnight
	CHECK(d.day_amount() == doctest::Approx(0.0f));
	CHECK(d.daylight() == doctest::Approx(gc::DayCycle::NIGHT_LIGHT));
	d.time = 0.25; // sunrise: in the east (+x)
	d.sun_dir(sun);
	CHECK(sun[0] == doctest::Approx(1.0f));
	CHECK(d.dusk() == doctest::Approx(1.0f));
	char clock[16];
	gc::DayCycle::clock_text(0.75, false, clock, sizeof(clock));
	CHECK(std::string(clock) == "6:00 PM");
	gc::DayCycle::clock_text(0.0, false, clock, sizeof(clock));
	CHECK(std::string(clock) == "12:00 AM");
	gc::DayCycle::clock_text(0.5, true, clock, sizeof(clock));
	CHECK(std::string(clock) == "12:00");
	d.time = 0.9;
	d.day_length = 100;
	d.update(20); // a fifth of a day: past midnight
	CHECK(d.time == doctest::Approx(0.1));
}

TEST_CASE("right-clicking a bed reports it (for sleeping) every time") {
	Play p;
	pool(p, "core:air");
	REQUIRE(p.world->set_block({1, 40, 0}, p.id("core:bed")));
	REQUIRE(p.world->set_block({1, 40, -1}, p.id("core:bed_n_head")));
	p.player.position[0] = 1.5f;
	p.player.position[1] = 41.0f;
	p.player.position[2] = 2.5f;
	p.player.pitch = -std::atan2(1.62f + 0.45f, 2.0f);
	gc::GameInput in;
	in.captured = true;
	in.use_pressed = true;
	for (int i = 0; i < 2; i++) {
		p.game->update(0.016f, in);
		CHECK(p.game->take_bed_used());
		CHECK_FALSE(p.game->take_bed_used());
	}
}

namespace {

// Game and mod ticks (20 a second) for this long.
void run_ticks(Play &p, float seconds) {
	for (int i = 0; i < int(seconds * 20); i++) {
		p.game->tick(0.05);
		p.mods.tick(0.05);
	}
}

int drops_of_item(Play &p, const std::string &item) {
	int n = 0;
	for (const auto &d : p.game->drops().drops()) {
		n += d.item == item ? d.count : 0;
	}
	return n;
}

} // namespace

TEST_CASE("plants (the core mod's Lua): sand falls, unsupported blocks pop off") {
	Play p;
	pool(p, "core:air");
	REQUIRE(p.world->set_block({1, 45, 1}, p.id("core:sand"))); // 5 above the floor (y 40)
	REQUIRE(p.world->set_block({0, 41, 1}, p.id("core:torch_wall_w"))); // against the west wall (x -1)
	run_ticks(p, 1.0f);
	CHECK(p.block(1, 45, 1) == "core:air");
	CHECK(p.block(1, 40, 1) == "core:sand");
	CHECK(p.block(0, 41, 1) == "core:torch_wall_w"); // still held by the wall
	REQUIRE(p.world->set_block({-1, 41, 1}, 0));    // the wall behind it goes
	run_ticks(p, 0.5f);
	CHECK(p.block(0, 41, 1) == "core:air");
	CHECK(drops_of_item(p, "core:torch") == 1);
}

TEST_CASE("plants: leaves without a log nearby decay") {
	Play p;
	pool(p, "core:air");
	REQUIRE(p.world->set_block({1, 40, 1}, p.id("core:log")));
	for (int x = 0; x <= 2; x++) {
		REQUIRE(p.world->set_block({x, 41, 1}, p.id("core:leaves")));
	}
	REQUIRE(p.world->set_block({1, 42, 1}, p.id("core:leaves_placed"))); // placed by a player: stays
	run_ticks(p, 4.0f);
	CHECK(p.block(0, 41, 1) == "core:leaves"); // the log holds them
	REQUIRE(p.world->set_block({1, 40, 1}, 0));
	run_ticks(p, 9.0f); // (the leaf above the log first, then its neighbours: up to 3 s each)
	for (int x = 0; x <= 2; x++) {
		CHECK(p.block(x, 41, 1) == "core:air");
	}
	CHECK(p.block(1, 42, 1) == "core:leaves_placed");
}

TEST_CASE("plants (a mod's crop): crops grow, and what's growing is saved") {
	Play p(with_kit());
	pool(p, "core:air");
	REQUIRE(p.world->set_block({1, 40, 1}, p.id("core:dirt")));
	REQUIRE(p.world->set_block({1, 41, 1}, p.id("kit:sprout")));
	run_ticks(p, 0.2f);
	const Data saved = p.game->save_plants();
	REQUIRE(saved.size() == 1);
	CHECK(saved[0][0] == 1);
	CHECK(saved[0][1] == 41);
	CHECK(saved[0][3].get<double>() > 50 * 0.6 - 1); // 50 s, give or take 40%
	CHECK(saved[0][3].get<double>() < 50 * 1.4);
	// Loaded with 2 seconds left, it grows into the next stage.
	Play q(with_kit());
	pool(q, "core:air");
	REQUIRE(q.world->set_block({1, 40, 1}, q.id("core:dirt")));
	REQUIRE(q.world->set_block({1, 41, 1}, q.id("kit:sprout")));
	run_ticks(q, 0.2f); // (it's tracked with a fresh timer: replaced below)
	q.game->load_plants(Data::array({Data::array({1, 41, 1, 2.0})}));
	run_ticks(q, 3.5f);
	CHECK(q.block(1, 41, 1) == "kit:crop");
}

TEST_CASE("a tree grows: the generator's shape, where there's room") {
	Play p;
	pool(p, "core:air");
	for (int y = 43; y <= 46; y++) { // (open the pool upwards: trees are tall)
		for (int x = -1; x <= 3; x++) {
			for (int z = -1; z <= 3; z++) {
				REQUIRE(p.world->set_block({x, y, z}, 0));
			}
		}
	}
	REQUIRE(p.world->set_block({1, 40, 1}, p.id("core:grass")));
	CHECK(p.game->grow_tree({1, 41, 1}, "core:oak"));
	CHECK(p.block(1, 41, 1) == "core:log");
	CHECK(p.block(1, 42, 1) == "core:log");
	CHECK_FALSE(p.game->grow_tree({1, 41, 1}, "core:nonexistent"));
}

TEST_CASE("with a mod's recipes: from a new survival world to a crafted tool and a container") {
	Play p(with_kit()); // a new survival player: just the starting torch
	CHECK(p.game->inventory().count_of("core:torch") == 1);
	// A tree trunk (4 logs) two blocks ahead, on a stone floor.
	const int h = 40;
	for (int x = -1; x <= 1; x++) {
		for (int z = -3; z <= 1; z++) {
			REQUIRE(p.world->set_block({x, h, z}, p.id("core:stone")));
			for (int y = h + 1; y <= h + 6; y++) {
				REQUIRE(p.world->set_block({x, y, z}, 0));
			}
		}
	}
	for (int y = h + 1; y <= h + 4; y++) {
		REQUIRE(p.world->set_block({0, y, -2}, p.id("core:log")));
	}
	p.player.position[0] = 0.5f;
	p.player.position[1] = float(h + 1);
	p.player.position[2] = 0.5f;
	p.player.yaw = 0;

	// Mine the logs by holding the button (2.0 hardness x 0.375 = 0.75 s each),
	// standing still; the drops fall and come to the player.
	gc::GameInput hold;
	hold.captured = true;
	hold.break_held = true;
	for (int y = h + 1; y <= h + 4; y++) {
		p.player.pitch = std::atan2(float(y) + 0.5f - (float(h + 1) + gc::Player::EYE_HEIGHT), 1.5f);
		for (int i = 0; i < 200 && p.block(0, y, -2) == "core:log"; i++) {
			p.game->update(1.0f / 60.0f, hold);
			p.game->physics_step(1.0f / 60.0f);
		}
		CHECK(p.block(0, y, -2) == "core:air");
	}
	for (int i = 0; i < 180; i++) { // let the last ones arrive
		p.game->physics_step(1.0f / 60.0f);
	}
	gc::Game &g = *p.game;
	auto &inv = g.inventory();
	REQUIRE(inv.count_of("core:log") == 4);

	// Craft: 16 planks, 4 sticks, a wooden pickaxe, a chest.
	const gc::Recipe *planks = find_recipe(g, "kit:log_to_planks");
	const gc::Recipe *sticks = find_recipe(g, "kit:planks_to_sticks");
	const gc::Recipe *pickaxe = find_recipe(g, "kit:wood_pickaxe");
	const gc::Recipe *chest = find_recipe(g, "kit:chest");
	REQUIRE((planks && sticks && pickaxe && chest));
	for (int i = 0; i < 4; i++) {
		REQUIRE(g.craft(*planks));
	}
	REQUIRE(g.craft(*sticks));
	REQUIRE(g.craft(*pickaxe));
	REQUIRE(g.craft(*chest));
	CHECK(inv.count_of("kit:wood_pickaxe") == 1);
	CHECK(inv.count_of("core:chest") == 1);

	// Put the chest in the hand and place it on the floor ahead, then open it.
	int slot = -1;
	for (int i = 0; i < gc::Inventory::SIZE; i++) {
		if (inv.slots[i].id == "core:chest") {
			slot = i;
		}
	}
	REQUIRE(slot >= 0);
	std::swap(inv.slots[slot], inv.slots[2]);
	gc::GameInput in;
	in.captured = true;
	in.number_key = 3;
	g.update(0.016f, in);
	CHECK(g.selected_item() == "core:chest");
	p.player.pitch = -std::atan2(gc::Player::EYE_HEIGHT, 2.0f); // the floor two blocks ahead
	in.number_key = 0;
	in.use_pressed = true;
	g.update(0.016f, in);
	CHECK(p.block(0, h + 1, -2) == "core:chest");
	CHECK(inv.count_of("core:chest") == 0);
	REQUIRE(g.block_entities().at({0, h + 1, -2}));
	p.player.pitch = -0.42f; // at the chest
	g.update(0.016f, in);
	const auto opened = g.take_opened();
	REQUIRE(opened);
	CHECK(*opened == BlockPos{0, h + 1, -2});
}

namespace {

// Stands the player on (1, 40, 1) looking straight down at it, holding `item`,
// and right-clicks.
void use_down(Play &p, const std::string &item) {
	p.player.position[0] = 1.5f;
	p.player.position[1] = 41.0f;
	p.player.position[2] = 1.5f;
	p.player.pitch = -1.5707f;
	p.game->inventory().slots[0] = {item, 1};
	gc::GameInput in;
	in.captured = true;
	in.use_pressed = true;
	in.number_key = 1;
	p.game->update(0.016f, in);
}

} // namespace

TEST_CASE("the base game has no farming, food or small plants; one grass, one wood") {
	Play p;
	for (const char *gone : {"core:seeds", "core:wheat", "core:bread"}) {
		CHECK_FALSE(p.content.items.count(gone));
	}
	for (const char *gone : {"core:farmland", "core:wheat_0", "core:tall_grass", "core:bluebell", "core:sunpetal",
				"core:dead_bush", "core:snow_grass", "core:birch_log", "core:spruce_leaves", "core:oak_sapling",
				"core:stone_slab", "core:cobblestone_slab", "core:planks_slab"}) {
		CHECK_FALSE(p.content.blocks.count(gone));
	}
	for (const auto &[id, def] : p.content.items) {
		CAPTURE(id);
		CHECK_FALSE(def.contains("food"));
		CHECK_FALSE(def.contains("plants"));
	}
	CHECK(p.content.trees.count("core:oak"));
	CHECK_FALSE(p.content.trees.count("core:birch"));
	// No saplings: trees sprout on grass by themselves.
	for (const auto &[id, def] : p.content.blocks) {
		CAPTURE(id);
		CHECK_FALSE(def.contains("grows_tree"));
	}
	CHECK(p.content.blocks.at("core:grass").contains("sprouts_trees"));
}

TEST_CASE("farming (a mod's rule): seeds are planted on dirt, and grow") {
	Play p(with_kit());
	pool(p, "core:air");
	REQUIRE(p.world->set_block({1, 40, 1}, p.id("core:stone")));
	use_down(p, "kit:seeds");
	CHECK(p.block(1, 41, 1) == "core:air"); // not on stone
	CHECK(p.game->inventory().slots[0].count == 1);
	REQUIRE(p.world->set_block({1, 40, 1}, p.id("core:dirt")));
	use_down(p, "kit:seeds");
	CHECK(p.block(1, 41, 1) == "kit:sprout");
	CHECK(p.game->inventory().slots[0].count == 0);
	run_ticks(p, 0.2f);
	CHECK(p.game->save_plants().size() == 1); // growing
}

TEST_CASE("fluids (the core mod's Lua): water spreads one level lower a block, and drains away") {
	Play p;
	pool(p, "core:air");
	REQUIRE(p.world->set_block({0, 40, 0}, p.id("core:water")));
	run_ticks(p, 3.0f);
	CHECK(p.block(0, 40, 0) == "core:water");
	CHECK(p.block(1, 40, 0) == "core:water_7");
	CHECK(p.block(2, 40, 0) == "core:water_6");
	CHECK(p.block(1, 40, 1) == "core:water_6");
	CHECK(p.block(2, 40, 2) == "core:water_4");
	CHECK(p.block(0, 41, 0) == "core:air");
	REQUIRE(p.world->set_block({0, 40, 0}, 0)); // the source goes: it all drains
	run_ticks(p, 10.0f);
	for (int x = 0; x <= 2; x++) {
		for (int z = 0; z <= 2; z++) {
			CHECK(p.block(x, 40, z) == "core:air");
		}
	}
	gc::Data pending = p.mods.rule("fluids_pending", Data::array()).value_or(Data());
	CHECK(pending == 0);
}

TEST_CASE("fluids: a lava source touching water turns to stone") {
	Play p;
	pool(p, "core:air");
	REQUIRE(p.world->set_block({0, 40, 0}, p.id("core:lava")));
	REQUIRE(p.world->set_block({1, 40, 0}, p.id("core:water")));
	run_ticks(p, 1.0f);
	CHECK(p.block(0, 40, 0) == "core:stone");
}

TEST_CASE("fluids: a big flood stays within the mods' time limit") {
	Play p;
	// A 40 x 40 sheet of unfed flowing water over a pit: all of it changes at once.
	for (int x = -20; x < 20; x++) {
		for (int z = -20; z < 20; z++) {
			REQUIRE(p.world->set_block({x, 45, z}, p.id("core:water_3")));
			for (int y = 40; y <= 44; y++) {
				REQUIRE(p.world->set_block({x, y, z}, 0));
			}
		}
	}
	double worst = 0;
	for (int i = 0; i < 20 * 30; i++) {
		p.game->tick(0.05);
		const auto t0 = std::chrono::steady_clock::now();
		p.mods.tick(0.05);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		worst = std::max(worst, ms);
	}
	MESSAGE("worst mods tick: " << worst << " ms");
	CHECK(p.mods.problems().empty());
	CHECK(worst < 20.0); // (the limit per call: three calls stopped turn the core mod off)
}

namespace {

int count_sounds(const std::vector<gc::SoundEvent> &sounds, const std::string &set, const std::string &kind) {
	return int(std::count_if(sounds.begin(), sounds.end(), [&](const gc::SoundEvent &e) {
		return e.set == set && e.kind == kind;
	}));
}

} // namespace

TEST_CASE("sounds: breaking and placing sound like the block, steps like the ground, pickups and hurts") {
	Play p;
	p.game->take_sounds();
	// Breaking stone by hand: the stone set's break, at the block.
	const BlockPos at{2, 60, 0};
	REQUIRE(p.world->set_block(at, p.id("core:stone")));
	p.player.position[0] = 0.5f;
	p.player.position[1] = 59.0f;
	p.player.position[2] = 0.5f;
	p.player.flying = true;
	p.game->break_block(at, "");
	auto sounds = p.game->take_sounds();
	REQUIRE(count_sounds(sounds, "core:stone", "break") == 1);
	CHECK(sounds[0].at[0] == doctest::Approx(2.5f));
	// Hurt: the effects' hurt.
	p.game->damage(2);
	CHECK(count_sounds(p.game->take_sounds(), "core:effects", "hurt") == 1);

	// Walking 10 blocks on grass: a footstep every 1.7 m.
	pool(p, "core:air");
	for (int x = -1; x <= 3; x++) {
		for (int z = -12; z <= 3; z++) {
			REQUIRE(p.world->set_block({x, 39, z}, p.id("core:grass")));
			for (int y = 40; y <= 43; y++) {
				REQUIRE(p.world->set_block({x, y, z}, 0));
			}
		}
	}
	p.player.flying = false;
	p.player.position[0] = 1.5f;
	p.player.position[1] = 40.0f;
	p.player.position[2] = 1.5f;
	p.player.yaw = 0; // towards -z
	gc::PlayerInput walk;
	for (int i = 0; i < 30; i++) { // settle on the floor
		p.player.step({}, *p.world, *p.rules);
		p.game->after_move(gc::Player::STEP);
	}
	p.game->take_sounds();
	walk.forward = 1;
	const float start = p.player.position[2];
	while (start - p.player.position[2] < 10.0f) {
		p.player.step(walk, *p.world, *p.rules);
		p.game->after_move(gc::Player::STEP);
	}
	const int steps = count_sounds(p.game->take_sounds(), "core:grass", "step");
	CHECK(steps >= 5);
	CHECK(steps <= 6);

	// Picking up a drop.
	const float near[3] = {p.player.position[0], p.player.position[1] + 0.5f, p.player.position[2]};
	p.game->drop_item("core:dirt", 1, near);
	for (int i = 0; i < 120; i++) {
		p.game->physics_step(1.0f / 60.0f);
	}
	CHECK(count_sounds(p.game->take_sounds(), "core:effects", "pickup") == 1);
}

TEST_CASE("creatures: they fall to the ground, walk where they head, notice walls, and can be picked") {
	Play p;
	pool(p, "core:air");
	const float at[3] = {1.5f, 44.0f, 1.5f};
	gc::Creatures &cs = p.game->creatures();
	CHECK(cs.spawn(p.content, "core:nothing", at) == 0); // unknown kind
	const int id = cs.spawn(p.content, "core:block_eater", at);
	REQUIRE(id > 0);
	for (int i = 0; i < 120; i++) {
		cs.step(1.0f / 60.0f, *p.world);
	}
	gc::Creature *c = cs.find(id);
	REQUIRE(c);
	CHECK(c->on_floor);
	CHECK(c->pos[1] == doctest::Approx(40.0f).epsilon(0.001)); // on the stone floor (y 39; the collision margin)
	// Heading -x at walking speed: it reaches the wall at x = -1 (its box is 0.9 wide).
	cs.apply(*c, {{"heading", 3.14159265 / 2}, {"speed", 1.2}});
	for (int i = 0; i < 180; i++) {
		cs.step(1.0f / 60.0f, *p.world);
	}
	c = cs.find(id);
	CHECK(c->pos[0] == doctest::Approx(0.45f).epsilon(0.01));
	const gc::Data seen = cs.describe(*c);
	CHECK(seen["hit_wall"] == true);
	CHECK(cs.describe(*c)["hit_wall"] == false); // (cleared once seen)
	CHECK(c->yaw == doctest::Approx(3.14159265f / 2).epsilon(0.01)); // turned to its heading
	// Picked by a ray through its box.
	const float eye[3] = {0.45f, 40.5f, 3.0f}, dir[3] = {0, 0, -1};
	float d;
	CHECK(cs.pick(eye, dir, 4.0f, d) == id);
	CHECK(d == doctest::Approx(3.0f - (1.5f + 0.45f)).epsilon(0.01));
	CHECK(cs.remove(id));
	CHECK(cs.pick(eye, dir, 4.0f, d) == 0);
}

TEST_CASE("Block Eaters (the core mod's Lua): rare, they come by day on grass, flee when hit") {
	Play p;
	// Grass all around the player, open to the sky.
	for (int x = -50; x <= 50; x++) {
		for (int z = -50; z <= 50; z++) {
			if (p.world->is_loaded(x, 70, z)) {
				p.world->set_block({x, 70, z}, p.id("core:grass"));
				for (int y = 71; y <= 74; y++) {
					p.world->set_block({x, y, z}, 0);
				}
			}
		}
	}
	p.player.position[0] = 0.5f;
	p.player.position[1] = 71.0f;
	p.player.position[2] = 0.5f;
	p.player.flying = true;
	auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds * 20); i++) {
			for (int k = 0; k < 3; k++) {
				p.game->physics_step(1.0f / 60.0f);
			}
			p.game->tick(0.05);
			p.mods.tick(0.05);
		}
	};
	p.game->set_day(0.0f); // night: no Block Eaters
	run(20);
	for (const gc::Creature &c : p.game->creatures().all()) {
		CHECK(c.kind != "core:block_eater");
	}
	p.game->set_day(1.0f);
	run(100); // (one comes every 45 s, two at most)
	const auto &all = p.game->creatures().all();
	REQUIRE(!all.empty());
	CHECK(all.size() <= 2);
	for (const gc::Creature &c : all) {
		CHECK(c.kind == "core:block_eater");
		CHECK(c.variant.empty()); // (none made of a block: only a mod spawns those)
		CHECK(c.pos[1] == doctest::Approx(71.0f).epsilon(0.01)); // on the grass
		const float d = std::hypot(c.pos[0] - 0.5f, c.pos[2] - 0.5f);
		CHECK(d < 80.0f); // (spawned up to 48 away, they wander; gone past DESPAWN_DISTANCE)
	}
	// One made of stone (as a mod would spawn it), hit four times (2 each:
	// bare hands, 8 health): it runs off, then is gone and drops a stone.
	const float spot[3] = {5.5f, 71.0f, 0.5f};
	const int id = p.game->creatures().spawn(p.content, "core:block_eater", spot, "core:stone");
	run(0.1f);
	const float from[3] = {0.5f, 71.0f, 0.5f};
	p.mods.creature_hit(id, 2, from, 0);
	run(1.0f);
	const gc::Creature *c = p.game->creatures().find(id);
	REQUIRE(c);
	CHECK(c->pos[0] > 6.0f); // ran off, away from the player
	CHECK(c->speed == doctest::Approx(3.4f));
	for (int i = 0; i < 3; i++) {
		p.mods.creature_hit(id, 2, from, 0);
	}
	CHECK(p.game->creatures().find(id) == nullptr);
	CHECK(drops_of_item(p, "core:stone") == 1);
}

namespace {

// Grass all around the origin at y 70, open to the sky; the player on it.
void open_field(Play &p) {
	for (int x = -50; x <= 50; x++) {
		for (int z = -50; z <= 50; z++) {
			if (p.world->is_loaded(x, 70, z)) {
				p.world->set_block({x, 70, z}, p.id("core:grass"));
				for (int y = 71; y <= 74; y++) {
					p.world->set_block({x, y, z}, 0);
				}
			}
		}
	}
	p.player.position[0] = 0.5f;
	p.player.position[1] = 71.0f;
	p.player.position[2] = 0.5f;
	p.player.flying = true;
}

void live_for(Play &p, float seconds) {
	for (int i = 0; i < int(seconds * 20); i++) {
		for (int k = 0; k < 3; k++) {
			p.game->physics_step(1.0f / 60.0f);
		}
		p.game->tick(0.05);
		p.mods.tick(0.05);
	}
}

} // namespace

TEST_CASE("Block Eaters eat the block in front of them (it's gone); trees sprout on grass by themselves") {
	Play p;
	open_field(p);
	p.game->set_day(1.0f);
	const float at[3] = {6.5f, 71.0f, 6.5f};
	const int id = p.game->creatures().spawn(p.content, "core:block_eater", at);
	REQUIRE(id > 0);
	auto holes = [&] {
		int n = 0;
		for (int x = -50; x <= 50; x++) {
			for (int z = -50; z <= 50; z++) {
				n += p.world->is_loaded(x, 70, z) && p.block(x, 70, z) == "core:air";
			}
		}
		return n;
	};
	auto logs = [&] {
		int n = 0;
		for (int x = -50; x <= 50; x++) {
			for (int z = -50; z <= 50; z++) {
				n += p.world->is_loaded(x, 71, z) && p.block(x, 71, z) == "core:log";
			}
		}
		return n;
	};
	CHECK(holes() == 0);
	CHECK(logs() == 0);
	live_for(p, 240); // (it grazes every few seconds and eats 3 times in 10; a tree tries every 20 s)
	CHECK(holes() > 0);
	const gc::Creature *c = p.game->creatures().find(id);
	if (c) {
		CHECK(c->variant.empty()); // (it doesn't take the form of what it ate)
	}
	CHECK(logs() > 0);
	// Not close to the player (at the origin): 24 blocks or more away.
	for (int x = -24; x <= 24; x++) {
		for (int z = -24; z <= 24; z++) {
			if (std::hypot(x + 0.5f, z + 0.5f) < 22.0f) {
				CHECK(p.block(x, 71, z) != "core:log");
			}
		}
	}
}

TEST_CASE("falling out of the world (below its bottom) hurts every half second; not in creative") {
	Play p;
	p.player.position[0] = 0.5f;
	p.player.position[1] = float(gc::World::BOTTOM_Y) - 2.0f;
	p.player.position[2] = 0.5f;
	live(p, 0.05f);
	CHECK(p.game->health() == gc::Game::MAX_HEALTH - 4); // at once
	live(p, 0.5f);
	CHECK(p.game->health() == gc::Game::MAX_HEALTH - 8);
	live(p, 2.0f);
	CHECK(p.game->health() == 0); // (gone)

	Play creative(true);
	creative.player.position[0] = 0.5f;
	creative.player.position[1] = float(gc::World::BOTTOM_Y) - 2.0f;
	creative.player.position[2] = 0.5f;
	live(creative, 1.0f);
	CHECK(creative.game->health() == gc::Game::MAX_HEALTH);
}

TEST_CASE("a hostile creature (a mod's: the base game has none) runs at the player and strikes; the sun makes it vanish in smoke") {
	Play p(with_kit());
	open_field(p);
	p.game->set_day(0.0f);
	const float at[3] = {8.5f, 71.0f, 0.5f};
	const int id = p.game->creatures().spawn(p.content, "kit:stalker", at);
	REQUIRE(id > 0);
	live_for(p, 6.0f);
	const gc::Creature *c = p.game->creatures().find(id);
	REQUIRE(c);
	CHECK(std::hypot(c->pos[0] - 0.5f, c->pos[2] - 0.5f) < 2.0f); // came to the player
	CHECK(p.game->health() <= gc::Game::MAX_HEALTH - 6);          // struck at least twice (3 each)
	CHECK(c->strikes >= 2);
	// Morning: under open sky, it's gone in a puff of smoke.
	p.game->take_effects();
	p.game->set_day(1.0f);
	live_for(p, 0.2f);
	CHECK(p.game->creatures().find(id) == nullptr);
	const auto effects = p.game->take_effects();
	REQUIRE(effects.size() == 1);
	CHECK(effects[0].name == "smoke");
}

TEST_CASE("no enemies: nights stay empty and nothing hurts the player") {
	Play p;
	open_field(p);
	p.game->set_day(0.0f);
	live_for(p, 30.0f);
	CHECK(p.game->creatures().all().empty());
	CHECK(p.game->health() == gc::Game::MAX_HEALTH);
	// By day the Block Eaters (harmless) still come.
	p.game->set_day(1.0f);
	live_for(p, 30.0f);
	CHECK_FALSE(p.game->creatures().all().empty());
	CHECK(p.game->health() == gc::Game::MAX_HEALTH);
}

TEST_CASE("game mode: one inventory in both; creative starts empty, picks things up, uses nothing up") {
	Play p; // (survival: the starting items)
	gc::Game &g = *p.game;
	g.inventory().slots[0] = {"core:torch", 7};
	g.set_creative(true);
	CHECK(g.creative());
	CHECK(g.inventory().count_of("core:torch") == 7); // (as it was)
	g.set_creative(false);
	CHECK(g.inventory().count_of("core:torch") == 7);
	CHECK(g.health() == gc::Game::MAX_HEALTH);

	// A new creative player: nothing in hand; the palette gives one of anything.
	Play c(true);
	gc::Inventory &inv = c.game->inventory();
	CHECK(std::all_of(inv.slots.begin(), inv.slots.end(), [](const Stack &s) { return s.empty(); }));
	c.game->inventory_action(gc::Data::array({"pick", "core:planks"}));
	CHECK(inv.cursor == Stack{"core:planks", 1});
	c.game->inventory_action(gc::Data::array({"click", 0}));
	CHECK(inv.slots[0] == Stack{"core:planks", 1});
	// It picks up what lies around.
	c.player.position[1] = 80.0f;
	c.player.flying = true;
	const float near[3] = {c.player.position[0], c.player.position[1] + 0.5f, c.player.position[2]};
	c.game->drop_item("core:coal", 3, near);
	for (int i = 0; i < 120; i++) {
		c.game->physics_step(1.0f / 60.0f);
	}
	CHECK(inv.count_of("core:coal") == 3);
	// Saved and loaded as it is.
	Play again(true);
	again.game->load_player(c.game->save_player());
	CHECK(again.game->inventory().count_of("core:coal") == 3);
	CHECK(again.game->inventory().count_of("core:planks") == 1);
}
