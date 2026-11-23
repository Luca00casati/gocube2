// Player skins: the common box-figure skin layout, 64 x 64 pixels (or the
// older 64 x 32), read from a PNG (see-through pixels: the outer layer's
// empty parts). game/creature_models dresses a player's figure in it; the
// session sends each player's to everyone. (src/content/skin.*)
module gocube::content::skin;
import gocube::content::image, std::math;

const int SKIN_SIZE = 64;
const sz SKIN_BYTES = (sz)SKIN_SIZE * SKIN_SIZE * 4; // RGBA

// Copies the w x h block at (sx, sy) to (dx, dy), mirrored left to right.
fn void copy_mirrored(Image* img, int sx, int sy, int dx, int dy, int w, int h) @private
{
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			img.rgba[((sz)(dy + y) * SKIN_SIZE + dx + x) * 4 : 4] = img.rgba[((sz)(sy + y) * SKIN_SIZE + sx + w - 1 - x) * 4 : 4];
		}
	}
}

// A limb of the older layout (4 x 12 x 4 at u, v) copied to (du, dv) as the
// other side's: each face mirrored, and the right and left sides swapped.
fn void mirror_limb(Image* img, int u, int v, int du, int dv) @private
{
	copy_mirrored(img, u + 4, v, du + 4, dv, 4, 4);          // top
	copy_mirrored(img, u + 8, v, du + 8, dv, 4, 4);          // bottom
	copy_mirrored(img, u + 8, v + 4, du, dv + 4, 4, 12);     // its left side becomes the right
	copy_mirrored(img, u + 4, v + 4, du + 4, dv + 4, 4, 12); // front
	copy_mirrored(img, u, v + 4, du + 8, dv + 4, 4, 12);     // its right side becomes the left
	copy_mirrored(img, u + 12, v + 4, du + 12, dv + 4, 4, 12); // back
}

// A skin from an image: 64 x 64 as it is; 64 x 32 (the older layout) made
// 64 x 64, its left arm and leg copied from the right ones (mirrored) and no
// outer layer but the hat's. False for any other size.
fn bool to_skin(Image* image, Image* skin)
{
	if (image.width != SKIN_SIZE || (image.height != SKIN_SIZE && image.height != SKIN_SIZE / 2) ||
		image.rgba.len != (sz)image.width * image.height * 4)
	{
		return false;
	}
	skin.free();
	*skin = { SKIN_SIZE, SKIN_SIZE, mem::new_array(char, SKIN_BYTES) };
	skin.rgba[:image.rgba.len] = image.rgba[..];
	if (image.height == SKIN_SIZE / 2)
	{
		mirror_limb(skin, 0, 16, 16, 48);  // the left leg from the right
		mirror_limb(skin, 40, 16, 32, 48); // the left arm from the right
	}
	return true;
}

// One part of the figure in the layout: its box is w x h x d pixels and its
// faces are laid out from (u, v), the usual way: the top and bottom in a row
// above, then the right side, front, left side and back.
struct SkinPart
{
	int u, v, w, h, d;
}

// The figure's parts: inner layer, then outer layer (a little bigger).
const SkinPart HEAD = { 0, 0, 8, 8, 8 };
const SkinPart HAT = { 32, 0, 8, 8, 8 };
const SkinPart BODY = { 16, 16, 8, 12, 4 };
const SkinPart JACKET = { 16, 32, 8, 12, 4 };
const SkinPart RIGHT_ARM = { 40, 16, 4, 12, 4 };
const SkinPart RIGHT_SLEEVE = { 40, 32, 4, 12, 4 };
const SkinPart LEFT_ARM = { 32, 48, 4, 12, 4 };
const SkinPart LEFT_SLEEVE = { 48, 48, 4, 12, 4 };
const SkinPart RIGHT_LEG = { 0, 16, 4, 12, 4 };
const SkinPart RIGHT_PANTS = { 0, 32, 4, 12, 4 };
const SkinPart LEFT_LEG = { 16, 48, 4, 12, 4 };
const SkinPart LEFT_PANTS = { 0, 48, 4, 12, 4 };

// Where a face of a part is in the skin, in pixels: x, y, width, height.
// `face`: 0 +x (the figure's right side; it faces -z), 1 -x, 2 top, 3
// bottom, 4 +z (back), 5 -z (front). Side faces can be cut to rows
// [from, to) of the part's h (an upper arm and forearm share one arm).
fn void skin_face(SkinPart p, int face, float[4]* rect, int from = 0, int to = -1)
{
	if (to < 0) to = p.h;
	from = math::clamp(from, 0, p.h);
	to = math::clamp(to, from, p.h);
	float x = 0;
	float y = (float)(p.v + p.d + from);
	float w = 0;
	float h = (float)(to - from);
	switch (face)
	{
		case 0: // right side (+x)
			x = (float)p.u;
			w = (float)p.d;
		case 1: // left side (-x)
			x = (float)(p.u + p.d + p.w);
			w = (float)p.d;
		case 2: // top
			x = (float)(p.u + p.d);
			y = (float)p.v;
			w = (float)p.w;
			h = (float)p.d;
		case 3: // bottom
			x = (float)(p.u + p.d + p.w);
			y = (float)p.v;
			w = (float)p.w;
			h = (float)p.d;
		case 4: // back (+z)
			x = (float)(p.u + 2 * p.d + p.w);
			w = (float)p.w;
		default: // front (-z)
			x = (float)(p.u + p.d);
			w = (float)p.w;
	}
	*rect = { x, y, w, h };
}
