// The sound maths that doesn't need a sound device (audio/audio plays it
// through XAudio2): reading sounds (.flac: flac.c3), how loud a sound at a place is in
// each ear, and the underwater low-pass filter. The numbers are miniaudio's
// (the library the game played its sounds through first), so the game
// sounds the same.
module gocube::sound;
import gocube::util::vec;
import std::math, std::io;

const ushort PCM = 1;
const ushort FLOAT = 3;

// A sound's samples, as XAudio2 plays them (read from .flac: flac.c3).
struct Samples
{
	ushort format;  // PCM (16 bits) or FLOAT (32 bits)
	ushort channels; // 1 or 2
	uint rate;       // frames a second
	ushort bits;     // 16 (PCM) or 32 (float)
	Vec{char} data;  // the frames, interleaved
}

fn void Samples.free(&self) => self.data.free();
fn sz Samples.frames(&self) => self.channels && self.bits ? self.data.len() / ((sz)self.channels * self.bits / 8) : 0;

// A .flac file. False and `error` (temp) if it can't be read or played.
fn bool read_sound(String path, Samples* out, String* error)
{
	char[]? bytes = file::load_temp(path);
	if (catch bytes)
	{
		*error = "can't read it";
		return false;
	}
	return parse_flac(bytes, out, error);
}

// How loud a sound at `at` is in the left and right speaker for ears at
// `listener` facing `forward` (y up): inverse distance falloff (1 within
// `min_distance`, then min / distance, no quieter than at `max_distance`),
// times each speaker's share by direction ((1 + cos) / 2 of the angle to a
// speaker facing straight left or right; never under MIN_CHANNEL_GAIN, so a
// sound on the right is still heard on the left). miniaudio's spatializer
// with its default settings.
const float MIN_CHANNEL_GAIN = 0.2f;

fn float[2] stereo_gains(float[3] listener, float[3] forward, float[3] at, float min_distance, float max_distance)
{
	float[3] rel = { at[0] - listener[0], at[1] - listener[1], at[2] - listener[2] };
	float distance = math::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
	float gain = 1.0f;
	if (min_distance < max_distance) gain = min_distance / (min_distance + (math::clamp(distance, min_distance, max_distance) - min_distance));
	gain = math::clamp(gain, 0.0f, 1.0f);
	float[2] gains = { gain, gain };
	if (distance <= 0.001f) return gains; // on top of the ears: no direction
	// The listener's axes: ahead, and right = ahead x up (up is +y).
	float[3] ahead = forward;
	float al = math::sqrt(ahead[0] * ahead[0] + ahead[1] * ahead[1] + ahead[2] * ahead[2]);
	if (al > 0.0f) foreach (&c : ahead) *c /= al;
	float[3] right = { -ahead[2], 0.0f, ahead[0] };
	float rl = math::sqrt(right[0] * right[0] + right[2] * right[2]);
	if (rl > 0.0f)
	{
		right[0] /= rl;
		right[2] /= rl;
	}
	else
	{
		right[0] = 1.0f; // looking straight up or down
	}
	// The speakers face straight left and right (miniaudio's default for
	// stereo: its "side" speakers, so sounds behind aren't quieter).
	float x = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / distance;
	float[2] to_speaker = { -x, x };
	for (int c = 0; c < 2; c++) gains[c] *= math::max((to_speaker[c] + 1.0f) * 0.5f, MIN_CHANNEL_GAIN);
	return gains;
}

// A one-pole low-pass filter (miniaudio's lpf1): y = x + a * (y1 - x).
fn float lowpass_coefficient(double cutoff_hz, double rate_hz) => (float)math::exp(-2.0 * 3.14159265358979323846 * cutoff_hz / rate_hz);

// Filters `frames` interleaved frames of `channels` in place; `state` holds
// each channel's last output (y1) and is carried to the next call.
fn void lowpass(float* samples, sz frames, int channels, float a, float* state)
{
	for (sz f = 0; f < frames; f++)
	{
		for (int c = 0; c < channels; c++)
		{
			float* x = &samples[f * channels + c];
			*x = *x + a * (state[c] - *x);
			state[c] = *x;
		}
	}
}
