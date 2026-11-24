// The sounds of blocks (breaking, placing, stepping on): recordings of a
// material (content sound sets "core:stone", "core:wood", "core:grass",
// "core:gravel", "core:sand", "core:snow", "core:dirt", "core:glass", each
// a list of "hits"), chosen from what the block is, and pitched from it.
//
// The material: see-through is glass; leaves, plants and grass blocks are
// grass; the softest ("hardness" 0.15 or less) is snow; a block that falls
// is gravel (a texture of "cells") or sand; woody ("flammable", "rings" or
// "stripes", a door, ladder or fence) is wood; hard is stone; the rest is
// dirt. A block's "sound" names a sound set instead (sandstone is stone).
//
// The pitch: lighter colours a little higher, smaller pieces (stairs, a
// layer of snow) higher, harder a touch higher; placing a little higher.
module gocube::sound::block;
import gocube::data, gocube::content::image;
import std::math;

const String SET_PREFIX = "block:"; // a sound set "block:<id>": the block's own

// What about a block shapes its sound.
struct BlockVoice
{
	float hard;    // 0 soft (snow, leaves) .. 1 hard (cobblestone)
	float grain;   // 0 fine (sand) .. 1 coarse bits (gravel)
	float wood;    // 0..1: woody
	float light;   // its colour's lightness, 0..1
	float small;   // 0 a full block .. 1 a small piece
	float glass;   // 0..1: see-through (1), or giving light (0.4)
	float rigid;   // 0 loose .. 1 rigid
	bool leaves;
	bool plant;    // grows (grass blocks, plants)
	bool falls;
	float hardness; // as the block says
}

fn float clamp01(float v) @private => math::clamp(v, 0.0f, 1.0f);

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
	BlockVoice v = { .light = 0.5f, .hardness = 1 };
	if (!def) return v;
	float noise = 0;
	bool cells, lines;
	if (Data* textures = def.get("textures"))
	{
		foreach (face : textures.fields()) texture_traits(&face.value, &noise, &cells, &lines);
	}
	v.hardness = def.float_of("hardness", 1.0f);
	v.hard = clamp01((v.hardness - 0.2f) / 1.8f);
	v.grain = cells ? 0.85f : clamp01(noise * 2.0f);
	v.leaves = def.flag("leaves");
	v.plant = def.has("sprouts_trees") || def.has("plant_on");
	v.falls = def.flag("falls");
	String shape = def.str("shape");
	switch (shape)
	{
		case "": v.small = 0;
		case "stairs": v.small = 0.25f;
		case "door":
		case "bed": v.small = 0.35f;
		case "fence": v.small = 0.5f;
		case "ladder": v.small = 0.6f;
		case "cross":
			v.small = 0.8f;
			v.plant = true;
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
	v.wood = math::max(def.flag("flammable") ? 1.0f : 0.0f, lines ? 0.6f : 0.0f, thin ? 0.5f : 0.0f);
	if (v.leaves) v.wood = 0;
	char[4] c;
	if (image::parse_html_color(def.str("color", "#808080"), &c))
	{
		v.light = (0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]) / 255.0f;
	}
	if (def.flag("transparent") && !v.leaves) v.glass = 1;
	if (def.int_of("light", 0) > 0) v.glass = math::max(v.glass, 0.4f);
	v.rigid = math::max(clamp01((v.hardness - 0.7f) / 0.6f), v.wood * clamp01(v.hardness / 0.8f), v.glass);
	if (v.falls || v.leaves) v.rigid = 0;
	return v;
}

// The material whose recordings a block plays ("stone", "grass", ...).
fn String material(BlockVoice* v)
{
	if (v.glass >= 0.5f) return "glass";
	if (v.leaves || v.plant) return "grass";
	if (v.hardness <= 0.15f) return "snow";
	if (v.falls) return v.grain > 0.5f ? "gravel" : "sand";
	if (v.wood >= 0.5f) return "wood";
	if (v.rigid >= 0.5f) return "stone";
	return "dirt";
}

// The sound set a block plays: its "sound", else its material's (temp).
fn String set_of(Data* def, BlockVoice* v)
{
	String named = def ? def.str("sound") : "";
	return named.len ? named : string::tformat("core:%s", material(v));
}

// How much higher (> 1) or lower than the recording a block's sound plays.
fn float pitch_of(BlockVoice* v, String kind)
{
	float p = (0.9f + 0.2f * v.light) * (1.0f + 0.25f * v.small) * (0.96f + 0.08f * v.hard);
	if (kind == "place") p *= 1.05f;
	return math::clamp(p, 0.7f, 1.5f);
}
