#include "content/image.h"

#include "data/lua_data.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

namespace gc {

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

// --- PPM ---------------------------------------------------------------------------

bool parse_ppm(std::string_view b, Image &out) {
	size_t p = 0;
	auto skip_space = [&] {
		while (p < b.size()) {
			if (b[p] == '#') { // a comment, to the end of the line
				while (p < b.size() && b[p] != '\n') {
					p++;
				}
			} else if (b[p] == ' ' || b[p] == '\t' || b[p] == '\r' || b[p] == '\n') {
				p++;
			} else {
				break;
			}
		}
	};
	auto number = [&](int &v) {
		skip_space();
		v = 0;
		size_t digits = 0;
		while (p < b.size() && b[p] >= '0' && b[p] <= '9' && digits < 6) {
			v = v * 10 + (b[p++] - '0');
			digits++;
		}
		return digits > 0;
	};
	if (b.size() < 2 || b[0] != 'P' || (b[1] != '6' && b[1] != '3')) {
		return false;
	}
	const bool binary = b[1] == '6';
	p = 2;
	int w = 0, h = 0, maxval = 0;
	if (!number(w) || !number(h) || !number(maxval) || w <= 0 || h <= 0 || w > 4096 || h > 4096 || maxval <= 0 ||
			maxval > 255) {
		return false;
	}
	const size_t count = size_t(w) * size_t(h) * 3;
	std::vector<uint8_t> rgb(count);
	if (binary) {
		p++; // the single whitespace after maxval
		if (b.size() < p + count) {
			return false;
		}
		std::copy(b.begin() + p, b.begin() + p + count, rgb.begin());
	} else {
		for (size_t i = 0; i < count; i++) {
			int v = 0;
			if (!number(v) || v > maxval) {
				return false;
			}
			rgb[i] = uint8_t(v);
		}
	}
	out.width = w;
	out.height = h;
	out.rgba.assign(size_t(w) * size_t(h) * 4, 0);
	for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
		uint8_t c[3];
		for (int k = 0; k < 3; k++) {
			c[k] = uint8_t((rgb[i * 3 + k] * 255 + maxval / 2) / maxval);
		}
		const bool clear = c[0] == 255 && c[1] == 0 && c[2] == 255; // magenta: see-through
		for (int k = 0; k < 3; k++) {
			out.rgba[i * 4 + k] = clear ? 0 : c[k];
		}
		out.rgba[i * 4 + 3] = clear ? 0 : 255;
	}
	return true;
}

bool read_ppm(const std::filesystem::path &path, Image &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		return false;
	}
	const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return parse_ppm(bytes, out);
}

// --- Painted faces -----------------------------------------------------------------

namespace {

uint32_t mix(uint32_t h) {
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

struct Color {
	float r = 1, g = 0, b = 1;
};

Color color_of(const Data &v, const char *fallback = "#ff00ff") {
	uint8_t c[4] = {255, 0, 255, 255};
	if (!(v.is_string() && parse_html_color(v.str(), c))) {
		parse_html_color(fallback, c);
	}
	return {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f};
}

double number(const Data &t, const char *key, double fallback, double lo, double hi) {
	const Data &v = t.is_object() ? t[key] : Data();
	return v.is_number() ? std::clamp(v.get<double>(), lo, hi) : fallback;
}

// A table's feature `key`: one table, or a list of them.
std::vector<const Data *> features(const Data &spec, const char *key) {
	std::vector<const Data *> out;
	if (!spec.is_object() || !spec.contains(key)) {
		return out;
	}
	const Data &v = spec[key];
	if (v.is_object()) {
		out.push_back(&v);
	} else if (v.is_array()) {
		for (const Data &e : v) {
			if (e.is_object() && out.size() < 16) {
				out.push_back(&e);
			}
		}
	}
	return out;
}

struct Painter {
	int n;          // pixels across
	uint32_t seed;  // from the spec
	float noise, blotch;
	int shades, blotch_size;
	std::vector<Color> px;
	std::vector<float> value; // each pixel's 0..1 lightness draw

	int wrap(int v) const { return ((v % n) + n) % n; }

	float rnd(int x, int y, uint32_t salt) const {
		const uint32_t h = mix(seed ^ mix(uint32_t(x) * 0x9e3779b1u + uint32_t(y) * 0x85ebca77u + salt * 0xc2b2ae3du));
		return float(h >> 8) / 16777216.0f;
	}

	// Value noise that wraps every n pixels, `size` pixels across.
	float smooth(float x, float y, int size, uint32_t salt) const {
		const int cells = std::max(1, n / std::max(1, size));
		const float s = float(n) / float(cells);
		const float fx = x / s, fy = y / s;
		const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
		float tx = fx - float(x0), ty = fy - float(y0);
		tx = tx * tx * (3 - 2 * tx);
		ty = ty * ty * (3 - 2 * ty);
		auto at = [&](int cx, int cy) { return rnd(((cx % cells) + cells) % cells, ((cy % cells) + cells) % cells, salt); };
		const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
		const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
		return a + (b - a) * ty;
	}

	// `c` lighter or darker by a 0..1 draw, in `shades` steps.
	Color shade(Color c, float v) const {
		const int level = std::min(shades - 1, int(v * float(shades)));
		const float f = shades > 1 ? 1.0f + noise * ((float(level) + 0.5f) / float(shades) * 2.0f - 1.0f) : 1.0f;
		return {c.r * f, c.g * f, c.b * f};
	}

	void set(int x, int y, Color c) { px[size_t(wrap(y)) * n + wrap(x)] = c; }
	float v(int x, int y) const { return value[size_t(wrap(y)) * n + wrap(x)]; }

	void cells(const Data &t, Color base, uint32_t salt) {
		const int size = int(number(t, "size", 5, 2, n));
		const int count = std::max(1, n / size);
		const float s = float(n) / float(count);
		const Color edge = color_of(t["edge"], "#000000");
		const std::vector<const Data *> colors = [&] {
			std::vector<const Data *> c;
			if (t["colors"].is_array()) {
				for (const Data &e : t["colors"]) {
					c.push_back(&e);
				}
			}
			return c;
		}();
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				float d1 = 1e9f, d2 = 1e9f;
				int best = 0;
				for (int cy = 0; cy < count; cy++) {
					for (int cx = 0; cx < count; cx++) {
						const float px_ = (float(cx) + 0.15f + 0.7f * rnd(cx, cy, salt)) * s;
						const float py_ = (float(cy) + 0.15f + 0.7f * rnd(cx, cy, salt + 1)) * s;
						float dx = std::fabs(float(x) + 0.5f - px_), dy = std::fabs(float(y) + 0.5f - py_);
						dx = std::min(dx, float(n) - dx);
						dy = std::min(dy, float(n) - dy);
						const float d = std::sqrt(dx * dx + dy * dy);
						if (d < d1) {
							d2 = d1;
							d1 = d;
							best = cy * count + cx;
						} else if (d < d2) {
							d2 = d;
						}
					}
				}
				const float own = rnd(best, 0, salt + 2);
				if (d2 - d1 < 1.0f) {
					set(x, y, shade(edge, v(x, y)));
				} else {
					const Color c = colors.empty() ? base
					                               : color_of(*colors[size_t(own * float(colors.size())) % colors.size()]);
					set(x, y, shade(c, 0.5f * v(x, y) + 0.5f * own));
				}
			}
		}
	}

	void stripes(const Data &t, uint32_t salt) {
		const Color c = color_of(t["color"]);
		const bool across = t["across"].is_boolean() && t["across"].get<bool>();
		const int every = int(number(t, "every", 4, 1, n));
		const int width = int(number(t, "width", 1, 1, every));
		const int offset = int(number(t, "offset", 0, -n, n));
		const float wobble = float(number(t, "wobble", 0, 0, 4));
		const float broken = float(number(t, "broken", 0, 0, 1));
		const bool seams = t.contains("seams");
		const Color seam = color_of(t["seams"]);
		for (int a = 0; a < n; a++) {     // along the stripes
			for (int b = 0; b < n; b++) { // across them
				const int shift = wobble > 0 ? int(std::lround((smooth(float(b) * 3.0f, float(a), 4, salt) - 0.5f) * 2.0f * wobble)) : 0;
				const int pos = b + offset + shift;
				const int x = across ? a : b, y = across ? b : a;
				const int stripe = int(std::floor(float(pos) / float(every)));
				if (((pos % every) + every) % every < width) {
					if (broken <= 0 || rnd(stripe, a / 3, salt + 1) >= broken) {
						set(x, y, shade(c, v(x, y)));
					}
				} else if (seams) { // one seam in each board, across it
					const int board = stripe;
					const int at = int(rnd(board, 0, salt + 2) * float(n));
					if (a == at) {
						set(x, y, shade(seam, v(x, y)));
					}
				}
			}
		}
	}

	void rings(const Data &t) {
		const Color c = color_of(t["color"]);
		const int every = int(number(t, "every", 3, 2, n));
		const bool has_border = t.contains("border");
		const Color border = color_of(t["border"]);
		const float bw = float(number(t, "border_width", 2, 0, n / 2.0));
		const float mid = float(n) / 2.0f;
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				const float dx = float(x) + 0.5f - mid, dy = float(y) + 0.5f - mid;
				const float d = std::sqrt(dx * dx + dy * dy) + (rnd(x, y, 77) - 0.5f) * 0.6f;
				if (has_border && d >= mid - bw) {
					set(x, y, shade(border, v(x, y)));
				} else if (int(d) % every == every - 1) {
					set(x, y, shade(c, v(x, y)));
				}
			}
		}
	}

	void band(const Data &t, uint32_t salt) {
		const Color c = color_of(t["color"]);
		const int depth = int(number(t, "depth", 3, 0, n));
		const int ragged = int(number(t, "ragged", 1, 0, n));
		for (int x = 0; x < n; x++) {
			const int d = depth + int(rnd(x, 0, salt) * float(ragged + 1));
			for (int y = 0; y < std::min(d, n); y++) {
				set(x, y, shade(c, v(x, y)));
			}
		}
	}

	void spots(const Data &t, uint32_t salt) {
		const Color c = color_of(t["color"]);
		const Color light = t.contains("light") ? color_of(t["light"]) : c;
		const Color dark = t.contains("dark") ? color_of(t["dark"]) : c;
		const int count = int(number(t, "count", 4, 0, 64));
		const int size = int(number(t, "size", 3, 1, 32));
		const bool dash = t["shape"].is_string() && t["shape"].str() == "dash";
		for (int k = 0; k < count; k++) {
			int x = int(rnd(k, 1, salt) * float(n)), y = int(rnd(k, 2, salt) * float(n));
			for (int i = 0; i < size; i++) {
				const float r = rnd(k, 10 + i, salt);
				set(x, y, r < 0.25f ? light : (r > 0.8f ? dark : c));
				if (dash) {
					x++;
				} else { // a little cluster: a step to a neighbour
					const int dir = int(rnd(k, 100 + i, salt) * 4.0f) & 3;
					x += dir == 0 ? 1 : (dir == 1 ? -1 : 0);
					y += dir == 2 ? 1 : (dir == 3 ? -1 : 0);
				}
			}
		}
	}
};

} // namespace

Image paint_face(const Data &spec, int size) {
	const std::string text = write_lua_data(spec);
	uint32_t seed = 2166136261u; // FNV-1a of the spec: the same spec, the same pixels
	for (unsigned char ch : text) {
		seed = (seed ^ ch) * 16777619u;
	}
	int n = int(number(spec, "pixels", 16, 1, size));
	int p2 = 1;
	while (p2 * 2 <= n) {
		p2 *= 2;
	}
	n = p2;
	Painter p{n, seed, float(number(spec, "noise", 0.1, 0, 1)), float(number(spec, "blotch", 0.3, 0, 1)),
			int(number(spec, "shades", 4, 1, 16)), int(number(spec, "blotch_size", 4, 1, n)), {}, {}};
	const Color base = color_of(spec.is_object() ? spec["color"] : Data());
	std::vector<Color> ramp;
	if (spec.is_object() && spec["colors"].is_array()) {
		for (const Data &c : spec["colors"]) {
			ramp.push_back(color_of(c));
		}
	}
	p.px.resize(size_t(n) * n);
	p.value.resize(size_t(n) * n);
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			const float v = p.rnd(x, y, 1) * (1 - p.blotch) + p.smooth(float(x), float(y), p.blotch_size, 2) * p.blotch;
			p.value[size_t(y) * n + x] = v;
			p.px[size_t(y) * n + x] = ramp.empty() ? p.shade(base, v)
			                                       : ramp[std::min(ramp.size() - 1, size_t(v * float(ramp.size())))];
		}
	}
	uint32_t salt = 1000;
	for (const Data *t : features(spec, "cells")) {
		p.cells(*t, base, salt += 10);
	}
	for (const Data *t : features(spec, "stripes")) {
		p.stripes(*t, salt += 10);
	}
	for (const Data *t : features(spec, "rings")) {
		p.rings(*t);
	}
	for (const Data *t : features(spec, "band")) {
		p.band(*t, salt += 10);
	}
	for (const Data *t : features(spec, "spots")) {
		p.spots(*t, salt += 10);
	}
	Image out;
	out.width = out.height = size;
	out.rgba.resize(size_t(size) * size * 4);
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			const Color c = p.px[size_t(y * n / size) * n + size_t(x * n / size)];
			uint8_t *o = &out.rgba[(size_t(y) * size + x) * 4];
			o[0] = uint8_t(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255.0f));
			o[1] = uint8_t(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255.0f));
			o[2] = uint8_t(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
			o[3] = 255;
		}
	}
	return out;
}

} // namespace gc
