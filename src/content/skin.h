#pragma once
// Player skins: the common box-figure skin layout, 64 x 64 pixels (or the
// older 64 x 32), read from a PPM (magenta is see-through: the outer layer's
// empty parts). game/creature_models.h dresses a player's figure in it; the
// session sends each player's to everyone (net/session.h).

#include "content/image.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gc {

constexpr int SKIN_SIZE = 64;
constexpr size_t SKIN_BYTES = size_t(SKIN_SIZE) * SKIN_SIZE * 4; // RGBA

// A skin from an image: 64 x 64 as it is; 64 x 32 (the older layout) made
// 64 x 64, its left arm and leg copied from the right ones (mirrored) and no
// outer layer but the hat's. False for any other size.
bool to_skin(const Image &image, Image &skin);

// One part of the figure in the layout: its box is w x h x d pixels and its
// faces are laid out from (u, v), the usual way: the top and bottom in a row
// above, then the right side, front, left side and back.
struct SkinPart {
	int u, v, w, h, d;
};

// The figure's parts: inner layer, then outer layer (a little bigger).
namespace skin_parts {
constexpr SkinPart HEAD{0, 0, 8, 8, 8}, HAT{32, 0, 8, 8, 8};
constexpr SkinPart BODY{16, 16, 8, 12, 4}, JACKET{16, 32, 8, 12, 4};
constexpr SkinPart RIGHT_ARM{40, 16, 4, 12, 4}, RIGHT_SLEEVE{40, 32, 4, 12, 4};
constexpr SkinPart LEFT_ARM{32, 48, 4, 12, 4}, LEFT_SLEEVE{48, 48, 4, 12, 4};
constexpr SkinPart RIGHT_LEG{0, 16, 4, 12, 4}, RIGHT_PANTS{0, 32, 4, 12, 4};
constexpr SkinPart LEFT_LEG{16, 48, 4, 12, 4}, LEFT_PANTS{0, 48, 4, 12, 4};
} // namespace skin_parts

// Where a face of a part is in the skin, in pixels: x, y, width, height.
// `face`: 0 +x (the figure's right side; it faces -z), 1 -x, 2 top, 3
// bottom, 4 +z (back), 5 -z (front). Side faces can be cut to rows
// [from, to) of the part's h (an upper arm and forearm share one arm).
void skin_face(const SkinPart &part, int face, float rect[4], int from = 0, int to = -1);

} // namespace gc
