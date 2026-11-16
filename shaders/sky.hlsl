// The sky: a colour gradient, a square pixel sun and moon, stars at night and
// a drifting layer of blocky clouds. Ported from the Godot version's
// sky.gdshader. Drawn first, behind everything (a full-screen triangle).

#include "godot_color.hlsli"

cbuffer Sky : register(b0) {
	float4x4 inv_view_proj;
	float3 top_color;    // (linear)
	float day;           // 0 night .. 1 day
	float3 horizon_color;
	float dusk;          // 1 at sunrise / sunset
	float3 ground_color;
	float sky_angle;     // the sky's turn (stars move with it)
	float3 sun_dir;      // towards the sun
	float time;
	float2 cloud_offset; // drifting clouds, in cloud cells
	float2 pad;
};

static const float SUN_SIZE = 0.075;   // half-width, in the sun's tangent plane
static const float MOON_SIZE = 0.055;
static const float CLOUD_SCALE = 22.0; // cells across one unit of the cloud plane

struct PSIn {
	float4 pos : SV_Position;
	float2 ndc : TEXCOORD0;
};

PSIn vs_main(uint id : SV_VertexID) {
	PSIn o;
	o.ndc = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
	o.pos = float4(o.ndc, 1.0, 1.0);
	return o;
}

float hash3(float3 p) {
	p = frac(p * float3(443.897, 441.423, 437.195));
	p += dot(p, p.yzx + 19.19);
	return frac((p.x + p.y) * p.z);
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
	float a = hash2(i), b = hash2(i + float2(1, 0)), c = hash2(i + float2(0, 1)), d = hash2(i + float2(1, 1));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Where `dir` hits the square around `center` (in its tangent plane): -1..1
// per axis, or outside (|value| > 1).
float2 square_uv(float3 dir, float3 center, float size) {
	float3 up_ref = abs(center.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 u = normalize(cross(up_ref, center));
	float3 v = cross(center, u);
	float d = dot(dir, center);
	if (d <= 0.0) {
		return float2(9.0, 9.0);
	}
	float3 p = dir / d - center; // onto the plane touching the sky at `center`
	return float2(dot(p, u), dot(p, v)) / size;
}

float3 rotate_z(float3 p, float a) {
	float c = cos(a), s = sin(a);
	return float3(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

float4 ps_main(PSIn i) : SV_Target {
	float4 far_point = mul(inv_view_proj, float4(i.ndc, 1.0, 1.0));
	float4 near_point = mul(inv_view_proj, float4(i.ndc, 0.0, 1.0));
	float3 dir = normalize(far_point.xyz / far_point.w - near_point.xyz / near_point.w);
	float h = dir.y;

	// Gradient: horizon colour near h = 0, top colour overhead, ground below.
	float3 col = lerp(horizon_color, top_color, smoothstep(0.0, 0.55, h));
	col = lerp(col, ground_color, smoothstep(0.0, -0.25, h));
	// A warm glow on the sun's side of the horizon at sunrise / sunset.
	float sun_side = max(dot(normalize(dir.xz + 1e-4), normalize(sun_dir.xz + 1e-4)), 0.0);
	col += float3(0.35, 0.12, 0.02) * dusk * pow(sun_side, 3.0) * (1.0 - smoothstep(0.0, 0.35, abs(h)));

	// Stars: fixed to the turning sky, only at night and above the horizon.
	float night = pow(1.0 - day, 2.0);
	if (night > 0.01 && h > 0.0) {
		float3 sd = rotate_z(dir, -sky_angle) * 140.0;
		float3 cell = floor(sd);
		float r = hash3(cell);
		if (r > 0.9965) {
			float dist = length(frac(sd) - 0.5);
			float size = 0.18 + 0.12 * hash3(cell + 7.0);
			float twinkle = 0.75 + 0.25 * sin(time * (1.5 + 3.0 * r) + r * 40.0);
			float star = step(dist, size) * twinkle * smoothstep(0.0, 0.25, h);
			col += float3(0.85, 0.9, 1.0) * star * night;
		}
	}

	// Sun: a bright square with a soft glow around it.
	float2 su = square_uv(dir, sun_dir, SUN_SIZE);
	float sun_vis = smoothstep(-0.12, 0.02, sun_dir.y);
	if (max(abs(su.x), abs(su.y)) < 1.0) {
		col = lerp(col, float3(1.0, 0.96, 0.78), sun_vis);
	}
	float glow = max(dot(dir, sun_dir), 0.0);
	col += float3(1.0, 0.8, 0.5) * pow(glow, 60.0) * 0.5 * sun_vis;

	// Moon: opposite the sun, an 8x8 pixel disc with darker craters.
	float3 moon_dir = -sun_dir;
	float2 mu = square_uv(dir, moon_dir, MOON_SIZE);
	if (max(abs(mu.x), abs(mu.y)) < 1.0 && moon_dir.y > -0.1) {
		float2 px = floor((mu * 0.5 + 0.5) * 8.0); // 0..7
		float2 c = px - 3.5;
		if (length(c) < 4.2) {
			float crater = step(0.72, hash2(px + 3.0));
			float3 moon = lerp(float3(0.86, 0.88, 0.92), float3(0.62, 0.64, 0.7), crater);
			col = lerp(col, moon, smoothstep(-0.1, 0.05, moon_dir.y));
		}
	}
	col += float3(0.6, 0.7, 0.9) * pow(max(dot(dir, moon_dir), 0.0), 120.0) * 0.25 * (1.0 - day);

	// Clouds: a flat layer above, blocky cells from smooth noise, drifting.
	if (h > 0.0) {
		float2 plane = dir.xz / h; // where the view ray meets a layer at height 1
		float2 cell = floor(plane * CLOUD_SCALE + cloud_offset);
		float n = vnoise(cell * 0.085) * 0.7 + vnoise(cell * 0.23 + 17.0) * 0.3;
		float cloud = step(0.6, n);
		float core = step(0.7, n);
		float fade = smoothstep(0.06, 0.32, h); // thin out towards the horizon
		float3 lit = lerp(float3(0.018, 0.02, 0.032), float3(1.0, 1.0, 1.0), day); // linear colour
		lit = lerp(lit, float3(1.0, 0.72, 0.55), dusk * 0.6);
		float3 cloud_col = lerp(lit * 0.86, lit, core);
		col = lerp(col, cloud_col, cloud * fade * 0.92);
	}
	return float4(godot_to_srgb(col), 1.0); // (as the Godot version writes it: godot_color.hlsli)
}
