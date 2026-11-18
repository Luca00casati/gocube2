#include "content/skin.h"

#include <algorithm>

namespace gc {

namespace {

// Copies the w x h block at (sx, sy) to (dx, dy), mirrored left to right.
void copy_mirrored(Image &img, int sx, int sy, int dx, int dy, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *from = &img.rgba[(size_t(sy + y) * SKIN_SIZE + size_t(sx + w - 1 - x)) * 4];
			std::copy(from, from + 4, &img.rgba[(size_t(dy + y) * SKIN_SIZE + size_t(dx + x)) * 4]);
		}
	}
}

// A limb of the older layout (4 x 12 x 4 at u, v) copied to (du, dv) as the
// other side's: each face mirrored, and the right and left sides swapped.
void mirror_limb(Image &img, int u, int v, int du, int dv) {
	copy_mirrored(img, u + 4, v, du + 4, dv, 4, 4);      // top
	copy_mirrored(img, u + 8, v, du + 8, dv, 4, 4);      // bottom
	copy_mirrored(img, u + 8, v + 4, du, dv + 4, 4, 12); // its left side becomes the right
	copy_mirrored(img, u + 4, v + 4, du + 4, dv + 4, 4, 12); // front
	copy_mirrored(img, u, v + 4, du + 8, dv + 4, 4, 12); // its right side becomes the left
	copy_mirrored(img, u + 12, v + 4, du + 12, dv + 4, 4, 12); // back
}

} // namespace

bool to_skin(const Image &image, Image &skin) {
	if (image.width != SKIN_SIZE || (image.height != SKIN_SIZE && image.height != SKIN_SIZE / 2) ||
			image.rgba.size() != size_t(image.width) * size_t(image.height) * 4) {
		return false;
	}
	skin.width = skin.height = SKIN_SIZE;
	skin.rgba.assign(SKIN_BYTES, 0);
	std::copy(image.rgba.begin(), image.rgba.end(), skin.rgba.begin());
	if (image.height == SKIN_SIZE / 2) {
		mirror_limb(skin, 0, 16, 16, 48);  // the left leg from the right
		mirror_limb(skin, 40, 16, 32, 48); // the left arm from the right
	}
	return true;
}

void skin_face(const SkinPart &p, int face, float rect[4], int from, int to) {
	if (to < 0) {
		to = p.h;
	}
	from = std::clamp(from, 0, p.h);
	to = std::clamp(to, from, p.h);
	float x = 0, y = float(p.v + p.d + from), w = 0, h = float(to - from);
	switch (face) {
		case 0: x = float(p.u); w = float(p.d); break;                 // right side (+x)
		case 1: x = float(p.u + p.d + p.w); w = float(p.d); break;     // left side (-x)
		case 2: x = float(p.u + p.d); y = float(p.v); w = float(p.w); h = float(p.d); break;         // top
		case 3: x = float(p.u + p.d + p.w); y = float(p.v); w = float(p.w); h = float(p.d); break;   // bottom
		case 4: x = float(p.u + 2 * p.d + p.w); w = float(p.w); break; // back (+z)
		default: x = float(p.u + p.d); w = float(p.w); break;          // front (-z)
	}
	rect[0] = x;
	rect[1] = y;
	rect[2] = w;
	rect[3] = h;
}

} // namespace gc
