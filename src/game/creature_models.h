#pragma once
// How creatures look, built from boxes: each part a flat colour, or a block's
// face texture tiled once a metre (a Block Eater that ate a block looks made
// of it). Vertices in world space for shaders/creatures.hlsl, which shades
// each face by its own normal and the light where the creature is.

#include "content/content.h"
#include "game/creatures.h"

#include <vector>

namespace gc {

struct CreatureVertex {
	float pos[3];    // world
	float local[3];  // in its box (the texture is tiled from it)
	float normal[3]; // its box's own (shading looks at it)
	float color[4];  // rgb: the light times the part's colour (raw); a: the red flash
	float tile[4];   // atlas rectangle of a block face, or all 0 (a flat colour)
	float skin[3] = {0, 0, -1}; // a player's skin: u, v (0..1) and its slot (-1: none)
};

// A plain cube (a puff of smoke): `size` across, centred at `at`, one
// colour on every side (raw, not lit).
void build_cube(const float at[3], float size, const float rgb[3], std::vector<CreatureVertex> &out);

// Another player: a figure posed by `f`, with its eyes at `eye`, facing
// `yaw`, lit by `light`, dressed in the skin in slot `skin` of the
// renderer's skins (content/skin.h: the usual box-figure layout and
// proportions, 32 pixels tall; elbows and knees bend, each half of a limb
// showing its half of the skin). `skin` -1: a plain figure coloured
// from its id. `hand`: where its right hand is (row-major, row
// vectors: a held item's model goes there).
void build_player(const Figure &f, int id, const float eye[3], float yaw, const float light[3],
		std::vector<CreatureVertex> &out, float hand[16], int skin = -1);

// A player's right arm in its skin (inner and outer layer), `pixel` metres a
// skin pixel, its length along the y of `m` (row-major, row vectors: the
// arm's centre at the origin, the shoulder towards +y), lit by `light`: the
// first-person arm.
void build_skinned_arm(const float m[16], float pixel, const float light[3], int skin, std::vector<CreatureVertex> &out);

// Appends the creature's boxes, posed for this moment (`alpha`: between its
// last two physics steps), lit by `light` (World.light_for at it; raw).
void build_creature(const Creature &c, const Content &content, const BlockTable &table, float alpha,
		const float light[3], std::vector<CreatureVertex> &out);

} // namespace gc
