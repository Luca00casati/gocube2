// Block images: PPM files (magenta is see-through) and faces painted from a
// colour and noise (content/image.h), and the base game's atlas made of them.

#include "content/content.h"
#include "content/image.h"
#include "helpers.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>

namespace {

const uint8_t *at(const gc::Image &im, int x, int y) {
	return &im.rgba[(size_t(y) * size_t(im.width) + size_t(x)) * 4];
}

} // namespace

TEST_CASE("PPM: binary and text, magenta is see-through, anything else is refused") {
	gc::Image im;
	std::string p6 = "P6\n# a comment\n2 1\n255\n";
	p6 += std::string("\x10\x20\x30\xff\x00\xff", 6);
	REQUIRE(gc::parse_ppm(p6, im));
	CHECK(im.width == 2);
	CHECK(im.height == 1);
	CHECK(at(im, 0, 0)[0] == 0x10);
	CHECK(at(im, 0, 0)[2] == 0x30);
	CHECK(at(im, 0, 0)[3] == 255);
	CHECK(at(im, 1, 0)[3] == 0); // magenta
	CHECK(at(im, 1, 0)[0] == 0);

	REQUIRE(gc::parse_ppm("P3 1 2 15  15 0 15  0 15 0", im)); // maxval 15: scaled to 255
	CHECK(at(im, 0, 0)[3] == 0);
	CHECK(at(im, 0, 1)[1] == 255);
	CHECK(at(im, 0, 1)[3] == 255);

	CHECK_FALSE(gc::parse_ppm("", im));
	CHECK_FALSE(gc::parse_ppm("P5 1 1 255 x", im));            // greyscale: not PPM
	CHECK_FALSE(gc::parse_ppm("P6 2 2 255\nabc", im));         // too short
	CHECK_FALSE(gc::parse_ppm("P6 1 1 65535\nabcdef", im));    // 16-bit
	CHECK_FALSE(gc::parse_ppm("P6 99999 1 255\nabc", im));     // too big
	CHECK_FALSE(gc::parse_ppm("P3 1 1 255 1 2", im));          // missing a value
}

TEST_CASE("painted faces: the same spec gives the same pixels, they tile, and each feature shows") {
	const gc::Data stone = gc::Data::object({{"color", "#75787e"}, {"noise", 0.3}, {"shades", 5}});
	const gc::Image a = gc::paint_face(stone, 32), b = gc::paint_face(stone, 32);
	REQUIRE(a.width == 32);
	REQUIRE(a.rgba.size() == 32 * 32 * 4);
	CHECK(a.rgba == b.rgba);
	std::set<int> shades;
	for (int y = 0; y < 32; y++) {
		for (int x = 0; x < 32; x++) {
			CHECK(at(a, x, y)[3] == 255);
			shades.insert(at(a, x, y)[0]);
			if (x % 2 == 0 && y % 2 == 0) { // 16 pixels across: each 2 x 2 texels
				CHECK(at(a, x + 1, y + 1)[0] == at(a, x, y)[0]);
			}
		}
	}
	CHECK(shades.size() >= 4);
	CHECK(shades.size() <= 5);

	// Another colour: other pixels (the spec seeds the noise).
	gc::Data other = stone;
	other["color"] = "#76787e";
	CHECK(gc::paint_face(other, 32).rgba != a.rgba);

	// A grassy band on top.
	gc::Data side = stone;
	side["band"] = gc::Data::object({{"color", "#00ff00"}, {"depth", 3}, {"ragged", 0}});
	const gc::Image s = gc::paint_face(side, 32);
	CHECK(at(s, 5, 0)[1] > at(s, 5, 0)[0]);
	CHECK(at(s, 5, 5)[1] > at(s, 5, 5)[0]); // 3 pixels = 6 texels
	CHECK(at(s, 5, 6)[1] < at(s, 5, 6)[2]); // stone below

	// Ore spots: some pixels of the ore's colour.
	gc::Data ore = stone;
	ore["spots"] = gc::Data::object({{"color", "#ffd000"}, {"count", 4}, {"size", 3}});
	const gc::Image o = gc::paint_face(ore, 32);
	int gold = 0;
	for (int i = 0; i < 32 * 32; i++) {
		gold += o.rgba[size_t(i) * 4] > o.rgba[size_t(i) * 4 + 2] + 60;
	}
	CHECK(gold >= 4 * 4);

	// A ramp of colours instead of shading.
	const gc::Data lava = gc::Data::object({{"colors", gc::Data::array({"#ff0000", "#ffff00"})}});
	const gc::Image l = gc::paint_face(lava, 16);
	for (int i = 0; i < 16 * 16; i++) {
		CHECK(l.rgba[size_t(i) * 4] == 255);
		CHECK((l.rgba[size_t(i) * 4 + 1] == 0 || l.rgba[size_t(i) * 4 + 1] == 255));
	}

	// Nonsense values fall back: no crash, still a whole face.
	const gc::Data bad = gc::Data::object({{"color", 5}, {"noise", "lots"}, {"pixels", -3},
			{"cells", gc::Data::array({1, "x"})}, {"spots", gc::Data::object({{"count", 1e9}, {"size", -1}})}});
	CHECK(gc::paint_face(bad, 32).rgba.size() == 32 * 32 * 4);
}

TEST_CASE("the base game's atlas: painted faces and PPM images, no missing ones") {
	gc::Content content;
	load_base_game(content);
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	const auto &blocks = table.blocks;
	auto info = [&](const char *id) { return blocks[size_t(content.block_index(id))]; };
	const auto &tiles = table.atlas.tiles;
	auto opaque = [&](int t) {
		for (size_t i = 3; i < tiles[size_t(t)].size(); i += 4) {
			if (tiles[size_t(t)][i] != 255) {
				return false;
			}
		}
		return true;
	};
	// Painted: solid; stone's faces, and its stairs', are the same tile.
	CHECK(opaque(info("core:stone").tiles[0]));
	CHECK(info("core:stone").tiles[0] == info("core:stone_stairs").tiles[0]);
	CHECK(info("core:grass").tiles[2] != info("core:grass").tiles[0]); // top, side
	CHECK(info("core:grass").tiles[3] == info("core:dirt").tiles[0]);  // bottom
	// A bed's two halves are one picture: the head has the pillow, the foot doesn't.
	CHECK(info("core:bed").tiles[2] != info("core:bed_n_head").tiles[2]); // (core:bed: the foot, facing n)
	CHECK(info("core:bed").tiles[0] == info("core:bed_n_head").tiles[0]); // (the same sides)
	CHECK_FALSE(info("core:stone").cutout);
	CHECK_FALSE(info("core:leaves").cutout);
	// Drawn (PPM): torches and glass are see-through in places.
	CHECK(info("core:torch").cutout);
	// A torch on a wall stands against it, but is textured as the one in the
	// middle of the floor (the drawn torch is there: elsewhere the image is see-through).
	for (const char *f : {"n", "e", "s", "w"}) {
		CAPTURE(f);
		const gocube::BlockInfo &wall = info(("core:torch_wall_" + std::string(f)).c_str());
		REQUIRE(wall.model.size() == 1);
		const gocube::Box &b = wall.model[0];
		CHECK((b.mn[0] + b.mx[0]) / 2 - b.tx == doctest::Approx(0.5f));
		CHECK((b.mn[2] + b.mx[2]) / 2 - b.tz == doctest::Approx(0.5f));
		CHECK((std::min(b.mn[0], b.mn[2]) < 0.1f || std::max(b.mx[0], b.mx[2]) > 0.9f)); // against a side
	}
	CHECK(info("core:glass").cutout);
	CHECK_FALSE(opaque(table.icon("core:snowball")));
	// Nothing fell back to magenta (an image not found).
	for (size_t t = 0; t < tiles.size(); t++) {
		CAPTURE(t);
		if (int(t) == table.icon("core:air")) {
			continue; // (air's icon: magenta, never shown)
		}
		const auto &px = tiles[t];
		int magenta = 0;
		for (size_t i = 0; i < px.size(); i += 4) {
			magenta += px[i] == 255 && px[i + 1] == 0 && px[i + 2] == 255 && px[i + 3] == 255;
		}
		CHECK(magenta < 8);
	}
}
