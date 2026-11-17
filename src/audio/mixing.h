#pragma once
// The sound maths that doesn't need a sound device (audio/audio.h plays it
// through XAudio2): reading .wav files, how loud a sound at a place is in each
// ear, and the underwater low-pass filter. The numbers are miniaudio's (the
// library the game played its sounds through before), so the game sounds the
// same: tests/test_audio.cpp pins them down.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gc::sound {

// A .wav file's samples, as they are in the file.
struct Wav {
	enum Format : uint16_t { PCM = 1, FLOAT = 3 };
	Format format = PCM;
	uint16_t channels = 0;     // 1 or 2
	uint32_t rate = 0;         // frames a second
	uint16_t bits = 0;         // 8, 16, 24 or 32 (PCM); 32 (float)
	std::vector<uint8_t> data; // the frames, interleaved
	size_t frames() const { return channels && bits ? data.size() / (size_t(channels) * bits / 8) : 0; }
};

// Mono or stereo PCM (8, 16, 24, 32 bit) or 32-bit float, also in
// WAVE_FORMAT_EXTENSIBLE. False and `error` for anything else.
bool parse_wav(std::string_view bytes, Wav &out, std::string &error);
bool read_wav(const std::filesystem::path &path, Wav &out, std::string &error);

// How loud a sound at `at` is in the left and right speaker for ears at
// `listener` facing `forward` (y up): inverse distance falloff (1 within
// `min_distance`, then min / distance, no quieter than at `max_distance`),
// times each speaker's share by direction ((1 + cos) / 2 of the angle to a
// speaker facing straight left or right; never under MIN_CHANNEL_GAIN, so a
// sound on the right is still heard on the left). miniaudio's spatializer
// with its default settings.
constexpr float MIN_CHANNEL_GAIN = 0.2f;
void stereo_gains(const float listener[3], const float forward[3], const float at[3], float min_distance,
		float max_distance, float gains[2]);

// A one-pole low-pass filter (miniaudio's lpf1): y = x + a * (y1 - x).
float lowpass_coefficient(double cutoff_hz, double rate_hz); // a
// Filters `frames` interleaved frames of `channels` in place; `state` holds
// each channel's last output (y1) and is carried to the next call.
void lowpass(float *samples, size_t frames, int channels, float a, float *state);

} // namespace gc::sound
