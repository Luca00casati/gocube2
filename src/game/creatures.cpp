#include "game/creatures.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

constexpr float PI = 3.14159265f;

float lerp_angle(float from, float to, float w) {
	float d = std::fmod(to - from, 2 * PI);
	if (d > PI) {
		d -= 2 * PI;
	} else if (d < -PI) {
		d += 2 * PI;
	}
	return from + d * w;
}

float number(const Json &def, const char *key, float fallback) {
	auto it = def.find(key);
	return it != def.end() && it->is_number() ? float(it->get<double>()) : fallback;
}

} // namespace

int Creatures::spawn(const Content &content, const std::string &kind, const float at[3], const std::string &variant) {
	auto it = content.creatures.find(kind);
	if (it == content.creatures.end() || int(list_.size()) >= MAX) {
		return 0;
	}
	const Json &def = it->second;
	Creature c;
	c.id = next_id_++;
	c.kind = kind;
	c.model = def.value("model", "");
	c.variant = variant;
	std::copy(at, at + 3, c.pos);
	std::copy(at, at + 3, c.prev);
	const Json box = def.value("box", Json::array());
	if (box.is_array() && box.size() == 3) {
		for (int k = 0; k < 3; k++) {
			c.box[k] = box[k].is_number() ? float(box[k].get<double>()) : c.box[k];
		}
	}
	c.gravity = number(def, "gravity", 24);
	c.step_height = number(def, "step_height", 1);
	c.turn_rate = number(def, "turn_rate", 8);
	c.jump = number(def, "jump", 0);
	c.light_height = number(def, "light_height", 0.5f);
	c.jump_when_blocked = def.value("jump_when_blocked", false);
	list_.push_back(std::move(c));
	return list_.back().id;
}

bool Creatures::remove(int id) {
	auto it = std::find_if(list_.begin(), list_.end(), [&](const Creature &c) { return c.id == id; });
	if (it == list_.end()) {
		return false;
	}
	list_.erase(it);
	return true;
}

Creature *Creatures::find(int id) {
	auto it = std::find_if(list_.begin(), list_.end(), [&](const Creature &c) { return c.id == id; });
	return it == list_.end() ? nullptr : &*it;
}

const Creature *Creatures::find(int id) const {
	auto it = std::find_if(list_.begin(), list_.end(), [&](const Creature &c) { return c.id == id; });
	return it == list_.end() ? nullptr : &*it;
}

void Creatures::step(float dt, const World &world) {
	for (Creature &c : list_) {
		std::copy(c.pos, c.pos + 3, c.prev);
		c.prev_yaw = c.yaw;
		const float dx = -std::sin(c.heading), dz = -std::cos(c.heading);
		c.vel[0] = dx * c.speed;
		c.vel[2] = dz * c.speed;
		c.vel[1] = std::max(c.vel[1] - c.gravity * dt, -40.0f);
		const float motion[3] = {c.vel[0] * dt, c.vel[1] * dt, c.vel[2] * dt};
		const MoveResult r = world.move_box(c.pos, c.box, motion, c.step_height);
		c.hit_wall = c.hit_wall || r.hit_wall;
		if (c.jump_when_blocked && r.hit_wall && r.on_floor && c.speed > 0) {
			c.vel[1] = c.jump; // over whatever is in the way
		} else if (r.on_floor) {
			c.vel[1] = 0;
		}
		c.on_floor = r.on_floor;
		c.blocked = c.speed > 0 &&
				std::hypot(r.position[0] - c.pos[0], r.position[2] - c.pos[2]) < c.speed * dt * 0.3f;
		std::copy(r.position, r.position + 3, c.pos);
		c.walking = c.speed > 0;
		c.yaw = lerp_angle(c.yaw, c.heading, 1.0f - std::exp(-c.turn_rate * dt));
	}
}

void Creatures::animate(float dt) {
	for (Creature &c : list_) {
		c.walk_phase += dt * (c.walking ? 9.0f : 0.0f);
		c.hurt_time = std::max(0.0f, c.hurt_time - dt);
		c.flash = std::max(0.0f, c.flash - dt * 3.0f);
		c.attacking = std::max(0.0f, c.attacking - dt);
		if (c.model != "ninja") {
			continue;
		}
		// The Godot version's Ninja._process: the figure from how it moves.
		const float smooth = 1.0f - std::exp(-10.0f * dt);
		const float moving = c.blocked ? 0.0f : std::hypot(c.vel[0], c.vel[2]);
		c.anim_speed += (moving - c.anim_speed) * smooth;
		const float vspeed = c.on_floor ? 0.0f : c.vel[1];
		c.figure.move_speed = c.anim_speed;
		c.figure.on_floor = std::abs(vspeed) < 1.5f;
		c.figure.ninja = true;
		if (c.strikes != c.seen_strikes) {
			c.seen_strikes = c.strikes;
			c.figure.swing();
		}
		c.anim_time += dt;
		const float run = std::min(c.anim_speed / 3.6f, 1.0f); // the tails hang still when it stands
		c.tails[0] = 0.5f * run + std::sin(c.anim_time * (6.0f + 3.0f * c.anim_speed)) * 0.25f * run;
		c.tails[1] = std::sin(c.anim_time * 4.3f) * 0.2f * run;
		c.figure.update(dt);
	}
}

int Creatures::pick(const float origin[3], const float dir[3], float max_distance, float &distance) const {
	int best = 0;
	distance = max_distance;
	for (const Creature &c : list_) {
		// Slab test against its box.
		const float mn[3] = {c.pos[0] - c.box[0] / 2, c.pos[1], c.pos[2] - c.box[2] / 2};
		const float mx[3] = {c.pos[0] + c.box[0] / 2, c.pos[1] + c.box[1], c.pos[2] + c.box[2] / 2};
		float t0 = 0, t1 = distance;
		bool hit = true;
		for (int k = 0; k < 3 && hit; k++) {
			if (std::abs(dir[k]) < 1e-8f) {
				hit = origin[k] >= mn[k] && origin[k] <= mx[k];
				continue;
			}
			float a = (mn[k] - origin[k]) / dir[k], b = (mx[k] - origin[k]) / dir[k];
			if (a > b) {
				std::swap(a, b);
			}
			t0 = std::max(t0, a);
			t1 = std::min(t1, b);
			hit = t0 <= t1;
		}
		if (hit && t0 < distance) {
			distance = t0;
			best = c.id;
		}
	}
	return best;
}

Json Creatures::describe(Creature &c) const {
	Json j = {{"id", c.id}, {"kind", c.kind}, {"variant", c.variant}, {"x", c.pos[0]}, {"y", c.pos[1]},
			{"z", c.pos[2]}, {"yaw", c.yaw}, {"heading", c.heading}, {"speed", c.speed}, {"on_floor", c.on_floor},
			{"hit_wall", c.hit_wall}, {"walking", c.walking}};
	c.hit_wall = false;
	return j;
}

void Creatures::apply(Creature &c, const Json &f) {
	if (!f.is_object()) {
		return;
	}
	auto num = [&](const char *key, float &out) {
		if (auto it = f.find(key); it != f.end() && it->is_number() && std::isfinite(it->get<double>())) {
			out = float(it->get<double>());
		}
	};
	num("heading", c.heading);
	num("speed", c.speed);
	c.speed = std::clamp(c.speed, 0.0f, 20.0f);
	if (auto it = f.find("yaw"); it != f.end() && it->is_number()) {
		c.yaw = c.prev_yaw = float(it->get<double>());
	}
	if (auto it = f.find("variant"); it != f.end() && it->is_string() && it->get<std::string>() != c.variant) {
		c.variant = it->get<std::string>();
		c.hurt_time = 0.3f; // a little squash as it changes
	}
	if (auto it = f.find("push"); it != f.end() && it->is_array() && it->size() == 3) {
		for (int k = 0; k < 3; k++) {
			if ((*it)[k].is_number()) {
				c.vel[k] = std::clamp(float((*it)[k].get<double>()), -40.0f, 40.0f);
			}
		}
	}
	if (f.value("hurt", false)) {
		c.hurt_time = 0.3f;
	}
	if (f.value("flash", false)) {
		c.flash = 1.0f;
	}
	if (f.value("attack", false)) {
		c.attacking = 0.35f;
		c.strikes++;
	}
}

} // namespace gc
