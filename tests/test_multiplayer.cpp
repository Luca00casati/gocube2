// Multiplayer: a host and a client in one process, over TCP and UDP on localhost.
// The client is driven as a person would (aim, click, walk); the host checks
// what it asks and does it as that player. Both worlds must agree after.
// (tests/mp_smoke.cpp does a round of play across two real processes.)

#include "net_play.h"

#include <doctest/doctest.h>

#include "content/skin.h"
#include "mods/mod_pack.h"

#include "platform/compression.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <process.h>

using gc::BlockPos;
using gc::Data;

namespace {

int test_port(int n) {
	return 26000 + (_getpid() % 1000) * 8 + n;
}

// Both worlds have this block at p.
bool both(NetPlay &a, NetPlay &b, const BlockPos &p, const std::string &id) {
	return a.play->block(p) == id && b.play->block(p) == id;
}

// A survival inventory as the host keeps it for a player who played before.
Data saved_inventory(const gc::Content &content, const std::vector<gc::Stack> &stacks) {
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
	// Positions go by UDP (checked both ways first).
	CHECK(run_until(both_sides, [&] { return c.session.udp_ready() && h.session.udp_ready(); }, 5));

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

TEST_CASE("multiplayer: with UDP blocked, everything goes over TCP") {
	auto host = NetPlay::host(test_port(6), true);
	auto client = NetPlay::join(test_port(6), "Client", [&] { host->frame(); }, nullptr, {}, {}, false);
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	REQUIRE(run_until(both_sides, [&] { return c.world().is_loaded(4, 100, 1); }, 20));
	// Each sees the other move.
	REQUIRE(run_until(both_sides, [&] { return c.session.avatars().size() == 1 && h.session.avatars().size() == 1; }, 5));
	h.player().position[0] = 3.5f;
	CHECK(run_until(both_sides, [&] { return std::abs(c.session.avatars()[0].eye[0] - 3.5f) < 0.01f; }, 5));
	CHECK_FALSE(c.session.udp_ready());
	CHECK_FALSE(h.session.udp_ready());
	// A change goes through the host.
	const BlockPos cell{4, 100, 1};
	c.game().inventory().slots[0] = {"core:planks", 1};
	c.input.number_key = 1;
	c.frame();
	aim_at_floor(c, cell.x, cell.z);
	c.input.use_pressed = true;
	CHECK(run_until(both_sides, [&] { return both(h, c, cell, "core:planks"); }, 5));
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

	// Stone too, by hand (there are no tools): it gives cobblestone.
	h.world().set_block(cell, h.play->id("core:stone"));
	REQUIRE(run_until(both_sides, [&] { return c.play->block(cell) == "core:stone"; }, 5));
	c.aim(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
	c.input.break_held = true;
	CHECK(run_until(both_sides, [&] { return both(h, c, cell, "core:air"); }, 10));
	c.input.break_held = false;
	CHECK(run_until(both_sides, [&] { return inv.count_of("core:cobblestone") == 1; }, 5));

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
	// Out of it, health comes back (the host heals us, a point every 2.5 s after 4 s).
	run_for(both_sides, 1.0f);
	const int burnt = c.game().health();
	CHECK(run_until(both_sides, [&] { return c.game().health() > burnt; }, 9));

	// The host keeps what we have when we leave.
	client.reset();
	REQUIRE(run_until({&h}, [&] { return h.session.avatars().empty(); }, 5));
	const Data saved = h.session.saved_players();
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
	c.game().inventory_action(Data::array({"container", chest.x, chest.y, chest.z, 0}));
	CHECK(c.game().inventory().cursor == gc::Stack{"core:coal", 4});
	c.game().inventory_action(Data::array({"container", chest.x, chest.y, chest.z, 1}));
	CHECK(run_until(both_sides, [&] {
		const gc::BlockEntity *e = h.game().block_entities().at(chest);
		return e && e->slots[1] == gc::Stack{"core:coal", 4} && e->slots[0].empty();
	}, 5));
	CHECK(c.game().inventory().cursor.empty());
	// What we hold is the host's say: nothing to put in now.
	c.game().inventory().cursor = {"core:diamond", 64};
	c.game().inventory_action(Data::array({"container", chest.x, chest.y, chest.z, 2}));
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

TEST_CASE("multiplayer: a survival client falling out of the world is hurt by the host") {
	auto host = NetPlay::host(test_port(7));
	auto client = NetPlay::join(test_port(7), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	REQUIRE(run_until(both_sides, [&] { return c.world().is_loaded(8, 100, 8); }, 20));
	walk(c, both_sides, 8.5f, 8.5f);
	run_for(both_sides, 0.3f);
	// The host opens a shaft under us, down through the world's bottom.
	for (int y = int(NetPlay::FLOOR_Y) - 1; y >= gc::World::BOTTOM_Y; y--) {
		for (int x = 7; x <= 9; x++) {
			for (int z = 7; z <= 9; z++) {
				h.world().set_block({x, y, z}, 0);
			}
		}
	}
	// We fall down it (as the game's physics would, at 40 m/s: the harness
	// moves players by hand), out of the world: the host hurts us.
	auto last = std::chrono::steady_clock::now();
	CHECK(run_until(both_sides, [&] {
		const auto now = std::chrono::steady_clock::now();
		c.player().position[1] -= 40.0f * std::chrono::duration<float>(now - last).count();
		last = now;
		return c.player().position[1] < float(gc::World::BOTTOM_Y) && c.game().health() < gc::Game::MAX_HEALTH;
	}, 20));
}

TEST_CASE("multiplayer: everyone sees each player's skin, through the host") {
	const std::vector<uint8_t> red(gc::SKIN_BYTES, 200), blue(gc::SKIN_BYTES, 40), green(gc::SKIN_BYTES, 120);
	auto host = NetPlay::host(test_port(0));
	host->session.set_skin(red);
	auto client = NetPlay::join(test_port(0), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	c.session.set_skin(blue);
	const std::vector<NetPlay *> both_sides = {&h, &c};
	const int client_id = c.session.my_id();
	CHECK(run_until(both_sides, [&] {
		const auto *theirs = h.session.skin_of(client_id), *hosts = c.session.skin_of(gc::Session::HOST_ID);
		return theirs && *theirs == blue && hosts && *hosts == red;
	}, 5));
	// A second player gets both; the first gets the second's.
	auto second = NetPlay::join(test_port(0), "Second", [&] { h.frame(); c.frame(); });
	REQUIRE(second);
	NetPlay &s = *second;
	s.session.set_skin(green);
	const std::vector<NetPlay *> all = {&h, &c, &s};
	CHECK(run_until(all, [&] {
		const auto *first = s.session.skin_of(client_id), *hosts = s.session.skin_of(gc::Session::HOST_ID);
		const auto *seconds = c.session.skin_of(s.session.my_id());
		return first && *first == blue && hosts && *hosts == red && seconds && *seconds == green;
	}, 5));
	// One that isn't a skin is none; the host keeps the one it had.
	const int changes = h.session.skins_changed();
	s.session.set_skin(std::vector<uint8_t>(100, 1));
	run_for(all, 0.5f);
	CHECK(h.session.skins_changed() == changes);
	// Gone when the player leaves.
	const int second_id = s.session.my_id();
	second.reset();
	CHECK(run_until({&h, &c}, [&] { return !h.session.skin_of(second_id); }, 5));
}

TEST_CASE("multiplayer: chat and commands through the host") {
	auto host = NetPlay::host(test_port(1));
	auto client = NetPlay::join(test_port(1), "Client", [&] { host->frame(); });
	REQUIRE(client);
	NetPlay &h = *host, &c = *client;
	const std::vector<NetPlay *> both_sides = {&h, &c};
	std::vector<std::string> host_lines, client_lines;
	auto collect = [&] {
		for (std::string &l : h.session.take_chat()) {
			host_lines.push_back(std::move(l));
		}
		for (std::string &l : c.session.take_chat()) {
			client_lines.push_back(std::move(l));
		}
	};
	auto seen = [](const std::vector<std::string> &lines, const std::string &part) {
		return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) { return l.find(part) != std::string::npos; });
	};
	auto wait_for = [&](std::vector<std::string> &lines, const std::string &part) {
		return run_until(both_sides, [&] {
			collect();
			return seen(lines, part);
		}, 5);
	};

	// A line goes to everyone, with the player's name.
	c.session.chat("hello all");
	CHECK(wait_for(host_lines, "<Client> hello all"));
	CHECK(wait_for(client_lines, "<Client> hello all"));
	h.session.chat("hi Client");
	CHECK(wait_for(client_lines, "<Host> hi Client"));

	// A command: the host runs it as the client; not an operator, it's refused.
	gc::Inventory &inv = c.game().inventory();
	const int torches = inv.count_of("core:torch");
	c.session.chat("/give Client core:torch 3");
	CHECK(wait_for(client_lines, "Only operators may use /give"));
	run_for(both_sides, 0.3f);
	CHECK(inv.count_of("core:torch") == torches);
	// The host makes it one: now its /give works (the host keeps its survival inventory).
	h.session.chat("/op Client");
	c.session.chat("/give Client core:torch 3");
	CHECK(wait_for(client_lines, "Gave 3 core:torch to Client"));
	CHECK(run_until(both_sides, [&] { return inv.count_of("core:torch") == torches + 3; }, 5));
	// /tp: the client goes where the host says.
	c.session.chat("/tp ~ ~ ~-4");
	const float z0 = c.player().position[2];
	CHECK(run_until(both_sides, [&] { return std::abs(c.player().position[2] - (z0 - 4.0f)) < 0.3f; }, 5));
	// /spawnpoint reaches the client too.
	c.session.chat("/spawnpoint");
	CHECK(run_until(both_sides, [&] {
		const auto &spawn = c.game().world_spawn();
		return spawn && std::abs((*spawn)[2] - c.player().position[2]) < 0.5f;
	}, 5));
	// /msg: only that player sees it.
	host_lines.clear();
	client_lines.clear();
	h.session.chat("/msg Client psst");
	CHECK(wait_for(client_lines, "[Host -> you] psst"));
	CHECK_FALSE(seen(client_lines, "<Host>"));

	// /gamemode: for everyone; each mode keeps its own inventory.
	run_for(both_sides, 3.0f); // (the chat allowance comes back)
	c.session.chat("/gamemode creative");
	CHECK(wait_for(host_lines, "The game is now in creative (Client)"));
	CHECK(run_until(both_sides, [&] { return c.game().creative() && h.game().creative(); }, 5));
	CHECK(inv.count_of("core:torch") <= 1); // (the palette: one of each block)
	CHECK(c.game().health() == gc::Game::MAX_HEALTH);
	h.session.chat("/gamemode survival");
	CHECK(run_until(both_sides, [&] { return !c.game().creative() && !h.game().creative(); }, 5));
	CHECK(run_until(both_sides, [&] { return inv.count_of("core:torch") == torches + 3; }, 5)); // (back, from the host)
	c.session.chat("/gamemode");
	CHECK(wait_for(client_lines, "The game is in survival."));

	// Flooding: the host takes a few lines at once (6), then about two a second.
	run_for(both_sides, 3.0f); // (what the lines above used comes back)
	collect();
	host_lines.clear();
	for (int i = 0; i < 30; i++) {
		c.session.chat("spam " + std::to_string(i));
	}
	run_for(both_sides, 1.0f);
	collect();
	const auto spam = std::count_if(host_lines.begin(), host_lines.end(), [](const std::string &l) { return l.find("spam") != std::string::npos; });
	CHECK(spam >= 5);
	CHECK(spam <= 9);
}

TEST_CASE("multiplayer: a player with other mods downloads the host's and joins with them") {
	// A mod only the host has (made here), with a file in a folder of its own.
	const std::filesystem::path root = std::filesystem::temp_directory_path() / ("gocube_test_" + std::to_string(_getpid()));
	const std::filesystem::path hosts = root / "host_mods", cache = root / "cache";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(hosts / "visitor" / "data");
	std::ofstream(hosts / "visitor" / "mod.lua") << "return {id = \"visitor\", version = \"1.2.0\", entry = \"main.lua\"}\n";
	std::ofstream(hosts / "visitor" / "main.lua") << "registry.register_item(\"token\", {max_stack = 8})\n";
	std::ofstream(hosts / "visitor" / "data" / "notes.txt") << "kept as it is\n";
	auto host = NetPlay::host(test_port(5), false, "Host", {hosts});
	const std::vector<std::string> host_mods = gc::Session::mod_list(host->play->mods.loaded());
	REQUIRE(host_mods.size() == 2);

	// We have the base game only: the host offers its mods; we lack one.
	gc::Session s;
	gc::Content c;
	gc::Mods m(c);
	m.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"});
	REQUIRE(s.join("127.0.0.1", test_port(5), "Visitor", gc::Session::mod_list(m.loaded())).empty());
	REQUIRE(run_until({host.get()}, [&] {
		s.poll();
		return s.mods_offered() || !s.ended().empty();
	}, 10));
	CHECK(s.host_mods() == host_mods);
	REQUIRE(s.missing_mods().size() == 1);
	const std::string entry = s.missing_mods().begin()->first;
	CHECK(entry.rfind("visitor@1.2.0#", 0) == 0);
	s.download_mods(cache);
	CHECK(run_until({host.get()}, [&] {
		s.poll();
		return s.mods_ready() || !s.ended().empty();
	}, 10));
	CHECK(s.ended().empty());
	// It's there, exactly (the same fingerprint), with its folder.
	const auto copies = gc::mod_copies({cache});
	REQUIRE(copies.count(entry) == 1);
	CHECK(std::filesystem::exists(copies.at(entry) / "data" / "notes.txt"));
	s.leave();

	// Again, with the host's mods: in.
	auto client = NetPlay::join(test_port(5), "Visitor", [&] { host->frame(); }, nullptr, host_mods, {cache});
	REQUIRE(client);
	CHECK(client->play->content.items.count("visitor:token") == 1);
	client.reset();
	host.reset();
	std::filesystem::remove_all(root);
}

TEST_CASE("mod packs: only plain paths inside the folder, nothing written if one isn't") {
	const std::filesystem::path root = std::filesystem::temp_directory_path() / ("gocube_pack_" + std::to_string(_getpid()));
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "a" / "sub");
	std::ofstream(root / "a" / "mod.lua") << "return {id = \"a\"}\n";
	std::ofstream(root / "a" / "sub" / "x.txt") << "x";
	std::ofstream(root / "a" / ".hidden") << "not sent";
	const std::vector<uint8_t> pack = gc::pack_mod(root / "a");
	REQUIRE(!pack.empty());
	CHECK(gc::unpack_mod(pack, root / "b").empty());
	CHECK(gc::mod_fingerprint(root / "a") == gc::mod_fingerprint(root / "b"));
	CHECK_FALSE(std::filesystem::exists(root / "b" / ".hidden"));
	CHECK(gc::unpack_mod(pack, root / "b") == "its folder is already there");
	// A pack with a path out of its folder (made by hand) is refused, and nothing is written.
	auto make = [](const std::string &path) {
		std::vector<uint8_t> raw;
		auto put = [&](uint32_t v) {
			for (int i = 0; i < 4; i++) {
				raw.push_back(uint8_t(v >> (8 * i)));
			}
		};
		put(1);
		put(uint32_t(path.size()));
		raw.insert(raw.end(), path.begin(), path.end());
		put(1);
		raw.push_back('!');
		std::vector<uint8_t> out(4);
		const uint32_t n = uint32_t(raw.size());
		std::memcpy(out.data(), &n, 4);
		const std::vector<uint8_t> packed = gc::compress_bytes(raw);
		out.insert(out.end(), packed.begin(), packed.end());
		return out;
	};
	for (const char *bad : {"../evil.txt", "sub/../../evil.txt", "C:/evil.txt", "/evil.txt", "a\\..\\evil.txt",
				"con.txt", ".git/config", "sub//x"}) {
		CAPTURE(bad);
		CHECK_FALSE(gc::unpack_mod(make(bad), root / "c").empty());
		CHECK_FALSE(std::filesystem::exists(root / "c"));
	}
	CHECK_FALSE(std::filesystem::exists(root / "evil.txt"));
	CHECK(gc::unpack_mod(make("ok/fine.txt"), root / "c").empty());
	std::vector<uint8_t> damaged = pack;
	damaged.resize(damaged.size() / 2);
	CHECK_FALSE(gc::unpack_mod(damaged, root / "d").empty());
	std::filesystem::remove_all(root);
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

TEST_CASE("compression (Windows' MSZIP): both ways, and nothing but the exact size comes out") {
	std::vector<uint8_t> raw;
	for (int i = 0; i < 200000; i++) {
		raw.push_back(uint8_t(i % 7 == 0 ? i * 31 : 'a' + i % 3));
	}
	const std::vector<uint8_t> packed = gc::compress_bytes(raw);
	REQUIRE_FALSE(packed.empty());
	CHECK(packed.size() < raw.size() / 4);
	std::vector<uint8_t> out;
	REQUIRE(gc::decompress_bytes(packed.data(), packed.size(), raw.size(), out));
	CHECK(out == raw);
	CHECK_FALSE(gc::decompress_bytes(packed.data(), packed.size(), raw.size() - 1, out)); // more than allowed
	CHECK(out.empty());
	CHECK_FALSE(gc::decompress_bytes(packed.data(), packed.size(), raw.size() + 1, out)); // less than said
	CHECK_FALSE(gc::decompress_bytes(packed.data(), packed.size() / 2, raw.size(), out)); // cut short
	std::vector<uint8_t> junk(1000, 0x5a);
	CHECK_FALSE(gc::decompress_bytes(junk.data(), junk.size(), 5000, out));
	CHECK_FALSE(gc::decompress_bytes(packed.data(), 0, raw.size(), out));
}
