#pragma once
// Items lying in the world: they pop out of broken blocks, fall, merge with
// the same item nearby, drift to the player and go into their inventory,
// and disappear after LIFETIME. As the Godot version's ItemDrop(s).

#include "content/content.h"
#include "content/item_models.h"
#include "game/inventory.h"
#include "world/world.h"

#include <functional>
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
		float turn = 0; // the model's starting spin (radians)
	};

	void spawn(const std::string &item, int count, const float at[3]);
	// One physics step: falling, drifting to the player (feet at `player`),
	// picking up into `inventory`. Returns what was picked up.
	std::vector<Stack> step(float dt, const World &world, const float player[3], bool player_alive, Inventory &inventory);
	const std::vector<Drop> &drops() const { return drops_; }

	// Their models (world coordinates; ItemModels), bobbing and slowly
	// spinning, each coloured by `light` at it (linear rgb: the light where it
	// is, for shaders/items.hlsl).
	void build_mesh(ItemModels &models, const std::function<void(const float at[3], float out[3])> &light,
			std::vector<WorldVertex> &out) const;

private:
	std::vector<Drop> drops_;
	std::mt19937 random_{std::random_device{}()};
};

} // namespace gc
