#include "content/item_models.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

struct V3 {
	float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

ModelVertex vertex(V3 p, V3 n, float u, float v) {
	return {{p.x, p.y, p.z}, {n.x, n.y, n.z}, {u, v}};
}

// The single box (in 0..1 of the cell) a slab, a one-box "boxes" block or a
// bed fills; the whole cell for everything else (ItemDrop._model_bounds).
void model_bounds(const Json &def, float mn[3], float mx[3]) {
	mn[0] = mn[1] = mn[2] = 0;
	mx[0] = mx[1] = mx[2] = 1;
	const std::string shape = def.value("shape", "cube");
	if (shape == "slab") {
		mx[1] = 0.5f;
	} else if (shape == "bed") {
		mx[1] = 9.0f / 16.0f;
	} else if (shape == "boxes") {
		const Json boxes = def.value("boxes", Json::array());
		if (boxes.is_array() && boxes.size() == 1 && boxes[0].is_array() && boxes[0].size() >= 6) {
			for (int k = 0; k < 3; k++) {
				mn[k] = float(boxes[0][k].get<double>() / 16.0);
				mx[k] = float(boxes[0][k + 3].get<double>() / 16.0);
			}
		}
	}
}

} // namespace

bool ItemModels::is_block(const std::string &item) const {
	const int index = content_.block_index(item);
	return index > 0 && (table_.blocks[size_t(index)].solid || content_.blocks.at(item).value("shape", "") == "bed");
}

const std::vector<ModelVertex> &ItemModels::model(const std::string &item) {
	auto it = models_.find(item);
	if (it != models_.end()) {
		return it->second;
	}
	std::vector<ModelVertex> out;
	const int index = content_.block_index(item);
	const Json *def = index > 0 ? &content_.blocks.at(item) : nullptr;
	if (def && def->value("shape", "") == "bed") {
		// A whole bed in one cell: the head half (pillow) towards -Z, the foot
		// half towards +Z, each squeezed to half its length; without the two
		// faces where they meet.
		const float h = 9.0f / 16.0f;
		const float foot_mn[3] = {0, 0, 0.5f}, foot_mx[3] = {1, h, 1};
		const float head_mn[3] = {0, 0, 0}, head_mx[3] = {1, h, 0.5f};
		add_box(out, index, foot_mn, foot_mx, 5);
		const int head = content_.block_index(item + "_n_head");
		add_box(out, head > 0 ? head : index, head_mn, head_mx, 4);
	} else if (index > 0 && table_.blocks[size_t(index)].solid) {
		float mn[3], mx[3];
		model_bounds(*def, mn, mx);
		add_box(out, index, mn, mx, -1);
	} else {
		add_extruded(out, table_.icon(item));
	}
	return models_.emplace(item, std::move(out)).first->second;
}

// ItemDrop._add_box: a CUBE_SIZE cube squeezed into the cell part mn..mx,
// each face its own texture; sides show the texture rows the block fills.
void ItemModels::add_box(std::vector<ModelVertex> &out, int block, const float mn[3], const float mx[3], int skip) const {
	static const V3 NORMALS[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	static const float CORNERS[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	const float s = CUBE_SIZE;
	for (int f = 0; f < 6; f++) {
		if (f == skip) {
			continue;
		}
		const V3 n = NORMALS[f];
		// Two axes across the face: right, and up (v runs down), as seen from outside.
		const V3 up = n.y > 0.5f ? V3{0, 0, -1} : (n.y < -0.5f ? V3{0, 0, 1} : V3{0, 1, 0});
		const V3 right = cross(up, n);
		float r[4];
		table_.atlas.tile_uv(table_.blocks[size_t(block)].tiles[f], r);
		ModelVertex q[4];
		for (int c = 0; c < 4; c++) {
			const float cx = CORNERS[c][0], cy = CORNERS[c][1];
			const V3 unit = n * 0.5f + right * (cx - 0.5f) - up * (cy - 0.5f) + V3{0.5f, 0.5f, 0.5f};
			const V3 p = {mn[0] + unit.x * (mx[0] - mn[0]), mn[1] + unit.y * (mx[1] - mn[1]),
					mn[2] + unit.z * (mx[2] - mn[2])};
			const float u = cx, v = std::abs(n.y) < 0.5f ? 1.0f - p.y : cy;
			q[c] = vertex((p - V3{0.5f, 0.5f, 0.5f}) * s, n, r[0] + (r[2] - r[0]) * u, r[1] + (r[3] - r[1]) * v);
		}
		for (int i : {0, 1, 2, 0, 2, 3}) {
			out.push_back(q[i]);
		}
	}
}

// ItemDrop._extruded_mesh: the icon with depth, front and back faces plus
// side walls wherever an opaque pixel borders a clear one.
void ItemModels::add_extruded(std::vector<ModelVertex> &out, int tile) const {
	if (tile < 0 || size_t(tile) >= table_.atlas.tiles.size()) {
		return;
	}
	const std::vector<uint8_t> &img = table_.atlas.tiles[size_t(tile)];
	const int w = Atlas::TILE, h = Atlas::TILE;
	float r[4];
	table_.atlas.tile_uv(tile, r);
	auto uv_of = [&](float x, float y, float &u, float &v) {
		u = r[0] + (r[2] - r[0]) * x / float(w);
		v = r[1] + (r[3] - r[1]) * y / float(h);
	};
	auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && img[size_t(y * w + x) * 4 + 3] >= 128; };
	const float px = ITEM_SIZE / float(std::max(w, h));
	const float t = ITEM_THICKNESS / 2.0f;
	auto quad = [&](V3 a, V3 b, V3 c, V3 d, V3 n, float u, float v) {
		for (const V3 &p : {a, b, c, a, c, d}) {
			out.push_back(vertex(p, n, u, v));
		}
	};
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (!solid(x, y)) {
				continue;
			}
			const float x0 = (float(x) - float(w) / 2.0f) * px, x1 = x0 + px;
			const float y0 = (float(h) / 2.0f - float(y)) * px, y1 = y0 - px; // image rows go down
			float cu, cv;
			uv_of(float(x) + 0.5f, float(y) + 0.5f, cu, cv);
			// Front (+Z) and back (-Z): the same pixel.
			float fu[4], fv[4];
			uv_of(float(x), float(y), fu[0], fv[0]);
			uv_of(float(x + 1), float(y), fu[1], fv[1]);
			uv_of(float(x + 1), float(y + 1), fu[2], fv[2]);
			uv_of(float(x), float(y + 1), fu[3], fv[3]);
			const V3 front[4] = {{x0, y0, t}, {x1, y0, t}, {x1, y1, t}, {x0, y1, t}};
			for (int i : {0, 2, 1, 0, 3, 2}) {
				out.push_back(vertex(front[i], {0, 0, 1}, fu[i], fv[i]));
			}
			const V3 back[4] = {{x0, y0, -t}, {x1, y0, -t}, {x1, y1, -t}, {x0, y1, -t}};
			for (int i : {0, 1, 2, 0, 2, 3}) {
				out.push_back(vertex(back[i], {0, 0, -1}, fu[i], fv[i]));
			}
			// Walls where the neighbour is clear.
			if (!solid(x, y - 1)) {
				quad({x0, y0, -t}, {x0, y0, t}, {x1, y0, t}, {x1, y0, -t}, {0, 1, 0}, cu, cv);
			}
			if (!solid(x, y + 1)) {
				quad({x0, y1, t}, {x0, y1, -t}, {x1, y1, -t}, {x1, y1, t}, {0, -1, 0}, cu, cv);
			}
			if (!solid(x - 1, y)) {
				quad({x0, y0, -t}, {x0, y1, -t}, {x0, y1, t}, {x0, y0, t}, {-1, 0, 0}, cu, cv);
			}
			if (!solid(x + 1, y)) {
				quad({x1, y0, t}, {x1, y1, t}, {x1, y1, -t}, {x1, y0, -t}, {1, 0, 0}, cu, cv);
			}
		}
	}
}

} // namespace gc
