// The sound maths: .flac files, each ear's loudness, the underwater filter.
module gocube::test::mixing_test;
import gocube::sound, gocube::audio, gocube::content, gocube::clock, gocube::util::vec;
import std::math, std::io, std::hash::md5;

// The samples a .flac file was made from, rebuilt from what the game reads
// (8, 16 or 24 bits as in the file): their MD5 is the one its STREAMINFO keeps.
fn void decodes_exactly(String path, ushort channels, uint rate, sz frames)
{
	char[] bytes = file::load_temp(path)!!;
	Samples s;
	defer s.free();
	String error;
	assert(sound::parse_flac(bytes, &s, &error), "%s: %s", path, error);
	assert(s.channels == channels && s.rate == rate && s.frames() == frames, "%s: %d channels, %d Hz, %d frames", path, s.channels, s.rate, s.frames());
	int bps = ((bytes[20] & 1) << 4 | bytes[21] >> 4) + 1;
	Vec{char} raw;
	defer raw.free();
	if (bps > 16)
	{
		assert(s.format == sound::FLOAT && s.bits == 32);
		float* f = (float*)s.data.items;
		for (sz i = 0; i < s.frames() * channels; i++)
		{
			int v = (int)math::round(f[i] * (float)(1 << (bps - 1)));
			for (int k = 0; k < bps / 8; k++) raw.push((char)(v >> (8 * k)));
		}
	}
	else
	{
		assert(s.format == sound::PCM && s.bits == 16);
		short* p = (short*)s.data.items;
		for (sz i = 0; i < s.frames() * channels; i++)
		{
			int v = p[i] >> (16 - bps);
			for (int k = 0; k < (bps + 7) / 8; k++) raw.push((char)(v >> (8 * k)));
		}
	}
	char[16] sum = md5::hash(raw.array_view());
	assert((String)sum[..] == (String)bytes[26:16], "%s: the samples aren't the ones encoded", path);
}

// Files from another encoder (libsndfile's: LPC, mid/side, 8 and 24 bits)
// and the game's own (tools/to_flac.py).
fn void flac_files() @test => @pool()
{
	decodes_exactly("test/audio/flac/mono16.flac", 1, 22050, 5000);
	decodes_exactly("test/audio/flac/stereo16.flac", 2, 48000, 9001);
	decodes_exactly("test/audio/flac/stereo24.flac", 2, 44100, 4500);
	decodes_exactly("test/audio/flac/mono8.flac", 1, 8000, 3000);
	decodes_exactly("content/sounds/wind.flac", 2, 48000, 25 * 48000);
	decodes_exactly("content/sounds/blocks/stone.flac", 1, 48000, 5664);
}

// What isn't a .flac file that plays, or is damaged, is turned down.
fn void broken_flac_files() @test => @pool()
{
	Samples s;
	String error;
	assert(!sound::parse_flac("not a flac at all", &s, &error));
	assert(!sound::read_sound("content/sounds/none.flac", &s, &error));
	char[] good = file::load_temp("test/audio/flac/stereo16.flac")!!;
	char[] copy = mem::talloc_array(char, good.len);
	copy[..] = good[..];
	copy[good.len / 2] ^= 0x10; // a bit flipped in the middle
	assert(!sound::parse_flac(copy, &s, &error));
	assert(error.contains("damaged") || error.contains("broken"), "%s", error);
	assert(!sound::parse_flac(good[:good.len / 2], &s, &error)); // cut short
	assert(sound::parse_flac(good, &s, &error), "%s", error);
	s.free();
}

fn void check_gains(float[3] listener, float[3] forward, float[3] at, float left, float right)
{
	float[2] g = sound::stereo_gains(listener, forward, at, 6.0f, 32.0f);
	assert(math::abs(g[0] - left) <= 0.001f * math::max(1.0f, math::abs(left)), "left %f, not %f", g[0], left);
	assert(math::abs(g[1] - right) <= 0.001f * math::max(1.0f, math::abs(right)), "right %f, not %f", g[1], right);
}

fn void each_ear_as_miniaudio() @test
{
	float[3] origin = { 0, 0, 0 };
	float[3] north = { 0, 0, -1 };
	float[3] east = { 1, 0, 0 };
	// Falloff: 1 within 6 blocks, then 6 / distance, never below 6 / 32.
	// Each ear: (1 + cos) / 2 of the angle to it, at least 0.2.
	check_gains(origin, north, { 10, 0, 0 }, 0.12f, 0.6f);
	check_gains(origin, north, { -10, 0, 0 }, 0.6f, 0.12f);
	check_gains(origin, north, { 0, 0, -10 }, 0.3f, 0.3f);
	check_gains(origin, north, { 0, 0, 10 }, 0.3f, 0.3f); // (no quieter behind)
	check_gains(origin, east, { 0, 0, 10 }, 0.12f, 0.6f); // facing east, south is on the right
	check_gains(origin, north, { 3, 0, 0 }, 0.2f, 1.0f);
	check_gains(origin, north, { 50, 0, 0 }, 0.0375f, 0.1875f);
	check_gains(origin, north, { 0, 0.0005f, 0 }, 1.0f, 1.0f);
	// Looking straight down: right is +x. Height counts in the distance.
	check_gains(origin, { 0, -1, 0 }, { 8, -6, 0 }, 0.6f * 0.2f, 0.6f * 0.9f);
}

fn void underwater_lowpass() @test
{
	float a = sound::lowpass_coefficient(700, 48000);
	assert(math::abs(a - math::exp(-2 * 3.14159265358979 * 700 / 48000)) < 1e-6);
	// A step on the left channel, nothing on the right: the left rises 1 - a^n.
	float[400] samples;
	for (int f = 0; f < 200; f++) samples[f * 2] = 1;
	float[2] state;
	sound::lowpass(&samples, 200, 2, a, &state);
	assert(math::abs(samples[0] - (1 - a)) < 1e-5);
	assert(math::abs(samples[2] - (1 - a * a)) < 1e-5);
	assert(samples[1] == 0.0f);
	assert(math::abs(state[0] - (1 - math::pow(a, 200.0f))) < 1e-4);
	// The state carries on to the next block.
	float[2] more = { 1, 0 };
	sound::lowpass(&more, 1, 2, a, &state);
	assert(math::abs(more[0] - (1 - math::pow(a, 201.0f))) < 1e-4);
}

// XAudio2 on this machine's sound device (silent without one): a sound
// plays, finishes, and everything shuts down.
fn void plays_through_xaudio2() @test
{
	Content content;
	defer content.free();
	DString why;
	defer why.free();
	assert(content.load("content", &why), "%s", why.str_view());
	content.freeze();
	Audio a;
	defer a.free();
	if (!a.init(&content, 0.0f)) return; // (no sound device here: nothing to check)
	a.set_listener({ 0, 0, 0 }, { 0, 0, -1 });
	a.play("core:effects", "splash", { 1, 0, 0 });
	assert(a.voice_count() == 1);
	a.set_underwater(true);
	for (int i = 0; i < 100 && a.voice_count() > 0; i++)
	{
		a.update();
		clock::sleep_ms(20);
	}
	assert(a.voice_count() == 0, "the sound didn't finish");
}
