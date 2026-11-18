#include "audio/audio.h"

#include "audio/mixing.h"
#include "log.h"

#include <mmdeviceapi.h>
#include <xapo.h>
#include <xaudio2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace gc {

namespace {

constexpr float MUFFLE_HZ = 700.0f; // the underwater low-pass

float db_to_linear(float db) {
	return std::pow(10.0f, db / 20.0f);
}

std::string narrow(const wchar_t *w) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(size_t(std::max(n, 1)), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	s.resize(std::strlen(s.c_str()));
	return s;
}

// --- Underwater: a one-pole low-pass as an XAudio2 effect ---------------------------
// (XAudio2's own one-pole filter is a different curve; this is the one the
// game always had: audio/mixing.h's.)

// {6C1F3D0A-9E4B-4C2A-8B57-3E2D9A41F0C7}
constexpr CLSID MUFFLE_CLSID = {0x6c1f3d0a, 0x9e4b, 0x4c2a, {0x8b, 0x57, 0x3e, 0x2d, 0x9a, 0x41, 0xf0, 0xc7}};
const XAPO_REGISTRATION_PROPERTIES MUFFLE_PROPERTIES = {MUFFLE_CLSID, L"GoCube underwater", L"", 1, 0,
		XAPO_FLAG_CHANNELS_MUST_MATCH | XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
				XAPO_FLAG_BUFFERCOUNT_MUST_MATCH | XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED,
		1, 1, 1, 1};

// Float samples (what XAudio2 mixes in), any channels and rate.
bool is_float(const WAVEFORMATEX *f) {
	if (!f || f->wBitsPerSample != 32) {
		return false;
	}
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
		return reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
	}
	return f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
}

// (IXAPO by hand: the SDK's CXAPOBase library is built for the DLL C runtime,
// which doesn't link with the static one.)
class Muffle : public IXAPO {
public:
	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IXAPO)) {
			*out = static_cast<IXAPO *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG n = --refs_;
		if (n == 0) {
			delete this;
		}
		return n;
	}

	// IXAPO
	HRESULT STDMETHODCALLTYPE GetRegistrationProperties(XAPO_REGISTRATION_PROPERTIES **props) override {
		*props = static_cast<XAPO_REGISTRATION_PROPERTIES *>(XAPOAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
		if (!*props) {
			return E_OUTOFMEMORY;
		}
		**props = MUFFLE_PROPERTIES;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE IsInputFormatSupported(const WAVEFORMATEX *output, const WAVEFORMATEX *input,
			WAVEFORMATEX **suggested) override {
		return check_format(output, input, suggested);
	}
	HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(const WAVEFORMATEX *input, const WAVEFORMATEX *output,
			WAVEFORMATEX **suggested) override {
		return check_format(input, output, suggested);
	}
	HRESULT STDMETHODCALLTYPE Initialize(const void *, UINT32) override { return S_OK; }
	void STDMETHODCALLTYPE Reset() override { state_.fill(0.0f); }
	HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 in_count, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *in,
			UINT32 out_count, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *out) override {
		if (in_count != 1 || out_count != 1 || !is_float(in[0].pFormat) || !is_float(out[0].pFormat) ||
				in[0].pFormat->nChannels != out[0].pFormat->nChannels ||
				in[0].pFormat->nChannels > state_.size()) {
			return XAPO_E_FORMAT_UNSUPPORTED;
		}
		channels_ = in[0].pFormat->nChannels;
		a_ = sound::lowpass_coefficient(MUFFLE_HZ, in[0].pFormat->nSamplesPerSec);
		state_.fill(0.0f);
		return S_OK;
	}
	void STDMETHODCALLTYPE UnlockForProcess() override {}
	void STDMETHODCALLTYPE Process(UINT32, const XAPO_PROCESS_BUFFER_PARAMETERS *in, UINT32,
			XAPO_PROCESS_BUFFER_PARAMETERS *out, BOOL enabled) override {
		// In place: the output is the input buffer. Disabled, it passes through
		// (and the filter's state waits, as a filter nothing goes through would).
		out[0].BufferFlags = in[0].BufferFlags;
		out[0].ValidFrameCount = in[0].ValidFrameCount;
		if (!enabled) {
			return;
		}
		float *samples = static_cast<float *>(in[0].pBuffer);
		if (in[0].BufferFlags == XAPO_BUFFER_SILENT) { // (silence still lets the filter's tail out)
			std::fill(samples, samples + size_t(in[0].ValidFrameCount) * channels_, 0.0f);
			out[0].BufferFlags = XAPO_BUFFER_VALID;
		}
		sound::lowpass(samples, in[0].ValidFrameCount, int(channels_), a_, state_.data());
	}
	UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 frames) override { return frames; }
	UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 frames) override { return frames; }

private:
	// The same float format on both sides.
	static HRESULT check_format(const WAVEFORMATEX *other, const WAVEFORMATEX *asked, WAVEFORMATEX **suggested) {
		if (suggested) {
			*suggested = nullptr;
		}
		const bool ok = is_float(asked) && other && asked->nChannels == other->nChannels &&
				asked->nSamplesPerSec == other->nSamplesPerSec && asked->nChannels <= XAUDIO2_MAX_AUDIO_CHANNELS;
		return ok ? S_OK : XAPO_E_FORMAT_UNSUPPORTED;
	}

	std::atomic<ULONG> refs_{1};
	size_t channels_ = 2;
	float a_ = 0.0f;
	std::array<float, XAUDIO2_MAX_AUDIO_CHANNELS> state_{};
};

// --- Losing the device ---------------------------------------------------------------

// XAudio2 stops for good when its device goes (unplugged headphones); the
// default device changing (plugging them in) doesn't move it. Either way the
// next update() starts over on the default device.
struct Restart : IXAudio2EngineCallback, IMMNotificationClient {
	std::atomic<bool> needed{false};

	// IXAudio2EngineCallback
	void STDMETHODCALLTYPE OnProcessingPassStart() override {}
	void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
	void STDMETHODCALLTYPE OnCriticalError(HRESULT) override { needed = true; }

	// IMMNotificationClient (owned by Audio::Impl: no reference counting)
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
			*out = static_cast<IMMNotificationClient *>(this);
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
		if (flow == eRender && role == eConsole) {
			needed = true;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h, without its GUID library).
constexpr PROPERTYKEY DEVICE_NAME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

} // namespace

// A loaded sound file: its samples, which every voice of it plays from.
struct Sound {
	sound::Wav wav;
	WAVEFORMATEX format{};
};

struct Voice {
	IXAudio2SourceVoice *voice = nullptr;
	const Sound *sound = nullptr;
	float at[3] = {0, 0, 0};
};

struct Audio::Impl {
	bool com = false; // CoInitializeEx to undo
	Restart restart;
	IMMDeviceEnumerator *devices = nullptr;
	bool listening = false; // restart is registered with `devices`

	IXAudio2 *engine = nullptr;
	IXAudio2MasteringVoice *master = nullptr;
	IXAudio2SubmixVoice *all = nullptr; // every sound goes through it (stereo), muffled underwater
	bool ok = false;

	std::map<std::wstring, std::unique_ptr<Sound>> loaded; // (never moved: voices point into them)
	std::deque<Voice> voices;
	Voice day, night;
	std::filesystem::path day_path, night_path;

	float listener[3] = {0, 0, 0}, forward[3] = {0, 0, -1};
	float master_volume = 1, effects = 1, ambience = 1, daylight = 0;
	bool underwater = false;

	const Sound *load(const std::filesystem::path &path);
	IXAudio2SourceVoice *start_voice(const Sound &s, bool loop);
	void place(Voice &v) const; // its gains for where the ears are
	void start_engine();
	void stop_engine();
	void start_ambience();
	void apply_ambience();
};

const Sound *Audio::Impl::load(const std::filesystem::path &path) {
	auto &s = loaded[path.wstring()];
	if (!s) {
		auto loading = std::make_unique<Sound>();
		std::string error;
		if (!sound::read_wav(path, loading->wav, error)) {
			log("Can't play %s: %s", path.string().c_str(), error.c_str());
			loaded.erase(path.wstring());
			return nullptr;
		}
		const sound::Wav &w = loading->wav;
		WAVEFORMATEX &f = loading->format;
		f.wFormatTag = w.format == sound::Wav::FLOAT ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
		f.nChannels = w.channels;
		f.nSamplesPerSec = w.rate;
		f.wBitsPerSample = w.bits;
		f.nBlockAlign = WORD(w.channels * w.bits / 8);
		f.nAvgBytesPerSec = w.rate * f.nBlockAlign;
		s = std::move(loading);
	}
	return s.get();
}

IXAudio2SourceVoice *Audio::Impl::start_voice(const Sound &s, bool loop) {
	if (s.wav.data.empty()) {
		return nullptr;
	}
	XAUDIO2_SEND_DESCRIPTOR send = {0, all};
	const XAUDIO2_VOICE_SENDS sends = {1, &send};
	IXAudio2SourceVoice *v = nullptr;
	if (FAILED(engine->CreateSourceVoice(&v, &s.format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends))) {
		return nullptr;
	}
	XAUDIO2_BUFFER buffer{};
	buffer.Flags = XAUDIO2_END_OF_STREAM;
	buffer.AudioBytes = UINT32(s.wav.data.size());
	buffer.pAudioData = s.wav.data.data();
	buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
	if (FAILED(v->SubmitSourceBuffer(&buffer))) {
		v->DestroyVoice();
		return nullptr;
	}
	return v;
}

void Audio::Impl::place(Voice &v) const {
	float g[2];
	sound::stereo_gains(listener, forward, v.at, UNIT_SIZE, MAX_DISTANCE, g);
	// Mono: into both speakers; stereo: each side into its own.
	const float mono[2] = {g[0], g[1]};
	const float stereo[4] = {g[0], 0.0f, 0.0f, g[1]};
	v.voice->SetOutputMatrix(all, v.sound->wav.channels, 2, v.sound->wav.channels == 1 ? mono : stereo);
}

void Audio::Impl::start_engine() {
	ok = false;
	if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
		log("No sound (XAudio2 didn't start): playing without sound");
		engine = nullptr;
		return;
	}
	engine->RegisterForCallbacks(&restart);
	if (FAILED(engine->CreateMasteringVoice(&master))) {
		log("No sound device: playing without sound");
		stop_engine();
		return;
	}
	XAUDIO2_VOICE_DETAILS details{};
	master->GetVoiceDetails(&details);
	Muffle *muffle = new Muffle();
	XAUDIO2_EFFECT_DESCRIPTOR effect = {static_cast<IXAPO *>(muffle), FALSE, 2};
	const XAUDIO2_EFFECT_CHAIN chain = {1, &effect};
	const HRESULT hr = engine->CreateSubmixVoice(&all, 2, details.InputSampleRate, 0, 0, nullptr, &chain);
	muffle->Release(); // (the voice holds it)
	if (FAILED(hr)) {
		log("Can't mix sounds: playing without sound");
		stop_engine();
		return;
	}
	if (underwater) {
		all->EnableEffect(0);
	}
	master->SetVolume(master_volume);
	std::string name = "the default device";
	IMMDevice *device = nullptr;
	if (devices && SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
		IPropertyStore *props = nullptr;
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
			PROPVARIANT pv;
			PropVariantInit(&pv);
			if (SUCCEEDED(props->GetValue(DEVICE_NAME, &pv)) && pv.vt == VT_LPWSTR) {
				name = narrow(pv.pwszVal);
			}
			PropVariantClear(&pv);
			props->Release();
		}
		device->Release();
	}
	log("Sound: %s, %u Hz, %u channels", name.c_str(), details.InputSampleRate, details.InputChannels);
	ok = true;
}

void Audio::Impl::stop_engine() {
	ok = false;
	for (Voice &v : voices) {
		v.voice->DestroyVoice();
	}
	voices.clear();
	for (Voice *v : {&day, &night}) {
		if (v->voice) {
			v->voice->DestroyVoice();
			v->voice = nullptr;
		}
	}
	if (all) {
		all->DestroyVoice();
		all = nullptr;
	}
	if (master) {
		master->DestroyVoice();
		master = nullptr;
	}
	if (engine) {
		engine->UnregisterForCallbacks(&restart);
		engine->Release();
		engine = nullptr;
	}
}

void Audio::Impl::start_ambience() {
	for (auto [v, path] : {std::pair{&day, &day_path}, std::pair{&night, &night_path}}) {
		v->sound = path->empty() ? nullptr : load(*path);
		v->voice = v->sound ? start_voice(*v->sound, true) : nullptr;
		if (v->voice) {
			// Not placed: in both ears as it is.
			const float mono[2] = {1.0f, 1.0f};
			const float stereo[4] = {1.0f, 0.0f, 0.0f, 1.0f};
			v->voice->SetOutputMatrix(all, v->sound->wav.channels, 2, v->sound->wav.channels == 1 ? mono : stereo);
			v->voice->SetVolume(0.0f);
			v->voice->Start();
		}
	}
	apply_ambience();
}

void Audio::Impl::apply_ambience() {
	const float base = db_to_linear(AMBIENCE_DB) * ambience;
	if (day.voice) {
		day.voice->SetVolume(base * daylight);
	}
	if (night.voice) {
		night.voice->SetVolume(base * (1.0f - daylight));
	}
}

Audio::Audio() : impl_(std::make_unique<Impl>()) {}

Audio::~Audio() {
	Impl &a = *impl_;
	a.stop_engine();
	if (a.devices) {
		if (a.listening) {
			a.devices->UnregisterEndpointNotificationCallback(&a.restart);
		}
		a.devices->Release();
	}
	a.loaded.clear();
	if (a.com) {
		CoUninitialize();
	}
}

bool Audio::active() const {
	return impl_->ok;
}

std::filesystem::path Audio::pick(const std::string &set, const std::string &kind) {
	if (!content_) {
		return {};
	}
	auto it = content_->sound_sets.find(set);
	if (it == content_->sound_sets.end()) {
		return {};
	}
	const Data files = it->second.value(kind, Data::array());
	if (!files.is_array() || files.empty()) {
		return {};
	}
	const Data &file = files[std::uniform_int_distribution<size_t>(0, files.size() - 1)(random_)];
	return file.is_string() ? content_->asset_path(it->second, file.get<std::string>()) : std::filesystem::path();
}

int Audio::voices() const {
	return int(impl_->voices.size());
}

bool Audio::init(const Content &content, float master_volume) {
	content_ = &content;
	Impl &a = *impl_;
	const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	a.com = SUCCEEDED(co); // (RPC_E_CHANGED_MODE: COM is already up on this thread, as something else wanted it)
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				reinterpret_cast<void **>(&a.devices)))) {
		a.listening = SUCCEEDED(a.devices->RegisterEndpointNotificationCallback(&a.restart));
	} else {
		a.devices = nullptr;
	}
	a.master_volume = master_volume;
	// The ambience: two loops, faded against each other with the daylight.
	a.day_path = pick("core:ambience", "day");
	a.night_path = pick("core:ambience", "night");
	a.start_engine();
	if (!a.ok) {
		return false;
	}
	a.start_ambience();
	return true;
}

void Audio::play(const std::string &set, const std::string &kind, const float at[3], float volume_db) {
	Impl &a = *impl_;
	if (!a.ok) {
		return;
	}
	const float dx = at[0] - a.listener[0], dy = at[1] - a.listener[1], dz = at[2] - a.listener[2];
	if (dx * dx + dy * dy + dz * dz > MAX_DISTANCE * MAX_DISTANCE) {
		return; // out of earshot
	}
	const std::filesystem::path path = pick(set, kind);
	if (path.empty()) {
		return;
	}
	const Sound *s = a.load(path);
	if (!s) {
		return;
	}
	if (int(a.voices.size()) >= VOICES) { // the oldest makes room
		a.voices.front().voice->DestroyVoice();
		a.voices.pop_front();
	}
	Voice v;
	v.sound = s;
	std::copy(at, at + 3, v.at);
	v.voice = a.start_voice(*s, false);
	if (!v.voice) {
		return;
	}
	a.place(v);
	v.voice->SetFrequencyRatio(std::uniform_real_distribution<float>(0.9f, 1.1f)(random_));
	v.voice->SetVolume(db_to_linear(volume_db) * a.effects);
	v.voice->Start();
	a.voices.push_back(v);
}

void Audio::set_listener(const float position[3], const float forward[3]) {
	Impl &a = *impl_;
	std::copy(position, position + 3, a.listener);
	std::copy(forward, forward + 3, a.forward);
}

void Audio::update_ambience(float day) {
	Impl &a = *impl_;
	a.daylight = std::clamp(day, 0.0f, 1.0f);
	a.apply_ambience();
}

void Audio::set_volumes(float master, float effects, float ambience) {
	Impl &a = *impl_;
	a.effects = std::clamp(effects, 0.0f, 1.0f);
	a.ambience = std::clamp(ambience, 0.0f, 1.0f);
	master = std::clamp(master, 0.0f, 1.0f);
	if (master != a.master_volume) {
		a.master_volume = master;
		if (a.master) {
			a.master->SetVolume(master);
		}
	}
}

void Audio::set_underwater(bool underwater) {
	Impl &a = *impl_;
	if (underwater == a.underwater) {
		return;
	}
	a.underwater = underwater;
	if (a.all) {
		underwater ? a.all->EnableEffect(0) : a.all->DisableEffect(0);
	}
}

void Audio::update() {
	Impl &a = *impl_;
	if (a.restart.needed.exchange(false)) {
		log("The sound device changed: starting sound again");
		a.stop_engine();
		a.start_engine();
		if (a.ok) {
			a.start_ambience();
		}
	}
	if (!a.ok) {
		return;
	}
	for (auto it = a.voices.begin(); it != a.voices.end();) {
		XAUDIO2_VOICE_STATE state;
		it->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		if (state.BuffersQueued == 0) { // played to the end
			it->voice->DestroyVoice();
			it = a.voices.erase(it);
		} else {
			a.place(*it); // (the ears move)
			++it;
		}
	}
}

} // namespace gc
