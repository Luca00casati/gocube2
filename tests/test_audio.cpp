// The sound maths (audio/mixing.h): reading .wav files, how loud a sound is in
// each ear, the underwater filter. The numbers are miniaudio's, which the game
// played through before XAudio2 (checked against its spatializer and lpf1).

#include "audio/mixing.h"

#include "check.h"

#include <cmath>
#include <cstring>
#include <string>

namespace {

namespace sound = gc::sound;

std::string wav(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits, const std::string &samples,
		bool extensible = false) {
	auto u16 = [](std::string &s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); };
	auto u32 = [&](std::string &s, uint32_t v) { u16(s, uint16_t(v & 0xFFFF)); u16(s, uint16_t(v >> 16)); };
	std::string fmt;
	u16(fmt, extensible ? 0xFFFE : tag);
	u16(fmt, channels);
	u32(fmt, rate);
	u32(fmt, rate * channels * bits / 8);
	u16(fmt, uint16_t(channels * bits / 8));
	u16(fmt, bits);
	if (extensible) {
		u16(fmt, 22);
		u16(fmt, bits);
		u32(fmt, 0);
		u16(fmt, tag); // the sub-format GUID starts with the format tag
		fmt += std::string(14, '\0');
	}
	std::string body = "WAVE";
	body += "fmt ";
	u32(body, uint32_t(fmt.size()));
	body += fmt;
	body += "LIST";
	u32(body, 3);
	body += "abc";
	body += '\0'; // (odd chunks are padded)
	body += "data";
	u32(body, uint32_t(samples.size()));
	body += samples;
	std::string out = "RIFF";
	u32(out, uint32_t(body.size()));
	return out + body;
}

void check_gains(const float listener[3], const float forward[3], const float at[3], float left, float right) {
	float g[2];
	sound::stereo_gains(listener, forward, at, 6.0f, 32.0f, g);
	CHECK(g[0] == check::Approx(left).epsilon(0.001));
	CHECK(g[1] == check::Approx(right).epsilon(0.001));
}

} // namespace

TEST_CASE(".wav files: PCM and float, mono and stereo; nothing else") {
	sound::Wav w;
	std::string error;
	REQUIRE(sound::parse_wav(wav(1, 1, 22050, 16, std::string(10, '\x01')), w, error));
	CHECK(w.format == sound::Wav::PCM);
	CHECK(w.channels == 1);
	CHECK(w.rate == 22050);
	CHECK(w.bits == 16);
	CHECK(w.frames() == 5);
	REQUIRE(sound::parse_wav(wav(3, 2, 48000, 32, std::string(17, '\0'), true), w, error));
	CHECK(w.format == sound::Wav::FLOAT);
	CHECK(w.frames() == 2); // (a broken last frame is left out)
	CHECK(w.data.size() == 16);
	CHECK(sound::parse_wav(wav(1, 2, 44100, 24, std::string(12, '\0')), w, error));

	CHECK_FALSE(sound::parse_wav("RIFF....WAVEjunk", w, error));
	CHECK_FALSE(sound::parse_wav("not a wav at all", w, error));
	CHECK_FALSE(sound::parse_wav(wav(2, 1, 22050, 4, "abcd"), w, error)); // ADPCM
	CHECK(error.find("compressed") != std::string::npos);
	CHECK_FALSE(sound::parse_wav(wav(1, 6, 48000, 16, std::string(24, '\0')), w, error)); // 5.1
	CHECK_FALSE(sound::parse_wav(wav(3, 1, 48000, 16, "ab"), w, error)); // 16-bit float

	// The game's own sounds all read.
	REQUIRE(sound::read_wav(GOCUBE_SOURCE_DIR "/mods/core/sounds/ambience_day.wav", w, error));
	CHECK(w.frames() > 0);
}

TEST_CASE("a sound is as loud in each ear as miniaudio made it") {
	const float origin[3] = {0, 0, 0}, north[3] = {0, 0, -1}, east[3] = {1, 0, 0};
	const float right10[3] = {10, 0, 0}, left10[3] = {-10, 0, 0}, ahead10[3] = {0, 0, -10}, behind10[3] = {0, 0, 10};
	// Falloff: 1 within 6 blocks, then 6 / distance, never below 6 / 32.
	// Each ear: (1 + cos) / 2 of the angle to it, at least 0.2.
	check_gains(origin, north, right10, 0.12f, 0.6f);
	check_gains(origin, north, left10, 0.6f, 0.12f);
	check_gains(origin, north, ahead10, 0.3f, 0.3f);
	check_gains(origin, north, behind10, 0.3f, 0.3f); // (no quieter behind)
	check_gains(origin, east, behind10, 0.12f, 0.6f); // facing east, south is on the right
	const float close_by[3] = {3, 0, 0}, far_off[3] = {50, 0, 0};
	check_gains(origin, north, close_by, 0.2f, 1.0f);
	check_gains(origin, north, far_off, 0.0375f, 0.1875f);
	const float on_top[3] = {0, 0.0005f, 0};
	check_gains(origin, north, on_top, 1.0f, 1.0f);
	// Looking straight down: right is +x. Height counts in the distance.
	const float down[3] = {0, -1, 0}, below[3] = {8, -6, 0};
	check_gains(origin, down, below, 0.6f * 0.2f, 0.6f * 0.9f); // (the left ear: 0.1, at least 0.2)
}

TEST_CASE("underwater: a one-pole low-pass at 700 Hz") {
	const float a = sound::lowpass_coefficient(700, 48000);
	CHECK(a == check::Approx(std::exp(-2 * 3.14159265358979 * 700 / 48000)));
	// A step settles to it, slowly; each channel on its own.
	float samples[200 * 2];
	for (int i = 0; i < 200; i++) {
		samples[i * 2] = 1.0f;
		samples[i * 2 + 1] = 0.0f;
	}
	float state[2] = {0, 0};
	sound::lowpass(samples, 200, 2, a, state);
	CHECK(samples[0] == check::Approx(1 - a));
	CHECK(samples[2] == check::Approx(1 - a * a));
	CHECK(samples[1] == 0.0f);
	CHECK(state[0] == check::Approx(1 - std::pow(a, 200.0f)));
	// Carried over to the next call.
	float more[2] = {1.0f, 0.0f};
	sound::lowpass(more, 1, 2, a, state);
	CHECK(more[0] == check::Approx(1 - std::pow(a, 201.0f)));
}
