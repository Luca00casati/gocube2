// Dyes: gems mixed in the dye vat (game/dye.h), dyed blocks as item ids
// ("core:stone#ff8800"), kept in chunks and their files, drawn by the mesher;
// the vat in play, and throwing (Q).

#include "content/content.h"
#include "game/dye.h"
#include "helpers.h"
#include "play.h"
#include "voxel/mesher.h"
#include "voxel/storage.h"

#include "check.h"

#include <algorithm>
#include <cmath>

using gc::BlockPos;
using gc::Pigment;
using gc::Stack;
using gocube::make_dye;

namespace {

constexpr gocube::Dye RED = 0xFFFF0000u, GREEN = 0xFF00FF00u, BLUE = 0xFF0000FFu, WHITE = 0xFFFFFFFFu, BLACK = 0xFF000000u,
		PINK = 0xFFFF8080u;

} // namespace

TEST_CASE("dye: gems mix like light, white and black shade; only the counts matter") {
	CHECK(gc::mix_dye({}) == gocube::NO_DYE);
	CHECK(gc::mix_dye({{RED, 1}}) == make_dye(255, 0, 0));
	CHECK(gc::mix_dye({{RED, 3}}) == make_dye(255, 0, 0)); // (more of one: the same colour)
	CHECK(gc::mix_dye({{RED, 1}, {GREEN, 1}}) == make_dye(255, 255, 0)); // yellow
	CHECK(gc::mix_dye({{GREEN, 1}, {RED, 1}}) == gc::mix_dye({{RED, 1}, {GREEN, 1}}));
	CHECK(gc::mix_dye({{RED, 2}, {GREEN, 1}}) == make_dye(255, 128, 0)); // orange
	CHECK(gc::mix_dye({{RED, 1}, {GREEN, 1}, {BLUE, 1}}) == make_dye(255, 255, 255));
	CHECK(gc::mix_dye({{RED, 3}, {BLUE, 1}}) == make_dye(255, 0, 85));
	CHECK(gc::mix_dye({{RED, 1}, {WHITE, 1}}) == make_dye(255, 128, 128)); // pink
	CHECK(gc::mix_dye({{BLUE, 1}, {BLACK, 1}}) == make_dye(0, 0, 128));   // navy
	CHECK(gc::mix_dye({{WHITE, 1}, {BLACK, 1}}) == make_dye(128, 128, 128));
	CHECK(gc::mix_dye({{BLACK, 2}}) == make_dye(0, 0, 0));
	CHECK(gc::mix_dye({{RED, 0}}) == gocube::NO_DYE);
}

TEST_CASE("dye: mixes snap to the nearest natural tone (the base game's dye_palette)") {
	const std::vector<gocube::Dye> palette = {make_dye(0xb8, 0x39, 0x2f), make_dye(0x3d, 0x62, 0xa8), make_dye(0xec, 0xeb, 0xe6),
			make_dye(0x23, 0x23, 0x25)};
	CHECK(gc::snap_dye(RED, palette) == palette[0]);
	CHECK(gc::snap_dye(BLUE, palette) == palette[1]);
	CHECK(gc::snap_dye(WHITE, palette) == palette[2]);
	CHECK(gc::snap_dye(BLACK, palette) == palette[3]);
	CHECK(gc::snap_dye(palette[1], palette) == palette[1]);
	CHECK(gc::snap_dye(RED, {}) == RED); // (no palette: any colour)
	CHECK(gc::snap_dye(gocube::NO_DYE, palette) == gocube::NO_DYE);
	// The base game's: every mix of the five gems lands on one of its tones.
	Play p;
	const std::optional<gc::Data> list = p.mods.rule("dye_palette", gc::Data::array());
	REQUIRE(list);
	std::vector<gocube::Dye> tones;
	for (const gc::Data &c : *list) {
		gocube::Dye d = gocube::NO_DYE;
		REQUIRE(gc::parse_dye(c.get<std::string>(), d));
		tones.push_back(d);
	}
	CHECK(tones.size() >= 24);
	const gocube::Dye mixed = gc::snap_dye(gc::mix_dye({{RED, 2}, {GREEN, 1}, {WHITE, 1}}), tones);
	CHECK(std::find(tones.begin(), tones.end(), mixed) != tones.end());
}

TEST_CASE("dye: a dyed block's item id") {
	CHECK(gc::undyed("core:stone#ff8800") == "core:stone");
	CHECK(gc::undyed("core:stone") == "core:stone");
	CHECK(gc::dye_of("core:stone#ff8800") == make_dye(255, 0x88, 0));
	CHECK(gc::dye_of("core:stone#FF8800") == make_dye(255, 0x88, 0));
	CHECK(gc::dye_of("core:stone") == gocube::NO_DYE);
	CHECK(gc::dye_of("core:stone#ff88") == gocube::NO_DYE);
	CHECK(gc::dye_of("core:stone#gg8800") == gocube::NO_DYE);
	CHECK(gc::dyed("core:stone", make_dye(1, 2, 3)) == "core:stone#010203");
	CHECK(gc::dyed("core:stone#ffffff", make_dye(1, 2, 3)) == "core:stone#010203"); // (dyed again)
	CHECK(gc::dyed("core:stone#ffffff", gocube::NO_DYE) == "core:stone");
	gc::Content content;
	load_base_game(content);
	content.freeze();
	CHECK(gc::display_name(content, "core:stone#ff8800") == "Stone (#ff8800)");
	// A pixel: the dye, lighter and darker as the texture is.
	// A pixel, stained: the dye where the texture is at its average, lighter
	// and darker as it is, a little of its own colour showing through.
	uint8_t mid[3] = {128, 128, 128}, dark[3] = {64, 64, 64};
	const float average = gc::pixel_lum(mid);
	gc::dye_pixel(mid, average, make_dye(200, 100, 0));
	gc::dye_pixel(dark, average, make_dye(200, 100, 0));
	CHECK(mid[0] == int(200 * 0.8f + 128 * 0.2f + 0.5f));
	CHECK(mid[2] == int(128 * 0.2f + 0.5f));
	CHECK(dark[0] < mid[0]);
}

TEST_CASE("dye: chunks keep their dyed blocks (files, the network); a new block loses the dye") {
	gocube::ChunkData c;
	c.set(1, 2, 3, 5);
	c.set_dye(1, 2, 3, make_dye(10, 20, 30));
	c.set_dye(4, 4, 4, make_dye(1, 1, 1)); // (air: no dye)
	CHECK(c.dye(1, 2, 3) == make_dye(10, 20, 30));
	CHECK(c.dye(4, 4, 4) == gocube::NO_DYE);
	CHECK(c.dyes.size() == 1);

	gocube::ChunkData back;
	const std::vector<uint8_t> bytes = gocube::encode_chunk(c);
	REQUIRE(gocube::decode_chunk(bytes.data(), bytes.size(), back));
	CHECK(back.get(1, 2, 3) == 5);
	CHECK(back.dye(1, 2, 3) == make_dye(10, 20, 30));
	CHECK(back.dyes.size() == 1);

	// A uniform chunk can have dyes too.
	gocube::ChunkData stone;
	stone.uniform_value = 7;
	stone.set_dye(0, 0, 0, make_dye(9, 9, 9));
	const std::vector<uint8_t> sb = gocube::encode_chunk(stone);
	gocube::ChunkData sback;
	REQUIRE(gocube::decode_chunk(sb.data(), sb.size(), sback));
	CHECK(sback.uniform);
	CHECK(sback.dye(0, 0, 0) == make_dye(9, 9, 9));

	// The older format (no dyes) still reads.
	const uint8_t v1[] = {'G', 'C', 'C', '1', 1, 7, 0};
	gocube::ChunkData old;
	REQUIRE(gocube::decode_chunk(v1, sizeof(v1), old));
	CHECK(old.uniform_value == 7);
	CHECK(old.dyes.empty());
	// Cut short, or more dyes than said: refused.
	CHECK_FALSE(gocube::decode_chunk(bytes.data(), bytes.size() - 1, back));
	std::vector<uint8_t> longer = bytes;
	longer.push_back(0);
	CHECK_FALSE(gocube::decode_chunk(longer.data(), longer.size(), back));

	// Another block there: undyed.
	c.set(1, 2, 3, 6);
	CHECK(c.dye(1, 2, 3) == gocube::NO_DYE);
}

TEST_CASE("dye: the mesher marks a dyed block's vertices (not the boxes that keep their colours)") {
	std::vector<gocube::BlockInfo> blocks(3);
	blocks[1].solid = blocks[1].opaque = true; // a cube
	blocks[2].solid = true;                    // a tub: a floor that keeps its colours, and the dye
	blocks[2].shape = gocube::SHAPE_BOXES;
	blocks[2].model = {{{0, 0, 0}, {1, 0.125f, 1}}, {{0.25f, 0.125f, 0.25f}, {0.75f, 0.5f, 0.75f}}};
	blocks[2].model[0].undyed = true;
	gocube::ChunkData c;
	c.set(2, 2, 2, 1);
	c.set(5, 2, 2, 1);
	c.set(8, 2, 2, 2);
	c.set_dye(2, 2, 2, make_dye(255, 0, 0));
	c.set_dye(8, 2, 2, make_dye(0, 0, 255));
	const gocube::ChunkData *none[gocube::FACE_COUNT] = {};
	gocube::AtlasLayout atlas;
	gocube::MeshOptions options;
	gocube::Surface opaque, cutout, fluid;
	gocube::mesh_chunk(c, none, nullptr, blocks, atlas, nullptr, options, opaque, cutout, fluid);
	REQUIRE(opaque.dyes.size() == opaque.colors.size() / 4);
	const auto red = std::count(opaque.dyes.begin(), opaque.dyes.end(), make_dye(255, 0, 0));
	const auto blue = std::count(opaque.dyes.begin(), opaque.dyes.end(), make_dye(0, 0, 255));
	const auto plain = std::count(opaque.dyes.begin(), opaque.dyes.end(), gocube::NO_DYE);
	CHECK(red == 24);  // the dyed cube: 6 faces
	CHECK(blue == 24); // the dye's box: 6 faces; its floor (6 more) keeps its colours
	CHECK(plain == 24 + 24);
}

namespace {

// A stone floor at y 40 (x -1..2, z -4..1) with room above; a dye vat at
// (1, 41, -3). The player stands at the far end, out of reach of drops.
struct VatRoom {
	Play p;
	static constexpr int H = 40;
	const BlockPos vat{1, H + 1, -3};
	VatRoom() {
		for (int x = -1; x <= 2; x++) {
			for (int z = -4; z <= 1; z++) {
				REQUIRE(p.world->set_block({x, H, z}, p.id("core:stone")));
				for (int y = H + 1; y <= H + 3; y++) {
					REQUIRE(p.world->set_block({x, y, z}, 0));
				}
			}
		}
		REQUIRE(p.world->set_block(vat, p.id("core:dye_vat")));
		away();
		p.player.flying = true;
	}
	// The player goes to the vat (picking up what's there), and away again.
	void collect() {
		p.player.position[0] = vat.x + 0.5f;
		p.player.position[2] = vat.z + 1.5f;
		steps(120);
		away();
	}
	void away() {
		p.player.position[0] = 0.5f;
		p.player.position[1] = float(H + 1);
		p.player.position[2] = 20.5f;
	}
	void steps(int n) {
		for (int i = 0; i < n; i++) {
			p.game->physics_step(1.0f / 60.0f);
		}
	}
	// Dropped into the vat (inside its walls).
	void in_vat(const std::string &item, int count) {
		const float at[3] = {vat.x + 0.5f, vat.y + 0.2f, vat.z + 0.5f};
		p.game->drop_item(item, count, at);
		steps(60);
	}
	gocube::Dye dye() const { return p.world->dye_at(vat.x, vat.y, vat.z); }
	// What a mix comes out as: the base game's nearest tone.
	gocube::Dye tone(gocube::Dye mixed) {
		std::vector<gocube::Dye> tones;
		const std::optional<gc::Data> list = p.mods.rule("dye_palette", gc::Data::array()); // (kept: not a temporary's)
		for (const gc::Data &c : *list) {
			gocube::Dye d = gocube::NO_DYE;
			gc::parse_dye(c.get<std::string>(), d);
			tones.push_back(d);
		}
		return gc::snap_dye(mixed, tones);
	}
	int lying(const std::string &item) const {
		int n = 0;
		for (const auto &d : p.game->drops().drops()) {
			n += d.item == item ? d.count : 0;
		}
		return n;
	}
};

} // namespace

TEST_CASE("dye vat: gems make its dye; blocks thrown in come out that colour, and stay dyed placed and broken") {
	VatRoom r;
	Play &p = r.p;
	const std::string pink_dirt = gc::dyed("core:dirt", r.tone(PINK));
	REQUIRE(p.game->block_entities().at(r.vat));
	CHECK(r.dye() == gocube::NO_DYE);

	r.in_vat("core:red_gem", 1);
	CHECK(p.block(r.vat) == "core:dye_vat_full");
	CHECK(r.dye() == r.tone(RED));
	r.in_vat("core:white_gem", 1);
	CHECK(r.dye() == r.tone(PINK));
	CHECK(p.game->block_entities().at(r.vat)->charge == 2 * gc::Game::DYE_PER_GEM);
	CHECK(p.game->drops().drops().empty()); // (the gems went into the dye)

	// Blocks: dyed, one charge each.
	r.in_vat("core:dirt", 3);
	CHECK(r.lying(pink_dirt) == 3);
	CHECK(r.lying("core:dirt") == 0);
	CHECK(p.game->block_entities().at(r.vat)->charge == 2 * gc::Game::DYE_PER_GEM - 3);
	r.steps(60);
	CHECK(r.lying(pink_dirt) == 3); // (not dyed again: it's that colour already)
	// Items that aren't blocks stay as they are.
	r.in_vat("core:coal", 1);
	CHECK(r.lying("core:coal") == 1);

	// The player comes for them.
	r.collect();
	gc::Inventory &inv = p.game->inventory();
	CHECK(inv.count_of(pink_dirt) == 3);
	CHECK(inv.count_of("core:dirt") == 0);
	CHECK(inv.can_fit(pink_dirt));

	// Placed: the world has it dyed. (Looking down ahead: at (0, 41, -2).)
	const BlockPos placed{0, VatRoom::H + 1, -2};
	p.player.position[0] = 0.5f;
	p.player.position[2] = 0.5f;
	inv.slots[0] = {pink_dirt, 3};
	p.player.yaw = 0;
	p.player.pitch = -std::atan2(1.62f, 2.0f);
	gc::GameInput in;
	in.captured = true;
	in.number_key = 1;
	in.use_pressed = true;
	p.game->update(0.016f, in);
	REQUIRE(p.block(placed) == "core:dirt");
	CHECK(p.world->dye_at(placed.x, placed.y, placed.z) == r.tone(PINK));
	CHECK(inv.slots[0].count == 2);
	// Broken: it drops dyed.
	p.game->break_block(placed, "");
	CHECK(r.lying(pink_dirt) == 1);
	CHECK(p.world->dye_at(placed.x, placed.y, placed.z) == gocube::NO_DYE);

	// Tipped out (a right click with an empty hand): the vat is empty again.
	gc::Actor me;
	me.body = &p.player;
	p.player.position[0] = r.vat.x + 0.5f;
	p.player.position[2] = r.vat.z + 2.5f;
	p.player.pitch = -std::atan2(1.62f - 1.0f + 0.5f, 2.0f); // (at the vat's middle)
	REQUIRE(p.player.target(*p.world).hit);
	REQUIRE(p.player.target(*p.world).position == r.vat);
	CHECK(p.game->use(me, false));
	CHECK(p.block(r.vat) == "core:dye_vat");
	CHECK(r.dye() == gocube::NO_DYE);
	CHECK(p.game->block_entities().at(r.vat)->charge == 0);
}

TEST_CASE("dye vat: it runs dry; it holds so many gems; the vat itself isn't dyed") {
	VatRoom r;
	Play &p = r.p;
	const std::string blue_planks = gc::dyed("core:planks", r.tone(BLUE));
	r.in_vat("core:blue_gem", 1);
	r.in_vat("core:planks", gc::Game::DYE_PER_GEM + 2);
	CHECK(r.lying(blue_planks) == gc::Game::DYE_PER_GEM);
	CHECK(r.lying("core:planks") == 2); // (no dye left for these)
	CHECK(p.block(r.vat) == "core:dye_vat"); // dry
	CHECK(p.game->block_entities().at(r.vat)->charge == 0);
	r.collect(); // (else the next dye would dye them)
	CHECK(p.game->inventory().count_of(blue_planks) == gc::Game::DYE_PER_GEM);
	CHECK(p.game->inventory().count_of("core:planks") == 2);

	r.in_vat("core:green_gem", gc::Game::VAT_MAX_GEMS + 3);
	CHECK(r.lying("core:green_gem") == 3); // (more than it holds: left there)
	CHECK(p.game->block_entities().at(r.vat)->charge == gc::Game::VAT_MAX_GEMS * gc::Game::DYE_PER_GEM);
	CHECK(r.dye() == r.tone(GREEN));

	// Broken: its gems come back, and the vat isn't dyed.
	p.game->break_block(r.vat, "");
	CHECK(r.lying("core:dye_vat") == 1);
	CHECK(r.lying("core:green_gem") == 3 + gc::Game::VAT_MAX_GEMS);
}

TEST_CASE("throwing (Q): one, or the whole stack (Ctrl), out of the inventory (creative too)") {
	for (const bool creative : {false, true}) {
		CAPTURE(creative);
		Play p(creative);
		p.player.position[1] = 80.0f;
		p.player.flying = true;
		p.game->inventory().slots[0] = {"core:planks", 5};
		gc::GameInput in;
		in.captured = true;
		in.number_key = 1;
		in.throw_pressed = true;
		p.game->update(0.016f, in);
		REQUIRE(p.game->drops().drops().size() == 1);
		const auto &d = p.game->drops().drops()[0];
		CHECK(d.item == "core:planks");
		CHECK(d.count == 1);
		CHECK(d.delay == gc::ItemDrops::THROW_DELAY);
		CHECK(std::hypot(d.vel[0], d.vel[2]) > 3.0f); // (away, where we look)
		CHECK(p.game->inventory().slots[0].count == 4);
		in.number_key = 0;
		in.throw_stack = true;
		p.game->update(0.016f, in);
		CHECK(p.game->inventory().slots[0].empty());
		CHECK(p.game->drops().drops().back().count == 4);
	}
}
