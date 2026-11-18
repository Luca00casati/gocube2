#include "ui/settings_screen.h"

#include "data/lua_data.h"


#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace gc {

namespace fs = std::filesystem;

namespace {

uintmax_t folder_size(const fs::path &dir) {
	uintmax_t total = 0;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (it->is_regular_file(ec)) {
			total += it->file_size(ec);
		}
	}
	return total;
}

std::string human_size(uintmax_t bytes) {
	char out[32];
	if (bytes < 1024) {
		std::snprintf(out, sizeof(out), "%llu B", static_cast<unsigned long long>(bytes));
	} else if (bytes < (1u << 20)) {
		std::snprintf(out, sizeof(out), "%.1f KiB", double(bytes) / 1024.0);
	} else {
		std::snprintf(out, sizeof(out), "%.1f MiB", double(bytes) / double(1u << 20));
	}
	return out;
}

std::string utf8(const fs::path &p) {
	const std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

const ui::Color DIM = ui::rgba(255, 255, 255, 153);
const ui::Color PROBLEM = ui::rgba(255, 128, 115);

void heading(ui::Gui &gui, const char *text) {
	gui.space(6);
	gui.text(text, 1.25f);
}

// A row of the settings: what it is on the left, its control (next) on the right.
void row(ui::Gui &gui, std::string_view label, float column) {
	gui.align_text_to_frame();
	gui.text(label);
	gui.same_line(column);
	gui.next_width(-4);
}

} // namespace

std::string SettingsScreen::key_name(int vk) {
	switch (vk) { // (names GetKeyNameText gives oddly or not at all)
		case VK_SPACE:
			return "Space";
		case VK_SHIFT:
			return "Shift";
		case VK_CONTROL:
			return "Ctrl";
		case VK_MENU:
			return "Alt";
		default:
			break;
	}
	UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
	const bool extended = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR ||
			vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE ||
			vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_NUMLOCK;
	wchar_t name[64];
	const int n = GetKeyNameTextW(LONG(scan << 16 | (extended ? 1 << 24 : 0)), name, 64);
	if (n <= 0) {
		char out[16];
		std::snprintf(out, sizeof(out), "Key %d", vk);
		return out;
	}
	char out[256];
	const int m = WideCharToMultiByte(CP_UTF8, 0, name, n, out, sizeof(out) - 1, nullptr, nullptr);
	return std::string(out, size_t(std::max(0, m)));
}

void SettingsScreen::open(const ModFolders &folders, const Mods &mods, bool server_mods) {
	folders_ = folders;
	server_mods_ = server_mods;
	capturing_.clear();
	rows_.clear();
	other_problems_.clear();
	// Every mod found (the base game always loads), where it comes from, its
	// size and whether it loaded (or why not).
	std::vector<std::string> loaded;
	for (const ModInfo &m : mods.loaded()) {
		loaded.push_back(m.id);
	}
	std::vector<std::string> seen = {"core"};
	for (const auto &[entry, dir] : mod_copies({folders.builtin, folders.user})) {
		Data manifest;
		std::string why;
		read_lua_data(dir / "mod.lua", manifest, why);
		ModRow r;
		r.id = manifest.value("id", "");
		if (r.id.empty() || r.id == "core") {
			continue;
		}
		seen.push_back(r.id);
		r.label = manifest.value("name", r.id) + " " + manifest.value("version", "");
		r.where = (dir.parent_path() == folders.builtin ? "built in" : "your mods folder") + std::string(" · ") +
				human_size(folder_size(dir));
		const bool is_loaded = std::find(loaded.begin(), loaded.end(), r.id) != loaded.end();
		r.status = is_loaded ? "loaded" : server_mods ? "not used with this server" : "didn't load";
		if (auto it = mods.problems().find(r.id); it != mods.problems().end()) {
			r.problem = it->second;
		}
		rows_.push_back(std::move(r));
	}
	for (const auto &[id, why] : mods.problems()) {
		if (std::find(seen.begin(), seen.end(), id) == seen.end()) {
			other_problems_.push_back(id + ": " + why); // (a folder that couldn't be read)
		}
	}
	cache_count_ = 0;
	cache_bytes_ = 0;
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(folders.downloaded, ec)) {
		if (e.is_directory(ec)) {
			cache_count_++;
			cache_bytes_ += folder_size(e.path());
		}
	}
}

bool SettingsScreen::draw(ui::Gui &gui, Settings &s, const Window &window, bool esc_pressed, const fs::path &file) {
	// Waiting for a key: the next one pressed (Esc cancels).
	bool keep_open = true;
	if (!capturing_.empty()) {
		for (int vk = 8; vk < 255; vk++) {
			const bool down = window.key_down(vk);
			const bool pressed = down && !was_down_[vk];
			was_down_[vk] = down;
			if (!pressed || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
					vk == VK_LMENU || vk == VK_RMENU) {
				continue; // (the side-less codes come too)
			}
			if (vk != VK_ESCAPE) {
				s.keys[capturing_] = vk;
			}
			capturing_.clear();
			break;
		}
	} else if (esc_pressed) {
		keep_open = false;
	}

	const ui::Input &in = gui.input();
	gui.background().fill({0, 0, in.width, in.height}, ui::rgba(0, 0, 0, 150));
	ui::WindowOptions o;
	o.pos = {in.width * 0.5f, in.height * 0.5f};
	o.pivot = {0.5f, 0.5f};
	o.size = {std::min(680.0f, in.width - 40), std::min(660.0f, in.height - 40)};
	gui.begin_window("Settings##screen", o);
	gui.set_cursor_x((gui.window_width() - gui.text_size("Settings", 1.6f).x) * 0.5f - gui.theme.padding.x);
	gui.text("Settings", 1.6f);
	gui.begin_child("settings", {0, -gui.frame_height() - gui.theme.spacing.y - 6}, false);
	const float column = std::floor(gui.available_width() * 0.4f);

	char label[96];
	// View distance.
	std::snprintf(label, sizeof(label), "View distance: %d blocks", s.view_distance);
	row(gui, label, column);
	int view = 0;
	for (int i = 0; i < int(std::size(Settings::VIEW_DISTANCES)); i++) {
		view = Settings::VIEW_DISTANCES[i] <= s.view_distance ? i : view;
	}
	if (gui.slider("##view", view, 0, int(std::size(Settings::VIEW_DISTANCES)) - 1)) {
		s.view_distance = Settings::VIEW_DISTANCES[view];
	}
	// Mouse sensitivity.
	std::snprintf(label, sizeof(label), "Mouse sensitivity: %.1f", s.mouse_sensitivity * 1000.0f);
	row(gui, label, column);
	float sens = s.mouse_sensitivity * 1000.0f;
	if (gui.slider("##sens", sens, 0.5f, 10.0f)) {
		s.mouse_sensitivity = std::round(sens * 10.0f) / 10000.0f;
	}
	// Frame rate limit.
	row(gui, "Frame rate limit", column);
	{
		std::vector<std::string> names;
		int at = 0;
		for (int i = 0; i < int(std::size(Settings::FPS_LIMITS)); i++) {
			const int limit = Settings::FPS_LIMITS[i];
			names.push_back(limit == 0 ? std::string("Unlimited") : std::to_string(limit) + " FPS");
			at = limit == s.max_fps ? i : at;
		}
		if (gui.combo("##fps", at, names)) {
			s.max_fps = Settings::FPS_LIMITS[at];
		}
	}
	row(gui, "VSync", column);
	gui.checkbox("Wait for the screen (no tearing)", s.vsync);
	row(gui, "Smooth lighting", column);
	gui.checkbox("Blend light at block corners", s.smooth_lighting);
	row(gui, "Sun shadows", column);
	{
		std::vector<std::string> names;
		int at = 0;
		for (int i = 0; i < int(std::size(Settings::SHADOW_MODES)); i++) {
			const std::string m = Settings::SHADOW_MODES[i];
			names.push_back(m == "blocks" ? "Blocks (whole faces, cheap)" : "Off");
			at = s.shadow_mode == m ? i : at;
		}
		if (gui.combo("##shadows", at, names)) {
			s.shadow_mode = Settings::SHADOW_MODES[at];
		}
	}
	row(gui, "Render scale", column);
	{
		std::vector<std::string> names;
		int at = 0;
		for (int i = 0; i < int(std::size(Settings::RENDER_SCALES)); i++) {
			const float f = Settings::RENDER_SCALES[i];
			names.push_back(std::to_string(int(std::lround(f * 100))) + "%" + (f >= 1.0f ? " (sharpest)" : ""));
			at = std::abs(f - s.render_scale) < 0.001f ? i : at;
		}
		if (gui.combo("##scale", at, names)) {
			s.render_scale = Settings::RENDER_SCALES[at];
		}
		if (gui.item_hovered()) {
			gui.tooltip("Draws the world at a lower resolution and stretches it: blurrier, but much less GPU.");
		}
	}
	struct Volume {
		const char *name;
		float *value;
	};
	for (const Volume &v : {Volume{"Master volume", &s.master_volume}, Volume{"Effects volume", &s.effects_volume},
				 Volume{"Ambience volume", &s.ambience_volume}}) {
		std::snprintf(label, sizeof(label), "%s: %d%%", v.name, int(std::lround(*v.value * 100)));
		row(gui, label, column);
		int percent = int(std::lround(*v.value * 20)) * 5;
		gui.push_id(v.name);
		if (gui.slider("##v", percent, 0, 100)) {
			*v.value = float(percent / 5 * 5) / 100.0f;
		}
		gui.pop_id();
	}
	row(gui, "Clock", column);
	gui.checkbox("24-hour clock (07:19 instead of 7:19 AM)", s.clock_24h);

	// Key bindings.
	heading(gui, "Keys (click, then press a key; Esc cancels)");
	for (const Settings::Action &a : Settings::actions()) {
		row(gui, a.label, column);
		const int k = s.key(a.id);
		std::string text = capturing_ == a.id ? "Press a key..." : key_name(k);
		if (capturing_ != a.id && s.actions_using(k).size() > 1) {
			text += "  (also used!)";
		}
		gui.push_id(a.id);
		if (gui.button(text + "##key", {200, 0})) {
			capturing_ = a.id;
			for (int vk = 0; vk < 256; vk++) {
				was_down_[vk] = window.key_down(vk); // (the click itself isn't a key)
			}
		}
		gui.pop_id();
	}
	gui.text_wrapped("Mouse: look, hold left = mine, right = place, wheel/1-9 = hotbar · Esc = pause · F3 = details",
			DIM);

	// Mods.
	heading(gui, "Mods");
	if (gui.button("Open the mods folder")) {
		std::error_code ec;
		fs::create_directories(folders_.user, ec);
		ShellExecuteW(nullptr, L"explore", folders_.user.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	if (rows_.empty()) {
		gui.text_wrapped("No mods installed. Put mod folders in the mods folder (" + utf8(folders_.user) + ").", DIM);
	}
	for (const ModRow &r : rows_) {
		bool on = std::find(s.disabled_mods.begin(), s.disabled_mods.end(), r.id) == s.disabled_mods.end();
		gui.push_id(r.id);
		if (gui.checkbox(r.label + "##on", on)) {
			std::erase(s.disabled_mods, r.id);
			if (!on) {
				s.disabled_mods.push_back(r.id);
			}
		}
		if (gui.item_hovered()) {
			gui.tooltip(r.id);
		}
		gui.pop_id();
		const bool off = std::find(s.disabled_mods.begin(), s.disabled_mods.end(), r.id) != s.disabled_mods.end();
		gui.text_wrapped(r.where + " · " + (off && r.status != "loaded" ? "turned off" : r.status), DIM);
		if (!r.problem.empty()) {
			gui.text_wrapped(r.problem, PROBLEM);
		}
	}
	for (const std::string &p : other_problems_) {
		gui.text_wrapped(p, PROBLEM);
	}
	gui.text_wrapped("Turning mods on or off takes effect the next time the game starts.", DIM);
	gui.text("Downloaded from servers");
	gui.align_text_to_frame();
	gui.text(cache_count_ ? std::to_string(cache_count_) + " mod(s), " + human_size(cache_bytes_) : std::string("None"),
			DIM);
	gui.same_line();
	gui.begin_disabled(cache_count_ == 0 || server_mods_);
	if (gui.button("Delete them")) {
		std::error_code ec;
		fs::remove_all(folders_.downloaded, ec);
		if (!ec) {
			cache_count_ = 0;
			cache_bytes_ = 0;
		}
	}
	gui.end_disabled();
	if (server_mods_ && gui.item_hovered_even_disabled()) {
		gui.tooltip("In use: you're playing with a server's mods.");
	}
	gui.text_wrapped("A server's mods are downloaded again if you join it later.", DIM);
	gui.checkbox("Servers' mods may run code (Lua, in the sandbox)", s.server_mod_code);
	if (gui.item_hovered()) {
		gui.tooltip("Off: you only play with servers whose mods are data (content and command files), "
					"so nothing from a server runs on your computer.");
	}
	gui.end_child();

	// Reset keys, Done.
	gui.set_cursor_x(gui.window_width() - 2 * gui.theme.padding.x - 228);
	if (gui.button("Reset keys", {110, 0})) {
		s.reset_keys();
	}
	gui.same_line();
	if (gui.button("Done", {110, 0})) {
		keep_open = false;
	}
	gui.end_window();
	if (!keep_open) {
		s.save(file);
	}
	return keep_open;
}

} // namespace gc
