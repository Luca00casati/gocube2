#pragma once
// The first-person arm and held item: a sleeve, a
// hand coming out of its cuff and the item in it, off the bottom right of the
// view. Swings when you hit, place, use or mine (a swing asked for during
// another plays right after it, whole), bobs while walking.
//
// build() gives their triangles in camera space (x right, y up, -z forward),
// coloured by the light where the eyes are: the renderer draws them over the
// world, the item over the arm.

#include "content/item_models.h"
#include "game/creature_models.h"
#include "world/world.h"

#include <array>
#include <string>
#include <vector>

namespace gc {

// Torches held in hand light the blocks around them like placed ones: spread by World::held_light in a
// cube of SIZE blocks around the viewer, moved in steps of STEP blocks (so
// walking doesn't spread it again every block); spread again only when a
// light moves to another block, the cube moves or the world changes. The
// terrain shaders read it as a 3D texture, one level per block.
class HandLights {
public:
	static constexpr int SIZE = 48, STEP = 8;
	// lights: x, y, z, level. True if the volume changed (upload it again).
	bool update(const World &world, const std::vector<std::array<float, 4>> &lights, const float viewer[3]);
	int level_at(const float at[3]) const; // 0..15
	const BlockPos &origin() const { return origin_; }
	const std::vector<uint8_t> &volume() const { return volume_; } // empty: no light
	int count() const { return count_; }

private:
	BlockPos origin_;
	std::vector<uint8_t> volume_;
	std::vector<int64_t> key_;
	int count_ = 0;
};

// Light for what isn't terrain (dropped and held items, the hand), worked out
// like the terrain shader does: sky light times `sky_light` (the day's: white by day, moonlight at night),
// placed light and held torches in the block light's colour, the brighter of
// the two, never below `least`. Linear rgb.
void light_for(const World &world, const HandLights &hand_lights, const float sky_light[3], const float at[3],
		float least, float out[3]);

class Hand {
public:
	static constexpr float SWING_TIME = 0.3f;

	void swing();
	// Each frame: `mining` keeps it swinging; `walking`: on the floor and moving.
	void update(float dt, bool mining, bool walking);
	// The arm and the item (textured: the colour is the light). `light`:
	// linear. The arm is the player's right arm in its skin (`skin`: the
	// renderer's slot, for draw_creatures) into `skinned`; without one
	// (null), a sleeve and hand into `arm` (untextured: the colour is their
	// tint times the light).
	void build(ItemModels &models, const std::string &item, const float light[3], std::vector<WorldVertex> &arm,
			std::vector<WorldVertex> &held, std::vector<CreatureVertex> *skinned = nullptr, int skin = -1) const;

	// The pose at `t` (0..1 of a swing): rotation about the shoulder, push
	// (camera space) and the held item's tilt, as Euler angles.
	struct Pose {
		float rotation[3], push[3], tilt[3];
	};
	static Pose swing_pose(float t);
	bool swinging() const { return swing_ >= 0; }

private:
	float swing_ = -1;       // time into the swing (-1: none)
	bool swing_next_ = false;
	float bob_ = 0;
	float bob_amount_ = 0;   // 0 standing .. 1 walking (smoothed, so the bob never snaps)
	Pose pose_ = {};
	float bob_offset_[3] = {0, 0, 0};
};

} // namespace gc
