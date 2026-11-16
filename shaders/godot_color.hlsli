// Colour as the Godot version's renderer (Godot's Compatibility renderer, the
// only one the development PC's GPU runs) turns it into pixels, measured on
// Godot 4.7 (see chunk.hlsl):
//  - textures and colours are used as they are (sRGB numbers);
//  - EMISSION and unshaded colour go into linear light through Godot's fast
//    approximation of the sRGB curve (godot_to_linear), the sun's share
//    (light()) is added as linear light;
//  - the result is written into a plain 8-bit target through its own
//    approximation the other way (godot_to_srgb: no linear toe, so the
//    darkest shades go to black), and see-through surfaces blend there, in
//    sRGB numbers.
// The world's shaders draw into a plain (not sRGB) target and end with
// godot_to_srgb; blending then happens as it does in the Godot version.

float3 godot_to_linear(float3 c) {
	return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
}

float3 godot_to_srgb(float3 l) {
	return max(1.055 * pow(max(l, 0.0), 0.416666667) - 0.055, 0.0);
}
