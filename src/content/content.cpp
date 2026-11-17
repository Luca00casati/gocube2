#include "content/content.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <utility>

namespace gc {

using gocube::BlockId;
using gocube::BlockInfo;
using gocube::Box;

namespace {

const char *const FACINGS[4] = {"n", "e", "s", "w"}; // towards -z, +x, +z, -x

int facing_index(const std::string &f) {
	for (int i = 0; i < 4; i++) {
		if (f == FACINGS[i]) {
			return i;
		}
	}
	return -1;
}

bool is_not_solid_shape(const std::string &shape) {
	return shape == "torch" || shape == "ladder" || shape == "cross"; // shapes you walk through
}

// The engine's shape for a definition's "shape" (-1: drawn as a cube).
int engine_shape(const std::string &shape) {
	if (shape == "boxes" || shape == "slab" || shape == "stairs" || shape == "torch" || shape == "ladder" ||
			shape == "door" || shape == "bed") {
		return gocube::SHAPE_BOXES;
	}
	if (shape == "fence") {
		return gocube::SHAPE_FENCE;
	}
	if (shape == "cross") {
		return gocube::SHAPE_CROSS;
	}
	return -1;
}

std::string str(const Data &j, const char *key, const std::string &fallback = "") {
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

double num(const Data &j, const char *key, double fallback) {
	auto it = j.find(key);
	return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

bool flag(const Data &j, const char *key, bool fallback = false) {
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

// Godot's int() of a JSON number (a float): truncates.
int inum(const Data &j, const char *key, int fallback) {
	return static_cast<int>(num(j, key, fallback));
}

// The variant blocks of a shaped block, in the Godot version's order:
// (suffix, state); the base block itself is suffix "".
std::vector<std::pair<std::string, Data>> variants(const Data &def) {
	std::vector<std::pair<std::string, Data>> out;
	const std::string shape = str(def, "shape", "cube");
	if (shape == "slab") {
		out.push_back({"", {{"half", "bottom"}}});
		out.push_back({"_top", {{"half", "top"}}});
	} else if (shape == "stairs" || shape == "ladder") {
		for (int i = 0; i < 4; i++) {
			out.push_back({i == 0 ? "" : std::string("_") + FACINGS[i], {{"facing", FACINGS[i]}}});
		}
	} else if (shape == "torch") {
		out.push_back({"", {{"facing", "floor"}}});
		for (const char *f : FACINGS) {
			out.push_back({std::string("_wall_") + f, {{"facing", f}}});
		}
	} else if (shape == "door") {
		for (const char *f : FACINGS) {
			for (const char *half : {"lower", "upper"}) {
				for (bool open : {false, true}) {
					std::string suffix = std::string("_") + f + "_" + half + (open ? "_open" : "");
					if (std::string(f) == "n" && std::string(half) == "lower" && !open) {
						suffix = "";
					}
					out.push_back({suffix, {{"facing", f}, {"half", half}, {"open", open}}});
				}
			}
		}
	} else if (shape == "bed") {
		// The foot where it's placed, the head one block further the way the player looked.
		for (const char *f : FACINGS) {
			for (const char *part : {"foot", "head"}) {
				const bool base = std::string(f) == "n" && std::string(part) == "foot";
				out.push_back({base ? "" : std::string("_") + f + "_" + part, {{"facing", f}, {"part", part}}});
			}
		}
	} else {
		out.push_back({"", Data::object()});
	}
	return out;
}

// Adds every shaped block's variants (and placed leaves) to `blocks`.
void expand(std::map<std::string, Data> &blocks) {
	std::vector<std::string> ids;
	for (const auto &[id, def] : blocks) {
		ids.push_back(id);
	}
	for (const std::string &id : ids) {
		Data &def = blocks[id];
		if (flag(def, "leaves") && !def.contains("variant_of")) {
			Data placed = def;
			placed["id"] = id + "_placed";
			placed["variant_of"] = id;
			placed["leaves"] = false; // placed by a player: never decays
			if (!def.contains("drops")) {
				placed["drops"] = id;
			}
			blocks[id + "_placed"] = placed;
		}
		const std::string shape = str(def, "shape", "cube");
		if (shape == "cube" || def.contains("variant_of")) {
			continue;
		}
		if (is_not_solid_shape(shape)) {
			def["solid"] = false;
		}
		def["transparent"] = true;
		const auto vs = variants(def);
		def["state"] = vs[0].second;
		for (const auto &[suffix, state] : vs) {
			if (suffix.empty()) {
				continue;
			}
			Data v = def;
			v["id"] = id + suffix;
			v["variant_of"] = id;
			v["state"] = state;
			if (!def.contains("drops")) {
				v["drops"] = id;
			}
			if (shape == "door" && state["half"] == "upper" && def.contains("textures_upper")) {
				v["textures"] = def["textures_upper"];
			}
			if (shape == "bed") {
				if (state["part"] == "head" && def.contains("textures_head")) {
					v["textures"] = def["textures_head"];
				}
				v["texture_turns"] = facing_index(state["facing"].get<std::string>()); // top drawn for "n"
			}
			blocks[id + suffix] = v;
		}
	}
}

// A box in 1/16 units: x0, y0, z0, x1, y1, z1, texture y offset.
using Box16 = std::array<double, 7>;

// Quarter turns clockwise seen from above: north -> east -> south -> west.
Box16 rotate(Box16 b, int turns) {
	for (int t = 0; t < turns; t++) {
		b = {16 - b[5], b[1], b[0], 16 - b[2], b[4], b[3], b[6]}; // (x, z) -> (16 - z, x)
	}
	return b;
}

std::vector<Box16> model_boxes(const std::string &shape, const Data &state) {
	std::vector<Box16> boxes;
	int turns = facing_index(str(state, "facing", "n"));
	if (shape == "slab") {
		boxes = {str(state, "half") == "top" ? Box16{0, 8, 0, 16, 16, 16, 0} : Box16{0, 0, 0, 16, 8, 16, 0}};
	} else if (shape == "stairs") {
		boxes = {{0, 0, 0, 16, 8, 16, 0}, {0, 8, 0, 16, 16, 8, 0}}; // the high side towards the facing
	} else if (shape == "torch") {
		if (str(state, "facing") == "floor") {
			return {{7, 0, 7, 9, 10, 9, 0}};
		}
		boxes = {{7, 4, 0.5, 9, 14, 2.5, 4}}; // on a wall, raised against it
	} else if (shape == "ladder") {
		boxes = {{0, 0, 0, 16, 16, 1, 0}};
	} else if (shape == "bed") {
		return {{0, 0, 0, 16, 9, 16, 0}};
	} else if (shape == "door") {
		boxes = {{0, 0, 0, 16, 16, 3, 0}};
		if (flag(state, "open")) {
			turns += 3; // swung open against the side wall
		}
	}
	for (Box16 &b : boxes) {
		b = rotate(b, turns >= 0 ? turns % 4 : 0);
	}
	return boxes;
}

std::vector<Box> to_boxes(const std::vector<Box16> &in) {
	std::vector<Box> out;
	for (const Box16 &b : in) {
		out.push_back({{float(b[0] / 16.0), float(b[1] / 16.0), float(b[2] / 16.0)},
				{float(b[3] / 16.0), float(b[4] / 16.0), float(b[5] / 16.0)}, float(b[6] / 16.0)});
	}
	return out;
}

// Godot's Color.html: "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa" ('#' optional).
bool parse_html_color(std::string s, uint8_t out[4]) {
	if (!s.empty() && s[0] == '#') {
		s.erase(0, 1);
	}
	auto hex = [](char c) {
		return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
	};
	for (char c : s) {
		if (hex(c) < 0) {
			return false;
		}
	}
	out[3] = 255;
	if (s.size() == 3 || s.size() == 4) {
		for (size_t i = 0; i < s.size(); i++) {
			out[i] = uint8_t(hex(s[i]) * 17);
		}
		return true;
	}
	if (s.size() == 6 || s.size() == 8) {
		for (size_t i = 0; i < s.size() / 2; i++) {
			out[i] = uint8_t(hex(s[2 * i]) * 16 + hex(s[2 * i + 1]));
		}
		return true;
	}
	return false;
}

} // namespace

std::string display_name(const Content &content, const std::string &id) {
	for (const auto *table : {&content.items, &content.blocks}) {
		auto it = table->find(id);
		if (it != table->end() && it->second.contains("name") && it->second["name"].is_string()) {
			return it->second["name"].get<std::string>();
		}
	}
	const size_t colon = id.find(':');
	std::string out;
	bool word_start = true;
	for (char c : id.substr(colon == std::string::npos ? 0 : colon + 1)) {
		if (c == '_') {
			out += ' ';
			word_start = true;
		} else {
			out += word_start && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
			word_start = false;
		}
	}
	return out;
}

uint32_t godot_string_hash(const std::string &s) {
	uint32_t h = 5381;
	for (unsigned char c : s) { // ids are ASCII: bytes are code points
		h = (h << 5) + h + c;
	}
	return h;
}

// --- Content ---------------------------------------------------------------------

std::map<std::string, Data> *Content::table(const std::string &section) {
	if (section == "blocks") return &blocks;
	if (section == "items") return &items;
	if (section == "recipes") return &recipes;
	if (section == "ores") return &ores;
	if (section == "trees") return &trees;
	if (section == "biomes") return &biomes;
	if (section == "sound_sets") return &sound_sets;
	if (section == "creatures") return &creatures;
	return nullptr;
}

void Content::register_entry(std::map<std::string, Data> &table, const std::string &mod, const std::string &id,
		const Data &def) {
	const std::string full = id.find(':') != std::string::npos ? id : mod + ":" + id;
	Data entry = def;
	entry["id"] = full;
	entry["mod"] = mod;
	table[full] = std::move(entry);
}

void Content::freeze(const std::vector<std::string> &saved_block_ids) {
	expand(blocks);
	assign_block_ids(saved_block_ids);
}

void Content::assign_block_ids(const std::vector<std::string> &saved) {
	block_ids_ = saved.empty() ? std::vector<std::string>{"core:air"} : saved;
	block_index_.clear();
	for (size_t i = 0; i < block_ids_.size(); i++) {
		block_index_[block_ids_[i]] = int(i);
	}
	for (const auto &[id, def] : blocks) { // std::map: already sorted
		if (!block_index_.count(id)) {
			block_index_[id] = int(block_ids_.size());
			block_ids_.push_back(id);
		}
	}
}

int Content::block_index(const std::string &id) const {
	auto it = block_index_.find(id);
	return it == block_index_.end() ? -1 : it->second;
}

std::filesystem::path Content::asset_path(const Data &def, const std::string &relative) const {
	std::string mod = str(def, "mod"), rel = relative;
	if (const size_t colon = rel.find(':'); colon != std::string::npos) { // "othermod:textures/x.png"
		mod = rel.substr(0, colon);
		rel = rel.substr(colon + 1);
	}
	auto it = mod_dirs.find(mod);
	const std::filesystem::path p(rel);
	if (it == mod_dirs.end() || rel.empty() || p.is_absolute() || rel[0] == '/' ||
			rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) {
		return {};
	}
	for (const auto &part : p) {
		if (part == "..") {
			return {}; // stays inside the mod
		}
	}
	return it->second / p;
}

// --- Generator tables --------------------------------------------------------------

namespace {

// A biome's "trees" or "plants": a list of {id, value} pairs, in their order
// (it reaches the generator); a table keyed by id has no order in Lua, so
// that form is taken in id order.
std::vector<std::pair<std::string, double>> id_values(const Data &j) {
	std::vector<std::pair<std::string, double>> out;
	if (j.is_array()) {
		for (const Data &e : j) {
			if (e.is_array() && e.size() == 2 && e[0].is_string() && e[1].is_number()) {
				out.push_back({e[0].get<std::string>(), e[1].get<double>()});
			}
		}
	} else if (j.is_object()) {
		for (const auto &[id, v] : j.items()) {
			if (v.is_number()) {
				out.push_back({id, v.get<double>()});
			}
		}
		std::sort(out.begin(), out.end());
	}
	return out;
}

} // namespace

GeneratorConfig generator_config(const Content &content, int generator_version) {
	GeneratorConfig g;
	const int block_count = int(content.block_ids().size());
	auto block = [&](const std::string &id) { // as World and GoCubeWorld together did
		const int i = content.block_index(id);
		return static_cast<BlockId>(i > 0 && i < block_count ? i : 0);
	};
	g.ids.stone = block("core:stone");
	g.ids.dirt = block("core:dirt");
	g.ids.grass = block("core:grass");
	g.ids.sand = block("core:sand");
	g.ids.water = block("core:water");
	g.ids.lava = block("core:lava");
	g.ids.ice = block("core:ice");

	// Ores, sorted by id; each one's veins are keyed to its id (the salt), so
	// adding a mod doesn't move existing veins.
	for (const auto &[id, def] : content.ores) {
		gocube::OreKind k;
		k.id = block(str(def, "block"));
		k.replaces = block(str(def, "replaces", "core:stone"));
		if (k.id == 0 || k.replaces == 0) {
			continue; // unknown block: not generated
		}
		k.min_y = inum(def, "min_y", -96);
		k.max_y = inum(def, "max_y", 0);
		k.veins_per_chunk = std::clamp(inum(def, "veins_per_chunk", 4), 0, 256);
		const Data size = def.value("size", Data::array({3, 6}));
		k.min_size = std::max(1, size.empty() ? 3 : int(size.front().get<double>()));
		k.max_size = std::max(k.min_size, size.empty() ? 6 : int(size.back().get<double>()));
		k.salt = godot_string_hash(id) & 0x7fffffffu;
		g.ores.push_back(k);
	}

	// Trees with a known log and leaves (or a cactus), sorted by id: biomes
	// refer to them by index in this order.
	static const char *const STYLES[] = {"oak", "big_oak", "birch", "spruce", "cactus", "round_oak"};
	const std::vector<std::string> tree_order = gc::tree_order(content);
	for (const std::string &id : tree_order) {
		const Data &def = content.trees.at(id);
		gocube::TreeKind t;
		t.log = block(str(def, "log"));
		t.leaves = block(str(def, "leaves"));
		const Data trunk = def.value("trunk", Data::array({4, 6}));
		t.min_trunk = std::clamp(trunk.empty() ? 4 : int(trunk.front().get<double>()), 2, 20);
		t.max_trunk = std::clamp(trunk.empty() ? 6 : int(trunk.back().get<double>()), t.min_trunk, 20);
		t.chance = std::clamp(inum(def, "chance", 30), 0, 100);
		std::string style = str(def, "style", "oak");
		if (style == "round_oak" && generator_version < 3) {
			style = "oak"; // older worlds' unexplored land matches what they saved
		}
		int s = 0;
		for (int i = 0; i < 6; i++) {
			if (style == STYLES[i]) {
				s = i;
			}
		}
		t.style = static_cast<uint8_t>(s);
		g.trees.push_back(t);
	}

	if (generator_version < 2) {
		return g; // worlds from before biomes
	}
	for (const auto &[id, def] : content.biomes) {
		gocube::BiomeKind b;
		b.temperature = float(num(def, "temperature", 0.0));
		b.humidity = float(num(def, "humidity", 0.0));
		b.height = float(num(def, "height", 20.0));
		b.base = float(num(def, "base", 0.0));
		b.surface = block(str(def, "surface"));
		b.filler = block(str(def, "filler"));
		b.filler_depth = std::clamp(inum(def, "filler_depth", 3), 1, 16);
		b.under = block(str(def, "under"));
		b.under_depth = std::clamp(inum(def, "under_depth", 0), 0, 16);
		b.frozen = flag(def, "frozen");
		b.lakes = std::clamp(inum(def, "lakes", 20), 0, 100);
		b.mountains = std::clamp(float(num(def, "mountains", 0.0)), 0.0f, 60.0f);
		b.rock = block(str(def, "rock"));
		b.rock_above = inum(def, "rock_above", 1 << 20);
		b.cap = block(str(def, "cap"));
		b.cap_above = inum(def, "cap_above", 1 << 20);
		b.cover = block(str(def, "cover"));
		b.cover_above = inum(def, "cover_above", 1 << 20);
		for (const auto &[tree, percent] : id_values(def.value("trees", Data()))) {
			auto it = std::find(tree_order.begin(), tree_order.end(), tree);
			if (it != tree_order.end()) {
				b.trees.push_back({int(it - tree_order.begin()), int(percent)});
			}
		}
		for (const auto &[plant, per_mille] : id_values(def.value("plants", Data()))) {
			if (content.block_index(plant) > 0) {
				b.plants.push_back({block(plant), int(per_mille)});
			}
		}
		if (b.surface && b.filler) {
			g.biomes.push_back(b);
		}
	}
	return g;
}

std::vector<std::string> tree_order(const Content &content) {
	std::vector<std::string> out;
	for (const auto &[id, def] : content.trees) {
		if (content.block_index(str(def, "log")) > 0 &&
				(content.block_index(str(def, "leaves")) > 0 || str(def, "style") == "cactus")) {
			out.push_back(id);
		}
	}
	return out;
}

void configure_generator(gocube::Generator &generator, const Content &content, uint32_t seed,
		int generator_version) {
	const GeneratorConfig g = generator_config(content, generator_version);
	generator.set_biomes(g.biomes);
	generator.configure(seed, g.ids, g.ores, g.trees);
}

// --- Block table and atlas ---------------------------------------------------------

namespace {

struct AtlasBuilder {
	const Content &content;
	Atlas &atlas;
	std::map<std::string, int> index; // texture path (+ turns) or "#rrggbbaa" -> tile

	using Pixels = std::vector<uint8_t>;
	static constexpr int T = Atlas::TILE;

	int add(const std::string &key, Pixels px) {
		atlas.tiles.push_back(std::move(px));
		index[key] = int(atlas.tiles.size()) - 1;
		return index[key];
	}

	static Pixels flat(const uint8_t c[4]) {
		Pixels px(T * T * 4);
		for (int i = 0; i < T * T; i++) {
			std::copy(c, c + 4, &px[size_t(i) * 4]);
		}
		return px;
	}

	// A PNG as 8-bit RGBA (libpng's simplified API, as Godot loads PNGs),
	// resized to TILE x TILE.
	static bool load_png(const std::filesystem::path &path, Pixels &out) {
		FILE *f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
			return false;
		}
		png_image image = {};
		image.version = PNG_IMAGE_VERSION;
		std::vector<uint8_t> data;
		bool ok = png_image_begin_read_from_stdio(&image, f) != 0;
		if (ok) {
			image.format = PNG_FORMAT_RGBA;
			data.resize(PNG_IMAGE_SIZE(image));
			ok = png_image_finish_read(&image, nullptr, data.data(), 0, nullptr) != 0;
		}
		png_image_free(&image);
		fclose(f);
		if (!ok || image.width == 0 || image.height == 0) {
			return false;
		}
		const size_t w = image.width, h = image.height;
		out.assign(size_t(T) * T * 4, 0);
		for (size_t y = 0; y < size_t(T); y++) { // nearest-neighbour resize to TILE x TILE
			for (size_t x = 0; x < size_t(T); x++) {
				const size_t sx = x * w / T, sy = y * h / T;
				std::copy(&data[(sy * w + sx) * 4], &data[(sy * w + sx) * 4] + 4, &out[(y * T + x) * 4]);
			}
		}
		return true;
	}

	static Pixels rotate_cw(const Pixels &in) {
		Pixels out(in.size());
		for (int y = 0; y < T; y++) {
			for (int x = 0; x < T; x++) { // destination (x, y) <- source (y, T - 1 - x)
				std::copy(&in[(size_t(T - 1 - x) * T + y) * 4], &in[(size_t(T - 1 - x) * T + y) * 4] + 4,
						&out[(size_t(y) * T + x) * 4]);
			}
		}
		return out;
	}

	// A tile from an image of the mod (key: its path), or -1 if it can't be read.
	int image_tile(const Data &def, const std::string &rel) {
		const std::filesystem::path full = content.asset_path(def, rel);
		const std::string key = full.string();
		if (auto it = index.find(key); it != index.end()) {
			return it->second;
		}
		Pixels px;
		if (full.empty() || !load_png(full, px)) {
			fprintf(stderr, "Image not found: %s (%s)\n", rel.c_str(), str(def, "id").c_str());
			return -1;
		}
		return add(key, std::move(px));
	}

	// A flat tile of the definition's "color" (else `fallback`).
	int color_tile(const Data &def, const char *fallback) {
		uint8_t c[4] = {255, 0, 255, 255};
		parse_html_color(def.contains("color") ? str(def, "color") : fallback, c);
		char key[16];
		snprintf(key, sizeof(key), "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
		if (auto it = index.find(key); it != index.end()) {
			return it->second;
		}
		return add(key, flat(c));
	}

	// One face's tile: its texture ("side"/"top"/"bottom", else "all"), else the
	// block's colour, else magenta.
	int face_tile(const Data &def, const char *face, int turns) {
		const Data textures = def.value("textures", Data::object());
		const Data *rel = textures.contains(face) ? &textures[face] : (textures.contains("all") ? &textures["all"] : nullptr);
		if (rel && rel->is_string()) {
			const std::filesystem::path full = content.asset_path(def, rel->get<std::string>());
			const std::string key = full.string() + (turns % 4 ? "@" + std::to_string(turns % 4) : "");
			if (auto it = index.find(key); it != index.end()) {
				return it->second;
			}
			Pixels px;
			if (!full.empty() && load_png(full, px)) {
				for (int t = 0; t < turns % 4; t++) {
					px = rotate_cw(px);
				}
				return add(key, std::move(px));
			}
			fprintf(stderr, "Block texture not found: %s (%s)\n", rel->get<std::string>().c_str(),
					str(def, "id").c_str());
		}
		uint8_t c[4] = {255, 0, 255, 255};
		if (def.contains("color")) {
			parse_html_color(str(def, "color"), c);
		}
		char key[16];
		snprintf(key, sizeof(key), "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
		if (auto it = index.find(key); it != index.end()) {
			return it->second;
		}
		return add(key, flat(c));
	}

	void build() {
		if (atlas.tiles.empty()) {
			const uint8_t magenta[4] = {255, 0, 255, 255};
			add("#missing", flat(magenta));
		}
		int cols = 1;
		while (cols * cols < int(atlas.tiles.size())) {
			cols *= 2;
		}
		const int rows = (int(atlas.tiles.size()) + cols - 1) / cols;
		const int cell = T * Atlas::CELL;
		atlas.layout = {cols, rows, Atlas::CELL, Atlas::FACE_X, Atlas::FACE_Y};
		atlas.width = cols * cell;
		atlas.height = rows * cell;
		atlas.rgba.assign(size_t(atlas.width) * atlas.height * 4, 0);
		for (size_t i = 0; i < atlas.tiles.size(); i++) {
			const int cx = int(i) % cols * cell, cy = int(i) / cols * cell;
			for (int ry = 0; ry < Atlas::CELL; ry++) {
				for (int rx = 0; rx < Atlas::CELL; rx++) {
					for (int y = 0; y < T; y++) {
						const size_t dst = ((size_t(cy) + ry * T + y) * atlas.width + cx + rx * T) * 4;
						std::copy(&atlas.tiles[i][size_t(y) * T * 4], &atlas.tiles[i][size_t(y) * T * 4] + T * 4,
								&atlas.rgba[dst]);
					}
				}
			}
		}
	}
};

bool has_alpha(const std::vector<uint8_t> &px) {
	for (size_t i = 3; i < px.size(); i += 4) {
		if (px[i] != 255) {
			return true;
		}
	}
	return false;
}

} // namespace

BlockTable build_block_table(const Content &content) {
	BlockTable table;
	AtlasBuilder atlas{content, table.atlas, {}};
	const Data empty = Data::object();
	for (const std::string &id : content.block_ids()) {
		auto it = content.blocks.find(id);
		const Data &def = it != content.blocks.end() ? it->second : empty;
		BlockInfo b;
		b.fluid = def.contains("fluid");
		b.solid = flag(def, "solid", true) && !b.fluid;
		b.opaque = b.solid && !flag(def, "transparent");
		const std::string shape = str(def, "shape", "cube");
		if (!(id == "core:air" || (!b.solid && !b.fluid && shape == "cube"))) {
			static const char *const FACES[6] = {"side", "side", "top", "bottom", "side", "side"}; // +X -X +Y -Y +Z -Z
			for (int f = 0; f < 6; f++) {
				b.tiles[f] = atlas.face_tile(def, FACES[f], f == 2 ? inum(def, "texture_turns", 0) : 0);
			}
		}
		for (int t : b.tiles) {
			b.cutout = b.cutout || (t < int(table.atlas.tiles.size()) && has_alpha(table.atlas.tiles[t]));
		}
		b.emit = static_cast<uint8_t>(std::clamp(inum(def, "light", 0), 0, gocube::LIGHT_MAX));
		b.fluid_level = static_cast<uint8_t>(std::clamp(inum(def, "level", 8), 1, 8));
		b.random_rotation = flag(def, "random_rotation");
		b.tint = flag(def, "tint");

		bool has_collision = false;
		const int es = engine_shape(shape);
		if (es == gocube::SHAPE_BOXES) {
			std::vector<Box16> boxes;
			if (shape == "boxes") {
				for (const Data &bx : def.value("boxes", Data::array())) {
					Box16 box{};
					for (size_t k = 0; k < 6 && k < bx.size(); k++) {
						box[k] = bx[k].get<double>();
					}
					boxes.push_back(box);
				}
			} else {
				boxes = model_boxes(shape, def.value("state", Data::object()));
			}
			b.shape = gocube::SHAPE_BOXES;
			b.model = to_boxes(boxes);
			b.collision = is_not_solid_shape(shape) ? std::vector<Box>{} : b.model;
			has_collision = true;
		} else if (es == gocube::SHAPE_CROSS) {
			b.shape = gocube::SHAPE_CROSS;
			has_collision = true; // none
		} else if (es == gocube::SHAPE_FENCE) {
			b.shape = gocube::SHAPE_FENCE;
		}
		if (!has_collision && b.solid) {
			b.collision.push_back({{0, 0, 0}, {1, 1, 1}});
		}
		// Its icon: an "icon" image, else its side face.
		if (def.contains("icon") && def["icon"].is_string()) {
			if (const int t = atlas.image_tile(def, def["icon"].get<std::string>()); t >= 0) {
				table.icons[id] = t;
			}
		}
		if (!table.icons.count(id)) {
			table.icons[id] = b.tiles[0] ? b.tiles[0] : atlas.color_tile(def, "#ff00ff");
		}
		table.blocks.push_back(std::move(b));
	}
	for (const auto &[id, def] : content.items) {
		int t = -1;
		if (def.contains("texture") && def["texture"].is_string()) {
			t = atlas.image_tile(def, def["texture"].get<std::string>());
		}
		table.icons[id] = t >= 0 ? t : atlas.color_tile(def, "#b0a080");
	}
	atlas.build();
	return table;
}

} // namespace gc
