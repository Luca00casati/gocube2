#include "ui/settings_screen.h"

#include "data/lua_data.h"

#include <imgui.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

void dim(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.6f));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

void problem(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.45f, 1));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

void heading(const char *text) {
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::SetWindowFontScale(1.25f);
	ImGui::TextUnformatted(text);
	ImGui::SetWindowFontScale(1.0f);
}

// A row of the settings table: what it is on the left, its control on the right.
void row(const char *label) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::TableNextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
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

bool SettingsScreen::draw(Settings &s, const Window &window, bool esc_pressed, const fs::path &file) {
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

	const ImGuiIO &io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(600.0f, io.DisplaySize.x - 40), std::min(660.0f, io.DisplaySize.y - 40)));
	ImGui::Begin("Settings##screen", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
	ImGui::SetWindowFontScale(1.6f);
	const float title_w = ImGui::CalcTextSize("Settings").x;
	ImGui::SetCursorPosX((ImGui::GetWindowWidth() - title_w) * 0.5f);
	ImGui::TextUnformatted("Settings");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::BeginChild("settings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6), ImGuiChildFlags_None);

	if (ImGui::BeginTable("main", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		char label[96];
		// View distance.
		std::snprintf(label, sizeof(label), "View distance: %d blocks", s.view_distance);
		row(label);
		int view = 0;
		for (int i = 0; i < int(std::size(Settings::VIEW_DISTANCES)); i++) {
			view = Settings::VIEW_DISTANCES[i] <= s.view_distance ? i : view;
		}
		if (ImGui::SliderInt("##view", &view, 0, int(std::size(Settings::VIEW_DISTANCES)) - 1, "")) {
			s.view_distance = Settings::VIEW_DISTANCES[view];
		}
		// Mouse sensitivity.
		std::snprintf(label, sizeof(label), "Mouse sensitivity: %.1f", s.mouse_sensitivity * 1000.0f);
		row(label);
		float sens = s.mouse_sensitivity * 1000.0f;
		if (ImGui::SliderFloat("##sens", &sens, 0.5f, 10.0f, "")) {
			s.mouse_sensitivity = std::round(sens * 10.0f) / 10000.0f;
		}
		// Frame rate limit.
		row("Frame rate limit");
		auto fps_name = [](int limit) { return limit == 0 ? std::string("Unlimited") : std::to_string(limit) + " FPS"; };
		if (ImGui::BeginCombo("##fps", fps_name(s.max_fps).c_str())) {
			for (int limit : Settings::FPS_LIMITS) {
				if (ImGui::Selectable(fps_name(limit).c_str(), limit == s.max_fps)) {
					s.max_fps = limit;
				}
			}
			ImGui::EndCombo();
		}
		row("VSync");
		ImGui::Checkbox("Wait for the screen (no tearing)", &s.vsync);
		row("Smooth lighting");
		ImGui::Checkbox("Blend light at block corners", &s.smooth_lighting);
		row("Sun shadows");
		auto shadow_name = [](const std::string &m) { return m == "blocks" ? "Blocks (whole faces, cheap)" : "Off"; };
		if (ImGui::BeginCombo("##shadows", shadow_name(s.shadow_mode))) {
			for (const char *m : Settings::SHADOW_MODES) {
				if (ImGui::Selectable(shadow_name(m), s.shadow_mode == m)) {
					s.shadow_mode = m;
				}
			}
			ImGui::EndCombo();
		}
		row("Render scale");
		auto scale_name = [](float f) {
			return std::to_string(int(std::lround(f * 100))) + "%" + (f >= 1.0f ? " (sharpest)" : "");
		};
		if (ImGui::BeginCombo("##scale", scale_name(s.render_scale).c_str())) {
			for (float f : Settings::RENDER_SCALES) {
				if (ImGui::Selectable(scale_name(f).c_str(), std::abs(f - s.render_scale) < 0.001f)) {
					s.render_scale = f;
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Draws the world at a lower resolution and stretches it: blurrier, but much less GPU.");
		}
		struct Volume {
			const char *name;
			float *value;
		};
		for (const Volume &v : {Volume{"Master volume", &s.master_volume}, Volume{"Effects volume", &s.effects_volume},
					 Volume{"Ambience volume", &s.ambience_volume}}) {
			std::snprintf(label, sizeof(label), "%s: %d%%", v.name, int(std::lround(*v.value * 100)));
			row(label);
			int percent = int(std::lround(*v.value * 20)) * 5;
			ImGui::PushID(v.name);
			if (ImGui::SliderInt("##v", &percent, 0, 100, "", ImGuiSliderFlags_AlwaysClamp)) {
				*v.value = float(percent / 5 * 5) / 100.0f;
			}
			ImGui::PopID();
		}
		row("Clock");
		ImGui::Checkbox("24-hour clock (07:19 instead of 7:19 AM)", &s.clock_24h);
		ImGui::EndTable();
	}

	// Key bindings.
	heading("Keys (click, then press a key; Esc cancels)");
	if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp)) {
		for (const Settings::Action &a : Settings::actions()) {
			row(a.label);
			const int k = s.key(a.id);
			std::string text = capturing_ == a.id ? "Press a key..." : key_name(k);
			if (capturing_ != a.id && s.actions_using(k).size() > 1) {
				text += "  (also used!)";
			}
			ImGui::PushID(a.id);
			if (ImGui::Button(text.c_str(), ImVec2(200, 0))) {
				capturing_ = a.id;
				for (int vk = 0; vk < 256; vk++) {
					was_down_[vk] = window.key_down(vk); // (the click itself isn't a key)
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	dim("Mouse: look, hold left = mine, right = place, wheel/1-9 = hotbar · Esc = pause · F3 = details");

	// Mods.
	heading("Mods");
	if (ImGui::Button("Open the mods folder")) {
		std::error_code ec;
		fs::create_directories(folders_.user, ec);
		ShellExecuteW(nullptr, L"explore", folders_.user.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	if (rows_.empty()) {
		dim("No mods installed. Put mod folders in the mods folder (" + utf8(folders_.user) + ").");
	}
	for (const ModRow &r : rows_) {
		bool on = std::find(s.disabled_mods.begin(), s.disabled_mods.end(), r.id) == s.disabled_mods.end();
		ImGui::PushID(r.id.c_str());
		if (ImGui::Checkbox(r.label.c_str(), &on)) {
			std::erase(s.disabled_mods, r.id);
			if (!on) {
				s.disabled_mods.push_back(r.id);
			}
		}
		ImGui::PopID();
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", r.id.c_str());
		}
		ImGui::SameLine();
		const bool off = std::find(s.disabled_mods.begin(), s.disabled_mods.end(), r.id) != s.disabled_mods.end();
		dim(r.where + " · " + (off && r.status != "loaded" ? "turned off" : r.status));
		if (!r.problem.empty()) {
			problem(r.problem);
		}
	}
	for (const std::string &p : other_problems_) {
		problem(p);
	}
	dim("Turning mods on or off takes effect the next time the game starts.");
	ImGui::TextUnformatted("Downloaded from servers");
	dim(cache_count_ ? std::to_string(cache_count_) + " mod(s), " + human_size(cache_bytes_) : std::string("None"));
	ImGui::SameLine();
	ImGui::BeginDisabled(cache_count_ == 0 || server_mods_);
	if (ImGui::Button("Delete them")) {
		std::error_code ec;
		fs::remove_all(folders_.downloaded, ec);
		if (!ec) {
			cache_count_ = 0;
			cache_bytes_ = 0;
		}
	}
	ImGui::EndDisabled();
	if (server_mods_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("In use: you're playing with a server's mods.");
	}
	dim("A server's mods are downloaded again if you join it later.");
	ImGui::EndChild();

	// Reset keys, Done.
	ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 240);
	if (ImGui::Button("Reset keys", ImVec2(110, 0))) {
		s.reset_keys();
	}
	ImGui::SameLine();
	if (ImGui::Button("Done", ImVec2(110, 0))) {
		keep_open = false;
	}
	ImGui::End();
	if (!keep_open) {
		s.save(file);
	}
	return keep_open;
}

} // namespace gc
