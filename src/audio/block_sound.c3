// The sounds of blocks (breaking, placing, stepping on), made from what the
// block is rather than from files: how hard it is ("hardness"), how grainy
// its texture ("cells", "noise"), whether it's woody ("flammable", "rings",
// "stripes", a thin shape), how light its colour, its shape (a small piece
// sounds higher), see-through ("transparent": glass rings), "leaves" (a
// rustle), "falls" (a trickle after) and "light" (a faint shimmer).
//
// All soft and dull: a muffled knock or crumble, no bright click. The same
// block, kind and variant always make the same sound. A block can still
// name sound files instead, with "sound" (a sound set).
module gocube::sound::block;
import gocube::sound, gocube::data, gocube::content::image, gocube::util::vec;
import std::math;

const uint RATE = 22050;
const String SET_PREFIX = "block:"; // a sound set "block:<id>": made here

// How many different sounds of each kind a block has.
fn int variants(String kind) => kind == "place" ? 2 : 3;

// What about a block shapes its sound.
struct BlockVoice
{
	float hard;    // 0 soft (snow, leaves) .. 1 hard (cobblestone)
	float grain;   // 0 one smooth knock .. 1 many small bits (gravel)
	float wood;    // 0..1: a hollow, boxy ring
	float pitch;   // 1: a mid grey block; lighter colours a little higher
	float small;   // 0 a full block .. 1 a small piece
	float glass;   // 0..1: a thin high ring
	bool leaves;   // a soft rustle
	bool falls;    // a trickle after
	bool dry;      // a little crackle
}

fn float clamp01(float v) @private => math::clamp(v, 0.0f, 1.0f);
fn float mix(float a, float b, float t) @private => a + (b - a) * t;

// False for blocks that make no sound (air, water, lava).
fn bool has_sound(Data* def) => def != null && !def.has("fluid") && def.flag("solid", true);

// A face's texture fields (the textures that are pictures have none).
fn void texture_traits(Data* t, float* noise, bool* cells, bool* lines) @private
{
	if (!t || !t.is_object()) return;
	*noise = math::max(*noise, t.float_of("noise", 0));
	if (t.has("cells")) *cells = true;
	if (t.has("rings") || t.has("stripes")) *lines = true;
}

fn BlockVoice voice_of(Data* def)
{
	BlockVoice v = { .pitch = 1 };
	if (!def) return v;
	float noise = 0;
	bool cells, lines;
	if (Data* textures = def.get("textures"))
	{
		foreach (face : textures.fields()) texture_traits(&face.value, &noise, &cells, &lines);
	}
	v.hard = clamp01((def.float_of("hardness", 1.0f) - 0.2f) / 1.8f);
	v.grain = cells ? 0.85f : clamp01(noise * 2.0f);
	v.leaves = def.flag("leaves");
	v.falls = def.flag("falls");
	v.dry = def.flag("flammable");
	String shape = def.str("shape");
	switch (shape)
	{
		case "": v.small = 0;
		case "stairs": v.small = 0.25f;
		case "door":
		case "bed": v.small = 0.35f;
		case "fence": v.small = 0.5f;
		case "ladder": v.small = 0.6f;
		case "cross": v.small = 0.8f;
		case "boxes":
			// How much of the block the boxes fill (16ths).
			float filled = 0;
			if (Data* boxes = def.get("boxes"))
			{
				foreach (b : boxes.items())
				{
					if (b.len() < 6) continue;
					filled += (b.at(3).as_float() - b.at(0).as_float()) * (b.at(4).as_float() - b.at(1).as_float())
						* (b.at(5).as_float() - b.at(2).as_float()) / 4096.0f;
				}
			}
			v.small = math::clamp(1.0f - filled, 0.0f, 0.8f);
		default: v.small = 0.3f;
	}
	bool thin = shape == "door" || shape == "ladder" || shape == "fence";
	v.wood = math::max(v.dry ? 1.0f : 0.0f, lines ? 0.6f : 0.0f, thin ? 0.5f : 0.0f);
	if (v.leaves) v.wood = 0;
	char[4] c;
	if (image::parse_html_color(def.str("color", "#808080"), &c))
	{
		float light = (0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]) / 255.0f;
		v.pitch = 0.82f + 0.4f * light;
	}
	if (def.flag("transparent") && !v.leaves) v.glass = 1;
	if (def.int_of("light", 0) > 0) v.glass = math::max(v.glass, 0.4f);
	return v;
}

// --- Making a sound --------------------------------------------------------------------

struct Noise @private { uint state; }

fn float Noise.next(&self) @private // -1..1
{
	self.state ^= self.state << 13;
	self.state ^= self.state >> 17;
	self.state ^= self.state << 5;
	return (float)((double)self.state / 4294967295.0 * 2.0 - 1.0);
}

fn float Noise.unit(&self) @private => self.next() * 0.5f + 0.5f;
fn float Noise.range(&self, float a, float b) @private => a + (b - a) * self.unit();

fn uint hash(String s, uint h = 2166136261) @private
{
	foreach (ch : s) h = (h ^ ch) * 16777619;
	return h;
}

// A one-pole low-pass at `hz`, in place.
fn void lowpass(float[] x, float hz, int passes = 1) @private
{
	float a = 1.0f - math::exp(-2.0f * (float)math::PI * hz / (float)RATE);
	for (int p = 0; p < passes; p++)
	{
		float y = 0;
		foreach (&s : x)
		{
			y += a * (*s - y);
			*s = y;
		}
	}
}

// A two-pole band-pass at `hz` (q: how long it rings), `src` into `out` (added, times `gain`).
fn void ring(float[] src, float[] out, float hz, float q, float gain) @private
{
	float w = 2.0f * (float)math::PI * hz / (float)RATE;
	if (w >= (float)math::PI) return;
	float r = math::exp(-w / (2.0f * q));
	float b1 = 2.0f * r * math::cos(w);
	float b2 = -r * r;
	float y1, y2;
	foreach (i, v : src)
	{
		float y = (1.0f - r) * v + b1 * y1 + b2 * y2;
		y2 = y1;
		y1 = y;
		out[i] += y * gain;
	}
}

// One knock (or crumble) of `v`, starting at `start`, added to `out`.
fn void knock(BlockVoice* v, Noise* rng, float[] out, sz start, float size, float pitch) @private
{
	sz n = out.len - start;
	float[] burst = mem::new_array(float, n);
	defer free(burst);
	float[] body = mem::new_array(float, n);
	defer free(body);
	// The noise: in small bits when grainy; soft blocks crumble longer.
	float half = mix(0.06f, 0.025f, v.hard) * rng.range(0.85f, 1.15f);
	if (v.leaves) half = 0.08f;
	float fall = (float)math::LN2 / (half * (float)RATE);
	float gate = 1;
	float grain = v.leaves ? 0.7f : v.grain;
	for (sz i = 0; i < n; i++)
	{
		if (rng.unit() < 0.004f * grain) gate = 1;
		gate *= 0.995f;
		float env = math::min(1.0f, (float)i / (0.004f * (float)RATE)) * math::exp(-fall * (float)i);
		burst[i] = rng.next() * env * ((1.0f - grain) + grain * gate);
	}
	// The body ringing: harder rings higher and longer; woody rings hollow.
	float f1 = (110.0f + 170.0f * v.hard) * pitch * (1.0f + 0.6f * v.small);
	float q1 = 1.5f + 5.0f * v.hard + 6.0f * v.wood;
	float weight = v.leaves ? 0.15f : 0.35f + 0.65f * math::max(v.hard, v.wood);
	ring(burst, body, f1, q1, 6.0f * weight);
	ring(burst, body, f1 * mix(2.3f, 2.6f, v.wood), q1 * 0.8f, 6.0f * weight * (0.15f + 0.3f * math::max(v.hard, v.wood)));
	if (v.glass > 0)
	{
		foreach (k, f : (float[3]){ 1700.0f, 2450.0f, 3300.0f })
		{
			ring(burst, body, f * pitch * rng.range(0.97f, 1.03f), 40.0f, 14.0f * v.glass / (float)(k + 1));
		}
	}
	// The dull part of the noise.
	float dull = mix(650.0f, 1300.0f, v.hard) * pitch;
	if (v.leaves) dull = 1600.0f;
	float noisy = v.leaves ? 1.0f : mix(1.0f, 0.45f, math::max(v.hard, v.wood));
	lowpass(burst, dull, 2);
	for (sz i = 0; i < n; i++) out[start + i] += size * (body[i] + noisy * burst[i]);
}

// A sound of `kind` ("break", "place", "step"), variant `variant`, for the
// block `id`: 16-bit mono at RATE (the caller frees it).
fn Wav make(BlockVoice* v, String id, String kind, int variant)
{
	Noise rng = { hash(kind, hash(id)) ^ (uint)(variant + 1) * 2654435761 };
	if (rng.state == 0) rng.state = 1;
	for (int i = 0; i < 4; i++) rng.next();
	float seconds = 0.28f;
	int bits = 3;
	float peak = 0.5f;
	switch (kind)
	{
		case "place":
			seconds = 0.16f;
			bits = 1;
			peak = 0.45f;
		case "step":
			seconds = 0.13f;
			bits = v.grain > 0.5f || v.leaves ? 2 : 1;
			peak = 0.35f;
	}
	if (v.glass > 0.5f && kind == "break") seconds = 0.4f;
	if (v.falls && kind != "step") seconds += 0.15f;
	sz n = (sz)(seconds * (float)RATE);
	float[] out = mem::new_array(float, n);
	defer free(out);
	float pitch = v.pitch * rng.range(0.93f, 1.07f) * (kind == "place" ? 1.08f : 1.0f);
	for (int b = 0; b < bits; b++)
	{
		sz start = b == 0 ? 0 : (sz)(rng.range(0.02f, (kind == "step" ? 0.05f : 0.14f)) * (float)RATE);
		knock(v, &rng, out, start, b == 0 ? 1.0f : rng.range(0.25f, 0.55f), pitch * rng.range(0.95f, 1.05f));
	}
	// Falling blocks trickle a little after.
	if (v.falls && kind != "step")
	{
		sz from = (sz)(0.1f * (float)RATE);
		for (int k = 0; k < 7; k++)
		{
			sz at = from + (sz)(rng.unit() * (float)(n - from) * 0.7f);
			BlockVoice bit = *v;
			bit.grain = 0.2f;
			knock(&bit, &rng, out, at, rng.range(0.08f, 0.18f) * (1.0f - (float)(at - from) / (float)(n - from)), pitch * 1.3f);
		}
	}
	// A little dry crackle.
	if (v.dry && kind == "break")
	{
		float[] crackle = mem::new_array(float, n);
		defer free(crackle);
		for (int k = 0; k < 4; k++)
		{
			sz at = (sz)(rng.unit() * (float)n * 0.6f);
			for (sz i = at; i < math::min(at + 30, n); i++) crackle[i] += rng.next() * (1.0f - (float)(i - at) / 30.0f);
		}
		lowpass(crackle, 1400.0f, 2);
		foreach (i, c : crackle) out[i] += c * 0.5f;
	}
	// Everything soft: no high end (glass keeps a little more), no offset, ends quietly.
	lowpass(out, v.glass > 0.5f ? 3800.0f : 2500.0f);
	float lo = 0;
	float a = 1.0f - math::exp(-2.0f * (float)math::PI * 40.0f / (float)RATE);
	foreach (&s : out)
	{
		lo += a * (*s - lo);
		*s -= lo;
	}
	sz fade = math::min(n, (sz)(0.03f * (float)RATE));
	for (sz i = 0; i < fade; i++) out[n - fade + i] *= 1.0f - (float)i / (float)fade;
	sz rise = math::min(n, (sz)16);
	for (sz i = 0; i < rise; i++) out[i] *= (float)i / (float)rise;
	float top = 1e-9f;
	foreach (s : out) top = math::max(top, math::abs(s));
	Wav w = { .format = sound::PCM, .channels = 1, .rate = RATE, .bits = 16 };
	foreach (s : out)
	{
		short q = (short)math::clamp(s / top * peak * 32767.0f, -32767.0f, 32767.0f);
		w.data.push((char)((ushort)q & 0xFF));
		w.data.push((char)((ushort)q >> 8));
	}
	return w;
}
