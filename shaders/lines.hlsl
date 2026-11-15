// Lines in the world (the outline around the block aimed at): one colour.

cbuffer Frame : register(b0) { // the start of chunk.hlsl's Frame
	float4x4 view_proj;
};

cbuffer Lines : register(b1) {
	float4 line_color;
};

float4 vs_main(float3 pos : POSITION) : SV_Position {
	return mul(view_proj, float4(pos, 1.0));
}

float4 ps_main() : SV_Target {
	return line_color;
}
