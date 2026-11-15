#include "game/drops.h"

#include <algorithm>
#include <cmath>

namespace gc {

void ItemDrops::spawn(const std::string &item, int count, const float at[3]) {
	if (count <= 0 || item.empty()) {
		return;
	}
	for (Drop &d : drops_) {
		const float dx = d.pos[0] - at[0], dy = d.pos[1] - at[1], dz = d.pos[2] - at[2];
		if (d.item == item && dx * dx + dy * dy + dz * dz < MERGE_DISTANCE * MERGE_DISTANCE) {
			d.count += count;
			return;
		}
	}
	std::uniform_real_distribution<float> side(-1.5f, 1.5f);
	Drop d;
	d.item = item;
	d.count = count;
	std::copy(at, at + 3, d.pos);
	d.vel[0] = side(random_);
	d.vel[1] = 3.0f;
	d.vel[2] = side(random_);
	drops_.push_back(d);
}

std::vector<Stack> ItemDrops::step(float dt, const World &world, const float player[3], bool player_alive,
		Inventory &inventory) {
	std::vector<Stack> picked;
	const float size[3] = {BOX, BOX, BOX};
	for (size_t i = 0; i < drops_.size();) {
		Drop &d = drops_[i];
		d.age += dt;
		if (d.age > LIFETIME) {
			drops_.erase(drops_.begin() + std::ptrdiff_t(i));
			continue;
		}
		float to[3] = {0, 0, 0};
		float dist = 1e9f;
		if (player_alive && d.age > PICKUP_DELAY) {
			to[0] = player[0] - d.pos[0];
			to[1] = player[1] + 0.9f - d.pos[1];
			to[2] = player[2] - d.pos[2];
			dist = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
			if (dist < PICKUP_RADIUS) {
				const int left = inventory.add(d.item, d.count);
				if (left < d.count) {
					picked.push_back({d.item, d.count - left});
				}
				d.count = left;
				if (d.count == 0) {
					drops_.erase(drops_.begin() + std::ptrdiff_t(i));
					continue;
				}
			}
		}
		if (dist < MAGNET_RADIUS && dist > 1e-4f && inventory.can_fit(d.item)) {
			for (int k = 0; k < 3; k++) { // drift towards the player
				d.vel[k] = to[k] / dist * 6.0f;
				d.pos[k] += d.vel[k] * dt;
			}
		} else {
			d.vel[1] = std::max(d.vel[1] - GRAVITY * dt, -30.0f);
			d.vel[0] *= 0.92f;
			d.vel[2] *= 0.92f;
			const float motion[3] = {d.vel[0] * dt, d.vel[1] * dt, d.vel[2] * dt};
			const MoveResult r = world.move_box(d.pos, size, motion);
			std::copy(r.position, r.position + 3, d.pos);
			if (r.on_floor) {
				d.vel[0] *= 0.5f;
				d.vel[1] = 0;
				d.vel[2] *= 0.5f;
			}
		}
		i++;
	}
	return picked;
}

void ItemDrops::build_mesh(const World &world, const BlockTable &table, const Content &content, float /*time*/,
		std::vector<WorldVertex> &out) const {
	auto tile_uv = [&](int tile, float uv[4]) { table.atlas.tile_uv(tile, uv); };
	for (const Drop &d : drops_) {
		int sky, blk;
		world.light_at(int(std::floor(d.pos[0])), int(std::floor(d.pos[1] + 0.3f)), int(std::floor(d.pos[2])), sky, blk);
		const float color[4] = {sky / 15.0f, blk / 15.0f, 1.0f, 1.0f};
		const float spin = d.age * 1.2f;
		const float c = std::cos(spin), s = std::sin(spin);
		const float centre[3] = {d.pos[0], d.pos[1] + BOX + 0.06f + 0.06f * std::sin(d.age * 2.5f), d.pos[2]};
		auto emit = [&](const float local[3], const float normal[3], float u, float v) {
			WorldVertex w;
			w.pos[0] = centre[0] + local[0] * c + local[2] * s;
			w.pos[1] = centre[1] + local[1];
			w.pos[2] = centre[2] - local[0] * s + local[2] * c;
			w.normal[0] = normal[0] * c + normal[2] * s;
			w.normal[1] = normal[1];
			w.normal[2] = -normal[0] * s + normal[2] * c;
			w.uv[0] = u;
			w.uv[1] = v;
			std::copy(color, color + 4, w.color);
			out.push_back(w);
		};
		// Two triangles for a quad: corners p0..p3 counter-clockwise from the
		// front, uv rectangle r.
		auto quad = [&](const float p[4][3], const float n[3], const float r[4]) {
			emit(p[0], n, r[0], r[3]);
			emit(p[1], n, r[2], r[3]);
			emit(p[2], n, r[2], r[1]);
			emit(p[0], n, r[0], r[3]);
			emit(p[2], n, r[2], r[1]);
			emit(p[3], n, r[0], r[1]);
		};
		const int block = content.block_index(d.item);
		const bool cube = block > 0 && table.blocks[block].shape == gocube::SHAPE_CUBE && !table.blocks[block].fluid;
		if (cube) {
			// A small cube, 0.25 across: each face's corners from its bottom
			// left, seen from outside (sides upright), in the engine's face order.
			const float h = 0.125f;
			static const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
			static const float P[6][4][3] = {
					{{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},     // +X
					{{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}, // -X
					{{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},     // +Y
					{{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}, // -Y
					{{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     // +Z
					{{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}, // -Z
			};
			for (int f = 0; f < 6; f++) {
				float p[4][3];
				for (int k = 0; k < 4; k++) {
					for (int j = 0; j < 3; j++) {
						p[k][j] = P[f][k][j] * h;
					}
				}
				float r[4];
				tile_uv(table.blocks[block].tiles[f], r);
				quad(p, N[f], r);
			}
		} else {
			// An item: its icon standing up, seen from both sides.
			const float w = 0.21f;
			float r[4];
			tile_uv(table.icon(d.item), r);
			const float front[4][3] = {{-w, -w, 0}, {w, -w, 0}, {w, w, 0}, {-w, w, 0}};
			const float back[4][3] = {{w, -w, 0}, {-w, -w, 0}, {-w, w, 0}, {w, w, 0}};
			const float nf[3] = {0, 0, 1}, nb[3] = {0, 0, -1};
			quad(front, nf, r);
			quad(back, nb, r);
		}
	}
}

} // namespace gc
