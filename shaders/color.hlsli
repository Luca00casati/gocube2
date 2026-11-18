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

// A dyed block's (or item's) texture: the dye's colour, lighter and darker
// as the texture is (so stone stays stony and planks keep their grain).
// dye.a 0: not dyed, the texture as it is.
float3 apply_dye(float3 tex, float4 dye) {
	if (dye.a < 0.5) {
		return tex;
	}
	float lum = dot(tex, float3(0.299, 0.587, 0.114));
	return dye.rgb * clamp(lum * 1.5 + 0.25, 0.0, 1.25);
}

float3 to_srgb(float3 l) {
	return max(1.055 * pow(max(l, 0.0), 0.416666667) - 0.055, 0.0);
}
