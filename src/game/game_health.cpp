// Game: health, damage, death and respawn, spawning on dry ground.

#include "game/game.h"
#include "game/vitals.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

constexpr float DROWN_DAMAGE = 2;     // per second without breath
constexpr float REGEN_DELAY = 4.0f;   // seconds without damage before healing
constexpr float REGEN_INTERVAL = 2.5f; // seconds per health point healed
constexpr int SPAWN_SEARCH = 64;
constexpr int TOP_Y = 127;            // the highest block y

int floor_i(float v) {
	return int(std::floor(v));
}

} // namespace

void Game::damage(int amount, const std::string &source) {
	if (creative_ || dead_ || amount <= 0) {
		return;
	}
	if (source == "mob") { // armour takes off 4% a point, up to 60%
		const float kept = 1.0f - std::min(0.6f, float(inventory_.armor_points()) * 0.04f);
		amount = std::max(1, int(std::lround(float(amount) * kept)));
	}
	health_ = std::max(0, health_ - amount);
	since_damage_ = 0;
	regen_timer_ = 0;
	since_hurt_ = 0;
	float eye[3];
	player_.eye(eye);
	play_sound("core:effects", "hurt", eye);
	if (health_ == 0) {
		dead_ = true;
		std::fill(player_.velocity, player_.velocity + 3, 0.0f);
	}
}

void Game::after_move(float dt) {
	const float y = player_.position[1];
	// The block under the feet (footsteps sound like it).
	const std::string below = rules_.id_of(
			world_.block_at(floor_i(player_.position[0]), floor_i(y - 0.1f), floor_i(player_.position[2])));
	// Falling: remember the highest point since leaving the ground.
	if (player_.on_floor || player_.in_water || player_.flying || player_.on_ladder) {
		if (player_.on_floor && !was_on_floor_ && !player_.in_water && !player_.flying && !player_.on_ladder) {
			const float fall = fall_peak_ - y;
			if (fall > 0.6f) {
				play_block_sound(below, "step", player_.position); // landing
				const std::optional<Data> r = mods_.rule("fall_damage", Data::array({fall}));
				damage(r && r->is_number() ? int(r->get<double>()) : int(std::floor(std::max(0.0f, fall - 3.0f))));
			}
		}
		fall_peak_ = y;
	} else {
		fall_peak_ = std::max(fall_peak_, y);
	}
	was_on_floor_ = player_.on_floor;

	// Into water fast: a splash. Walking: a footstep every STRIDE metres
	// (sooner sneaking), in the sound of the block underfoot.
	const float vy = std::min(player_.velocity[1], last_vy_);
	if (player_.in_water && !was_in_water_ && vy < -4.0f) {
		play_sound("core:effects", "splash", player_.position);
	}
	was_in_water_ = player_.in_water;
	last_vy_ = player_.velocity[1];
	if (feet_known_ && player_.on_floor && !player_.in_water) {
		constexpr float STRIDE = 1.7f;
		step_distance_ += std::hypot(player_.position[0] - last_feet_[0], player_.position[2] - last_feet_[2]);
		if (step_distance_ >= STRIDE * (player_.sneaking ? 0.6f : 1.0f)) {
			step_distance_ = 0;
			play_block_sound(below, "step", player_.position, -6.0f);
		}
	}
	std::copy(player_.position, player_.position + 3, last_feet_);
	feet_known_ = true;

	if (creative_ || dead_ || remote_) { // (a client: the host keeps our health)
		return;
	}
	auto block_def = [&](float bx, float by, float bz) -> const Data & {
		return rules_.def(world_.block_at(floor_i(bx), floor_i(by), floor_i(bz)));
	};
	// Fluids that hurt ("damage": lava) burn every half second; the first touch at once.
	const Data &body = block_def(player_.position[0], y + 0.4f, player_.position[2]);
	if (body.contains("damage") && body["damage"].is_number()) {
		burn_timer_ += dt;
		if (burn_timer_ >= 0.5f) {
			burn_timer_ = 0;
			damage(int(body["damage"].get<double>()), "lava");
		}
	} else {
		burn_timer_ = 0.5f;
	}
	// Below the world's bottom: hurt every half second, the first at once.
	if (y < float(World::BOTTOM_Y)) {
		void_timer_ += dt;
		if (void_timer_ >= 0.5f) {
			void_timer_ = 0;
			damage(void_damage(mods_, y), "void");
		}
	} else {
		void_timer_ = 0.5f;
	}
	// Breath: lost with the head in water, then drowning; back three times as fast.
	const Data &head = block_def(player_.position[0], y + Player::EYE_HEIGHT, player_.position[2]);
	if (head.contains("fluid")) {
		breath_ = std::max(0.0f, breath_ - dt);
		if (breath_ <= 0) {
			drown_timer_ += dt;
			if (drown_timer_ >= 1.0f) {
				drown_timer_ = 0;
				damage(int(DROWN_DAMAGE));
			}
		}
	} else {
		breath_ = std::min(MAX_BREATH, breath_ + dt * 3.0f);
		drown_timer_ = 0;
	}
	// Healing after a while without damage.
	since_damage_ += dt;
	if (health_ < MAX_HEALTH && since_damage_ >= REGEN_DELAY) {
		regen_timer_ += dt;
		if (regen_timer_ >= REGEN_INTERVAL) {
			regen_timer_ = 0;
			health_++;
		}
	}
}

void Game::unstuck() {
	const float size[3] = {Player::RADIUS * 2, Player::HEIGHT, Player::RADIUS * 2};
	for (int dy = 0; dy < 256; dy++) {
		const float p[3] = {player_.position[0], player_.position[1] + float(dy), player_.position[2]};
		if (world_.is_box_free(p, size)) {
			player_.position[1] = p[1];
			return;
		}
	}
}

void Game::respawn() {
	float at[3] = {0.5f, 40.0f, 0.5f};
	if (world_spawn_) {
		std::copy(world_spawn_->begin(), world_spawn_->end(), at);
	}
	if (bed_) {
		if (rules_.def(world_.block_at(bed_->x, bed_->y, bed_->z)).value("bed", false)) {
			at[0] = bed_->x + 0.5f;
			at[1] = bed_->y + 0.5625f;
			at[2] = bed_->z + 0.5f;
		} else {
			bed_.reset();
			show_notice("Your bed was missing, so you're back at the world spawn");
		}
	}
	health_ = MAX_HEALTH;
	breath_ = MAX_BREATH;
	dead_ = false;
	std::fill(player_.velocity, player_.velocity + 3, 0.0f);
	std::copy(at, at + 3, player_.position);
	fall_peak_ = at[1];
	if (world_.is_loaded(floor_i(at[0]), floor_i(at[1]), floor_i(at[2]))) {
		unstuck();
	}
	if (remote_) {
		remote_->respawned(); // (the host gives us our health back)
	}
}

void Game::set_world_spawn(const Data &saved) {
	if (saved.is_array() && saved.size() == 3 && saved[0].is_number()) {
		world_spawn_ = std::array<float, 3>{saved[0].get<float>(), saved[1].get<float>(), saved[2].get<float>()};
	}
}

// The nearest column (searching outward, every second one, up to
// SPAWN_SEARCH blocks) whose top is a biome's surface (grass, sand, snow)
// and not under water; else the nearest dry one. False while the ground
// isn't loaded yet.
bool Game::try_place_on_ground() {
	const int ox = floor_i(player_.position[0]), oz = floor_i(player_.position[2]);
	if (!world_.is_loaded(ox, 0, oz)) {
		return false;
	}
	std::vector<int> surfaces;
	for (const auto &[id, b] : content_.biomes) {
		surfaces.push_back(content_.block_index(b.value("surface", "")));
	}
	std::optional<std::array<float, 3>> fallback;
	for (int radius = 0; radius <= SPAWN_SEARCH; radius += 2) {
		for (int dz = -radius; dz <= radius; dz += 2) {
			for (int dx = -radius; dx <= radius; dx += 2) {
				if (std::max(std::abs(dx), std::abs(dz)) != radius) {
					continue; // only the ring at this radius
				}
				const int x = ox + dx, z = oz + dz;
				if (!world_.is_loaded(x, TOP_Y, z) || !world_.is_loaded(x, -96, z)) {
					if (radius == 0) {
						return false; // wait for the column under the player
					}
					continue;
				}
				const float from[3] = {x + 0.5f, float(TOP_Y), z + 0.5f}, down[3] = {0, -1, 0};
				const RayHit hit = world_.raycast(from, down, 400);
				if (!hit.hit || rules_.is_fluid(world_.block_at(hit.previous.x, hit.previous.y, hit.previous.z))) {
					continue;
				}
				const std::array<float, 3> feet = {x + 0.5f, float(hit.position.y + 1), z + 0.5f};
				if (std::find(surfaces.begin(), surfaces.end(), int(hit.block)) != surfaces.end()) {
					std::copy(feet.begin(), feet.end(), player_.position);
					return true;
				}
				if (!fallback) {
					fallback = feet;
				}
			}
		}
	}
	if (fallback) {
		std::copy(fallback->begin(), fallback->end(), player_.position);
	}
	return true;
}

} // namespace gc
