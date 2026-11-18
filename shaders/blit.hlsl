// Render scale: the world drawn at a fraction of the window's size, stretched
// over it with bilinear filtering.

Texture2D source : register(t0);
SamplerState bilinear : register(s0);

struct PSIn {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

// One triangle covering the screen.
PSIn vs_main(uint id : SV_VertexID) {
	PSIn o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}

float4 ps_main(PSIn i) : SV_Target {
	return float4(source.Sample(bilinear, i.uv).rgb, 1.0);
}
