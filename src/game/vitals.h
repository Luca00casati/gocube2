#pragma once
// The host: a joined survival player's health, worked out from where its
// client says it is, by the same rules as the local player's (Game): falls
// (the mods' fall_damage rule), fluids that hurt (lava), falling out of the
// world (void_damage), drowning, creatures
// (less its armour), healing after a while without damage, and food. The
// client only shows it.

#include "game/blocks.h"
#include "mods/mods.h"
#include "world/world.h"

#include <optional>

namespace gc {

class Vitals {
public:
	static constexpr int MAX_HEALTH = 20;
	// (Game's own health, in single player, heals and drowns the same.)
	static constexpr float REGEN_DELAY = 4.0f;    // seconds without damage before healing
	static constexpr float REGEN_INTERVAL = 2.5f; // seconds per health point healed
	static constexpr int DROWN_DAMAGE = 2;        // per second without breath
	static constexpr float MAX_BREATH = 10.0f;

	int health = MAX_HEALTH;
	float breath = MAX_BREATH;
	bool dead = false;
	bool changed = true; // to send to the client

	void hurt(int amount);
	void heal(int amount);
	void respawn();
	// A new position the host accepted (feet). `held_up`: flying or in bed
	// (no fall). A fall ends on the ground, in a fluid or on a ladder.
	void moved(const World &world, const BlockRules &rules, Mods &mods, const float feet[3], bool held_up);
	// Time passing: fluids that hurt burn, below the world hurts, breath runs
	// out with the head in a fluid, health comes back. `in_bed`: lying down
	// (nothing burns or drowns).
	void tick(const World &world, const BlockRules &rules, Mods &mods, const float feet[3], bool in_bed, float dt);

private:
	std::optional<float> fall_peak_;
	float since_damage_ = 0, regen_timer_ = 0, drown_timer_ = 0, burn_timer_ = 0.5f, void_timer_ = 0.5f;
};

// Damage every half second for feet at `feet_y`, below World::BOTTOM_Y (the
// mods' void_damage rule; 4 if none answers). Game's and Vitals'.
int void_damage(Mods &mods, float feet_y);

} // namespace gc
