// The sound maths: .wav files, each ear's loudness, the underwater filter.
// (tests/test_audio.cpp)
module gocube::test::mixing_test;
import gocube::sound, gocube::audio, gocube::content, gocube::clock, gocube::util::vec;
import std::math, std::io;

fn void u16(Vec{char}* s, uint v) => s.push_all({ (char)(v & 0xFF), (char)(v >> 8) });
fn void u32(Vec{char}* s, uint v)
{
	u16(s, v & 0xFFFF);
	u16(s, v >> 16);
}

// A .wav file of these samples (the caller frees it).
fn Vec{char} wav(ushort tag, ushort channels, uint rate, ushort bits, char[] samples, bool extensible = false)
{
	Vec{char} fmt;
	defer fmt.free();
	u16(&fmt, extensible ? 0xFFFE : tag);
	u16(&fmt, channels);
	u32(&fmt, rate);
	u32(&fmt, rate * channels * bits / 8);
	u16(&fmt, channels * bits / 8);
	u16(&fmt, bits);
	if (extensible)
	{
		u16(&fmt, 22);
		u16(&fmt, bits);
		u32(&fmt, 0);
		u16(&fmt, tag); // the sub-format GUID starts with the format tag
		for (int i = 0; i < 14; i++) fmt.push(0);
	}
	Vec{char} body;
	defer body.free();
	body.push_all("WAVEfmt ");
	u32(&body, (uint)fmt.len());
	body.push_all(fmt.array_view());
	body.push_all("LIST");
	u32(&body, 3);
	body.push_all("abc");
	body.push(0); // (odd chunks are padded)
	body.push_all("data");
	u32(&body, (uint)samples.len);
	body.push_all(samples);
	Vec{char} out;
	out.push_all("RIFF");
	u32(&out, (uint)body.len());
	out.push_all(body.array_view());
	return out;
}

fn bool parses(Vec{char} file, Wav* w, String* error)
{
	defer file.free();
	w.free();
	return sound::parse_wav(file.array_view(), w, error);
}

fn void wav_files() @test
{
	Wav w;
	defer w.free();
	String error;
	char[10] ten = { [0..9] = 1 };
	char[17] seventeen;
	char[12] twelve;
	char[24] twentyfour;
	assert(parses(wav(1, 1, 22050, 16, &ten), &w, &error));
	assert(w.format == sound::PCM && w.channels == 1 && w.rate == 22050 && w.bits == 16 && w.frames() == 5);
	assert(parses(wav(3, 2, 48000, 32, &seventeen, true), &w, &error));
	assert(w.format == sound::FLOAT && w.frames() == 2 && w.data.len() == 16); // (a broken last frame is left out)
	assert(parses(wav(1, 2, 44100, 24, &twelve), &w, &error));
	assert(!sound::parse_wav("RIFF....WAVEjunk", &w, &error));
	assert(!sound::parse_wav("not a wav at all", &w, &error));
	assert(!parses(wav(2, 1, 22050, 4, "abcd"), &w, &error)); // ADPCM
	assert(error.contains("compressed"));
	assert(!parses(wav(1, 6, 48000, 16, &twentyfour), &w, &error)); // 5.1
	assert(!parses(wav(3, 1, 48000, 16, "ab"), &w, &error)); // 16-bit float
	w.free();
	@pool()
	{
		assert(sound::read_wav("../mods/core/sounds/ambience_day.wav", &w, &error), "%s", error);
	};
	assert(w.frames() > 0);
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
	assert(content.load("../mods/core", &why), "%s", why.str_view());
	content.freeze();
	Audio a;
	defer a.free();
	if (!a.init(&content, 0.0f)) return; // (no sound device here: nothing to check)
	a.set_listener({ 0, 0, 0 }, { 0, 0, -1 });
	a.play("core:effects", "pickup", { 1, 0, 0 });
	assert(a.voice_count() == 1);
	a.set_underwater(true);
	for (int i = 0; i < 100 && a.voice_count() > 0; i++)
	{
		a.update();
		clock::sleep_ms(20);
	}
	assert(a.voice_count() == 0, "the sound didn't finish");
}
