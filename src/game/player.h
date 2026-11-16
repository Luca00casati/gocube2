#pragma once
// The player: walking, sprinting, sneaking, jumping, swimming, climbing ladders
// and flying, in fixed physics steps (Player::STEP); breaking and placing
// blocks. Movement and the numbers are the Godot version's (player.gd).
// Creative-style for now: blocks break at once and any block can be placed
// (mining time, inventories and health come with survival).

#include "game/blocks.h"
#include "world/world.h"

#include <functional>
#include <string>
#include <vector>

namespace gc {

// A block the player broke or placed (for mods' block_broken and block_placed events).
struct BlockChange {
	BlockPos pos;
	std::string id; // the block broken, or placed
	bool placed = false;
};

struct PlayerInput {
	float forward = 0, right = 0; // -1..1 each
	bool jump = false, sneak = false, sprint = false;
};

class Player {
public:
	static constexpr float STEP = 1.0f / 60.0f; // physics step, seconds
	static constexpr float EYE_HEIGHT = 1.62f;
	static constexpr float HEIGHT = 1.8f;
	static constexpr float RADIUS = 0.3f;
	static constexpr float REACH = 6.0f;

	float position[3] = {0, 0, 0}; // feet
	float velocity[3] = {0, 0, 0};
	float yaw = 0, pitch = 0;      // radians: yaw 0 looks towards -z, positive turns left; pitch up
	bool flying = false;
	bool on_floor = false, in_water = false, on_ladder = false;
	bool sneaking = false; // in the last step (shorter footsteps)

	// One physics step (STEP seconds).
	void step(const PlayerInput &input, const World &world, const BlockRules &rules);

	void eye(float out[3]) const;
	void look(float out[3]) const; // unit view direction
	RayHit target(const World &world) const; // the block aimed at, within REACH

	// Breaks the block aimed at (a door or bed goes as a whole). False if none.
	// What was broken is added to `changes` if given.
	bool break_target(World &world, const BlockRules &rules, std::vector<BlockChange> *changes = nullptr) const;
	// Right click: opens or closes a door aimed at (unless sneaking), else
	// places `block` against the face aimed at. False if nothing happened.
	// What was placed is added to `changes` if given. `occupied`: whether
	// someone else stands in a cell (no solid block goes there).
	bool use_or_place(World &world, const BlockRules &rules, const std::string &block, bool sneak,
			std::vector<BlockChange> *changes = nullptr,
			const std::function<bool(const BlockPos &)> &occupied = {}) const;
	// Whether a player standing with feet at `feet` overlaps the block `cell`.
	static bool overlaps_block(const float feet[3], const BlockPos &cell);

private:
	bool against_wall_ = false;
};

} // namespace gc
