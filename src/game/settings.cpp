#include "game/settings.h"

#include "data/lua_data.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace gc {

const std::vector<Settings::Action> &Settings::actions() {
	static const std::vector<Action> list = {
			{"move_forward", "Move forward", 'W'},
			{"move_back", "Move back", 'S'},
			{"move_left", "Move left", 'A'},
			{"move_right", "Move right", 'D'},
			{"jump", "Jump / swim up / fly up", 0x20},  // space
			{"sneak", "Sneak / fly down", 0x10},       // shift
			{"sprint", "Sprint", 0x11},                // ctrl
			{"toggle_fly", "Toggle flying", 'F'},
			{"inventory", "Inventory", 'E'},
			{"throw", "Throw (Ctrl: the stack)", 'Q'},
	};
	return list;
}

Settings::Settings() {
	reset_keys();
}

int Settings::key(const std::string &action) const {
	auto it = keys.find(action);
	return it == keys.end() ? 0 : it->second;
}

void Settings::reset_keys() {
	keys.clear();
	for (const Action &a : actions()) {
		keys[a.id] = a.key;
	}
}

std::vector<std::string> Settings::actions_using(int k) const {
	std::vector<std::string> out;
	for (const auto &[action, bound] : keys) {
		if (bound == k) {
			out.push_back(action);
		}
	}
	return out;
}

bool Settings::load(const std::filesystem::path &path, std::string *error) {
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		return true; // (defaults)
	}
	Data d;
	std::string why;
	if (!read_lua_data(path, d, why) || !d.is_object()) {
		if (error) {
			*error = why.empty() ? "it isn't a table of settings" : why;
		}
		return false;
	}
	auto number = [&](const char *name, double lo, double hi, auto &out) {
		if (auto it = d.find(name); it != d.end() && it->is_number() && std::isfinite(it->get<double>())) {
			out = static_cast<std::remove_reference_t<decltype(out)>>(std::clamp(it->get<double>(), lo, hi));
		}
	};
	auto boolean = [&](const char *name, bool &out) {
		if (auto it = d.find(name); it != d.end() && it->is_boolean()) {
			out = it->get<bool>();
		}
	};
	auto text = [&](const char *name, std::string &out, size_t max) {
		if (auto it = d.find(name); it != d.end() && it->is_string() && it->get<std::string>().size() <= max) {
			out = it->get<std::string>();
		}
	};
	number("view_distance", 16, 512, view_distance);
	number("mouse_sensitivity", 0.0005, 0.01, mouse_sensitivity);
	number("max_fps", 0, 1000, max_fps);
	boolean("vsync", vsync);
	boolean("server_mod_code", server_mod_code);
	boolean("smooth_lighting", smooth_lighting);
	text("shadow_mode", shadow_mode, 16);
	if (std::none_of(std::begin(SHADOW_MODES), std::end(SHADOW_MODES), [&](const char *m) { return shadow_mode == m; })) {
		shadow_mode = "blocks"; //
	}
	number("render_scale", 0.25, 1.0, render_scale);
	boolean("clock_24h", clock_24h);
	number("worker_threads", 0, 64, worker_threads);
	number("master_volume", 0, 1, master_volume);
	number("effects_volume", 0, 1, effects_volume);
	number("ambience_volume", 0, 1, ambience_volume);
	if (auto it = d.find("keys"); it != d.end() && it->is_object()) {
		for (const Action &a : actions()) {
			if (auto k = it->find(a.id); k != it->end() && k->is_number_integer() && k->get<int>() > 0 && k->get<int>() < 256) {
				keys[a.id] = k->get<int>();
			}
		}
	}
	if (auto it = d.find("disabled_mods"); it != d.end() && it->is_array()) {
		disabled_mods.clear();
		for (const Data &id : *it) {
			if (id.is_string()) {
				disabled_mods.push_back(id.get<std::string>());
			}
		}
	}
	text("player_name", player_name, 64);
	text("last_address", last_address, 128);
	return true;
}

bool Settings::save(const std::filesystem::path &path) const {
	Data keys_data = Data::object();
	for (const auto &[action, k] : keys) {
		keys_data[action] = k;
	}
	const Data d = {{"view_distance", view_distance}, {"mouse_sensitivity", mouse_sensitivity}, {"max_fps", max_fps},
			{"vsync", vsync}, {"server_mod_code", server_mod_code}, {"smooth_lighting", smooth_lighting}, {"shadow_mode", shadow_mode},
			{"render_scale", render_scale}, {"clock_24h", clock_24h}, {"worker_threads", worker_threads},
			{"master_volume", master_volume}, {"effects_volume", effects_volume}, {"ambience_volume", ambience_volume},
			{"keys", keys_data}, {"disabled_mods", disabled_mods}, {"player_name", player_name},
			{"last_address", last_address}};
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	return save_lua_data(path, d);
}

} // namespace gc
