// The player's movement against the world: standing, falling, jumping, walls,
// and breaking and placing blocks (seed 4242, generated, nothing saved).

#include "content/content.h"
#include "helpers.h"
#include "game/blocks.h"
#include "game/player.h"
#include "world/world.h"

#include "check.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

using gc::BlockPos;

namespace {

struct TestWorld {
	gc::Content content;
	gc::BlockTable table;
	gocube::Generator generator;
	std::unique_ptr<gc::World> world;
	std::unique_ptr<gc::BlockRules> rules;

	TestWorld() {
		load_base_game(content);
		content.freeze();
		table = gc::build_block_table(content);
		rules = std::make_unique<gc::BlockRules>(content);
		gc::configure_generator(generator, content, 4242);
		world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator);
		world->set_view_distance(32);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!(world->is_loaded(0, 0, 0) && world->stats().queued == 0)) {
			world->update(0.5f, 10.0f, 0.5f);
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		// A stone floor high above the land (whatever is at the origin), open above.
		for (int x = -8; x <= 8; x++) {
			for (int z = -8; z <= 8; z++) {
				for (int y = FLOOR_Y - 2; y <= FLOOR_Y; y++) { // (three thick: digging one leaves a floor)
					REQUIRE(world->set_block({x, y, z}, id("core:stone")));
				}
				for (int y = FLOOR_Y + 1; y <= FLOOR_Y + 6; y++) {
					REQUIRE(world->set_block({x, y, z}, 0));
				}
			}
		}
	}
	static constexpr int FLOOR_Y = 110;
	gocube::BlockId id(const char *name) const { return gocube::BlockId(content.block_index(name)); }
	// The top solid block of a column (the ground, after edits).
	int ground(int x, int z) const {
		for (int y = 127; y > -96; y--) {
			if (table.blocks[world->block_at(x, y, z)].solid) {
				return y;
			}
		}
		return -96;
	}
	void run(gc::Player &p, const gc::PlayerInput &in, int steps) {
		for (int i = 0; i < steps; i++) {
			p.step(in, *world, *rules);
		}
	}
};

} // namespace

TEST_CASE("the player stands on the ground, falls into holes and jumps about 1.26 blocks") {
	TestWorld w;
	// A flat spot: a column whose four neighbours are as high.
	int fx = 0, fz = 0, h = 0;
	bool found = false;
	for (int x = -20; x < 20 && !found; x++) {
		for (int z = -20; z < 20 && !found; z++) {
			const int g = w.ground(x, z);
			found = g > 0 && w.ground(x + 1, z) == g && w.ground(x - 1, z) == g && w.ground(x, z + 1) == g &&
					w.ground(x, z - 1) == g;
			fx = x, fz = z, h = g;
		}
	}
	REQUIRE(found);
	gc::Player p;
	p.position[0] = fx + 0.5f;
	p.position[1] = float(h) + 3.0f; // dropped from 2 blocks up
	p.position[2] = fz + 0.5f;
	w.run(p, {}, 120);
	CHECK(p.position[1] == check::Approx(h + 1.0f).epsilon(0.002));
	CHECK(p.on_floor);

	// Jump: the highest point.
	gc::PlayerInput jump;
	jump.jump = true;
	p.step(jump, *w.world, *w.rules);
	float peak = p.position[1];
	for (int i = 0; i < 60; i++) {
		p.step({}, *w.world, *w.rules);
		peak = std::max(peak, p.position[1]);
	}
	CHECK(peak - (h + 1.0f) == check::Approx(1.26f).epsilon(0.05));
	CHECK(p.position[1] == check::Approx(h + 1.0f).epsilon(0.002)); // back down

	// Dig the block under the player: it falls one block, no further.
	REQUIRE(w.world->set_block({fx, h, fz}, w.id("core:air")));
	w.run(p, {}, 60);
	CHECK(p.position[1] == check::Approx(w.ground(fx, fz) + 1.0f).epsilon(0.002));
}

TEST_CASE("walls stop the player, and placing never puts a block inside it") {
	TestWorld w;
	const int h = w.ground(0, 0);
	gc::Player p;
	p.position[0] = 0.5f;
	p.position[1] = float(h) + 1.0f;
	p.position[2] = 0.5f;
	p.flying = true; // (unaffected by the ground's shape)
	// A clear path, then a wall two blocks ahead (towards -z), three high.
	for (int y = h + 1; y <= h + 3; y++) {
		for (int x = -1; x <= 1; x++) {
			for (int z = -1; z <= 1; z++) {
				REQUIRE(w.world->set_block({x, y, z}, w.id("core:air")));
			}
		}
	}
	for (int y = h + 1; y <= h + 3; y++) {
		for (int x = -2; x <= 2; x++) {
			REQUIRE(w.world->set_block({x, y, -2}, w.id("core:stone")));
		}
	}
	gc::PlayerInput forward;
	forward.forward = 1;
	w.run(p, forward, 120);
	CHECK(p.position[2] == check::Approx(-1.0f + gc::Player::RADIUS).epsilon(0.01)); // touching the wall

	// Looking straight down: placing into its own cell is refused.
	p.pitch = -1.55f;
	p.flying = false;
	w.run(p, {}, 60);
	const BlockPos feet{0, int(std::floor(p.position[1])), int(std::floor(p.position[2]))};
	CHECK(gc::Player::overlaps_block(p.position, feet));
	const gocube::BlockId before = w.world->block_at(feet.x, feet.y, feet.z);
	p.use_or_place(*w.world, *w.rules, "core:stone", false);
	CHECK(w.world->block_at(feet.x, feet.y, feet.z) == before);
}

TEST_CASE("doors are placed whole, open and close, and break whole") {
	TestWorld w;
	const int h = w.ground(0, 0);
	gc::Player p;
	p.position[0] = 0.5f;
	p.position[1] = float(h) + 1.0f;
	p.position[2] = 3.5f;
	// Make a floor and clear the air in front, then look at the floor two blocks ahead.
	for (int z = -1; z <= 3; z++) {
		REQUIRE(w.world->set_block({0, h, z}, w.id("core:stone")));
		for (int y = h + 1; y <= h + 3; y++) {
			REQUIRE(w.world->set_block({0, y, z}, w.id("core:air")));
		}
	}
	p.yaw = 0; // towards -z
	p.pitch = -std::atan2(1.62f, 2.0f); // at the floor's top two blocks ahead
	REQUIRE(p.use_or_place(*w.world, *w.rules, "core:wood_door", false));
	const int door_z = 1;
	CHECK(w.content.block_ids()[w.world->block_at(0, h + 1, door_z)] == "core:wood_door");
	CHECK(w.content.block_ids()[w.world->block_at(0, h + 2, door_z)] == "core:wood_door_n_upper");

	p.pitch = -0.2f; // at the door
	REQUIRE(p.use_or_place(*w.world, *w.rules, "core:stone", false)); // opens it
	CHECK(w.content.block_ids()[w.world->block_at(0, h + 1, door_z)] == "core:wood_door_n_lower_open");
	CHECK(w.content.block_ids()[w.world->block_at(0, h + 2, door_z)] == "core:wood_door_n_upper_open");
	// Open, it swings against the side of its cell (x 0..3/16): aim at it there.
	p.position[0] = 0.09f;
	REQUIRE(p.use_or_place(*w.world, *w.rules, "core:stone", false)); // closes it
	CHECK(w.content.block_ids()[w.world->block_at(0, h + 1, door_z)] == "core:wood_door");
	CHECK(w.content.block_ids()[w.world->block_at(0, h + 2, door_z)] == "core:wood_door_n_upper");

	p.position[0] = 0.5f;
	REQUIRE(p.break_target(*w.world, *w.rules));
	CHECK(w.world->block_at(0, h + 1, door_z) == 0);
	CHECK(w.world->block_at(0, h + 2, door_z) == 0);
}
