#pragma once
// Items lying in the world: they pop out of broken blocks, fall, merge with
// the same item nearby, drift to the player and go into their inventory,
// and disappear after LIFETIME. As the Godot version's ItemDrop(s).

#include "content/content.h"
#include "game/inventory.h"
#include "world/world.h"

#include <random>
#include <string>
#include <vector>

namespace gc {

class ItemDrops {
public:
	static constexpr float BOX = 0.25f;          // collision box size
	static constexpr float GRAVITY = 20.0f;
	static constexpr float PICKUP_DELAY = 0.5f;  // not grabbed the instant it pops out
	static constexpr float MAGNET_RADIUS = 2.6f;
	static constexpr float PICKUP_RADIUS = 1.2f;
	static constexpr float LIFETIME = 300.0f;
	static constexpr float MERGE_DISTANCE = 0.8f;

	struct Drop {
		std::string item;
		int count = 0;
		float pos[3] = {0, 0, 0}; // bottom centre
		float vel[3] = {0, 0, 0};
		float age = 0;
	};

	void spawn(const std::string &item, int count, const float at[3]);
	// One physics step: falling, drifting to the player (feet at `player`),
	// picking up into `inventory`. Returns what was picked up.
	std::vector<Stack> step(float dt, const World &world, const float player[3], bool player_alive, Inventory &inventory);
	const std::vector<Drop> &drops() const { return drops_; }

	// Their geometry (world coordinates), for this moment (`time`: for the bob and spin).
	void build_mesh(const World &world, const BlockTable &table, const Content &content, float time,
			std::vector<WorldVertex> &out) const;

private:
	std::vector<Drop> drops_;
	std::mt19937 random_{std::random_device{}()};
};

} // namespace gc
