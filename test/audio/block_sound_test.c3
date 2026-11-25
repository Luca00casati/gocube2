// Blocks' sounds: each plays its material's recordings (sound::block), and
// every sound file the content names is there and reads.
module gocube::test::block_sound_test;
import gocube::sound, gocube::sound::block, gocube::content, gocube::data, gocube::util::vec;
import std::math, std::io;

fn Content* load() @private
{
	Content* c = mem::new(Content);
	DString why;
	defer why.free();
	assert(c.load("content", &why), "%s", why.str_view());
	return c;
}

fn void unload(Content* c) @private
{
	c.free();
	free(c);
}

fn String material_of(Content* c, String id) @private
{
	BlockVoice v = block::voice_of(c.blocks.get(id));
	return block::material(&v);
}

fn String set_of(Content* c, String id) @private
{
	Data* def = c.blocks.get(id);
	BlockVoice v = block::voice_of(def);
	return block::set_of(def, &v);
}

// Each block's material, from what it is.
fn void each_block_sounds_like_its_material() @test => @pool()
{
	Content* c = load();
	defer unload(c);
	String[2][*] expected = {
		{ "core:stone", "stone" }, { "core:cobblestone", "stone" },
		{ "core:dirt", "dirt" }, { "core:grass", "grass" }, { "core:leaves", "grass" },
		{ "core:sand", "sand" }, { "core:gravel", "gravel" }, { "core:snow", "snow" }, { "core:glass", "glass" },
		{ "core:log", "wood" }, { "core:planks", "wood" }, { "core:planks_fence", "wood" },
	};
	foreach (e : expected) assert(material_of(c, e[0]) == e[1], "%s: %s, not %s", e[0], material_of(c, e[0]), e[1]);
	// What properties can't tell, "sound" does.
	assert(set_of(c, "core:sandstone") == "core:stone");
	assert(set_of(c, "core:ice") == "core:glass");
	assert(set_of(c, "core:stone") == "core:stone");
	assert(!block::has_sound(c.blocks.get("core:water")) && !block::has_sound(c.blocks.get("core:air")));
}

// Every block that makes a sound has recordings to play.
fn void every_block_has_recordings() @test => @pool()
{
	Content* c = load();
	defer unload(c);
	foreach (e : c.blocks.all())
	{
		Data* def = c.blocks.get(e.id);
		if (!block::has_sound(def)) continue;
		String set = set_of(c, e.id);
		Data* s = c.sound_sets.get(set);
		assert(s, "%s: no sound set %s", e.id, set);
		Data* hits = s.get("hits");
		assert(hits && hits.len() > 0, "%s: %s has no hits", e.id, set);
	}
}

// Every file a sound set names reads, and a block's recordings are short.
fn void every_sound_file_reads() @test
{
	Content* c = load();
	defer unload(c);
	int files;
	foreach (e : c.sound_sets.all())
	{
		foreach (kind : e.def.fields())
		{
			foreach (f : kind.value.items())
			{
				@pool()
				{
					String path = c.asset_path(f.as_string());
					Samples w;
					defer w.free();
					String error;
					assert(sound::read_sound(path, &w, &error), "%s: %s", path, error);
					double seconds = (double)w.frames() / (double)w.rate;
					assert(seconds > 0.02, "%s is empty", path);
					if (kind.key == "hits") assert(seconds < 1.0, "%s: %.2f s, long for a block", path, seconds);
					files++;
				};
			}
		}
	}
	assert(files > 15, "%d", files);
}

// The pitch (of a material with one recording): lighter, smaller and
// harder a little higher; never far off.
fn void pitch_follows_the_block() @test => @pool()
{
	Content* c = load();
	defer unload(c);
	BlockVoice stone = block::voice_of(c.blocks.get("core:stone"));
	BlockVoice fence = block::voice_of(c.blocks.get("core:planks_fence"));
	BlockVoice planks = block::voice_of(c.blocks.get("core:planks"));
	BlockVoice dirt = block::voice_of(c.blocks.get("core:dirt"));
	BlockVoice sand = block::voice_of(c.blocks.get("core:sand"));
	assert(block::pitch_of(&fence, "break") > block::pitch_of(&planks, "break"));
	assert(block::pitch_of(&sand, "break") > block::pitch_of(&dirt, "break"));
	assert(block::pitch_of(&stone, "place") > block::pitch_of(&stone, "break"));
	foreach (e : c.blocks.all())
	{
		BlockVoice v = block::voice_of(c.blocks.get(e.id));
		float p = block::pitch_of(&v, "break");
		assert(p >= 0.7f && p <= 1.5f, "%s: %f", e.id, p);
	}
}
