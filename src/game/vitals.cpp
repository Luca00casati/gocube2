#include "game/vitals.h"

#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

constexpr float REGEN_DELAY = 4.0f, REGEN_INTERVAL = 2.5f; // as Game's
constexpr int DROWN_DAMAGE = 2;

const Data &def_at(const World &world, const BlockRules &rules, float x, float y, float z) {
	return rules.def(world.block_at(int(std::floor(x)), int(std::floor(y)), int(std::floor(z))));
}

} // namespace

void Vitals::hurt(int amount) {
	if (dead || amount <= 0) {
		return;
	}
	health = std::max(0, health - amount);
	since_damage_ = 0;
	regen_timer_ = 0;
	dead = health == 0;
	changed = true;
}

void Vitals::heal(int amount) {
	if (dead || amount <= 0 || health >= MAX_HEALTH) {
		return;
	}
	health = std::min(MAX_HEALTH, health + amount);
	changed = true;
}

void Vitals::respawn() {
	health = MAX_HEALTH;
	breath = MAX_BREATH;
	dead = false;
	fall_peak_.reset();
	since_damage_ = 0;
	drown_timer_ = 0;
	changed = true;
}

void Vitals::moved(const World &world, const BlockRules &rules, Mods &mods, const float feet[3], bool held_up) {
	if (dead) {
		return;
	}
	const bool in_fluid = def_at(world, rules, feet[0], feet[1] + 0.9f, feet[2]).contains("fluid");
	const bool climbing = def_at(world, rules, feet[0], feet[1], feet[2]).value("climbable", false) ||
			def_at(world, rules, feet[0], feet[1] + 1.0f, feet[2]).value("climbable", false);
	const float below[3] = {feet[0], feet[1] - 0.05f, feet[2]};
	const float body[3] = {Player::RADIUS * 2, Player::HEIGHT, Player::RADIUS * 2};
	if (held_up || in_fluid || climbing || !fall_peak_) {
		fall_peak_ = feet[1];
	} else if (!world.is_box_free(below, body)) { // on the ground
		const float fall = *fall_peak_ - feet[1];
		if (fall > 0.6f) {
			const std::optional<Data> r = mods.rule("fall_damage", Data::array({fall}));
			hurt(r && r->is_number() ? int(r->get<double>()) : int(std::floor(std::max(0.0f, fall - 3.0f))));
		}
		fall_peak_ = feet[1];
	} else {
		fall_peak_ = std::max(*fall_peak_, feet[1]);
	}
}

int void_damage(Mods &mods, float feet_y) {
	const float depth = float(World::BOTTOM_Y) - feet_y;
	const std::optional<Data> r = mods.rule("void_damage", Data::array({depth}));
	return r && r->is_number() ? int(r->get<double>()) : 4;
}

void Vitals::tick(const World &world, const BlockRules &rules, Mods &mods, const float feet[3], bool in_bed, float dt) {
	if (dead) {
		return;
	}
	if (feet[1] < float(World::BOTTOM_Y)) {
		void_timer_ += dt;
		if (void_timer_ >= 0.5f) {
			void_timer_ = 0;
			hurt(void_damage(mods, feet[1]));
		}
	} else {
		void_timer_ = 0.5f; // (at once, the moment it falls out)
	}
	const Data &body = in_bed ? Data::object() : def_at(world, rules, feet[0], feet[1] + 0.4f, feet[2]);
	if (body.contains("damage") && body["damage"].is_number()) {
		burn_timer_ += dt;
		if (burn_timer_ >= 0.5f) {
			burn_timer_ = 0;
			hurt(int(body["damage"].get<double>()));
		}
	} else {
		burn_timer_ = 0.5f; // the first touch hurts at once
	}
	if (!in_bed && def_at(world, rules, feet[0], feet[1] + Player::EYE_HEIGHT, feet[2]).contains("fluid")) {
		breath = std::max(0.0f, breath - dt);
		if (breath <= 0) {
			drown_timer_ += dt;
			if (drown_timer_ >= 1.0f) {
				drown_timer_ = 0;
				hurt(DROWN_DAMAGE);
			}
		}
	} else {
		breath = std::min(MAX_BREATH, breath + dt * 3.0f);
		drown_timer_ = 0;
	}
	since_damage_ += dt;
	if (health < MAX_HEALTH && since_damage_ >= REGEN_DELAY) {
		regen_timer_ += dt;
		if (regen_timer_ >= REGEN_INTERVAL) {
			regen_timer_ = 0;
			health++;
			changed = true;
		}
	}
}

} // namespace gc
