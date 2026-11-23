// The last pass over the world's picture (when it's drawn to its own
// target): stretched over the window with bilinear filtering (render scale
// below 1) and, if `retro` is on, the retro screen filter: vivid colours
// (more saturated, a touch of contrast), a soft vignette darkening the
// edges, and fine film grain that changes every frame. (The UI is drawn
// after, untouched.)

cbuffer Blit : register(b0) {
	float retro;     // 1: the filter
	float time;      // seconds (the grain's frame)
	float2 size;     // the window, in pixels
};

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

static const float VIVID = 0.35;      // extra saturation (0: as drawn)
static const float CONTRAST = 0.08;   // a gentle S-curve
static const float3 WARM = float3(1.02, 1.0, 0.97);
static const float VIGNETTE = 0.42;   // how dark the corners get
static const float GRAIN = 0.045;     // the grain's strength

float hash(float2 p) {
	p = frac(p * float2(443.897, 441.423));
	p += dot(p, p.yx + 19.19);
	return frac((p.x + p.y) * p.x);
}

float4 ps_main(PSIn i) : SV_Target {
	float3 c = source.Sample(bilinear, i.uv).rgb;
	if (retro > 0.5) {
		float grey = dot(c, float3(0.299, 0.587, 0.114));
		c = saturate(lerp(grey.xxx, c, 1.0 + VIVID)) * WARM;
		c = lerp(c, c * c * (3.0 - 2.0 * c), CONTRAST);
		float2 d = (i.uv - 0.5) * float2(size.x / size.y, 1.0);
		c *= 1.0 - VIGNETTE * smoothstep(0.35, 1.05, length(d));
		float n = hash(floor(i.pos.xy) + frac(time * 7.13) * 911.0) - 0.5;
		c += n * GRAIN * (0.6 + 0.4 * (1.0 - grey)); // (more in the dark, as film)
	}
	return float4(saturate(c), 1.0);
}
