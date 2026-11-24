// Blocks' sounds, made from what they are (sound::block).
module gocube::test::block_sound_test;
import gocube::sound, gocube::sound::block, gocube::content, gocube::data, gocube::util::vec;
import std::math, std::io;

fn float[] samples_of(Wav* w) // (temp)
{
	float[] out = mem::temp_array(float, w.data.len() / 2);
	foreach (i, &s : out) *s = (float)(short)((ushort)w.data[i * 2] | (ushort)w.data[i * 2 + 1] << 8) / 32768.0f;
	return out;
}

// How much of a sound is above ~1.5 kHz, 0..1 (roughly: two one-pole
// passes, so a low ring counts a little too; the old bright sound files
// were about 0.9).
fn float bright_share(float[] x)
{
	float a = 1.0f - math::exp(-2.0f * (float)math::PI * 1500.0f / (float)block::RATE);
	float lo1, lo2, all, high;
	foreach (v : x)
	{
		lo1 += a * (v - lo1);
		lo2 += a * (lo1 - lo2);
		all += v * v;
		high += (v - lo2) * (v - lo2);
	}
	return high / math::max(all, 1e-9f);
}

fn Data* def_of(Content* c, String id) => c.blocks.get(id);

// Every block that can be broken has its sounds: the same each time, soft
// (little high end, glass aside), starting and ending quietly, not clipped.
fn void every_block_sounds_soft_and_the_same() @test => @pool()
{
	Content content;
	defer content.free();
	DString why;
	defer why.free();
	assert(content.load("content", &why), "%s", why.str_view());
	int made;
	foreach (e : content.blocks.all())
	{
		Data* def = def_of(&content, e.id);
		if (!block::has_sound(def)) continue;
		BlockVoice v = block::voice_of(def);
		foreach (kind : (String[3]){ "break", "place", "step" })
		{
			for (int n = 0; n < block::variants(kind); n++)
			{
				Wav w = block::make(&v, e.id, kind, n);
				defer w.free();
				Wav again = block::make(&v, e.id, kind, n);
				defer again.free();
				assert(w.data.array_view() == again.data.array_view(), "%s %s changes", e.id, kind);
				assert(w.rate == block::RATE && w.channels == 1 && w.bits == 16);
				float[] x = samples_of(&w);
				assert(x.len > block::RATE / 20, "%s %s is too short", e.id, kind);
				float peak = 0;
				foreach (s : x) peak = math::max(peak, math::abs(s));
				assert(peak > 0.3f && peak < 0.51f, "%s %s peak %f", e.id, kind, peak);
				assert(math::abs(x[0]) < 0.01f && math::abs(x[^1]) < 0.01f, "%s %s clicks", e.id, kind);
				if (v.glass < 0.5f) assert(bright_share(x) < 0.25f, "%s %s is bright: %f", e.id, kind, bright_share(x));
				made++;
			}
		}
	}
	assert(made > 100, "%d", made);
	// Variants differ.
	BlockVoice stone = block::voice_of(def_of(&content, "core:stone"));
	Wav a = block::make(&stone, "core:stone", "break", 0);
	defer a.free();
	Wav b = block::make(&stone, "core:stone", "break", 1);
	defer b.free();
	assert(a.data.array_view() != b.data.array_view());
}

// What a block is shapes its sound.
fn void sound_follows_the_block() @test
{
	Content content;
	defer content.free();
	DString why;
	defer why.free();
	assert(content.load("content", &why), "%s", why.str_view());
	BlockVoice stone = block::voice_of(def_of(&content, "core:cobblestone"));
	BlockVoice dirt = block::voice_of(def_of(&content, "core:dirt"));
	BlockVoice gravel = block::voice_of(def_of(&content, "core:gravel"));
	BlockVoice planks = block::voice_of(def_of(&content, "core:planks"));
	BlockVoice glass = block::voice_of(def_of(&content, "core:glass"));
	BlockVoice leaves = block::voice_of(def_of(&content, "core:leaves"));
	BlockVoice snow = block::voice_of(def_of(&content, "core:snow"));
	BlockVoice stairs = block::voice_of(def_of(&content, "core:stone_stairs"));
	assert(stone.hard > 0.9f && dirt.hard < 0.25f);           // hardness
	assert(gravel.grain > 0.8f && gravel.falls);               // cells; falls
	assert(planks.wood == 1.0f && stone.wood == 0.0f && planks.dry); // flammable
	assert(glass.glass == 1.0f && stone.glass == 0.0f);        // transparent
	assert(leaves.leaves && leaves.glass == 0.0f && leaves.wood == 0.0f);
	assert(snow.small > 0.75f && stairs.small > 0.2f && stone.small == 0.0f); // shape
	assert(snow.pitch > stone.pitch);                          // a light colour
	assert(!block::has_sound(def_of(&content, "core:water")) && !block::has_sound(def_of(&content, "core:air")));
	assert(block::variants("break") == 3 && block::variants("place") == 2);
}
