// Multiplayer: a host and a client in one process, over ENet on localhost.
// The client is driven as a person would (aim, click, walk); the host checks
// what it asks and does it as that player. Both worlds must agree after.
// (tests/mp_smoke.cpp does a round of play across two real processes.)

#include "net_play.h"

#include <doctest/doctest.h>

#include <process.h>

using gc::BlockPos;
using gc::Json;

namespace {

int test_port(int n) {
	return 26000 + (_getpid() % 1500) * 5 + n;
}

// Both worlds have this block at p.
bool both(NetPlay &a, NetPlay &b, const BlockPos &p, const std::string &id) {
	return a.play->block(p) == id && b.play->block(p) == id;
}

// A survival inventory as the host keeps it for a player who played before.
Json saved_inventory(const gc::Content &content, const std::vector<gc::Stack> &stacks) {
	gc::Inventory inv(content);
	for (size_t i = 0; i < stacks.size(); i++) {
		inv.slots[i] = stacks[i];
	}
	return inv.serialize();
}

// The top face of the floor's stone under cell (x, z): where to aim to place on it.
void aim_at_floor(NetPlay &n, int x, int z) {
	n.aim(x + 0.5f, NetPlay::FLOOR_Y - 0.001f, z + 0.5f);
}

} // namespace

TEST_CASE("multiplayer: joining gets the host's world, and block changes go both ways (creative)") {
	auto host = NetPlay::host(test_port(0), true);
	const BlockPos before{5, 100, 5}, after{6, 100, 5};
	host->world().set_block(before, host->play->id("core:lamp"));
	host->game().inventory().slots[0] = {"core:torch", 1}; // (others see its light)
	auto client = NetPlay::join(test_port(0), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	CHECK(c.game().creative());
	REQUIRE(run_until(both_sides, [&] { return c.world().is_loaded(5, 100, 5); }, 20));
	CHECK(c.play->block(before) == "core:lamp"); // an edit made before we joined

	h.world().set_block(after, h.play->id("core:gold_ore"));
	CHECK(run_until(both_sides, [&] { return c.play->block(after) == "core:gold_ore"; }, 5)); // and after

	// Each sees the other, with its name; the host's torch lights our world.
	CHECK(run_until(both_sides, [&] {
		return c.session.avatars().size() == 1 && h.session.avatars().size() == 1 && !c.session.held_lights().empty();
	}, 5));
	CHECK(c.session.avatars()[0].name == "Host");
	CHECK(h.session.avatars()[0].name == "Client");
	CHECK(c.session.held_lights()[0][3] == 13.0f);
	CHECK(c.session.player_count() == 2);

	// We place a block: the host does it, both worlds have it.
	const BlockPos cell{4, 100, 1};
	c.game().inventory().slots[0] = {"core:planks", 1};
	c.input.number_key = 1;
	c.frame();
	aim_at_floor(c, cell.x, cell.z);
	c.input.use_pressed = true;
	CHECK(run_until(both_sides, [&] { return both(h, c, cell, "core:planks"); }, 5));
	// And break it.
	c.aim(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
	c.input.break_pressed = true;
	CHECK(run_until(both_sides, [&] { return both(h, c, cell, "core:air"); }, 5));

	// A block the host changes in a chunk we haven't loaded comes as the whole
	// chunk, kept for when we get there.
	const BlockPos far{900, 60, 900};
	REQUIRE_FALSE(c.world().is_loaded(far.x, far.y, far.z));
	h.player().position[0] = h.player().position[2] = 900.5f; // (the host goes there: its world loads there)
	REQUIRE(run_until({&h}, [&] { return h.world().is_loaded(far.x, far.y, far.z); }, 30));
	REQUIRE(h.world().set_block(far, h.play->id("core:coal_ore")));
	CHECK(run_until(both_sides, [&] { return !c.world().chunk_bytes({28, 1, 28}).empty(); }, 5));
	h.player().position[0] = h.player().position[2] = 0.5f;
	REQUIRE(run_until(both_sides, [&] { return h.world().is_loaded(-6, 100, -6); }, 30));

	// Out of reach, nothing happens; a flood of requests is cut down to a burst.
	h.world().set_block({14, 100, -14}, h.play->id("core:dirt")); // (20 blocks from us)
	c.session.request_break({14, 100, -14}, 0);
	int placed = 0;
	for (int x = -6; x <= -2; x++) {
		for (int z = -6; z <= -2; z++) {
			for (int y = 100; y < 104; y++) {
				h.world().set_block({x, y, z}, h.play->id("core:dirt"));
				placed++;
			}
		}
	}
	REQUIRE(placed == 100);
	run_for(both_sides, 0.5f);
	for (int x = -6; x <= -2; x++) {
		for (int z = -6; z <= -2; z++) {
			for (int y = 100; y < 104; y++) {
				c.session.request_break({x, y, z}, 0);
			}
		}
	}
	run_for(both_sides, 1.0f);
	int broken = 0;
	for (int x = -6; x <= -2; x++) {
		for (int z = -6; z <= -2; z++) {
			for (int y = 100; y < 104; y++) {
				broken += h.play->block(x, y, z) == "core:air" ? 1 : 0;
			}
		}
	}
	CHECK(broken >= 38);
	CHECK(broken <= 60); // (a burst of 40, then 20 a second)
	CHECK(h.play->block(14, 100, -14) == "core:dirt");

	// When the client leaves, the host sees it go.
	client.reset();
	CHECK(run_until({&h}, [&] { return h.session.avatars().empty(); }, 5));
}

TEST_CASE("multiplayer: a survival client's inventory, placing, mining time, drops and moves are the host's") {
	auto host = NetPlay::host(test_port(1));
	const gc::Content &content = host->play->content;
	host->session.set_saved_players({{"Client", {{"inventory", saved_inventory(content, {{"core:planks", 4},
			{"core:wood_door", 1}, {"core:bed", 3}})}}}});
	auto client = NetPlay::join(test_port(1), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	gc::Inventory &inv = c.game().inventory();
	CHECK(run_until(both_sides, [&] { return inv.count_of("core:planks") == 4 && inv.count_of("core:bed") == 3; }, 5));
	REQUIRE(run_until(both_sides, [&] { return c.world().is_loaded(4, 100, 1); }, 20));

	// Placing takes the item from the host's copy of our inventory.
	const BlockPos cell{4, 100, 1};
	REQUIRE(c.hold("core:planks"));
	c.frame();
	aim_at_floor(c, cell.x, cell.z);
	c.input.use_pressed = true;
	CHECK(run_until(both_sides, [&] { return both(h, c, cell, "core:planks") && inv.count_of("core:planks") == 3; }, 5));

	// Without the item in that slot, nothing is placed.
	const BlockPos spot{4, 100, 3};
	float eye[3], dir[3];
	c.player().eye(eye);
	const float to[3] = {spot.x + 0.5f - eye[0], NetPlay::FLOOR_Y - 0.001f - eye[1], spot.z + 0.5f - eye[2]};
	const float len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
	for (int k = 0; k < 3; k++) {
		dir[k] = to[k] / len;
	}
	c.session.request_use(eye, dir, 5, "core:stone", false); // (slot 5 is empty)
	run_for(both_sides, 0.5f);
	CHECK(both(h, c, spot, "core:air"));

	// Breaking at once, without mining, is refused.
	c.session.request_break(cell, c.game().selected());
	run_for(both_sides, 0.5f);
	CHECK(both(h, c, cell, "core:planks"));

	// Mining it: the break waits for the mining time; the host drops it, it
	// comes to us, into its copy of our inventory.
	c.aim(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
	c.input.break_held = true;
	CHECK(run_until(both_sides, [&] { return both(h, c, cell, "core:air"); }, 10));
	c.input.break_held = false;
	CHECK(run_until(both_sides, [&] { return inv.count_of("core:planks") == 4 && h.game().drops().drops().empty(); }, 5));

	// A door, opened; a bed, placed whole (one used).
	const BlockPos door{1, 100, 4};
	REQUIRE(c.hold("core:wood_door"));
	c.frame();
	aim_at_floor(c, door.x, door.z);
	c.input.use_pressed = true;
	CHECK(run_until(both_sides, [&] {
		return h.play->block(door.x, 101, door.z).ends_with("_upper") && c.play->block(door.x, 101, door.z).ends_with("_upper");
	}, 5));
	c.aim(door.x + 0.5f, 100.5f, door.z + 0.5f);
	c.input.use_pressed = true;
	CHECK(run_until(both_sides, [&] { return c.play->block(door).ends_with("_open") && h.play->block(door).ends_with("_open"); }, 5));
	const int beds = inv.count_of("core:bed");
	REQUIRE(c.hold("core:bed"));
	c.frame();
	aim_at_floor(c, 0, 2);
	c.input.use_pressed = true;
	CHECK(run_until(both_sides, [&] {
		int parts = 0;
		for (int x = -1; x <= 1; x++) {
			for (int z = 1; z <= 3; z++) {
				parts += h.play->block(x, 100, z).starts_with("core:bed") ? 1 : 0;
			}
		}
		return parts == 2 && inv.count_of("core:bed") == beds - 1;
	}, 5));

	// Nobody may be built into a block: not where the host stands.
	REQUIRE(c.hold("core:planks"));
	c.frame();
	const BlockPos host_cell{0, 100, 0};
	c.player().eye(eye);
	const float to_host[3] = {0.5f - eye[0], NetPlay::FLOOR_Y - 0.001f - eye[1], 0.5f - eye[2]};
	const float lh = std::sqrt(to_host[0] * to_host[0] + to_host[1] * to_host[1] + to_host[2] * to_host[2]);
	for (int k = 0; k < 3; k++) {
		dir[k] = to_host[k] / lh;
	}
	c.session.request_use(eye, dir, c.game().selected(), "", false);
	run_for(both_sides, 0.5f);
	CHECK(h.play->block(host_cell) == "core:air");

	// A teleport is put back; walking is fine.
	const float start[3] = {c.player().position[0], c.player().position[1], c.player().position[2]};
	c.player().position[0] += 200;
	CHECK(run_until(both_sides, [&] { return std::abs(c.player().position[0] - start[0]) < 2.0f; }, 5));
	REQUIRE(run_until(both_sides, [&] { return c.world().is_loaded(-14, 100, -14) && c.world().is_loaded(14, 100, 14); }, 30));
	walk(c, both_sides, -3.5f, 6.5f);
	run_for(both_sides, 0.5f);
	CHECK(std::abs(c.player().position[0] + 3.5f) < 0.2f);

	// Health is the host's: lava under our feet burns, the host says so.
	const BlockPos feet{int(std::floor(c.player().position[0])), 100, int(std::floor(c.player().position[2]))};
	h.world().set_block(feet, h.play->id("core:lava"));
	CHECK(run_until(both_sides, [&] { return c.game().health() < gc::Game::MAX_HEALTH; }, 5));
	h.world().set_block(feet, 0);

	// The host keeps what we have when we leave.
	client.reset();
	REQUIRE(run_until({&h}, [&] { return h.session.avatars().empty(); }, 5));
	const Json saved = h.session.saved_players();
	gc::Inventory kept(content);
	kept.deserialize(saved["Client"]["inventory"]);
	CHECK(kept.count_of("core:planks") == 4); // (placed one, mined it back)
	CHECK(kept.count_of("core:bed") == 2);
}

TEST_CASE("multiplayer: chests through the host") {
	auto host = NetPlay::host(test_port(2));
	auto client = NetPlay::join(test_port(2), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	const BlockPos chest{3, 100, 0};
	h.world().set_block(chest, h.play->id("core:chest"));
	h.game().block_entities().click(chest, 0, {"core:coal", 4});
	REQUIRE(run_until(both_sides, [&] { return c.play->block(chest) == "core:chest"; }, 20));
	// We open it (a right click), the host sends what's in it.
	c.aim(chest.x + 0.5f, chest.y + 0.5f, chest.z + 0.5f);
	c.input.use_pressed = true;
	c.frame();
	REQUIRE(c.game().take_opened().has_value());
	c.session.open_container(chest);
	CHECK(run_until(both_sides, [&] {
		const gc::BlockEntity *e = c.game().block_entities().at(chest);
		return e && e->slots[0] == gc::Stack{"core:coal", 4};
	}, 5));
	// Take the coal, put it in slot 1: the host does the same.
	c.game().inventory_action(Json::array({"container", chest.x, chest.y, chest.z, 0}));
	CHECK(c.game().inventory().cursor == gc::Stack{"core:coal", 4});
	c.game().inventory_action(Json::array({"container", chest.x, chest.y, chest.z, 1}));
	CHECK(run_until(both_sides, [&] {
		const gc::BlockEntity *e = h.game().block_entities().at(chest);
		return e && e->slots[1] == gc::Stack{"core:coal", 4} && e->slots[0].empty();
	}, 5));
	CHECK(c.game().inventory().cursor.empty());
	// What we hold is the host's say: nothing to put in now.
	c.game().inventory().cursor = {"core:diamond", 64};
	c.game().inventory_action(Json::array({"container", chest.x, chest.y, chest.z, 2}));
	run_for(both_sides, 0.5f);
	CHECK(h.game().block_entities().at(chest)->slots[2].empty());
	CHECK(c.game().inventory().cursor.empty()); // (the host's copy came back)
}

TEST_CASE("multiplayer: the host's creatures, and a client hitting one") {
	auto host = NetPlay::host(test_port(3));
	auto client = NetPlay::join(test_port(3), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	const float at[3] = {5.5f, 100.0f, 2.5f};
	const int id = h.game().creatures().spawn(h.play->content, "core:block_eater", at, "core:stone");
	REQUIRE(id > 0);
	CHECK(run_until(both_sides, [&] {
		const gc::Creature *seen = c.game().creatures().find(id);
		return seen && seen->variant == "core:stone";
	}, 5));
	const gc::Creature *mine = c.game().creatures().find(id);
	REQUIRE(mine);
	c.aim(mine->pos[0], mine->pos[1] + 0.5f, mine->pos[2]);
	c.input.break_pressed = true;
	CHECK(run_until(both_sides, [&] {
		const gc::Creature *real = h.game().creatures().find(id);
		return !real || real->flash > 0 || real->hurt_time > 0;
	}, 5));
}

TEST_CASE("multiplayer: the host turns players away (a name taken, other mods)") {
	auto host = NetPlay::host(test_port(4));
	std::string why;
	CHECK_FALSE(NetPlay::join(test_port(4), "Host", [&] { host->frame(); }, &why));
	CHECK(why.find("already playing") != std::string::npos);
	why.clear();
	CHECK_FALSE(NetPlay::join(test_port(4), "Other", [&] { host->frame(); }, &why, {"core@0.1.0#000000000000"}));
	CHECK(why.find("mismatch") != std::string::npos);
	CHECK(gc::Session::clean_name("  Bob\x01\xE2\x80\x8B Smith  ") == "Bob Smith");
	CHECK(gc::Session::clean_name("a very long name that goes on") == "a very long name");
}
