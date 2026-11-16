#pragma once
// How creatures look, built from boxes as the Godot version's are (its
// BlockEater._build_model and Figure): each part a flat colour, or a block's
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
};

// A plain cube (a puff of smoke): `size` across, centred at `at`, one
// colour on every side (raw, not lit).
void build_cube(const float at[3], float size, const float rgb[3], std::vector<CreatureVertex> &out);

// Another player (the Godot version's Avatar): a figure whose looks come
// from its id, posed by `f`, with its eyes at `eye`, facing `yaw`, lit by
// `light`. `hand`: where its right hand is (row-major, row vectors: a held
// item's model goes there).
void build_player(const Figure &f, int id, const float eye[3], float yaw, const float light[3],
		std::vector<CreatureVertex> &out, float hand[16]);

// Appends the creature's boxes, posed for this moment (`alpha`: between its
// last two physics steps), lit by `light` (World.light_for at it; raw).
void build_creature(const Creature &c, const Content &content, const BlockTable &table, float alpha,
		const float light[3], std::vector<CreatureVertex> &out);

} // namespace gc
