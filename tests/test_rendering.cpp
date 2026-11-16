// Milestone 6's rendering data, worked out on the CPU: item models (the
// Godot version's ItemDrop.model_mesh), the light a held torch spreads, the
// first-person arm's swing, and block shadows.

#include "content/content.h"
#include "content/item_models.h"
#include "game/hand.h"
#include "helpers.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <thread>

using gc::BlockPos;

namespace {

struct Models {
	gc::Content content;
	gc::BlockTable table;
	std::unique_ptr<gc::ItemModels> models;
	Models() {
		load_base_game(content);
		content.freeze();
		table = gc::build_block_table(content);
		models = std::make_unique<gc::ItemModels>(content, table);
	}
};

void extent(const std::vector<gc::ModelVertex> &m, int axis, float &lo, float &hi) {
	lo = 1e9f;
	hi = -1e9f;
	for (const auto &v : m) {
		lo = std::min(lo, v.pos[axis]);
		hi = std::max(hi, v.pos[axis]);
	}
}

// The base game and a world (seed 4242) loaded around the origin.
struct LoadedWorld {
	gc::Content content;
	gc::BlockTable table;
	gocube::Generator generator;
	std::unique_ptr<gc::World> world;
	LoadedWorld() {
		load_base_game(content);
		content.freeze();
		table = gc::build_block_table(content);
		gc::configure_generator(generator, content, 4242, gc::GENERATOR_VERSION);
		world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator);
		world->set_view_distance(32);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!(world->is_loaded(0, 0, 0) && world->stats().queued == 0)) {
			world->update(0.5f, 10.0f, 0.5f);
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	gocube::BlockId id(const char *name) const { return gocube::BlockId(content.block_index(name)); }
};

} // namespace

TEST_CASE("item models: a small cube for blocks (a slab half of it, a bed whole), items' icons with depth") {
	Models m;
	const auto &stone = m.models->model("core:stone");
	CHECK(stone.size() == 36); // 6 faces
	CHECK(m.models->is_block("core:stone"));
	float lo, hi;
	extent(stone, 1, lo, hi);
	CHECK(lo == doctest::Approx(-0.125f));
	CHECK(hi == doctest::Approx(0.125f));
	// A slab: its bottom half, standing on the cell's bottom.
	extent(m.models->model("core:stone_slab"), 1, lo, hi);
	CHECK(lo == doctest::Approx(-0.125f));
	CHECK(hi == doctest::Approx(0.0f));
	// A bed: both halves, 9/16 high, without the faces where they meet.
	const auto &bed = m.models->model("core:bed");
	CHECK(bed.size() == 60);
	CHECK(m.models->is_block("core:bed"));
	extent(bed, 1, lo, hi);
	CHECK(hi - lo == doctest::Approx(0.25f * 9.0f / 16.0f));
	// A torch (not solid) and a stick (an item): extruded icons, thin, 0.42 across at most.
	for (const char *item : {"core:torch", "core:stick"}) {
		const auto &model = m.models->model(item);
		CAPTURE(item);
		REQUIRE(!model.empty());
		CHECK(model.size() % 3 == 0);
		CHECK_FALSE(m.models->is_block(item));
		extent(model, 2, lo, hi);
		CHECK(hi == doctest::Approx(gc::ItemModels::ITEM_THICKNESS / 2));
		CHECK(lo == doctest::Approx(-gc::ItemModels::ITEM_THICKNESS / 2));
		extent(model, 0, lo, hi);
		CHECK(hi - lo <= gc::ItemModels::ITEM_SIZE + 1e-4f);
	}
	CHECK(&m.models->model("core:torch") == &m.models->model("core:torch")); // built once
}

TEST_CASE("a held torch lights the blocks around it, one level less a block, not through walls") {
	LoadedWorld w;
	// Open air around (0, 40, 0), with a stone wall at x = 3.
	for (int x = -4; x <= 4; x++) {
		for (int y = 37; y <= 43; y++) {
			for (int z = -4; z <= 4; z++) {
				REQUIRE(w.world->set_block({x, y, z}, x == 3 ? w.id("core:stone") : 0));
			}
		}
	}
	gc::HandLights lights;
	const float eye[3] = {0.5f, 40.5f, 0.5f};
	CHECK(lights.update(*w.world, {{0.5f, 40.5f, 0.5f, 14.0f}}, eye));
	CHECK_FALSE(lights.update(*w.world, {{0.7f, 40.2f, 0.9f, 14.0f}}, eye)); // the same block: nothing to redo
	auto level = [&](float x, float y, float z) {
		const float at[3] = {x, y, z};
		return lights.level_at(at);
	};
	CHECK(level(0.5f, 40.5f, 0.5f) == 14);
	CHECK(level(1.5f, 40.5f, 0.5f) == 13);
	CHECK(level(2.5f, 41.5f, 0.5f) == 11);
	CHECK(level(3.5f, 40.5f, 0.5f) == 0);  // the wall
	CHECK(level(4.5f, 40.5f, 0.5f) <= 6);  // behind it: only around (the long way)
	// A block changed: worked out again; no light: nothing.
	REQUIRE(w.world->set_block({1, 40, 0}, w.id("core:stone")));
	CHECK(lights.update(*w.world, {{0.5f, 40.5f, 0.5f, 14.0f}}, eye));
	CHECK(lights.update(*w.world, {}, eye));
	CHECK(lights.volume().empty());
	CHECK(level(0.5f, 40.5f, 0.5f) == 0);
}

TEST_CASE("light for items and the hand: the brighter of sky and block light, never below the least") {
	LoadedWorld w;
	for (int y = 60; y <= 70; y++) { // (open sky: nothing above)
		REQUIRE(w.world->set_block({0, y, 0}, 0));
	}
	gc::HandLights none;
	const float day[3] = {1, 1, 1}, night[3] = {0.02f * 0.6f, 0.02f * 0.7f, 0.02f};
	const float at[3] = {0.5f, 200.0f, 0.5f}; // above the world: full sky light
	float out[3];
	gc::light_for(*w.world, none, day, at, 0.07f, out);
	CHECK(out[0] == doctest::Approx(1.0f));
	gc::light_for(*w.world, none, night, at, 0.07f, out);
	CHECK(out[2] == doctest::Approx(0.07f)); // (never below `least`)
}

TEST_CASE("the arm swings once for each swing asked for, the next one after the last, and while mining") {
	gc::Hand hand;
	CHECK_FALSE(hand.swinging());
	hand.swing();
	CHECK(hand.swinging());
	hand.update(0.1f, false, false);
	hand.swing(); // during the first: plays right after it
	hand.update(0.25f, false, false);
	CHECK(hand.swinging());
	hand.update(0.31f, false, false);
	CHECK_FALSE(hand.swinging());
	hand.update(0.01f, true, false); // mining
	CHECK(hand.swinging());
	// A swing starts and ends at rest; the blow is in the middle.
	const auto rest = gc::Hand::swing_pose(0.0f), end = gc::Hand::swing_pose(1.0f), hit = gc::Hand::swing_pose(0.5f);
	for (int k = 0; k < 3; k++) {
		CHECK(rest.rotation[k] == doctest::Approx(0.0f));
		CHECK(end.rotation[k] == doctest::Approx(0.0f));
	}
	CHECK(hit.rotation[1] == doctest::Approx(0.32f));
	CHECK(hit.tilt[0] == doctest::Approx(-0.8f));
}

TEST_CASE("block shadows: faces the sun can't reach are marked, and moving the sun remeshes") {
	LoadedWorld w;
	w.world->take_meshes(); // (the meshes from before: not looked at)
	// A pillar on a stone floor high up, in open sky; the sun low in the east
	// (+x): the floor just west of the pillar is in its shadow, east of it isn't.
	for (int x = -6; x <= 6; x++) {
		for (int z = -2; z <= 2; z++) {
			for (int y = 60; y <= 70; y++) {
				REQUIRE(w.world->set_block({x, y, z}, y == 60 ? w.id("core:stone") : 0));
			}
		}
	}
	for (int y = 61; y <= 66; y++) {
		REQUIRE(w.world->set_block({0, y, 0}, w.id("core:stone")));
	}
	w.world->set_sun_angle(30.0f * 3.14159265f / 180.0f);
	w.world->set_block_shadows(true);
	// The top faces of the floor blocks at (x, 60, 0): their alpha marks (0.75: in shadow).
	std::map<int, float> alpha;
	auto top_alpha = [&](int x) {
		for (int tries = 0; tries < 2000 && !alpha.count(x); tries++) {
			w.world->update(0.5f, 10.0f, 0.5f);
			for (const gc::ChunkMesh &mesh : w.world->take_meshes()) {
				const gocube::Surface &s = mesh.opaque;
				const float ox = float(mesh.pos.x * gocube::CHUNK), oy = float(mesh.pos.y * gocube::CHUNK),
							oz = float(mesh.pos.z * gocube::CHUNK);
				for (size_t v = 0; v + 3 < s.positions.size() / 3; v += 4) {
					const float *p = &s.positions[v * 3];
					const float *n = &s.normals[v * 3];
					// A quad's corners: the face above (x, 60, 0) spans x..x+1 at y 61.
					float mnx = 1e9f, mnz = 1e9f;
					bool flat = true;
					for (int c = 0; c < 4; c++) {
						mnx = std::min(mnx, ox + p[c * 3]);
						mnz = std::min(mnz, oz + p[c * 3 + 2]);
						flat = flat && oy + p[c * 3 + 1] == 61.0f;
					}
					if (n[1] > 0.5f && flat && mnz == 0.0f) {
						alpha[int(mnx)] = s.colors[v * 4 + 3]; // (the newest mesh's)
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return alpha.count(x) ? alpha[x] : -1.0f;
	};
	CHECK(top_alpha(-2) == doctest::Approx(0.75f)); // in the pillar's shadow
	CHECK(top_alpha(2) == doctest::Approx(1.0f));   // in the sun
}
