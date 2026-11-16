// Terrain: opaque blocks, cutout blocks (see-through pixels) and fluids.
// Ported from the Godot version's chunk.gdshaderinc and chunk_fluid.gdshader.
//
// The engine bakes block lighting into the vertex colour: r = sky light,
// g = block light (0..1 = levels 0..15), b = corner occlusion (1 = none),
// a = 1, less 0.5 on faces whose green varies across the land (grass,
// leaves), less 0.25 on faces in a block's shadow.
//
// Everything that doesn't come from the sun (block light, the sky's light in
// shade) is the "shade" light; faces turned towards the sun add the sun's
// share on top, unless they're in a block's shadow (block shadows: the mesher
// works out which faces the sun reaches) or under a cloud's.
//
// Fog: the Godot version's Environment depth fog, in the haze's colour:
// pow(smoothstep(fog_start, fog_end, distance), 1.6), by 3D distance.
//
// Colours as the Godot version shows them: it runs on Godot's Compatibility
// renderer (OpenGL; the only one the development PC's GPU has), which takes
// textures and colour uniforms as they are (sRGB numbers, not decoded) and
// writes EMISSION out as a pixel value, while the sun's share (its light())
// is added as linear light. So: everything but the sun is worked out from
// the raw texture (`atlas` is the atlas' plain view here) and turned into
// linear light at the end (to_linear), and the sun's share is added after.
// Measured on Godot 4.7 (tests/data/godot_screenshot.gd and probes):
// linear result = to_linear(EMISSION) + DIFFUSE_LIGHT.

cbuffer Frame : register(b0) {
	float4x4 view_proj;
	float3 camera_pos;
	float daylight;        // scales sky light (day/night)
	float3 sun_dir;        // towards the sun
	float sun_amount;      // how much the sun shines (0 at night)
	float3 sky_tint;       // sky light's colour
	float min_light;       // so pitch-dark caves aren't fully black
	float3 fog_color;
	float fog_start;
	float3 block_light_color;
	float fog_end;
	float time;            // seconds, for ripples
	float2 cloud_offset;   // the sky's clouds' drift, in cloud cells
	float frame_pad1;
	float3 hand_light_origin; // the held-light cube's corner (world blocks)
	float hand_light_count;   // torches held (0: hand_light isn't read)
};

cbuffer Draw : register(b1) {
	float3 chunk_origin;   // world position of the chunk's (0, 0, 0)
	float draw_pad;
};

Texture2D albedo : register(t0);
// Torches held in hand: a light level per block (r * 255 = 0..15), spread like
// placed light (HandLights), in the cube of HAND_LIGHT_SIZE blocks from
// hand_light_origin (texture x, y, z = world x, z, y). Each face takes the
// level of the block in front of it, so held light falls in whole blocks too.
Texture3D<float> hand_light : register(t1);
SamplerState nearest_mip : register(s0);
static const float HAND_LIGHT_SIZE = 48.0; // HandLights::SIZE

static const float4 FACE_LIGHT = float4(1.0, 0.78, 0.6, 0.45); // top, north/south, east/west, bottom
static const float SHADE = 0.55; // in shade, sky light keeps 1 - SHADE * sun_amount
static const float CLOUD_SHADE = 0.85;

struct VSIn {
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

struct PSIn {
	float4 pos : SV_Position;
	float3 world : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float2 uv : TEXCOORD2;
	float4 color : COLOR0;
};

PSIn vs_main(VSIn i) {
	PSIn o;
	o.world = i.pos + chunk_origin;
	o.pos = mul(view_proj, float4(o.world, 1.0));
	o.normal = i.normal;
	o.uv = i.uv;
	o.color = i.color;
	return o;
}

// Each light level is 80% as bright as the one above; level 0 is dark.
float light_curve(float level01) {
	return level01 <= 0.0 ? 0.0 : pow(0.8, 15.0 - level01 * 15.0);
}

float hash2(float2 p) {
	float3 p3 = frac(p.xyx * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

float vnoise(float2 p) {
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), f.x), lerp(hash2(i + float2(0, 1)), hash2(i + float2(1, 1)), f.x), f.y);
}

// Cloud shadows: soft patches from the same kind of noise as the sky's
// clouds, drifting with them.
static const float CLOUD_CELL = 9.0; // metres per cloud cell on the ground

// 0 clear .. 1 under a cloud, soft-edged.
float cloud_cover(float2 xz) {
	float2 c = xz / CLOUD_CELL + cloud_offset;
	float n = vnoise(c * 0.085) * 0.7 + vnoise(c * 0.23 + 17.0) * 0.3;
	return smoothstep(0.56, 0.64, n);
}

// The green of grass and leaves drifts slowly across the land.
float3 green_variation(float2 xz) {
	float v = vnoise(xz / 41.0) * 0.65 + vnoise(xz / 13.0 + 5.0) * 0.35;
	return lerp(float3(0.8, 0.94, 0.78), float3(1.14, 1.08, 0.8), v);
}

// An sRGB number to linear light.
float3 to_linear(float3 c) {
	c = max(c, 0.0);
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 apply_fog(float3 color, float3 world) {
	float d = length(world - camera_pos);
	return lerp(color, fog_color, pow(smoothstep(fog_start, fog_end, d), 1.6));
}

// Sky and block light at a fragment: the light in shade and in full sun.
void lights(PSIn i, out float3 shade, out float3 full) {
	float3 sky = light_curve(i.color.r) * daylight * sky_tint;
	float3 blk = block_light_color * light_curve(i.color.g);
	if (hand_light_count > 0.5) {
		float3 cell = floor(i.world + i.normal * 0.5) - hand_light_origin; // the block in front of the face
		if (all(cell >= 0.0) && all(cell < HAND_LIGHT_SIZE)) {
			float level = round(hand_light.Load(int4(int3(cell.xzy), 0)) * 255.0) / 15.0;
			blk = max(blk, block_light_color * light_curve(level));
		}
	}
	blk = max(blk, min_light);
	full = max(sky, blk);
	shade = max(sky * (1.0 - SHADE * sun_amount), blk);
}

float4 shade_terrain(PSIn i, float4 tex) {
	float3 n = i.normal;
	float face = n.y > 0.5 ? FACE_LIGHT.x : (n.y < -0.5 ? FACE_LIGHT.w : (abs(n.z) > 0.5 ? FACE_LIGHT.y : FACE_LIGHT.z));
	float3 base = tex.rgb;
	float marks = round(i.color.a * 4.0); // 4, 3 (shadow), 2 (tinted), 1 (tinted, shadow)
	float sunlit = 1.0 - fmod(marks, 2.0);
	if (marks < 2.5) { // grass, leaves: vary only the green pixels (not a grass side's dirt)
		float green = smoothstep(0.02, 0.08, tex.g - max(tex.r, tex.b));
		base *= lerp(float3(1.0, 1.0, 1.0), green_variation(i.world.xz), green);
	}
	base *= face * i.color.b;
	float3 shade, full;
	lights(i, shade, full);
	float facing = smoothstep(0.0, 0.25, dot(n, sun_dir));
	float3 sun = base * (full - shade) * sunlit * facing * (1.0 - CLOUD_SHADE * cloud_cover(i.world.xz));
	float3 color = to_linear(base * shade) + sun;
	return float4(apply_fog(color, i.world), 1.0);
}

float4 ps_opaque(PSIn i) : SV_Target {
	return shade_terrain(i, albedo.Sample(nearest_mip, i.uv));
}

float4 ps_cutout(PSIn i) : SV_Target {
	float4 tex = albedo.Sample(nearest_mip, i.uv);
	clip(tex.a - 0.5); // see-through pixels: glass, torches, ladders, plants
	return shade_terrain(i, tex);
}

// Gentle moving ripples (0..1).
float ripples(float2 p) {
	float a = sin(p.x * 1.7 + time * 0.9) * sin(p.y * 1.3 - time * 0.7);
	float b = sin((p.x + p.y) * 2.3 - time * 1.3);
	return 0.5 + 0.25 * a + 0.25 * b;
}

static const float3 UNDERSIDE_COLOR = float3(0.72, 0.88, 1.0);
static const float UNDERSIDE_ALPHA = 0.82;

float4 ps_fluid(PSIn i, bool front : SV_IsFrontFace) : SV_Target {
	float3 shade, full;
	lights(i, shade, full);
	float r = ripples(i.world.xz);
	float4 base = albedo.Sample(nearest_mip, i.uv); // each fluid's tile: its colour and transparency
	float3 color = base.rgb * (0.92 + 0.16 * r);
	float alpha = base.a;
	if (!front && alpha < 0.99) {
		// Seen from underneath (in the water): a bright, nearly opaque ceiling.
		color = lerp(base.rgb, UNDERSIDE_COLOR, 0.45 + 0.3 * r);
		alpha = UNDERSIDE_ALPHA;
		shade = full;
	}
	float sunlit = i.color.a > 0.875 ? 1.0 : 0.0; // 0.75: in a block's shadow
	// (Unlike the terrain, every side of the surface takes the sun: the Godot
	// version's fluid light() has no facing term.)
	float3 sun = color * (full - shade) * sunlit * (1.0 - CLOUD_SHADE * cloud_cover(i.world.xz));
	color = to_linear(color * shade) + sun;
	return float4(apply_fog(color, i.world), alpha);
}
