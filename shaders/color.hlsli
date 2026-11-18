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

float3 to_srgb(float3 l) {
	return max(1.055 * pow(max(l, 0.0), 0.416666667) - 0.055, 0.0);
}
