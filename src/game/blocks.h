#pragma once
// Block rules for playing: which blocks can be placed, which variant a
// placement makes (slabs, stairs, torches, ladders, doors, beds), the other
// half of a door or bed, opening doors, and what needs support.
// Ported from the Godot version's BlockShapes, PlantSim.supported and Player.

#include "content/content.h"
#include "world/world.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gc {

class BlockRules {
public:
	explicit BlockRules(const Content &content);

	const std::string &id_of(int block) const; // "" if unknown
	int index_of(const std::string &id) const { return content_.block_index(id); }
	const Json &def(const std::string &id) const; // {} if unknown
	const Json &def(int block) const { return def(id_of(block)); }

	// Blocks a player can place (not air, fluids, variants or hidden ones), by id.
	const std::vector<std::string> &placeable() const { return placeable_; }
	bool is_placeable(const std::string &id) const;
	std::string base_of(const std::string &id) const; // a variant's base block (or the block itself)
	bool replaceable(int block) const;  // placing into it replaces it (tall grass, flowers)
	bool climbable(int block) const;    // ladders
	bool is_fluid(int block) const;

	using Placement = std::vector<std::pair<BlockPos, std::string>>;
	// What placing `base` against the face `hit` makes: [(cell, block id)];
	// doors and beds place two. `look`: the view direction; `hit_y`: the height
	// of the point hit (slabs go in the half you aim at). [] if it can't go there.
	Placement placement(const std::string &base, const RayHit &hit, const float look[3], float hit_y) const;
	// The other half of a door or bed at `pos`.
	std::optional<BlockPos> other_part(const std::string &id, const BlockPos &pos) const;
	// The door at `pos` opened or closed (both halves), or [] if it isn't a door.
	Placement toggle_door(const std::string &id, const BlockPos &pos) const;
	// Whether `id` can stand at `pos`: plants on the right block, torches and
	// ladders on something solid, doors on the floor.
	bool supported(const World &world, const std::string &id, const BlockPos &pos) const;

private:
	bool solid_support(const World &world, const BlockPos &pos) const;
	std::string variant(const std::string &base, const std::string &facing) const;
	std::string door_id(const std::string &base, const std::string &facing, const std::string &half, bool open) const;

	const Content &content_;
	std::vector<std::string> placeable_;
};

} // namespace gc
