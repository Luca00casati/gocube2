// Terrain: opaque blocks, cutout blocks (see-through pixels) and fluids.
//
// The engine bakes block lighting into the vertex colour: r = sky light,
// g = block light (0..1 = levels 0..15), b = corner occlusion (1 = none),
// a = 1, less 0.5 on faces whose green varies across the land (grass,
// leaves), less 0.25 on faces in a block's shadow.
//
// Everything that doesn't come from the sun (block light, the sky's light in
// shade) is the "shade" light; faces turned towards the sun (by night the
// moon) get the full light instead, unless they're in a block's shadow (block
// shadows: the mesher works out which faces the sun reaches).
//
// A matte, old-school look: the texture's colour (sRGB numbers, as stored:
// `atlas` is the atlas' plain view) times the face's shade, the corner
// occlusion and the light, and nothing added: a face in full light is its
// texture, never brighter or washed out. Fog: in the haze's colour (the
// sky's horizon), pow(smoothstep(fog_start, fog_end, distance), 1.6), by 3D
// distance.

#include "color.hlsli"

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
	float player_light;    // the player's own light: this level at the eyes, one less per block (hand.c3: PLAYER_LIGHT)
	float2 frame_pad;
};

cbuffer Draw : register(b1) {
	float3 chunk_origin;   // world position of the chunk's (0, 0, 0)
	float draw_pad;
};

Texture2D albedo : register(t0);
SamplerState nearest_mip : register(s0);

static const float4 FACE_LIGHT = float4(1.0, 0.8, 0.6, 0.5); // top, north/south, east/west, bottom
static const float SHADE = 0.3; // in shade, sky light keeps 1 - SHADE * sun_amount

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

// (`color` in sRGB numbers; the haze comes in linear, as the sky has it.)
float3 apply_fog(float3 color, float3 world) {
	float d = length(world - camera_pos);
	return lerp(color, to_srgb(fog_color), pow(smoothstep(fog_start, fog_end, d), 1.6));
}

// Sky and block light at a fragment: the light in shade and in full sun.
void lights(PSIn i, out float3 shade, out float3 full) {
	float3 sky = light_curve(i.color.r) * daylight * sky_tint;
	// The player's light, at the block in front of the face (whole blocks, as placed light).
	float3 away = abs(floor(i.world + i.normal * 0.5) - floor(camera_pos));
	float own = max(player_light - (away.x + away.y + away.z), 0.0) / 15.0;
	float3 blk = max(block_light_color * light_curve(max(i.color.g, own)), min_light);
	full = max(sky, blk);
	shade = max(sky * (1.0 - SHADE * sun_amount), blk);
}

float4 shade_terrain(PSIn i, float4 tex) {
	float3 n = i.normal;
	float face = n.y > 0.5 ? FACE_LIGHT.x : (n.y < -0.5 ? FACE_LIGHT.w : (abs(n.z) > 0.5 ? FACE_LIGHT.y : FACE_LIGHT.z));
	float3 base = tex.rgb;
	float marks = round(i.color.a * 4.0); // 4, 3 (shadow), 2 (tinted), 1 (tinted, shadow)
	float sunlit = 1.0 - fmod(marks, 2.0);
	base *= face * i.color.b;
	float3 shade, full;
	lights(i, shade, full);
	float facing = smoothstep(0.0, 0.25, dot(n, sun_dir));
	float3 light = lerp(shade, full, sunlit * facing);
	return float4(apply_fog(base * light, i.world), 1.0);
}

float4 ps_opaque(PSIn i) : SV_Target {
	return shade_terrain(i, albedo.Sample(nearest_mip, i.uv));
}

float4 ps_cutout(PSIn i) : SV_Target {
	float4 tex = albedo.Sample(nearest_mip, i.uv);
	clip(tex.a - 0.5); // see-through pixels: glass, ladders, plants
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
	// (Unlike the terrain, every side of the surface takes the sun: the fluids'
	// version's fluid light() has no facing term.)
	color *= lerp(shade, full, sunlit);
	return float4(apply_fog(color, i.world), alpha); // (blended over what's there, in sRGB numbers)
}
