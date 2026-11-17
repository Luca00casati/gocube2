#include "audio/mixing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gc::sound {

namespace {

uint16_t u16(const char *p) {
	return uint16_t(uint8_t(p[0]) | uint8_t(p[1]) << 8);
}

uint32_t u32(const char *p) {
	return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
}

constexpr uint16_t EXTENSIBLE = 0xFFFE;

} // namespace

bool parse_wav(std::string_view bytes, Wav &out, std::string &error) {
	if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE") {
		error = "not a .wav file";
		return false;
	}
	bool have_format = false, have_data = false;
	Wav w;
	size_t i = 12;
	while (i + 8 <= bytes.size()) {
		const std::string_view id = bytes.substr(i, 4);
		const uint32_t size = u32(bytes.data() + i + 4);
		const size_t body = i + 8;
		if (size > bytes.size() - body) {
			if (id == "data" && have_format) { // (some writers leave the size wrong: take what's there)
				w.data.assign(bytes.begin() + std::ptrdiff_t(body), bytes.end());
				have_data = true;
			}
			break;
		}
		const char *p = bytes.data() + body;
		if (id == "fmt ") {
			if (size < 16) {
				error = "its format chunk is too short";
				return false;
			}
			uint16_t tag = u16(p);
			w.channels = u16(p + 2);
			w.rate = u32(p + 4);
			w.bits = u16(p + 14);
			if (tag == EXTENSIBLE) {
				if (size < 40) {
					error = "its format chunk is too short";
					return false;
				}
				tag = u16(p + 24); // (the sub-format GUID's first two bytes)
			}
			if (tag != Wav::PCM && tag != Wav::FLOAT) {
				error = "it is compressed (only PCM and float .wav files play)";
				return false;
			}
			w.format = Wav::Format(tag);
			have_format = true;
		} else if (id == "data") {
			w.data.assign(p, p + size);
			have_data = true;
		}
		i = body + size + (size & 1); // (chunks are padded to even sizes)
	}
	if (!have_format || !have_data) {
		error = "it has no format or no samples";
		return false;
	}
	if (w.channels != 1 && w.channels != 2) {
		error = "it has " + std::to_string(w.channels) + " channels (sounds are mono or stereo)";
		return false;
	}
	const bool bits_ok = w.format == Wav::FLOAT ? w.bits == 32
												: (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32);
	if (!bits_ok || w.rate < 1000 || w.rate > 200000) {
		error = "its sample format isn't one that plays";
		return false;
	}
	w.data.resize(w.frames() * w.channels * (w.bits / 8)); // (whole frames only)
	out = std::move(w);
	return true;
}

bool read_wav(const std::filesystem::path &path, Wav &out, std::string &error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "can't read it";
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return parse_wav(ss.str(), out, error);
}

void stereo_gains(const float listener[3], const float forward[3], const float at[3], float min_distance,
		float max_distance, float gains[2]) {
	const float rel[3] = {at[0] - listener[0], at[1] - listener[1], at[2] - listener[2]};
	const float distance = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
	float gain = 1.0f;
	if (min_distance < max_distance) {
		gain = min_distance / (min_distance + (std::clamp(distance, min_distance, max_distance) - min_distance));
	}
	gains[0] = gains[1] = std::clamp(gain, 0.0f, 1.0f);
	if (distance <= 0.001f) {
		return; // on top of the ears: no direction
	}
	// The listener's axes: ahead, and right = ahead x up (up is +y).
	float ahead[3] = {forward[0], forward[1], forward[2]};
	const float al = std::sqrt(ahead[0] * ahead[0] + ahead[1] * ahead[1] + ahead[2] * ahead[2]);
	if (al > 0.0f) {
		for (float &c : ahead) {
			c /= al;
		}
	}
	float right[3] = {-ahead[2], 0.0f, ahead[0]};
	const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
	if (rl > 0.0f) {
		right[0] /= rl;
		right[2] /= rl;
	} else {
		right[0] = 1.0f; // looking straight up or down
	}
	// The speakers face straight left and right (miniaudio's default for
	// stereo: its "side" speakers, so sounds behind aren't quieter).
	const float x = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / distance;
	const float to_speaker[2] = {-x, x};
	for (int c = 0; c < 2; c++) {
		gains[c] *= std::max((to_speaker[c] + 1.0f) * 0.5f, MIN_CHANNEL_GAIN);
	}
}

float lowpass_coefficient(double cutoff_hz, double rate_hz) {
	return float(std::exp(-2.0 * 3.14159265358979323846 * cutoff_hz / rate_hz));
}

void lowpass(float *samples, size_t frames, int channels, float a, float *state) {
	for (size_t f = 0; f < frames; f++) {
		for (int c = 0; c < channels; c++) {
			float &x = samples[f * size_t(channels) + size_t(c)];
			x = x + a * (state[c] - x);
			state[c] = x;
		}
	}
}

} // namespace gc::sound
