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
	d.turn = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(random_);
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

void ItemDrops::build_mesh(ItemModels &models, const std::function<void(const float at[3], float out[3])> &light,
		std::vector<WorldVertex> &out) const {
	for (const Drop &d : drops_) {
		const float at[3] = {d.pos[0], d.pos[1] + 0.3f, d.pos[2]};
		float lit[3];
		light(at, lit);
		// The Godot version's model: BOX up from the drop's feet, bobbing, and
		// spinning slowly about y from a random start.
		const float spin = d.turn + d.age * 1.2f;
		const float c = std::cos(spin), s = std::sin(spin);
		const float origin[3] = {d.pos[0], d.pos[1] + BOX + 0.06f + 0.06f * std::sin(d.age * 2.5f), d.pos[2]};
		for (const ModelVertex &mv : models.model(d.item)) {
			WorldVertex w;
			w.pos[0] = origin[0] + mv.pos[0] * c + mv.pos[2] * s;
			w.pos[1] = origin[1] + mv.pos[1];
			w.pos[2] = origin[2] - mv.pos[0] * s + mv.pos[2] * c;
			std::copy(mv.normal, mv.normal + 3, w.normal); // (shaded by the model's own normal)
			std::copy(mv.uv, mv.uv + 2, w.uv);
			std::copy(lit, lit + 3, w.color);
			w.color[3] = 1;
			out.push_back(w);
		}
	}
}

} // namespace gc
