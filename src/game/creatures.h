#pragma once
// Creatures' bodies: where they are, how they move (gravity, walking up steps,
// turning towards where they head), how they look while they do (the walk
// cycle, a squash or a red flash when hurt) and being hit. What they do is the
// mods' (the core mod's code, src/core_mod/core_mod.cpp): it spawns them, sets
// where they head and how fast, and handles hits, through the mods' `creatures` API.
//
// A kind is content (registry.register_creature): "model" (how it's drawn:
// "block_eater" or "figure": a person), "box" {width, height, depth} (collision, picking),
// "gravity", "step_height" (walks up that much), "turn_rate" (how fast it turns
// towards its heading), "jump" and "jump_when_blocked" (leaps over what stops
// it), "light_height" (where its light is taken).

#include "content/content.h"
#include "game/figure.h"
#include "world/world.h"

#include <string>
#include <vector>

namespace gc {

struct Creature {
	int id = 0;
	std::string kind;     // its definition's id ("core:block_eater")
	std::string model;
	std::string variant;  // what it's made of: "" its own look, or a block id
	float pos[3] = {0, 0, 0};  // feet (bottom centre)
	float prev[3] = {0, 0, 0}; // before the last physics step (drawing interpolates)
	float vel[3] = {0, 0, 0};
	float yaw = 0;        // facing (radians; 0 = -z), turning towards heading
	float prev_yaw = 0;
	float heading = 0;    // where its behaviour wants it to go
	float speed = 0;      // m/s along heading
	bool on_floor = false;
	bool hit_wall = false; // walked into something since its behaviour last looked
	bool blocked = false;  // this step: wanted to move and barely did (legs stop)
	bool walking = false;
	// Looks: time left of a hurt squash, a red flash (0..1), a strike playing.
	float hurt_time = 0, flash = 0, attacking = 0;
	int strikes = 0;      // goes up with every strike (the figure plays one)
	float walk_phase = 0;
	// A figure's (model "figure"): its pose, how fast it seems to go.
	Figure figure;
	float anim_speed = 0;
	int seen_strikes = 0;
	// From its kind.
	float box[3] = {0.6f, 1.8f, 0.6f};
	float gravity = 24, step_height = 1, turn_rate = 8, jump = 0, light_height = 0.5f;
	bool jump_when_blocked = false;
	// A client's copy of the host's creature: it glides to where the host
	// last said it was (no physics here).
	bool remote = false;
	float target[3] = {0, 0, 0};
	float target_yaw = 0;
};

// A creature as the host sends it to clients.
struct CreatureState {
	int id = 0;
	std::string kind, variant;
	float pos[3] = {0, 0, 0};
	float yaw = 0;
	bool walking = false, on_floor = true, blocked = false;
	float flash = 0, hurt_time = 0;
	int strikes = 0;
};

class Creatures {
public:
	static constexpr int MAX = 64; // whatever the mods ask for

	// A creature of `kind` with its feet at `at`; its id, or 0 (unknown kind, too many).
	int spawn(const Content &content, const std::string &kind, const float at[3], const std::string &variant = "");
	bool remove(int id);
	Creature *find(int id);
	const Creature *find(int id) const;
	const std::vector<Creature> &all() const { return list_; }

	// One physics step for every creature (its body only: what it decides is the mods').
	void step(float dt, const World &world);
	// Each frame: the looks (walk cycle, squash, flash, strike).
	void animate(float dt);

	// The creature a ray meets first within max_distance (its box), or 0.
	int pick(const float origin[3], const float dir[3], float max_distance, float &distance) const;

	// The host: every creature's state. A client: the host's (creatures come,
	// go and glide to where it says).
	std::vector<CreatureState> states() const;
	void apply_states(const Content &content, const std::vector<CreatureState> &states);

	// For the mods: a creature as data ({id, kind, variant, x, y, z, yaw,
	// heading, speed, on_floor, hit_wall, walking}); reading it clears hit_wall.
	Data describe(Creature &c) const;
	// From the mods: heading, speed, variant, yaw (at once), push {x, y, z}
	// (its velocity: a knock back), hurt (a squash), flash (red), attack (a strike).
	void apply(Creature &c, const Data &fields);

private:
	std::vector<Creature> list_;
	int next_id_ = 1;
};

} // namespace gc
