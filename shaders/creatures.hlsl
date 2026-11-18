// Creatures and players' figures: unlit like the terrain, with a shade per
// face of the box's own normal. A part is a flat colour, a block face tiled
// once a metre from the box's own coordinates (picked by the face's axis), so
// it looks made of that block, or a player's skin (a slice of `skins`, at the
// vertex's own coordinates). A red flash when hurt (game/creature_models.h
// builds the boxes).

#include "color.hlsli"

cbuffer Creatures : register(b0) { // (laid out as items.hlsl's)
	float4x4 view_proj;
	float3 eye;
	float fog_start;
	float3 fog_color;
	float fog_end;
	float4 unused;
};

Texture2D albedo : register(t0);
Texture2DArray skins : register(t1);
SamplerState nearest_mip : register(s0);

struct VSIn {
	float3 pos : POSITION;
	float3 local : TEXCOORD0;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float4 tile : TEXCOORD1;
	float3 skin : TEXCOORD2;
};

struct PSIn {
	float4 pos : SV_Position;
	float3 place : TEXCOORD0;
	float3 local : TEXCOORD1;
	float3 normal : TEXCOORD2;
	float4 color : COLOR0;
	float4 tile : TEXCOORD3;
	float2 skin_uv : TEXCOORD4;
	nointerpolation float skin_slot : TEXCOORD5;
};

PSIn vs_main(VSIn i) {
	PSIn o;
	o.pos = mul(view_proj, float4(i.pos, 1.0));
	o.place = i.pos;
	o.local = i.local;
	o.normal = i.normal;
	o.color = i.color;
	o.tile = i.tile;
	o.skin_uv = i.skin.xy;
	o.skin_slot = i.skin.z;
	return o;
}

float4 ps_main(PSIn i) : SV_Target {
	float3 n = abs(i.normal);
	float2 uv = n.y > 0.5 ? i.local.xz : (n.x > 0.5 ? i.local.zy : i.local.xy);
	float4 c = float4(1, 1, 1, 1);
	if (i.skin_slot >= 0) {
		c = skins.SampleLevel(nearest_mip, float3(i.skin_uv, i.skin_slot), 0);
	} else if (i.tile.z > i.tile.x) {
		float2 g = float2(uv.x, -uv.y);
		float2 size = i.tile.zw - i.tile.xy;
		// (Gradients from the unwrapped coordinate: no seams where it wraps.)
		c = albedo.SampleGrad(nearest_mip, i.tile.xy + frac(g + 0.5) * size, ddx(g) * size, ddy(g) * size);
	}
	clip(c.a - 0.5);
	float shade = i.normal.y > 0.5 ? 1.0 : (i.normal.y < -0.5 ? 0.55 : (n.x > 0.5 ? 0.8 : 0.7));
	float3 color = lerp(c.rgb * shade * i.color.rgb, float3(0.85, 0.08, 0.06), i.color.a * 0.6);
	color = to_linear(color);
	float d = length(i.place - eye);
	color = lerp(color, fog_color, pow(smoothstep(fog_start, fog_end, d), 1.6));
	return float4(to_srgb(color), 1.0);
}
