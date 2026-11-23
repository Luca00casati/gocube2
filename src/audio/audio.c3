// The game's sounds, through XAudio2: a sound set's sound (content
// "sound_sets": a list of files per kind), a random one of its files, its
// pitch varied a little so repeats don't sound the same, heard from where it
// is (full volume within UNIT_SIZE blocks, then falling off, silent beyond
// MAX_DISTANCE). Two loops as loud as the game says: the wind (set_wind:
// how open it is around) and a low hum (set_hum); now and then a piece of
// music (play_music: in both ears, one at a time); underwater, everything
// is muffled.
//
// Sounds (.wav) are read once and kept; at most VOICES play at a time (the
// oldest stops for a new one). How loud each is in each ear and the
// underwater filter are audio/mixing's. Without a sound device the game runs
// silent; when the device goes, sound starts again on the default one.
module gocube::audio @link("xaudio2", "ole32");
import gocube::sound, gocube::fs, gocube::content, gocube::data, gocube::log, gocube::win, gocube::util::vec, gocube::util::map, gocube::util::rand;
import std::math, std::atomic, std::thread, std::io;

const float MAX_DISTANCE = 32.0f;
const float UNIT_SIZE = 6.0f;
const float AMBIENCE_DB = -12.0f;
const float MUSIC_DB = -8.0f;
const float HUM_DB = -14.0f;
const int VOICES = 24;
const float MUFFLE_HZ @private = 700.0f; // the underwater low-pass

// --- XAudio2 (packed, as xaudio2.h and xapo.h declare it) -------------------------------

struct WaveFormat @packed
{
	ushort format_tag, channels;
	uint samples_per_sec, avg_bytes_per_sec;
	ushort block_align, bits_per_sample, size;
}

struct WaveFormatExtensible @packed
{
	WaveFormat format;
	ushort valid_bits;
	uint channel_mask;
	uint sub_format_data1;
	char[12] sub_format_rest;
}

struct XBuffer @packed
{
	uint flags, audio_bytes;
	char* audio_data;
	uint play_begin, play_length, loop_begin, loop_length, loop_count;
	void* context;
}

struct SendDescriptor @packed
{
	uint flags;
	void* output_voice;
}

struct VoiceSends @packed
{
	uint count;
	SendDescriptor* sends;
}

struct EffectDescriptor @packed
{
	void* effect;
	Bool initial_state;
	uint output_channels;
}

struct EffectChain @packed
{
	uint count;
	EffectDescriptor* descriptors;
}

struct VoiceDetails @packed
{
	uint creation_flags, active_flags, input_channels, input_sample_rate;
}

struct VoiceState @packed
{
	void* current_buffer_context;
	uint buffers_queued;
	ulong samples_played;
}

struct XAudio2Vtbl
{
	void* query_interface, add_ref, release;
	void* register_for_callbacks, unregister_for_callbacks, create_source_voice, create_submix_voice, create_mastering_voice;
	void* start_engine, stop_engine, commit_changes, get_performance_data, set_debug_configuration;
}

struct XAudio2
{
	XAudio2Vtbl* vt;
}

// IXAudio2Voice's methods, then IXAudio2SourceVoice's.
struct VoiceVtbl
{
	void* get_voice_details, set_output_voices, set_effect_chain, enable_effect, disable_effect, get_effect_state, set_effect_parameters;
	void* get_effect_parameters, set_filter_parameters, get_filter_parameters, set_output_filter_parameters, get_output_filter_parameters;
	void* set_volume, get_volume, set_channel_volumes, get_channel_volumes, set_output_matrix, get_output_matrix, destroy_voice;
	void* start, stop, submit_source_buffer, flush_source_buffers, discontinuity, exit_loop, get_state, set_frequency_ratio;
	void* get_frequency_ratio, set_source_sample_rate;
}

struct XVoice
{
	VoiceVtbl* vt;
}

const uint XAUDIO2_END_OF_STREAM = 0x0040;
const uint XAUDIO2_LOOP_INFINITE = 255;
const uint XAUDIO2_VOICE_NOSAMPLESPLAYED = 0x0100;
const float XAUDIO2_DEFAULT_FREQ_RATIO = 2.0f;
const uint XAUDIO2_DEFAULT_PROCESSOR = 1;
const ushort WAVE_FORMAT_PCM = 1;
const ushort WAVE_FORMAT_IEEE_FLOAT = 3;
const ushort WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
const int MAX_CHANNELS = 64;

extern fn Hresult xaudio2_create(XAudio2** out, uint flags, uint processor) @cname("XAudio2Create");
extern fn void* co_task_mem_alloc(usz size) @cname("CoTaskMemAlloc");
extern fn Hresult co_initialize_ex(void* reserved, uint flags) @cname("CoInitializeEx");
extern fn void co_uninitialize() @cname("CoUninitialize");

alias VoiceFn1 @private = fn void(void*);
alias SetVolumeFn @private = fn Hresult(void*, float, uint);
alias EffectFn @private = fn Hresult(void*, uint, uint);
alias OutputMatrixFn @private = fn Hresult(void*, void*, uint, uint, float*, uint);
alias StartFn @private = fn Hresult(void*, uint, uint);
alias SubmitFn @private = fn Hresult(void*, XBuffer*, void*);
alias StateFn @private = fn void(void*, VoiceState*, uint);
alias DetailsFn @private = fn void(void*, VoiceDetails*);
alias CreateSourceFn @private = fn Hresult(void*, XVoice**, WaveFormat*, uint, float, void*, VoiceSends*, EffectChain*);
alias CreateSubmixFn @private = fn Hresult(void*, XVoice**, uint, uint, uint, uint, VoiceSends*, EffectChain*);
alias CreateMasterFn @private = fn Hresult(void*, XVoice**, uint, uint, uint, WString, EffectChain*, uint);
alias CallbacksFn @private = fn Hresult(void*, void*);
alias ReleaseFn @private = fn uint(void*);

fn void XVoice.destroy(&self) => ((VoiceFn1)self.vt.destroy_voice)(self);
fn void XVoice.set_volume(&self, float v) => ((SetVolumeFn)self.vt.set_volume)(self, v, 0);
fn void XVoice.enable_effect(&self, uint i) => ((EffectFn)self.vt.enable_effect)(self, i, 0);
fn void XVoice.disable_effect(&self, uint i) => ((EffectFn)self.vt.disable_effect)(self, i, 0);
fn void XVoice.set_output_matrix(&self, XVoice* to, uint from_channels, uint to_channels, float* matrix)
	=> ((OutputMatrixFn)self.vt.set_output_matrix)(self, to, from_channels, to_channels, matrix, 0);
fn void XVoice.details(&self, VoiceDetails* out) => ((DetailsFn)self.vt.get_voice_details)(self, out);
fn void XVoice.start(&self) => ((StartFn)self.vt.start)(self, 0, 0);
fn Hresult XVoice.submit(&self, XBuffer* b) => ((SubmitFn)self.vt.submit_source_buffer)(self, b, null);
fn void XVoice.state(&self, VoiceState* out) => ((StateFn)self.vt.get_state)(self, out, XAUDIO2_VOICE_NOSAMPLESPLAYED);
fn void XVoice.set_frequency_ratio(&self, float r) => ((SetVolumeFn)self.vt.set_frequency_ratio)(self, r, 0);

// --- Underwater: a one-pole low-pass as an XAudio2 effect (IXAPO by hand) ---------------
// (XAudio2's own one-pole filter is a different curve; this is the one the
// game always had: audio/mixing's.)

struct XapoRegistration @packed
{
	ComGuid clsid;
	WChar[256] friendly_name;
	WChar[256] copyright;
	uint major, minor, flags, min_input_buffers, max_input_buffers, min_output_buffers, max_output_buffers;
}

struct ComGuid @packed
{
	uint data1;
	ushort data2, data3;
	char[8] data4;
}

struct LockParameters @packed
{
	WaveFormat* format;
	uint max_frame_count;
}

struct ProcessParameters @packed
{
	float* buffer;
	uint flags; // XAPO_BUFFER_SILENT 0, VALID 1
	uint valid_frame_count;
}

const ComGuid IID_UNKNOWN @private = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
const ComGuid IID_XAPO @private = { 0xA410B984, 0x9839, 0x4819, { 0xA0, 0xBE, 0x28, 0x56, 0xAE, 0x6B, 0x3A, 0xDB } };
const ComGuid MUFFLE_CLSID @private = { 0x6c1f3d0a, 0x9e4b, 0x4c2a, { 0x8b, 0x57, 0x3e, 0x2d, 0x9a, 0x41, 0xf0, 0xc7 } };
const Hresult E_NOINTERFACE @private = (Hresult)0x80004002;
const Hresult E_OUTOFMEMORY @private = (Hresult)0x8007000E;
const Hresult XAPO_E_FORMAT_UNSUPPORTED @private = (Hresult)0x88970001;

struct MuffleVtbl
{
	void* query_interface, add_ref, release, get_registration_properties, is_input_format_supported, is_output_format_supported;
	void* initialize, reset, lock_for_process, unlock_for_process, process, calc_input_frames, calc_output_frames;
}

struct Muffle
{
	MuffleVtbl* vt;
	Atomic{uint} refs;
	uint channels;
	float a;
	float[MAX_CHANNELS] state;
}

fn bool same_guid(ComGuid* a, ComGuid* b) @private => mem::equals(a, b, ComGuid::size);

// Float samples (what XAudio2 mixes in), any channels and rate.
fn bool is_float(WaveFormat* f) @private
{
	if (!f || f.bits_per_sample != 32) return false;
	if (f.format_tag == WAVE_FORMAT_EXTENSIBLE && f.size >= 22) return ((WaveFormatExtensible*)f).sub_format_data1 == WAVE_FORMAT_IEEE_FLOAT;
	return f.format_tag == WAVE_FORMAT_IEEE_FLOAT;
}

// The same float format on both sides.
fn Hresult check_format(WaveFormat* other, WaveFormat* asked, WaveFormat** suggested) @private
{
	if (suggested) *suggested = null;
	bool ok = is_float(asked) && other && asked.channels == other.channels && asked.samples_per_sec == other.samples_per_sec && asked.channels <= MAX_CHANNELS;
	return ok ? 0 : XAPO_E_FORMAT_UNSUPPORTED;
}

fn Hresult muffle_query(Muffle* self, ComGuid* iid, void** out) @private
{
	if (same_guid(iid, &&IID_UNKNOWN) || same_guid(iid, &&IID_XAPO))
	{
		*out = self;
		self.refs.add(1);
		return 0;
	}
	*out = null;
	return E_NOINTERFACE;
}

fn uint muffle_add_ref(Muffle* self) @private => self.refs.add(1) + 1;

fn uint muffle_release(Muffle* self) @private
{
	uint n = self.refs.sub(1) - 1;
	if (n == 0) free(self);
	return n;
}

fn Hresult muffle_registration(Muffle* self, XapoRegistration** out) @private
{
	XapoRegistration* p = co_task_mem_alloc(XapoRegistration::size); // (XAPOAlloc)
	*out = p;
	if (!p) return E_OUTOFMEMORY;
	*p = { .clsid = MUFFLE_CLSID, .major = 1, .flags = 0x3F, .min_input_buffers = 1, .max_input_buffers = 1, .min_output_buffers = 1, .max_output_buffers = 1 };
	foreach (i, c : "GoCube underwater") p.friendly_name[i] = c;
	return 0;
}

fn Hresult muffle_input_supported(Muffle* self, WaveFormat* output, WaveFormat* input, WaveFormat** suggested) @private => check_format(output, input, suggested);
fn Hresult muffle_output_supported(Muffle* self, WaveFormat* input, WaveFormat* output, WaveFormat** suggested) @private => check_format(input, output, suggested);
fn Hresult muffle_initialize(Muffle* self, void* data, uint size) @private => 0;
fn void muffle_reset(Muffle* self) @private => self.state = {};

fn Hresult muffle_lock(Muffle* self, uint in_count, LockParameters* in, uint out_count, LockParameters* out) @private
{
	if (in_count != 1 || out_count != 1 || !is_float(in[0].format) || !is_float(out[0].format) || in[0].format.channels != out[0].format.channels ||
		in[0].format.channels > MAX_CHANNELS) return XAPO_E_FORMAT_UNSUPPORTED;
	self.channels = in[0].format.channels;
	self.a = sound::lowpass_coefficient(MUFFLE_HZ, in[0].format.samples_per_sec);
	self.state = {};
	return 0;
}

fn void muffle_unlock(Muffle* self) @private {}

fn void muffle_process(Muffle* self, uint in_count, ProcessParameters* in, uint out_count, ProcessParameters* out, Bool enabled) @private
{
	// In place: the output is the input buffer. Disabled, it passes through
	// (and the filter's state waits, as a filter nothing goes through would).
	out[0].flags = in[0].flags;
	out[0].valid_frame_count = in[0].valid_frame_count;
	float* samples = in[0].buffer;
	if (enabled)
	{
		if (in[0].flags == 0) // silent (silence still lets the filter's tail out)
		{
			mem::clear(samples, (usz)in[0].valid_frame_count * self.channels * 4);
			out[0].flags = 1;
		}
		sound::lowpass(samples, in[0].valid_frame_count, (int)self.channels, self.a, &self.state);
	}
	record(out[0].flags != 0 ? samples : null, in[0].valid_frame_count * self.channels);
}

// --- Recording what's heard (--record) ---------------------------------------------------
// Every sample of the mix (after the underwater filter: what goes to the
// speakers, before the master volume), kept from start_recording() and
// written out by save_recording().

struct Recording @private
{
	Mutex mutex;
	bool on;
	Vec{float} samples;
}

Recording recording @private;

fn void recording_init() @init @private => recording.mutex.init()!!;

// (The audio thread's: `samples` null for silence.)
fn void record(float* samples, uint count) @private
{
	if (!recording.on) return;
	recording.mutex.lock();
	defer recording.mutex.unlock();
	for (uint i = 0; i < count; i++) recording.samples.push(samples ? samples[i] : 0.0f);
}

fn void Audio.start_recording(&self)
{
	recording.mutex.lock();
	recording.samples.clear();
	recording.on = true;
	recording.mutex.unlock();
}

fn void put_u32(Vec{char}* o, uint v) @private { for (int k = 0; k < 4; k++) o.push((char)(v >> (k * 8))); }
fn void put_u16(Vec{char}* o, ushort v) @private { o.push((char)v); o.push((char)(v >> 8)); }

// Writes what was heard since start_recording() to `path` (a 32-bit float
// stereo WAV at the device's rate) and stops recording. False if it
// couldn't (or there was no sound).
fn bool Audio.save_recording(&self, String path) => @pool()
{
	if (!self.master) return false;
	VoiceDetails details;
	self.master.details(&details);
	recording.mutex.lock();
	recording.on = false;
	Vec{float} samples = recording.samples;
	recording.samples = {};
	recording.mutex.unlock();
	defer samples.free();
	uint rate = details.input_sample_rate;
	uint bytes = (uint)samples.len() * 4;
	Vec{char} out;
	defer out.free();
	foreach (c : "RIFF") out.push(c);
	put_u32(&out, 36 + bytes);
	foreach (c : "WAVEfmt ") out.push(c);
	put_u32(&out, 16);
	put_u16(&out, WAVE_FORMAT_IEEE_FLOAT);
	put_u16(&out, 2);
	put_u32(&out, rate);
	put_u32(&out, rate * 8);
	put_u16(&out, 8);
	put_u16(&out, 32);
	foreach (c : "data") out.push(c);
	put_u32(&out, bytes);
	char* raw = (char*)samples.items;
	for (uint i = 0; i < bytes; i++) out.push(raw[i]);
	bool ok = fs::save_atomically(path, out.array_view());
	log::log("Recorded %.1f s of sound to %s", (double)samples.len() / 2.0 / (double)math::max(rate, 1u), path);
	return ok;
}

fn uint muffle_frames(Muffle* self, uint frames) @private => frames;

MuffleVtbl muffle_vtbl @private = {
	.query_interface = &muffle_query, .add_ref = &muffle_add_ref, .release = &muffle_release,
	.get_registration_properties = &muffle_registration, .is_input_format_supported = &muffle_input_supported,
	.is_output_format_supported = &muffle_output_supported, .initialize = &muffle_initialize, .reset = &muffle_reset,
	.lock_for_process = &muffle_lock, .unlock_for_process = &muffle_unlock, .process = &muffle_process,
	.calc_input_frames = &muffle_frames, .calc_output_frames = &muffle_frames };

// --- Losing the device ------------------------------------------------------------------
// XAudio2 stops for good when its device goes (unplugged headphones): the
// next update() starts over on the default device.

struct EngineCallbackVtbl
{
	void* on_processing_pass_start, on_processing_pass_end, on_critical_error;
}

struct EngineCallback
{
	EngineCallbackVtbl* vt;
	Atomic{bool} needed;
}

fn void on_pass(EngineCallback* self) @private {}
fn void on_critical_error(EngineCallback* self, Hresult error) @private => self.needed.store(true);

EngineCallbackVtbl engine_callback_vtbl @private = { &on_pass, &on_pass, &on_critical_error };

// --- The player -------------------------------------------------------------------------

// A loaded sound file: its samples, which every voice of it plays from.
struct Sound
{
	Wav wav;
	WaveFormat format;
}

struct Voice
{
	XVoice* voice;
	Sound* sound;
	float[3] at;
}

struct Audio
{
	Content* content;
	Rng random;
	bool com; // CoInitializeEx to undo
	EngineCallback restart;
	XAudio2* engine;
	XVoice* master;
	XVoice* all; // every sound goes through it (stereo), muffled underwater
	bool ok;
	Map{String, Sound*} loaded; // by path (keys owned; never moved: voices point into them)
	Vec{Voice} voices;
	Voice wind, hum, music;
	String wind_path, hum_path; // (heap)
	float[3] listener, forward;
	float master_volume, effects, ambience, music_volume, wind_amount, hum_amount;
	bool underwater;
}

fn float db_to_linear(float db) @private => math::pow(10.0f, db / 20.0f);

fn Sound* Audio.load(&self, String path)
{
	if (Sound* s = self.loaded.get_or(path, null)) return s;
	Sound* s = mem::new(Sound);
	String error;
	if (!sound::read_wav(path, &s.wav, &error))
	{
		log::log("Can't play %s: %s", path, error);
		free(s);
		return null;
	}
	Wav* w = &s.wav;
	s.format = { .format_tag = w.format == sound::FLOAT ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM, .channels = w.channels,
		.samples_per_sec = w.rate, .bits_per_sample = w.bits, .block_align = (ushort)(w.channels * w.bits / 8) };
	s.format.avg_bytes_per_sec = w.rate * s.format.block_align;
	self.loaded[path.copy(mem)] = s;
	return s;
}

fn XVoice* Audio.start_voice(&self, Sound* s, bool loop)
{
	if (s.wav.data.is_empty()) return null;
	SendDescriptor send = { 0, self.all };
	VoiceSends sends = { 1, &send };
	XVoice* v;
	if (((CreateSourceFn)self.engine.vt.create_source_voice)(self.engine, &v, &s.format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, null, &sends, null) < 0) return null;
	XBuffer buffer = { .flags = XAUDIO2_END_OF_STREAM, .audio_bytes = (uint)s.wav.data.len(), .audio_data = s.wav.data.items,
		.loop_count = loop ? XAUDIO2_LOOP_INFINITE : 0 };
	if (v.submit(&buffer) < 0)
	{
		v.destroy();
		return null;
	}
	return v;
}

// Its gains for where the ears are: mono into both speakers, stereo each side into its own.
fn void Audio.place(&self, Voice* v)
{
	float[2] g = sound::stereo_gains(self.listener, self.forward, v.at, UNIT_SIZE, MAX_DISTANCE);
	float[4] matrix = v.sound.wav.channels == 1 ? { g[0], g[1], 0, 0 } : { g[0], 0, 0, g[1] };
	v.voice.set_output_matrix(self.all, v.sound.wav.channels, 2, &matrix);
}

fn void Audio.start_engine(&self)
{
	self.ok = false;
	if (xaudio2_create(&self.engine, 0, XAUDIO2_DEFAULT_PROCESSOR) < 0)
	{
		log::log("No sound (XAudio2 didn't start): playing without sound");
		self.engine = null;
		return;
	}
	self.restart.vt = &engine_callback_vtbl;
	((CallbacksFn)self.engine.vt.register_for_callbacks)(self.engine, &self.restart);
	if (((CreateMasterFn)self.engine.vt.create_mastering_voice)(self.engine, &self.master, 0, 0, 0, null, null, 6) < 0) // (AudioCategory_GameEffects)
	{
		log::log("No sound device: playing without sound");
		self.stop_engine();
		return;
	}
	VoiceDetails details;
	self.master.details(&details);
	Muffle* muffle = mem::new(Muffle);
	muffle.vt = &muffle_vtbl;
	muffle.refs.store(1);
	muffle.channels = 2;
	EffectDescriptor effect = { muffle, 0, 2 };
	EffectChain chain = { 1, &effect };
	Hresult hr = ((CreateSubmixFn)self.engine.vt.create_submix_voice)(self.engine, &self.all, 2, details.input_sample_rate, 0, 0, null, &chain);
	muffle_release(muffle); // (the voice holds it)
	if (hr < 0)
	{
		log::log("Can't mix sounds: playing without sound");
		self.stop_engine();
		return;
	}
	if (self.underwater) self.all.enable_effect(0);
	self.master.set_volume(self.master_volume);
	log::log("Sound: %d Hz, %d channels", details.input_sample_rate, details.input_channels);
	self.ok = true;
}

fn void Audio.stop_engine(&self)
{
	self.ok = false;
	foreach (&v : self.voices) v.voice.destroy();
	self.voices.clear();
	foreach (v : (Voice*[3]){ &self.wind, &self.hum, &self.music })
	{
		if (v.voice) v.voice.destroy();
		v.voice = null;
	}
	if (self.all) self.all.destroy();
	if (self.master) self.master.destroy();
	self.all = self.master = null;
	if (self.engine)
	{
		((CallbacksFn)self.engine.vt.unregister_for_callbacks)(self.engine, &self.restart);
		((ReleaseFn)self.engine.vt.release)(self.engine);
		self.engine = null;
	}
}

// A voice not placed anywhere: in both ears as it is.
fn void Audio.unplaced(&self, Voice* v)
{
	float[4] matrix = v.sound.wav.channels == 1 ? { 1, 1, 0, 0 } : { 1, 0, 0, 1 };
	v.voice.set_output_matrix(self.all, v.sound.wav.channels, 2, &matrix);
}

fn void Audio.start_loop(&self, Voice* v, String path)
{
	v.sound = path.len ? self.load(path) : null;
	v.voice = v.sound ? self.start_voice(v.sound, true) : null;
	if (!v.voice) return;
	self.unplaced(v);
	v.voice.set_volume(0.0f);
	v.voice.start();
}

fn void Audio.start_ambience(&self)
{
	self.start_loop(&self.wind, self.wind_path);
	self.start_loop(&self.hum, self.hum_path);
	self.apply_ambience();
}

fn void Audio.apply_ambience(&self)
{
	if (self.wind.voice) self.wind.voice.set_volume(db_to_linear(AMBIENCE_DB) * self.ambience * self.wind_amount);
	if (self.hum.voice) self.hum.voice.set_volume(db_to_linear(HUM_DB) * self.ambience * self.hum_amount);
	if (self.music.voice) self.music.voice.set_volume(db_to_linear(MUSIC_DB) * self.music_volume);
}

// A random file of a sound set's kind (heap; "" if none).
fn String Audio.pick(&self, String set, String kind) => @pool()
{
	if (!self.content) return "";
	Data* def = self.content.sound_sets.get(set);
	if (!def) return "";
	Data* files = def.get(kind);
	if (!files || !files.is_array() || files.len() == 0) return "";
	Data* file = files.at((sz)(self.random.unit() * (double)files.len()) % files.len());
	return file.is_string() ? self.content.asset_path(file.as_string()).copy(mem) : "";
}

// Opens the default sound device; false (and silent) if there is none.
// `content` must outlive this.
fn bool Audio.init(&self, Content* content, float master_volume = 1.0f)
{
	*self = { .content = content, .random = rand::seed_random(), .forward = { 0, 0, -1 }, .master_volume = master_volume, .effects = 1, .ambience = 1,
		.music_volume = 1 };
	self.com = co_initialize_ex(null, 0) >= 0; // (COINIT_MULTITHREADED; RPC_E_CHANGED_MODE: COM is already up on this thread)
	self.wind_path = self.pick("core:ambience", "wind");
	self.hum_path = self.pick("core:ambience", "hum");
	self.start_engine();
	if (!self.ok) return false;
	self.start_ambience();
	return true;
}

fn void Audio.free(&self)
{
	self.stop_engine();
	self.loaded.@each(; String k, Sound* s)
	{
		free(k.ptr);
		s.wav.free();
		free(s);
	};
	self.loaded.free();
	self.voices.free();
	if (self.wind_path.len) free(self.wind_path.ptr);
	if (self.hum_path.len) free(self.hum_path.ptr);
	if (self.com) co_uninitialize();
	*self = {};
}

fn bool Audio.active(&self) => self.ok;
fn int Audio.voice_count(&self) => (int)self.voices.len();

fn void Audio.play(&self, String set, String kind, float[3] at, float volume_db = 0)
{
	if (!self.ok) return;
	float dx = at[0] - self.listener[0];
	float dy = at[1] - self.listener[1];
	float dz = at[2] - self.listener[2];
	if (dx * dx + dy * dy + dz * dz > MAX_DISTANCE * MAX_DISTANCE) return; // out of earshot
	String path = self.pick(set, kind);
	if (!path.len) return;
	defer free(path.ptr);
	Sound* s = self.load(path);
	if (!s) return;
	if (self.voices.len() >= VOICES) // the oldest makes room
	{
		self.voices[0].voice.destroy();
		self.voices.remove_at(0);
	}
	Voice v = { .sound = s, .at = at };
	v.voice = self.start_voice(s, false);
	if (!v.voice) return;
	self.place(&v);
	v.voice.set_frequency_ratio(self.random.range(0.9f, 1.1f));
	v.voice.set_volume(db_to_linear(volume_db) * self.effects);
	v.voice.start();
	self.voices.push(v);
}

// Each frame: where the ears are (the camera) and which way they face.
fn void Audio.set_listener(&self, float[3] position, float[3] forward)
{
	self.listener = position;
	self.forward = forward;
}

// How much wind: 0 (none: in a cave) .. 1 (out in the open, up high).
fn void Audio.set_wind(&self, float amount)
{
	self.wind_amount = math::clamp(amount, 0.0f, 1.0f);
	self.apply_ambience();
}

// How loud the hum: 0 .. 1 (right by one).
fn void Audio.set_hum(&self, float amount)
{
	self.hum_amount = math::clamp(amount, 0.0f, 1.0f);
	self.apply_ambience();
}

// Starts a random file of a sound set's kind as music (one at a time; a
// piece already playing stops).
fn void Audio.play_music(&self, String set, String kind)
{
	if (!self.ok) return;
	String path = self.pick(set, kind);
	if (!path.len) return;
	defer free(path.ptr);
	Sound* s = self.load(path);
	if (!s) return;
	if (self.music.voice) self.music.voice.destroy();
	self.music = { .sound = s };
	self.music.voice = self.start_voice(s, false);
	if (!self.music.voice) return;
	self.unplaced(&self.music);
	self.apply_ambience();
	self.music.voice.start();
}

fn bool Audio.music_playing(&self) => self.music.voice != null;

// Volumes, 0..1: everything, the game's sounds, the ambience (wind), music.
fn void Audio.set_volumes(&self, float master, float effects, float ambience, float music = 1.0f)
{
	self.effects = math::clamp(effects, 0.0f, 1.0f);
	self.ambience = math::clamp(ambience, 0.0f, 1.0f);
	self.music_volume = math::clamp(music, 0.0f, 1.0f);
	self.apply_ambience();
	master = math::clamp(master, 0.0f, 1.0f);
	if (master == self.master_volume) return;
	self.master_volume = master;
	if (self.master) self.master.set_volume(master);
}

fn void Audio.set_underwater(&self, bool underwater)
{
	if (underwater == self.underwater) return;
	self.underwater = underwater;
	if (!self.all) return;
	if (underwater)
	{
		self.all.enable_effect(0);
	}
	else
	{
		self.all.disable_effect(0);
	}
}

// Frees sounds that have finished (call once a frame).
fn void Audio.update(&self)
{
	if (self.restart.needed.load())
	{
		self.restart.needed.store(false);
		log::log("The sound device changed: starting sound again");
		self.stop_engine();
		self.start_engine();
		if (self.ok) self.start_ambience();
	}
	if (!self.ok) return;
	if (self.music.voice)
	{
		VoiceState state;
		self.music.voice.state(&state);
		if (state.buffers_queued == 0) // the piece is over
		{
			self.music.voice.destroy();
			self.music.voice = null;
		}
	}
	for (sz i = 0; i < self.voices.len();)
	{
		VoiceState state;
		self.voices[i].voice.state(&state);
		if (state.buffers_queued == 0) // played to the end
		{
			self.voices[i].voice.destroy();
			self.voices.remove_at(i);
			continue;
		}
		self.place(self.voices.get_ref(i)); // (the ears move)
		i++;
	}
}
