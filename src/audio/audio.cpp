#include "audio/audio.h"

#include "log.h"

#include "miniaudio.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

namespace gc {

struct Audio::Impl {
	ma_engine engine{};
	bool ok = false;
	ma_sound_group all{};    // every sound goes through it ...
	bool all_ok = false;
	ma_lpf_node muffle{};    // ... and through this underwater
	bool muffle_ok = false;
	bool underwater = false;
	std::deque<std::unique_ptr<ma_sound>> voices;
	// One sound per file, never played: it keeps the file's decoded samples,
	// which every voice of it shares (ma_sound_init_copy).
	std::map<std::wstring, std::unique_ptr<ma_sound>> loaded;
	ma_sound day{}, night{};
	bool day_ok = false, night_ok = false;
	float listener[3] = {0, 0, 0};
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {}

Audio::~Audio() {
	Impl &a = *impl_;
	for (auto &v : a.voices) {
		ma_sound_uninit(v.get());
	}
	a.voices.clear();
	for (auto &[path, s] : a.loaded) {
		ma_sound_uninit(s.get());
	}
	a.loaded.clear();
	if (a.day_ok) {
		ma_sound_uninit(&a.day);
	}
	if (a.night_ok) {
		ma_sound_uninit(&a.night);
	}
	if (a.all_ok) {
		ma_sound_group_uninit(&a.all);
	}
	if (a.muffle_ok) {
		ma_lpf_node_uninit(&a.muffle, nullptr);
	}
	if (a.ok) {
		ma_engine_uninit(&a.engine);
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
	const Json files = it->second.value(kind, Json::array());
	if (!files.is_array() || files.empty()) {
		return {};
	}
	const Json &file = files[std::uniform_int_distribution<size_t>(0, files.size() - 1)(random_)];
	return file.is_string() ? content_->asset_path(it->second, file.get<std::string>()) : std::filesystem::path();
}

int Audio::voices() const {
	return int(impl_->voices.size());
}

bool Audio::init(const Content &content, float master_volume) {
	content_ = &content;
	Impl &a = *impl_;
	if (ma_engine_init(nullptr, &a.engine) != MA_SUCCESS) {
		log("No sound device: playing without sound");
		return false;
	}
	a.ok = true;
	ma_engine_set_volume(&a.engine, master_volume);
	if (ma_device *device = ma_engine_get_device(&a.engine)) {
		log("Sound: %s, %u Hz", device->playback.name, ma_engine_get_sample_rate(&a.engine));
	}
	// Underwater muffling: a low-pass filter (the Godot version's, 700 Hz,
	// 6 dB an octave) every sound can be routed through.
	const ma_uint32 channels = ma_engine_get_channels(&a.engine), rate = ma_engine_get_sample_rate(&a.engine);
	const ma_lpf_node_config lc = ma_lpf_node_config_init(channels, rate, 700.0, 1);
	a.muffle_ok = ma_lpf_node_init(ma_engine_get_node_graph(&a.engine), &lc, nullptr, &a.muffle) == MA_SUCCESS;
	if (a.muffle_ok) {
		ma_node_attach_output_bus(&a.muffle, 0, ma_engine_get_endpoint(&a.engine), 0);
	}
	a.all_ok = ma_sound_group_init(&a.engine, 0, nullptr, &a.all) == MA_SUCCESS;

	// The ambience: two loops, faded against each other with the daylight.
	auto ambience = [&](const char *kind, ma_sound &s) {
		const std::filesystem::path path = pick("core:ambience", kind);
		if (path.empty() || ma_sound_init_from_file_w(&a.engine, path.c_str(), MA_SOUND_FLAG_DECODE,
									a.all_ok ? &a.all : nullptr, nullptr, &s) != MA_SUCCESS) {
			return false;
		}
		ma_sound_set_spatialization_enabled(&s, MA_FALSE);
		ma_sound_set_looping(&s, MA_TRUE);
		ma_sound_set_volume(&s, 0.0f);
		ma_sound_start(&s);
		return true;
	};
	a.day_ok = ambience("day", a.day);
	a.night_ok = ambience("night", a.night);
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
	if (int(a.voices.size()) >= VOICES) { // the oldest makes room
		ma_sound_uninit(a.voices.front().get());
		a.voices.pop_front();
	}
	auto &source = a.loaded[path.wstring()];
	if (!source) {
		source = std::make_unique<ma_sound>();
		if (ma_sound_init_from_file_w(&a.engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, source.get()) !=
				MA_SUCCESS) {
			log("Can't play %s", path.string().c_str());
			source.reset();
			a.loaded.erase(path.wstring());
			return;
		}
	}
	auto s = std::make_unique<ma_sound>();
	if (ma_sound_init_copy(&a.engine, source.get(), 0, a.all_ok ? &a.all : nullptr, s.get()) != MA_SUCCESS) {
		return;
	}
	ma_sound_set_position(s.get(), at[0], at[1], at[2]);
	ma_sound_set_attenuation_model(s.get(), ma_attenuation_model_inverse);
	ma_sound_set_min_distance(s.get(), UNIT_SIZE);
	ma_sound_set_max_distance(s.get(), MAX_DISTANCE);
	ma_sound_set_rolloff(s.get(), 1.0f);
	ma_sound_set_pitch(s.get(), std::uniform_real_distribution<float>(0.9f, 1.1f)(random_));
	ma_sound_set_volume(s.get(), ma_volume_db_to_linear(volume_db));
	ma_sound_start(s.get());
	a.voices.push_back(std::move(s));
}

void Audio::set_listener(const float position[3], const float forward[3]) {
	Impl &a = *impl_;
	std::copy(position, position + 3, a.listener);
	if (!a.ok) {
		return;
	}
	ma_engine_listener_set_position(&a.engine, 0, position[0], position[1], position[2]);
	ma_engine_listener_set_direction(&a.engine, 0, forward[0], forward[1], forward[2]);
}

void Audio::update_ambience(float day) {
	Impl &a = *impl_;
	const float base = ma_volume_db_to_linear(AMBIENCE_DB);
	day = std::clamp(day, 0.0f, 1.0f);
	if (a.day_ok) {
		ma_sound_set_volume(&a.day, base * day);
	}
	if (a.night_ok) {
		ma_sound_set_volume(&a.night, base * (1.0f - day));
	}
}

void Audio::set_underwater(bool underwater) {
	Impl &a = *impl_;
	if (!a.ok || !a.all_ok || underwater == a.underwater) {
		return;
	}
	a.underwater = underwater;
	ma_node *to = underwater && a.muffle_ok ? reinterpret_cast<ma_node *>(&a.muffle)
											: reinterpret_cast<ma_node *>(ma_engine_get_endpoint(&a.engine));
	ma_node_attach_output_bus(&a.all, 0, to, 0);
}

void Audio::update() {
	Impl &a = *impl_;
	for (auto it = a.voices.begin(); it != a.voices.end();) {
		if (ma_sound_at_end(it->get())) {
			ma_sound_uninit(it->get());
			it = a.voices.erase(it);
		} else {
			++it;
		}
	}
}

} // namespace gc
