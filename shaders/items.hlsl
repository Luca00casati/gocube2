// Dropped items and the first-person hand: unlit like the world, with a shade
// per face of the model's own (unturned) normal and the light where they are.
//
// Vertices come already placed (world space for drops, camera space for the
// hand); `view_proj` takes them to the screen. Colour: the light, times the
// texture (the atlas' plain view: raw sRGB numbers), or the colour alone (the
// sleeve and hand: their tint times the light).

#include "color.hlsli"

cbuffer Items : register(b0) {
	float4x4 view_proj;
	float3 eye;          // where distances are measured from (fog)
	float fog_start;
	float3 fog_color;
	float fog_end;
	float use_texture;   // 0: the vertex colour alone (the sleeve and hand)
	float3 items_pad;
};

Texture2D albedo : register(t0);
SamplerState nearest_mip : register(s0);

struct VSIn {
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

struct PSIn {
	float4 pos : SV_Position;
	float3 place : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float2 uv : TEXCOORD2;
	float4 color : COLOR0;
};

PSIn vs_main(VSIn i) {
	PSIn o;
	o.pos = mul(view_proj, float4(i.pos, 1.0));
	o.place = i.pos;
	o.normal = i.normal;
	o.uv = i.uv;
	o.color = i.color;
	return o;
}

float4 ps_main(PSIn i) : SV_Target {
	float4 c = use_texture > 0.5 ? albedo.Sample(nearest_mip, i.uv) : float4(1, 1, 1, 1);
	clip(c.a - 0.5);
	float3 n = abs(i.normal);
	float shade = i.normal.y > 0.5 ? 1.0 : (i.normal.y < -0.5 ? 0.55 : (n.x > 0.5 ? 0.8 : 0.7));
	float3 color = to_linear(max(c.rgb * shade * i.color.rgb, 0.0));
	float d = length(i.place - eye);
	color = lerp(color, fog_color, pow(smoothstep(fog_start, fog_end, d), 1.6));
	return float4(to_srgb(color), 1.0);
}
