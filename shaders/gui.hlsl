// The UI (src/ui/gui_d3d.cpp): every draw command is a quad in screen
// pixels. Rectangles, rounded or not, are worked out per pixel from their
// distance to the edge, so their edges are smooth; text comes from the glyph
// atlas (coverage, rasterized by GDI); item icons from the block atlas,
// dyed here as content.cpp's dye_pixel dyes blocks. Colours are what the
// screen shows (sRGB numbers, as the UI chose them): no conversion.

Texture2D<float> glyphs : register(t0);
Texture2D icons : register(t1);
SamplerState nearest : register(s0);

cbuffer Screen : register(b0) {
	float2 to_clip; // 2 / the screen's size in pixels
};

// Modes (params.z).
#define FILL 0
#define STROKE 1
#define GLYPH 2
#define ICON 3

static const float DYE_SHOW = 0.2; // as content.h's

struct VSIn {
	float2 pos : POSITION;    // pixels
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;    // straight alpha
	float4 dye : COLOR1;      // ICON: the dye (a = 0: none)
	float4 rect : TEXCOORD1;  // FILL, STROKE: the rectangle (x0, y0, x1, y1)
	float4 params : TEXCOORD2; // rounding, thickness, mode, ICON: the tile's average brightness
};

struct PSIn {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	nointerpolation float4 dye : COLOR1;
	nointerpolation float4 rect : TEXCOORD1;
	nointerpolation float4 params : TEXCOORD2;
};

PSIn vs_main(VSIn i) {
	PSIn o;
	o.pos = float4(i.pos.x * to_clip.x - 1, 1 - i.pos.y * to_clip.y, 0, 1);
	o.uv = i.uv;
	o.color = i.color;
	o.dye = i.dye;
	o.rect = i.rect;
	o.params = i.params;
	return o;
}

// Signed distance from p to a rounded rectangle (negative inside).
float rounded_box(float2 p, float4 r, float radius) {
	const float2 c = (r.xy + r.zw) * 0.5, h = max((r.zw - r.xy) * 0.5, 0);
	radius = min(radius, min(h.x, h.y));
	const float2 q = abs(p - c) - h + radius;
	return length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
}

float4 ps_main(PSIn i) : SV_Target {
	const int mode = int(i.params.z + 0.5);
	float4 c = i.color;
	if (mode == FILL) {
		c.a *= saturate(0.5 - rounded_box(i.pos.xy, i.rect, i.params.x));
	} else if (mode == STROKE) {
		// Centred on the rectangle inset by half the thickness.
		const float h = i.params.y * 0.5;
		const float d = rounded_box(i.pos.xy, i.rect + float4(h, h, -h, -h), i.params.x);
		c.a *= saturate(h + 0.5 - abs(d));
	} else if (mode == GLYPH) {
		c.a *= glyphs.Sample(nearest, i.uv);
	} else {
		const float4 t = icons.Sample(nearest, i.uv);
		float3 rgb = t.rgb;
		if (i.dye.a > 0) {
			const float k = clamp(dot(rgb, float3(0.299, 0.587, 0.114)) / max(i.params.w, 0.04), 0, 1.6);
			rgb = i.dye.rgb * k * (1 - DYE_SHOW) + rgb * DYE_SHOW;
		}
		c = float4(rgb, t.a * c.a);
	}
	return float4(c.rgb * c.a, c.a); // (premultiplied)
}
