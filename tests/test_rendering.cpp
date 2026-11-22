// Rendering data worked out on the CPU: item models, the light a held torch
// spreads, the first-person arm, block shadows, skins, the profiler.

#include "content/content.h"
#include "content/item_models.h"
#include "content/skin.h"
#include "game/creature_models.h"
#include "game/figure.h"
#include "profiler.h"
#include "game/hand.h"
#include "helpers.h"
#include "world/world.h"

#include "check.h"

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
		load_base_game(content, true); // (with the kit's slab)
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
		gc::configure_generator(generator, content, 4242);
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
	CHECK(lo == check::Approx(-0.125f));
	CHECK(hi == check::Approx(0.125f));
	// A slab: its bottom half, standing on the cell's bottom.
	extent(m.models->model("kit:slab"), 1, lo, hi);
	CHECK(lo == check::Approx(-0.125f));
	CHECK(hi == check::Approx(0.0f));
	// A bed: both halves, 9/16 high, without the faces where they meet.
	const auto &bed = m.models->model("core:bed");
	CHECK(bed.size() == 60);
	CHECK(m.models->is_block("core:bed"));
	extent(bed, 1, lo, hi);
	CHECK(hi - lo == check::Approx(0.25f * 9.0f / 16.0f));
	// A torch (not solid) and coal (an item): extruded icons, thin, 0.42 across at most.
	for (const char *item : {"core:torch", "core:coal"}) {
		const auto &model = m.models->model(item);
		CAPTURE(item);
		REQUIRE(!model.empty());
		CHECK(model.size() % 3 == 0);
		CHECK_FALSE(m.models->is_block(item));
		extent(model, 2, lo, hi);
		CHECK(hi == check::Approx(gc::ItemModels::ITEM_THICKNESS / 2));
		CHECK(lo == check::Approx(-gc::ItemModels::ITEM_THICKNESS / 2));
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
	CHECK(out[0] == check::Approx(1.0f));
	gc::light_for(*w.world, none, night, at, 0.07f, out);
	CHECK(out[2] == check::Approx(0.07f)); // (never below `least`)
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
		CHECK(rest.rotation[k] == check::Approx(0.0f));
		CHECK(end.rotation[k] == check::Approx(0.0f));
	}
	CHECK(hit.rotation[1] == check::Approx(0.32f));
	CHECK(hit.tilt[0] == check::Approx(-0.8f));
}

TEST_CASE("the first-person arm is the skin's right arm, held forward, its hand where the item is") {
	gc::Hand hand;
	hand.update(0.0f, false, false);
	gc::Content content;
	load_base_game(content);
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	gc::ItemModels models(content, table);
	const float light[3] = {1, 1, 1};
	std::vector<gc::WorldVertex> arm, held;
	std::vector<gc::CreatureVertex> skinned;
	hand.build(models, "", light, arm, held, &skinned, 8);
	CHECK(arm.empty()); // (no plain sleeve)
	REQUIRE(!skinned.empty());
	float nearest = 1e9f;
	for (const gc::CreatureVertex &v : skinned) {
		CHECK(v.skin[2] == 8.0f);
		const float u = v.skin[0] * 64, w = v.skin[1] * 64;
		CHECK(((u >= 40 && u <= 56 && w >= 16 && w <= 32) || (u >= 40 && u <= 56 && w >= 32 && w <= 48))); // arm or sleeve
		CHECK(v.pos[0] > 0); // on the right
		nearest = std::min(nearest, v.pos[2]);
	}
	CHECK(nearest < -0.7f); // reaching forward to the hand
	// Without a skin: the plain sleeve and hand.
	hand.build(models, "", light, arm, held);
	CHECK(!arm.empty());
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
	CHECK(top_alpha(-2) == check::Approx(0.75f)); // in the pillar's shadow
	CHECK(top_alpha(2) == check::Approx(1.0f));   // in the sun
}

TEST_CASE("player skins: the usual layout, 64x64 or the older 64x32; the figure wears it") {
	// The base game's skin.
	gc::Image image, skin;
	REQUIRE(gc::read_png(std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods" / "core" / "textures" / "player.png", image));
	REQUIRE(gc::to_skin(image, skin));
	CHECK(skin.rgba.size() == gc::SKIN_BYTES);
	auto alpha = [&](const gc::Image &im, int x, int y) { return im.rgba[(size_t(y) * size_t(im.width) + size_t(x)) * 4 + 3]; };
	CHECK(alpha(skin, 12, 12) == 255); // the face
	CHECK(alpha(skin, 40, 4) == 0);    // no hat (magenta)

	// The older layout: 64x32, its left leg and arm copied from the right ones, mirrored.
	gc::Image old;
	old.width = 64;
	old.height = 32;
	old.rgba.assign(64 * 32 * 4, 255);
	for (int y = 20; y < 32; y++) { // the right leg's front: a mark in its outer column (x 4 of 4..7)
		old.rgba[(size_t(y) * 64 + 4) * 4 + 0] = 10;
	}
	REQUIRE(gc::to_skin(old, skin));
	CHECK(skin.height == 64);
	auto red = [&](int x, int y) { return skin.rgba[(size_t(y) * 64 + size_t(x)) * 4]; };
	CHECK(red(20 + 3, 52 + 5) == 10); // the left leg's front (20..23, 52..63): the mark on the other side
	CHECK(red(20 + 0, 52 + 5) == 255);
	gc::Image odd;
	odd.width = odd.height = 32;
	odd.rgba.assign(32 * 32 * 4, 0);
	CHECK_FALSE(gc::to_skin(odd, skin));

	// Where faces are: the head's front is 8..16 x 8..16; an upper arm is the arm's first 6 rows.
	float r[4];
	gc::skin_face(gc::skin_parts::HEAD, 5, r);
	CHECK((r[0] == 8 && r[1] == 8 && r[2] == 8 && r[3] == 8));
	gc::skin_face(gc::skin_parts::RIGHT_ARM, 5, r, 0, 6);
	CHECK((r[0] == 44 && r[1] == 20 && r[2] == 4 && r[3] == 6));
	gc::skin_face(gc::skin_parts::RIGHT_ARM, 3, r);
	CHECK((r[0] == 48 && r[1] == 16 && r[2] == 4 && r[3] == 4)); // the bottom

	// The figure in skin slot 3: every face from that skin, inside it; 1.8 m tall.
	gc::Figure f;
	const float eye[3] = {0, 1.62f, 0}, light[3] = {1, 1, 1};
	float hand[16];
	std::vector<gc::CreatureVertex> mesh;
	gc::build_player(f, 2, eye, 0, light, mesh, hand, 3);
	REQUIRE(!mesh.empty());
	float low = 1e9f, high = -1e9f;
	bool face_seen = false;
	for (const gc::CreatureVertex &v : mesh) {
		CHECK(v.skin[2] == 3.0f);
		CHECK((v.skin[0] >= 0 && v.skin[0] <= 1 && v.skin[1] >= 0 && v.skin[1] <= 1));
		low = std::min(low, v.pos[1]);
		high = std::max(high, v.pos[1]);
		// The front of the head (facing -z, at the top): from the face's pixels.
		if (v.normal[2] < -0.5f && v.pos[1] > 1.4f && v.pos[2] < -0.2f && v.pos[2] > -0.24f) {
			face_seen = true;
			CHECK((v.skin[0] * 64 >= 8 && v.skin[0] * 64 <= 16 && v.skin[1] * 64 >= 8 && v.skin[1] * 64 <= 16));
		}
	}
	CHECK(face_seen);
	CHECK((low > -0.03f && low < 0.01f)); // the feet (the trousers' outer layer a little lower)
	CHECK(high == check::Approx(1.8f + 0.03f).epsilon(0.02)); // (the hat, a pixel bigger)
	// Without a skin: the coloured figure, no skin.
	mesh.clear();
	gc::build_player(f, 2, eye, 0, light, mesh, hand);
	REQUIRE(!mesh.empty());
	CHECK(mesh[0].skin[2] == -1.0f);
}

TEST_CASE("the profiler: a frame's times added up, smoothed, the worst of the last frames; nested timers under") {
	gc::Profiler p;
	for (int i = 0; i < 10; i++) {
		p.begin_frame();
		p.add("draw", 4.0);
		p.add("draw", 1.0, 0); // (twice in a frame: added up)
		p.add("draw: terrain", i == 5 ? 30.0 : 3.0, 1);
	}
	p.begin_frame();
	REQUIRE(p.entries().size() == 2);
	const gc::Profiler::Entry &draw = p.entries()[0], &terrain = p.entries()[1];
	CHECK(draw.name == "draw");
	CHECK(draw.average == check::Approx(5.0));
	CHECK(draw.worst == check::Approx(5.0));
	CHECK(terrain.depth == 1);
	CHECK(terrain.worst == check::Approx(30.0)); // (one slow frame shows)
	CHECK(terrain.average > 3.0);
	CHECK(terrain.average < 5.0);
	// A timer in a timer is one deeper.
	{
		gc::Profiler::Scope outer(p, "outer");
		gc::Profiler::Scope inner(p, "inner");
	}
	CHECK(p.entries()[2].name == "inner"); // (added when it ends: first)
	CHECK(p.entries()[2].depth == 1);
	CHECK(p.entries()[3].depth == 0);
}
