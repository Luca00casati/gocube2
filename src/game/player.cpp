#include "game/player.h"

#include "maths.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

constexpr float WALK_SPEED = 4.3f;
constexpr float SPRINT_SPEED = 5.6f;
constexpr float SNEAK_SPEED = 1.3f;
constexpr float FLY_SPEED = 11.0f;
constexpr float FLY_SPRINT_SPEED = 22.0f;
constexpr float JUMP_VELOCITY = 8.4f; // with GRAVITY: ~1.26 blocks high, enough to climb one block
constexpr float GRAVITY = 28.0f;
constexpr float MAX_FALL_SPEED = 60.0f;
constexpr float ACCEL_GROUND = 14.0f;
constexpr float ACCEL_AIR = 3.0f;
constexpr float SWIM_SPEED_FACTOR = 0.6f;
constexpr float CLIMB_SPEED = 3.0f;
constexpr float SWIM_UP_SPEED = 3.5f;
constexpr float WATER_CLIMB_SPEED = 8.0f;
constexpr float SINK_SPEED = 2.0f;

} // namespace

void Player::eye(float out[3]) const {
	out[0] = position[0];
	out[1] = position[1] + EYE_HEIGHT;
	out[2] = position[2];
}

void Player::look(float out[3]) const {
	out[0] = -std::sin(yaw) * std::cos(pitch);
	out[1] = std::sin(pitch);
	out[2] = -std::cos(yaw) * std::cos(pitch);
}

void Player::step(const PlayerInput &in, const World &world, const BlockRules &rules) {
	const float dt = STEP;
	sneaking = in.sneak;
	// The wish direction from the input, turned by yaw only (looking down doesn't slow you).
	float ix = in.right, iz = -in.forward;
	const float il = std::sqrt(ix * ix + iz * iz);
	if (il > 1.0f) {
		ix /= il;
		iz /= il;
	}
	const float c = std::cos(yaw), s = std::sin(yaw);
	const float wish_x = ix * c + iz * s, wish_z = -ix * s + iz * c;

	auto block_at = [&](float x, float y, float z) { return world.block_at(floor_i(x), floor_i(y), floor_i(z)); };
	in_water = rules.is_fluid(block_at(position[0], position[1] + 0.9f, position[2]));
	on_ladder = rules.climbable(block_at(position[0], position[1], position[2])) ||
			rules.climbable(block_at(position[0], position[1] + 1.0f, position[2]));

	if (flying) {
		const float speed = in.sprint ? FLY_SPRINT_SPEED : FLY_SPEED;
		const float vertical = float(in.jump) - float(in.sneak);
		const float target[3] = {wish_x * speed, vertical * speed, wish_z * speed};
		const float k = 1.0f - std::exp(-10.0f * dt);
		for (int a = 0; a < 3; a++) {
			velocity[a] += (target[a] - velocity[a]) * k;
		}
	} else {
		float speed = in.sneak ? SNEAK_SPEED : (in.sprint ? SPRINT_SPEED : WALK_SPEED);
		float accel = on_floor ? ACCEL_GROUND : ACCEL_AIR;
		if (in_water) {
			speed *= SWIM_SPEED_FACTOR;
			accel = ACCEL_GROUND;
		}
		const float k = 1.0f - std::exp(-accel * dt);
		velocity[0] += (wish_x * speed - velocity[0]) * k;
		velocity[2] += (wish_z * speed - velocity[2]) * k;
		if (on_ladder) {
			// Climbing: up with Space or walking into it, down slowly otherwise;
			// sneaking holds on.
			if (in.jump || in.forward > 0.1f) {
				velocity[1] = CLIMB_SPEED;
			} else if (in.sneak) {
				velocity[1] = 0.0f;
			} else {
				velocity[1] = std::max(velocity[1] - GRAVITY * dt, -CLIMB_SPEED);
			}
		} else if (in_water) {
			// Buoyant: sink slowly, Space swims up.
			velocity[1] = std::max(velocity[1] - GRAVITY * 0.3f * dt, -SINK_SPEED);
			if (in.jump) {
				velocity[1] = std::min(velocity[1] + 20.0f * dt, SWIM_UP_SPEED);
				// At the surface (head out of the water): climb out over the edge.
				const bool head_out = !rules.is_fluid(block_at(position[0], position[1] + 1.8f, position[2]));
				if (against_wall_ && head_out) {
					velocity[1] = WATER_CLIMB_SPEED;
				}
			}
		} else {
			velocity[1] = std::max(velocity[1] - GRAVITY * dt, -MAX_FALL_SPEED);
			if (on_floor && in.jump) {
				velocity[1] = JUMP_VELOCITY;
			}
		}
	}

	const float size[3] = {RADIUS * 2, HEIGHT, RADIUS * 2};
	const float motion[3] = {velocity[0] * dt, velocity[1] * dt, velocity[2] * dt};
	const MoveResult r = world.move_box(position, size, motion, 0.0f);
	const float moved[3] = {r.position[0] - position[0], r.position[1] - position[1], r.position[2] - position[2]};
	std::copy(r.position, r.position + 3, position);
	on_floor = r.on_floor;
	against_wall_ = r.hit_wall && std::hypot(wish_x, wish_z) > 0.1f;
	// Lose the velocity that ran into something.
	if (r.on_floor || r.hit_ceiling) {
		velocity[1] = 0.0f;
	}
	if (r.hit_wall) {
		if (std::fabs(moved[0]) < std::fabs(motion[0]) - 1e-4f) {
			velocity[0] = 0.0f;
		}
		if (std::fabs(moved[2]) < std::fabs(motion[2]) - 1e-4f) {
			velocity[2] = 0.0f;
		}
	}
	if (flying && on_floor && in.sneak) {
		flying = false; // flying down onto the ground ends flight
	}
}

RayHit Player::target(const World &world) const {
	float e[3], d[3];
	eye(e);
	look(d);
	return world.raycast(e, d, REACH);
}

bool Player::break_target(World &world, const BlockRules &rules, std::vector<BlockChange> *changes) const {
	const RayHit hit = target(world);
	if (!hit.hit) {
		return false;
	}
	const std::string broken = rules.id_of(hit.block);
	const int air = std::max(0, rules.index_of("core:air"));
	world.set_block(hit.position, gocube::BlockId(air));
	if (changes) {
		changes->push_back({hit.position, broken, false});
	}
	if (auto other = rules.other_part(broken, hit.position)) { // a door or bed goes as a whole
		const std::string there = rules.id_of(world.block_at(other->x, other->y, other->z));
		if (rules.base_of(there) == rules.base_of(broken)) {
			world.set_block(*other, gocube::BlockId(air));
			if (changes) {
				changes->push_back({*other, there, false});
			}
		}
	}
	return true;
}

bool Player::overlaps_block(const float feet[3], const BlockPos &cell) {
	const float mn[3] = {feet[0] - RADIUS, feet[1], feet[2] - RADIUS};
	const float mx[3] = {feet[0] + RADIUS, feet[1] + HEIGHT, feet[2] + RADIUS};
	const float cmn[3] = {float(cell.x), float(cell.y), float(cell.z)};
	for (int k = 0; k < 3; k++) {
		if (mx[k] <= cmn[k] || mn[k] >= cmn[k] + 1.0f) {
			return false;
		}
	}
	return true;
}

bool Player::use_or_place(World &world, const BlockRules &rules, const std::string &dyed_block, bool sneak,
		std::vector<BlockChange> *changes, const std::function<bool(const BlockPos &)> &occupied) const {
	const std::string block = undyed(dyed_block); // (placed with its dye)
	const gocube::Dye dye = dye_of(dyed_block);
	RayHit hit = target(world);
	if (!hit.hit) {
		return false;
	}
	// Doors open and close (sneak to place against them instead).
	if (!sneak) {
		const auto toggle = rules.toggle_door(rules.id_of(hit.block), hit.position);
		if (!toggle.empty()) {
			for (const auto &[cell, id] : toggle) { // (a dyed door stays dyed)
				world.set_block(cell, gocube::BlockId(std::max(0, rules.index_of(id))), world.dye_at(cell.x, cell.y, cell.z));
			}
			return true;
		}
	}
	if (!rules.is_placeable(block)) {
		return false;
	}
	// Placing into tall grass or a flower replaces it.
	if (rules.replaceable(hit.block)) {
		hit.previous = hit.position;
		hit.normal = {0, 1, 0};
	}
	float e[3], d[3];
	eye(e);
	look(d);
	const float hit_y = e[1] + d[1] * hit.distance;
	const BlockRules::Placement places = rules.placement(block, hit, d, hit_y);
	if (places.empty()) {
		return false;
	}
	for (const auto &[cell, id] : places) {
		const int there = world.block_id_at(cell.x, cell.y, cell.z);
		if (there < 0 || (there != 0 && !rules.is_fluid(there) && !rules.replaceable(there))) {
			return false; // occupied (e.g. no room for a door's upper half), or not loaded
		}
		// A torch or ladder needs something to hang on; a door's upper half
		// stands on its lower half, placed along with it.
		bool on_lower = false;
		for (const auto &[c2, id2] : places) {
			on_lower = on_lower || (c2 == cell + BlockPos{0, -1, 0} && rules.base_of(id2) == rules.base_of(id));
		}
		if (!on_lower && !rules.supported(world, id, cell)) {
			return false;
		}
		const Data &def = rules.def(id);
		const bool solid = !def.contains("solid") || !def["solid"].is_boolean() || def["solid"].get<bool>();
		if (solid && (overlaps_block(position, cell) || (occupied && occupied(cell)))) {
			return false; // it would be inside you, or someone else
		}
	}
	for (const auto &[cell, id] : places) {
		const int index = rules.index_of(id);
		if (index > 0) {
			world.set_block(cell, gocube::BlockId(index), dye);
			if (changes) {
				changes->push_back({cell, id, true});
			}
		}
	}
	return true;
}

} // namespace gc
