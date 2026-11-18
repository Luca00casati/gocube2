// Colour, as the world's shaders work it out:
//  - textures and colours are used as they are (sRGB numbers);
//  - glowing and unshaded colour go into linear light through a fast
//    approximation of the sRGB curve (to_linear); the sun's share is added
//    as linear light;
//  - the result is written into a plain 8-bit target through the curve the
//    other way (to_srgb: no linear toe, so the darkest shades go to black),
//    and see-through surfaces blend there, in sRGB numbers.

float3 to_linear(float3 c) {
	return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
}

// A dyed block's (or item's) texture, stained: the dye's colour where the
// texture is as bright as its average (`avg`: the tile's, from the atlas'
// smallest mip), lighter and darker as the texture is, with DYE_SHOW of the
// material's own colour showing through (so stone stays stony and planks
// woody). dye.a 0: not dyed, the texture as it is. (content.h's dye_pixel
// is the same, for the icons.)
static const float DYE_SHOW = 0.2;
float3 apply_dye(float3 tex, float3 avg, float4 dye) {
	if (dye.a < 0.5) {
		return tex;
	}
	float lum = dot(tex, float3(0.299, 0.587, 0.114));
	float alum = max(dot(avg, float3(0.299, 0.587, 0.114)), 0.04);
	return lerp(dye.rgb * clamp(lum / alum, 0.0, 1.6), tex, DYE_SHOW);
}

// The atlas' mip level where a tile's cell (Atlas::CELL copies of
// Atlas::TILE texels) is one texel: the tile's average colour.
static const float TILE_AVERAGE_MIP = 7.0;

float3 to_srgb(float3 l) {
	return max(1.055 * pow(max(l, 0.0), 0.416666667) - 0.055, 0.0);
}
