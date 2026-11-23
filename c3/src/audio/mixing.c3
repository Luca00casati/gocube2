// The sound maths that doesn't need a sound device (audio/audio plays it
// through XAudio2): reading .wav files, how loud a sound at a place is in
// each ear, and the underwater low-pass filter. The numbers are miniaudio's
// (the library the game played its sounds through first), so the game
// sounds the same. (src/audio/mixing.*)
module gocube::sound;
import gocube::util::vec;
import std::math, std::io;

const ushort PCM = 1;
const ushort FLOAT = 3;
const ushort EXTENSIBLE @private = 0xFFFE;

// A .wav file's samples, as they are in the file.
struct Wav
{
	ushort format;  // PCM or FLOAT
	ushort channels; // 1 or 2
	uint rate;       // frames a second
	ushort bits;     // 8, 16, 24 or 32 (PCM); 32 (float)
	Vec{char} data;  // the frames, interleaved
}

fn void Wav.free(&self) => self.data.free();
fn sz Wav.frames(&self) => self.channels && self.bits ? self.data.len() / ((sz)self.channels * self.bits / 8) : 0;

fn ushort u16(char[] b, sz i) @private => (ushort)(b[i] | (ushort)b[i + 1] << 8);
fn uint u32(char[] b, sz i) @private => (uint)u16(b, i) | (uint)u16(b, i + 2) << 16;

// Mono or stereo PCM (8, 16, 24, 32 bit) or 32-bit float, also in
// WAVE_FORMAT_EXTENSIBLE. False and `error` (temp) for anything else.
fn bool parse_wav(char[] bytes, Wav* out, String* error)
{
	if (bytes.len < 12 || (String)bytes[0:4] != "RIFF" || (String)bytes[8:4] != "WAVE")
	{
		*error = "not a .wav file";
		return false;
	}
	bool have_format;
	bool have_data;
	Wav w;
	sz i = 12;
	while (i + 8 <= bytes.len)
	{
		String id = (String)bytes[i:4];
		uint size = u32(bytes, i + 4);
		sz body = i + 8;
		if ((sz)size > bytes.len - body)
		{
			if (id == "data" && have_format) // (some writers leave the size wrong: take what's there)
			{
				w.data.push_all(bytes[body..]);
				have_data = true;
			}
			break;
		}
		if (id == "fmt ")
		{
			if (size < 16)
			{
				w.free();
				*error = "its format chunk is too short";
				return false;
			}
			ushort tag = u16(bytes, body);
			w.channels = u16(bytes, body + 2);
			w.rate = u32(bytes, body + 4);
			w.bits = u16(bytes, body + 14);
			if (tag == EXTENSIBLE)
			{
				if (size < 40)
				{
					w.free();
					*error = "its format chunk is too short";
					return false;
				}
				tag = u16(bytes, body + 24); // (the sub-format GUID's first two bytes)
			}
			if (tag != PCM && tag != FLOAT)
			{
				w.free();
				*error = "it is compressed (only PCM and float .wav files play)";
				return false;
			}
			w.format = tag;
			have_format = true;
		}
		else if (id == "data")
		{
			w.data.clear();
			w.data.push_all(bytes[body : (sz)size]);
			have_data = true;
		}
		i = body + size + (size & 1); // (chunks are padded to even sizes)
	}
	if (!have_format || !have_data)
	{
		w.free();
		*error = "it has no format or no samples";
		return false;
	}
	if (w.channels != 1 && w.channels != 2)
	{
		w.free();
		*error = string::tformat("it has %d channels (sounds are mono or stereo)", w.channels);
		return false;
	}
	bool bits_ok = w.format == FLOAT ? w.bits == 32 : (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32);
	if (!bits_ok || w.rate < 1000 || w.rate > 200000)
	{
		w.free();
		*error = "its sample format isn't one that plays";
		return false;
	}
	w.data.resize(w.frames() * w.channels * (w.bits / 8)); // (whole frames only)
	*out = w;
	return true;
}

fn bool read_wav(String path, Wav* out, String* error)
{
	char[]? bytes = file::load_temp(path);
	if (catch bytes)
	{
		*error = "can't read it";
		return false;
	}
	return parse_wav(bytes, out, error);
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
