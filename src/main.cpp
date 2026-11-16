// GoCube: a voxel sandbox. The title menu lists the worlds (play, create,
// delete); a world is a folder with world.lua (a Lua data table); content and code
// come from mods (Lua, sandboxed), the base game ("core") first. Esc pauses.
// Started with --world, --world-dir or a testing option, it goes straight
// into that world.
//
// Controls: click to look around with the mouse (Esc lets go). W A S D walk,
// Space jumps (swims up, flies up), Shift sneaks (flies down), Ctrl sprints,
// F flies. Left click breaks, right click places (or opens a door; sneak to
// place against it), the wheel picks the block to place, middle click picks
// the block aimed at.
//
// Options:
//   --world=NAME     play the saved world NAME (in %APPDATA%\GoCube\worlds;
//                    a new one is made if it doesn't exist)
// Mods are found in mods\ next to the game (built-in, the base game first) and
// in %APPDATA%\GoCube\mods (one folder per mod).
//
//   --world-dir=DIR  play the world in this folder
//                    Without either, the world isn't saved.
//   --seed=N         seed for a new world (default 777)
//   --generator=N    world generator version for an unsaved world (default: the newest)
//   --view=N         view distance in blocks (default 128)
//   --pos=X,Y,Z      start flying at this position (feet)
//   --look=YAW,PITCH start view direction in degrees (0, 0: towards -z)
//   --warp           render on WARP, Windows' software adapter (no GPU needed)
//   --hidden         don't show the window (tests)
//   --frames=N       quit after N frames; the exit code is 1 if the debug layer
//                    reported errors or warnings. With --screenshot, counted
//                    from when the world around has finished loading.
//   --screenshot=F   save the last frame as a PNG
//   --render-scale=F draw the world at this part of the window's resolution
//                    (0.25..1, stretched over it; the Godot version offers
//                    1, 0.85, 0.75 and 0.5)
//   --no-vsync       present as fast as possible
//   --debug / --no-debug   force the Direct3D 11 debug layer on or off
//                    (default: on in debug builds)

#include "content/content.h"
#include "data/lua_data.h"
#include "game/blocks.h"
#include "game/day_cycle.h"
#include "game/game.h"
#include "game/hand.h"
#include "game/player.h"
#include "log.h"
#include "mods/mods.h"
#include "platform/window.h"
#include "render/chunk_renderer.h"
#include "render/renderer.h"
#include "ui/screens.h"
#include "world/save.h"
#include "world/world.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

struct Options {
	gc::RendererOptions renderer;
	bool hidden = false;
	long frames = 0; // 0: until the window is closed
	std::string screenshot;
	std::string world_name;
	std::filesystem::path world_dir;
	uint32_t seed = 777;
	int generator = gc::GENERATOR_VERSION;
	int view = 128;
	float render_scale = 1.0f;
	bool has_pos = false;
	float pos[3] = {0, 0, 0};
	bool has_look = false;
	float look[2] = {0, 0};
	std::string mode = "survival"; // for a new world
	std::vector<std::string> drop; // items dropped in front of the player at the start
	std::string screen;            // a screen open at the start ("inventory")
	double time = -1;              // time of day to start at (0..1; -1: the world's)
	bool menu = false;             // the title menu even with testing options (--frames, --screenshot: of it)
};

std::filesystem::path utf8_path(const char *s) {
	return std::filesystem::path(reinterpret_cast<const char8_t *>(s));
}

std::string utf8_string(const std::filesystem::path &p) {
	const std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

Options parse(int argc, char **argv) {
	Options o;
#ifdef GOCUBE_DEBUG
	o.renderer.debug = true;
#endif
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		auto value = [&](const char *name) -> const char * {
			const size_t n = strlen(name);
			return strncmp(a, name, n) == 0 ? a + n : nullptr;
		};
		const char *v = nullptr;
		if (!strcmp(a, "--warp")) {
			o.renderer.warp = true;
		} else if (!strcmp(a, "--hidden")) {
			o.hidden = true;
		} else if ((v = value("--frames="))) {
			o.frames = strtol(v, nullptr, 10);
		} else if ((v = value("--screenshot="))) {
			o.screenshot = v;
		} else if ((v = value("--world="))) {
			o.world_name = v;
		} else if ((v = value("--world-dir="))) {
			o.world_dir = utf8_path(v);
		} else if ((v = value("--seed="))) {
			o.seed = uint32_t(strtoll(v, nullptr, 10));
		} else if ((v = value("--generator="))) {
			o.generator = int(strtol(v, nullptr, 10));
		} else if ((v = value("--drop="))) {
			std::string list = v;
			for (size_t from = 0, to; from <= list.size(); from = to + 1) {
				to = list.find(',', from);
				to = to == std::string::npos ? list.size() : to;
				if (to > from) {
					o.drop.push_back(list.substr(from, to - from));
				}
			}
		} else if (!strcmp(a, "--menu")) {
			o.menu = true;
		} else if ((v = value("--time="))) {
			o.time = strtod(v, nullptr);
		} else if ((v = value("--screen="))) {
			o.screen = v;
		} else if ((v = value("--mode="))) {
			o.mode = v;
		} else if ((v = value("--view="))) {
			o.view = int(strtol(v, nullptr, 10));
		} else if ((v = value("--render-scale="))) {
			o.render_scale = strtof(v, nullptr);
		} else if ((v = value("--pos="))) {
			o.has_pos = sscanf_s(v, "%f,%f,%f", &o.pos[0], &o.pos[1], &o.pos[2]) == 3;
		} else if ((v = value("--look="))) {
			o.has_look = sscanf_s(v, "%f,%f", &o.look[0], &o.look[1]) == 2;
		} else if (!strcmp(a, "--no-vsync")) {
			o.renderer.vsync = false;
		} else if (!strcmp(a, "--debug")) {
			o.renderer.debug = true;
		} else if (!strcmp(a, "--no-debug")) {
			o.renderer.debug = false;
		} else {
			gc::log("Unknown option %s (ignored)", a);
		}
	}
	return o;
}

// The folder holding mods/: next to the game, or a parent of it (a build tree).
std::filesystem::path find_mods() {
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	for (std::filesystem::path dir = std::filesystem::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
		if (std::filesystem::exists(dir / "mods" / "core" / "mod.lua")) {
			return dir / "mods";
		}
		if (dir == dir.parent_path()) {
			break;
		}
	}
	return {};
}

// %APPDATA%\GoCube
std::filesystem::path user_dir() {
	PWSTR appdata = nullptr;
	std::filesystem::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
		out = std::filesystem::path(appdata) / "GoCube";
	}
	CoTaskMemFree(appdata);
	return out;
}

struct Controls {
	bool fly_key_was_down = false;
	bool f3_was_down = false;
	bool details = false; // F3: the details screen
	bool number_was_down[10] = {};
	bool key_was_down[256] = {};

	// A key that went down since the last frame (while the window has focus).
	bool pressed(const gc::Window &window, int vk) {
		const bool down = window.key_down(vk);
		const bool was = key_was_down[vk & 255];
		key_was_down[vk & 255] = down;
		return down && !was;
	}
};

enum class Screen { NONE, INVENTORY, CONTAINER, PAUSE };

// Number keys 1..9 pressed this frame (0: none).
int number_pressed(const gc::Window &window, Controls &controls) {
	int pressed = 0;
	for (int k = 1; k <= 9; k++) {
		const bool down = window.mouse_captured() && window.key_down('0' + k);
		if (down && !controls.number_was_down[k]) {
			pressed = k;
		}
		controls.number_was_down[k] = down;
	}
	return pressed;
}

// The hotbar, the mining bar and the crosshair.
// The death screen; true when Respawn is pressed.
bool death_screen() {
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(90, 0, 0, 140));
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::Begin("Dead", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
	ImGui::SetWindowFontScale(2.5f);
	ImGui::TextUnformatted("You died");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Dummy(ImVec2(0, 10));
	const bool respawn = ImGui::Button("Respawn", ImVec2(ImGui::GetContentRegionAvail().x, 36));
	ImGui::End();
	return respawn;
}

// Text with a black outline, as the Godot version's HUD draws it.
void outlined_text(ImDrawList *dl, ImVec2 at, ImU32 col, const char *text) {
	for (const ImVec2 o : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(-1, -1), ImVec2(1, 1)}) {
		dl->AddText(ImVec2(at.x + o.x, at.y + o.y), IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xff), text);
	}
	dl->AddText(at, col, text);
}

// A bar with its text on it (the Godot version's Hotbar._bar).
void status_bar(ImDrawList *dl, ImVec2 a, ImVec2 size, float fraction, ImU32 fill, ImU32 highlight, const char *text) {
	const ImVec2 b(a.x + size.x, a.y + size.y);
	dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 140));
	const float fx = a.x + size.x * std::clamp(fraction, 0.0f, 1.0f);
	dl->AddRectFilled(a, ImVec2(fx, b.y), fill);
	dl->AddRectFilled(a, ImVec2(fx, a.y + 3), highlight);
	dl->AddRect(a, b, IM_COL32(255, 255, 255, 128));
	const ImVec2 ts = ImGui::CalcTextSize(text);
	outlined_text(dl, ImVec2(a.x + (size.x - ts.x) / 2, a.y + (size.y - ts.y) / 2), IM_COL32(255, 255, 255, 255), text);
}

// The HUD, as the Godot version's: the crosshair, the mining progress under
// it, the 9 hotbar slots, health (and breath, and armour) bars above them in
// survival, the held item's name, a red flash when hurt, notices.
void draw_hud(const gc::Game &game, const gc::BlockTable &table, const gc::ChunkRenderer &chunks,
		const gc::BlockRules &rules, bool captured) {
	const ImGuiIO &io = ImGui::GetIO();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImVec2 c(std::floor(io.DisplaySize.x * 0.5f), std::floor(io.DisplaySize.y * 0.5f));
	if (captured) { // two bars, 16 x 2
		const ImU32 col = IM_COL32(255, 255, 255, 191);
		dl->AddRectFilled(ImVec2(c.x - 8, c.y - 1), ImVec2(c.x + 8, c.y + 1), col);
		dl->AddRectFilled(ImVec2(c.x - 1, c.y - 8), ImVec2(c.x + 1, c.y - 1), col);
		dl->AddRectFilled(ImVec2(c.x - 1, c.y + 1), ImVec2(c.x + 1, c.y + 8), col);
	}
	if (game.mining_progress() > 0) { // 120 x 8, under the crosshair
		const ImVec2 a(c.x - 60, c.y + 22), b(c.x + 60, c.y + 30);
		dl->AddRectFilled(a, b, IM_COL32(26, 26, 26, 153), 3.0f);
		dl->AddRectFilled(a, ImVec2(a.x + 120 * game.mining_progress(), b.y), IM_COL32(178, 196, 222, 255), 3.0f);
	}
	const float slot = 52, gap = 4;
	const float total = gc::Inventory::HOTBAR * slot + (gc::Inventory::HOTBAR - 1) * gap;
	const float x0 = std::floor((io.DisplaySize.x - total) / 2), y0 = io.DisplaySize.y - slot - 12;
	const ImTextureID atlas = ImTextureID(chunks.ui_atlas());
	for (int i = 0; i < gc::Inventory::HOTBAR; i++) {
		const ImVec2 a(x0 + i * (slot + gap), y0), b(a.x + slot, a.y + slot);
		const bool chosen = i == game.selected();
		dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 115));
		const gc::Stack &s = game.inventory().slots[i];
		if (!s.empty()) {
			float uv[4];
			table.atlas.tile_uv(table.icon(s.id), uv);
			dl->AddImage(atlas, ImVec2(a.x + 8, a.y + 8), ImVec2(b.x - 8, b.y - 8), ImVec2(uv[0], uv[1]),
					ImVec2(uv[2], uv[3]));
			if (!game.creative() && s.count > 1) {
				const std::string n = std::to_string(s.count);
				const ImVec2 size = ImGui::CalcTextSize(n.c_str());
				outlined_text(dl, ImVec2(b.x - 6 - size.x, b.y - 6 - size.y), IM_COL32(255, 255, 255, 255), n.c_str());
			}
		}
		if (chosen) {
			dl->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(255, 255, 255, 242), 0.0f, 0, 3.0f);
		} else {
			dl->AddRect(a, b, IM_COL32(255, 255, 255, 64));
		}
	}
	// Survival: health on the left above the hotbar, breath on the right
	// (while it's being used), armour above health.
	const int armor = game.inventory().armor_points();
	if (!game.creative()) {
		const float width = (total - 12.0f) / 2.0f, y = y0 - 20;
		char text[32];
		std::snprintf(text, sizeof(text), "%d / %d", game.health(), gc::Game::MAX_HEALTH);
		status_bar(dl, ImVec2(x0, y), ImVec2(width, 14), float(game.health()) / gc::Game::MAX_HEALTH,
				IM_COL32(209, 41, 46, 255), IM_COL32(222, 117, 120, 255), text);
		if (game.breath() < gc::Game::MAX_BREATH) {
			status_bar(dl, ImVec2(x0 + total - width, y), ImVec2(width, 14), game.breath() / gc::Game::MAX_BREATH,
					IM_COL32(89, 166, 242, 255), IM_COL32(150, 199, 246, 255), "Air");
		}
		if (armor > 0) { // full iron armour is 15 points
			std::snprintf(text, sizeof(text), "Armour %d", armor);
			status_bar(dl, ImVec2(x0, y - 18), ImVec2(width, 12), armor / 15.0f, IM_COL32(158, 163, 179, 255),
					IM_COL32(193, 196, 206, 255), text);
		}
	}
	// Hurt: a red flash. A notice at the top.
	if (game.since_hurt() < 0.4f) {
		dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(200, 0, 0, int(90 * (1.0f - game.since_hurt() / 0.4f))));
	}
	if (game.notice_time() > 0) {
		const ImVec2 size = ImGui::CalcTextSize(game.notice().c_str());
		const int alpha = int(255 * std::min(1.0f, game.notice_time()));
		outlined_text(dl, ImVec2(c.x - size.x / 2, 60), IM_COL32(255, 255, 255, alpha), game.notice().c_str());
	}
	// The held item's name above the hotbar (above the bars in survival).
	const gc::Stack &held = game.inventory().slots[game.selected()];
	if (!held.empty()) {
		const std::string name = gc::display_name(rules.content(), held.id);
		const ImVec2 size = ImGui::CalcTextSize(name.c_str());
		float bottom = y0 - 10;
		if (!game.creative()) {
			bottom -= armor > 0 ? 46.0f : 18.0f;
		}
		outlined_text(dl, ImVec2(c.x - size.x / 2, bottom - size.y), IM_COL32(255, 255, 255, 255), name.c_str());
	}
}

gc::PlayerInput read_input(const gc::Window &window) {
	gc::PlayerInput in;
	if (!window.mouse_captured()) {
		return in;
	}
	in.forward = float(window.key_down('W')) - float(window.key_down('S'));
	in.right = float(window.key_down('D')) - float(window.key_down('A'));
	in.jump = window.key_down(VK_SPACE);
	in.sneak = window.key_down(VK_SHIFT);
	in.sprint = window.key_down(VK_CONTROL);
	return in;
}

// Top left: the Godot version's status line (frame rate, the time, mode,
// flying), and with F3 the details (its debug screen); problems with mods
// always.
void draw_ui(const gc::Renderer &renderer, const gc::Player &player, const gc::World &world,
		const gc::ChunkRenderer &chunks, const gc::WorldSave *save, uint32_t seed, const gc::Game &game,
		const gc::Mods &mods, bool captured, double day_time, bool details) {
	const ImGuiIO &io = ImGui::GetIO();
	{
		char clock[16], line[160];
		gc::DayCycle::clock_text(day_time, false, clock, sizeof(clock));
		std::snprintf(line, sizeof(line), "%.0f FPS \u00b7 %s \u00b7 %s \u00b7 %s \u00b7 E inventory", io.Framerate, clock,
				game.creative() ? "creative" : "survival", player.flying ? "flying (F)" : "walking (F to fly)");
		outlined_text(ImGui::GetForegroundDrawList(), ImVec2(8, 8), IM_COL32(255, 255, 255, 255), line);
	}
	float y = 30;
	for (const auto &[id, why] : mods.problems()) {
		const std::string text = "Mod " + id + ": " + why;
		outlined_text(ImGui::GetForegroundDrawList(), ImVec2(8, y), IM_COL32(255, 160, 110, 255), text.c_str());
		y += 18;
	}
	if (!captured) {
		outlined_text(ImGui::GetForegroundDrawList(), ImVec2(8, y), IM_COL32(220, 220, 220, 255),
				"Click to play. W A S D, Space, Shift, Ctrl, F to fly. Left click breaks, right click places, "
				"1-9 or the wheel choose a slot. F3: details");
		y += 18;
	}
	if (!details) {
		return;
	}
	ImGui::SetNextWindowPos(ImVec2(8, y + 4), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.6f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoInputs;
	if (ImGui::Begin("Info", nullptr, flags)) {
		const gc::WorldStats ws = world.stats();
		const gc::ChunkRenderStats &cs = chunks.stats();
		ImGui::Text("GoCube %s  -  %s", GOCUBE_VERSION, renderer.adapter_name().c_str());
		ImGui::Text("%.0f fps (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
		char clock[16];
		gc::DayCycle::clock_text(day_time, false, clock, sizeof(clock));
		ImGui::SameLine();
		ImGui::Text("  -  %s", clock);
		ImGui::Text("Position %.1f, %.1f, %.1f%s  -  seed %u", player.position[0], player.position[1],
				player.position[2], player.flying ? "  (flying)" : "", seed);
		ImGui::Text("Chunks: %d loaded, %d meshed, %d drawn, %d jobs waiting", ws.loaded, ws.meshed, cs.drawn, ws.queued);
		if (save) {
			ImGui::Text("World: %s  (%d edited chunks not saved yet)", utf8_string(save->dir().filename()).c_str(), ws.dirty);
		} else {
			ImGui::TextDisabled("World not saved (use --world=NAME)");
		}
		ImGui::Text("%s  -  %d items lying around", game.creative() ? "Creative" : "Survival",
				int(game.drops().drops().size()));
		std::string names;
		for (const gc::ModInfo &m : mods.loaded()) {
			names += (names.empty() ? "" : ", ") + m.id + " " + m.version;
		}
		ImGui::Text("Mods: %s", names.c_str());
	}
	ImGui::End();
}

// sRGB (as colours are written, and as the Godot version gave them to its
// shaders) to linear.
DirectX::XMFLOAT3 linear(const gc::Rgb &c) {
	// (Godot's fast approximation of the curve, as its renderer uses: shaders/godot_color.hlsli.)
	auto f = [](float v) { return v * (v * (v * 0.305306011f + 0.682171111f) + 0.012522878f); };
	return {f(c.r), f(c.g), f(c.b)};
}

// The sky and the terrain's light for this time of day.
void apply_day(const gc::DayCycle &day, float view, gc::SkyParams &sky) {
	sky.fog_start = day.fog_begin(view);
	sky.fog_end = day.fog_end(view);
	sky.daylight = day.daylight();
	sky.sun_amount = day.sun_amount();
	sky.sky_tint = {day.sky_tint().r, day.sky_tint().g, day.sky_tint().b}; // (raw: see chunk.hlsl)
	float sun[3];
	day.sun_dir(sun);
	sky.sun_dir = {sun[0], sun[1], sun[2]};
	sky.fog_color = linear(day.haze());
	sky.sky_top = linear(day.sky_top());
	sky.sky_horizon = linear(day.sky_horizon());
	sky.sky_ground = sky.fog_color; // below the horizon: the haze, so faded hills meet the sky without a seam
	sky.day = day.day_amount();
	sky.dusk = day.dusk();
	sky.sky_angle = day.angle();
	sky.cloud_offset[0] = day.cloud_offset[0];
	sky.cloud_offset[1] = day.cloud_offset[1];
}

gc::Json player_state(const gc::Player &p) {
	gc::Json s = gc::Json::object();
	s["position"] = {p.position[0], p.position[1], p.position[2]};
	s["rotation"] = {p.pitch, p.yaw};
	s["flying"] = p.flying;
	return s;
}

void restore_player(gc::Player &p, const gc::Json &s) {
	auto pos = s.find("position");
	if (pos != s.end() && pos->is_array() && pos->size() == 3) {
		for (int i = 0; i < 3; i++) {
			p.position[i] = (*pos)[i].get<float>();
		}
	}
	auto rot = s.find("rotation");
	if (rot != s.end() && rot->is_array() && rot->size() == 2) {
		p.pitch = (*rot)[0].get<float>();
		p.yaw = (*rot)[1].get<float>();
	}
	auto fly = s.find("flying");
	p.flying = fly != s.end() && fly->is_boolean() && fly->get<bool>();
}

} // namespace

enum class End { QUIT, MENU, FAILED };

// What to play: a world folder (empty: not saved), and for a new one its seed and mode.
struct WorldChoice {
	std::filesystem::path dir;
	uint32_t seed = 777;
	std::string mode = "survival";
};

enum class PauseChoice { NONE, RESUME, QUIT };

// The pause menu (single player: the world waits behind it).
PauseChoice pause_menu(bool saved, bool from_menu) {
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::Begin("Paused", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
	PauseChoice choice = PauseChoice::NONE;
	if (ImGui::Button("Resume", ImVec2(260, 36))) {
		choice = PauseChoice::RESUME;
	}
	const char *quit = from_menu ? (saved ? "Save and quit to title" : "Quit to title") : (saved ? "Save and quit" : "Quit");
	if (ImGui::Button(quit, ImVec2(260, 36))) {
		choice = PauseChoice::QUIT;
	}
	ImGui::TextDisabled("Esc to resume");
	ImGui::End();
	return choice;
}

// A world in the title menu's list.
struct WorldEntry {
	std::filesystem::path dir;
	std::string name, mode, saved, seed;
};

struct TitleMenu {
	bool refresh = true;
	std::vector<WorldEntry> worlds;
	int selected = -1;
	bool confirm_delete = false;
	char new_name[64] = "New World";
	char new_seed[24] = "";
	int new_mode = 0; // 0 survival, 1 creative
};

std::vector<WorldEntry> find_worlds() {
	std::vector<WorldEntry> out;
	std::error_code ec;
	for (const auto &e : std::filesystem::directory_iterator(user_dir() / "worlds", ec)) {
		if (!e.is_directory(ec)) {
			continue;
		}
		WorldEntry w;
		w.dir = e.path();
		w.name = utf8_string(e.path().filename());
		gc::Json meta;
		std::string why;
		if (std::filesystem::exists(e.path() / "world.lua", ec)) {
			if (gc::read_lua_data(e.path() / "world.lua", meta, why) && meta.is_object()) {
				w.mode = meta.value("mode", "creative");
				w.saved = meta.value("saved", meta.value("created", ""));
				if (meta.contains("seed") && meta["seed"].is_number()) {
					w.seed = std::to_string(int64_t(meta["seed"].get<double>()));
				}
			} else {
				w.mode = "(damaged)";
			}
		} else if (std::filesystem::exists(e.path() / "world.json", ec)) {
			w.mode = "(old world.json: not read)"; // (opening it says why)
		} else {
			continue;
		}
		out.push_back(w);
	}
	std::sort(out.begin(), out.end(), [](const WorldEntry &a, const WorldEntry &b) { return a.saved > b.saved; });
	return out;
}

// Moves a folder to the Recycle Bin.
bool to_recycle_bin(const std::filesystem::path &dir) {
	std::wstring from = dir.wstring();
	from.push_back(L'\0'); // (a double-null-terminated list)
	SHFILEOPSTRUCTW op = {};
	op.wFunc = FO_DELETE;
	op.pFrom = from.c_str();
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
	return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// A folder name for a new world, or why it can't be one.
std::string name_problem(const std::string &name) {
	if (name.empty() || name.find_first_not_of(' ') == std::string::npos) {
		return "Give the world a name.";
	}
	if (name.find_first_of("<>:\"/\\|?*") != std::string::npos || name.back() == '.' || name.back() == ' ') {
		return "A world's name can't have < > : \" / \\ | ? * or end with a dot or space.";
	}
	std::error_code ec;
	if (std::filesystem::exists(user_dir() / "worlds" / utf8_path(name.c_str()), ec)) {
		return "There's already a world with that name.";
	}
	return "";
}

// One frame of the title menu at a time, until a world is chosen (true) or
// the window closes (false).
bool title_menu(const Options &options, gc::Window &window, gc::Renderer &renderer, TitleMenu &menu,
		std::string &problem, std::optional<WorldChoice> &choice) {
	long frame = 0;
	while (window.pump()) {
		if (window.take_resized()) {
			renderer.resize(window.width(), window.height());
		}
		if (window.minimized()) {
			Sleep(16);
			continue;
		}
		if (menu.refresh) {
			menu.worlds = find_worlds();
			menu.selected = menu.worlds.empty() ? -1 : 0;
			menu.refresh = false;
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		const ImGuiIO &io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, io.DisplaySize.x - 40), std::min(560.0f, io.DisplaySize.y - 40)));
		ImGui::Begin("GoCube", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
		ImGui::SetWindowFontScale(2.0f);
		ImGui::TextUnformatted("GoCube");
		ImGui::SetWindowFontScale(1.0f);
		ImGui::TextDisabled("%s", GOCUBE_VERSION);
		ImGui::Separator();

		ImGui::TextUnformatted("Worlds");
		ImGui::BeginChild("worlds", ImVec2(0, 250), ImGuiChildFlags_Borders);
		for (int i = 0; i < int(menu.worlds.size()); i++) {
			const WorldEntry &w = menu.worlds[i];
			ImGui::PushID(i);
			const std::string label = w.name + "##world";
			if (ImGui::Selectable(label.c_str(), menu.selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
				menu.selected = i;
				menu.confirm_delete = false;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					choice = WorldChoice{w.dir, 0, w.mode};
				}
			}
			ImGui::SameLine(300);
			ImGui::TextDisabled("%s  -  seed %s  -  %s", w.mode.c_str(), w.seed.c_str(), w.saved.c_str());
			ImGui::PopID();
		}
		if (menu.worlds.empty()) {
			ImGui::TextDisabled("No worlds yet: create one below.");
		}
		ImGui::EndChild();
		const bool has = menu.selected >= 0 && menu.selected < int(menu.worlds.size());
		ImGui::BeginDisabled(!has);
		if (ImGui::Button("Play", ImVec2(120, 32)) && has) {
			choice = WorldChoice{menu.worlds[menu.selected].dir, 0, menu.worlds[menu.selected].mode};
		}
		ImGui::SameLine();
		if (!menu.confirm_delete) {
			if (ImGui::Button("Delete...", ImVec2(120, 32))) {
				menu.confirm_delete = true;
			}
		} else if (has) {
			ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Move \"%s\" to the Recycle Bin?", menu.worlds[menu.selected].name.c_str());
			ImGui::SameLine();
			if (ImGui::Button("Delete")) {
				if (!to_recycle_bin(menu.worlds[menu.selected].dir)) {
					problem = "The world couldn't be moved to the Recycle Bin.";
				}
				menu.confirm_delete = false;
				menu.refresh = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Keep it")) {
				menu.confirm_delete = false;
			}
		}
		ImGui::EndDisabled();

		ImGui::Dummy(ImVec2(0, 8));
		ImGui::Separator();
		ImGui::TextUnformatted("New world");
		ImGui::SetNextItemWidth(260);
		ImGui::InputText("Name", menu.new_name, sizeof(menu.new_name));
		ImGui::SetNextItemWidth(260);
		ImGui::InputTextWithHint("Seed", "random", menu.new_seed, sizeof(menu.new_seed), ImGuiInputTextFlags_CharsDecimal);
		ImGui::SetNextItemWidth(260);
		ImGui::Combo("Mode", &menu.new_mode, "Survival\0Creative\0");
		const std::string why = name_problem(menu.new_name);
		ImGui::BeginDisabled(!why.empty());
		if (ImGui::Button("Create and play", ImVec2(200, 32)) && why.empty()) {
			WorldChoice c;
			c.dir = user_dir() / "worlds" / utf8_path(menu.new_name);
			c.seed = menu.new_seed[0] ? uint32_t(strtoull(menu.new_seed, nullptr, 10)) : uint32_t(std::random_device{}());
			c.mode = menu.new_mode == 1 ? "creative" : "survival";
			choice = c;
		}
		ImGui::EndDisabled();
		if (!why.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", why.c_str());
		}
		if (!problem.empty()) {
			ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%s", problem.c_str());
		}
		ImGui::End();
		ImGui::Render();
		const float clear[4] = {0.18f, 0.32f, 0.62f, 1.0f};
		renderer.begin_frame(clear);
		const bool last_frame = options.frames > 0 && ++frame >= options.frames; // (testing)
		renderer.end_frame(last_frame && !options.screenshot.empty() ? options.screenshot.c_str() : nullptr);
		if (choice) {
			return true;
		}
		if (last_frame) {
			return false;
		}
	}
	return false;
}

// Plays a world until the player quits (to the title menu, or the game).
End play(const Options &options, const WorldChoice &choice, bool from_menu, gc::Window &window,
		gc::Renderer &renderer, std::string &problem) {
	// Content: the mods (the base game first).
	const std::filesystem::path builtin_mods = find_mods();
	if (builtin_mods.empty()) {
		gc::log("Couldn't find the mods folder (with the base game, mods\\core)");
		return End::FAILED;
	}
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({builtin_mods, user_dir() / "mods"});
	if (mods.loaded().empty() || mods.loaded().front().id != "core") {
		auto it = mods.problems().find("core");
		gc::log("Couldn't load the base game: %s", it != mods.problems().end() ? it->second.c_str() : "it's missing");
		return End::FAILED;
	}

	// The world: its folder decides the seed, generator and block ids.
	std::optional<gc::WorldSave> save;
	const std::filesystem::path &dir = choice.dir;
	uint32_t seed = choice.seed;
	int generator_version = options.generator;
	if (!dir.empty()) {
		save.emplace();
		if (std::string e = save->open(dir, choice.seed, choice.mode); !e.empty()) {
			gc::log("Can't open the world in %s: %s", utf8_string(dir).c_str(), e.c_str());
			problem = e;
			return End::FAILED;
		}
		seed = save->seed();
		generator_version = save->generator();
		content.freeze(save->block_ids());
		save->set_block_ids(content.block_ids());
		gc::log("World %s: seed %u, generator %d, %s", utf8_string(dir).c_str(), seed, generator_version,
				save->mode().c_str());
	} else {
		content.freeze();
	}
	const gc::BlockTable table = gc::build_block_table(content);
	const gc::BlockRules rules(content);
	gocube::Generator generator;
	gc::configure_generator(generator, content, seed, generator_version);
	gc::log("%zu blocks, %zu atlas tiles", table.blocks.size(), table.atlas.tiles.size());

	gc::ChunkRenderer chunks;
	if (!chunks.init(renderer.device(), table.atlas)) {
		gc::log("Couldn't set up the terrain renderer");
		return End::FAILED;
	}

	auto world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator,
			save ? utf8_string(save->chunks_dir()) : std::string());
	world->set_view_distance(options.view);
	mods.attach(world.get(), &rules);

	gc::Player player;
	player.position[0] = 0.5f;
	player.position[1] = float(generator.height_at(0, 0)) + 1.0f; // on the generated ground
	player.position[2] = 0.5f;
	if (save) {
		restore_player(player, save->player());
	}
	if (options.has_pos) {
		std::copy(options.pos, options.pos + 3, player.position);
		player.flying = true; // stays where it was put
	}
	if (options.has_look) {
		player.yaw = DirectX::XMConvertToRadians(options.look[0]);
		player.pitch = DirectX::XMConvertToRadians(options.look[1]);
	}
	float previous_position[3];
	std::copy(player.position, player.position + 3, previous_position);
	Controls controls;
	Screen screen = options.screen == "inventory" ? Screen::INVENTORY : Screen::NONE;
	gc::BlockPos container; // (Screen::CONTAINER)
	const bool creative = (save ? save->mode() : choice.mode) == "creative";
	gc::Game game(content, table, rules, *world, mods, player, creative);
	game.set_generator(&generator);
	if (save) {
		game.load_player(save->player());
		game.block_entities().deserialize(save->field("block_entities"));
		game.set_world_spawn(save->field("spawn"));
		game.load_plants(save->field("plants"));
	} else {
		game.start_new_player();
	}
	if (!options.has_pos && !(save && save->player().contains("position"))) {
		game.spawn_on_ground(); // a new player: on dry ground near the origin, once it's loaded
	}
	for (size_t i = 0; i < options.drop.size(); i++) { // (for trying things out)
		float look[3];
		player.look(look);
		const float side = (float(i) - float(options.drop.size() - 1) * 0.5f) * 0.6f;
		const float at[3] = {player.position[0] + look[0] * 4.0f - look[2] * side, player.position[1] + 1.0f,
				player.position[2] + look[2] * 4.0f + look[0] * side}; // (beyond the magnet radius)
		game.drop_item(options.drop[i], 1, at);
	}

	gc::Camera camera;
	camera.far_z = float(options.view) * 2.0f;
	gc::ItemModels item_models(content, table);
	gc::Hand hand;
	gc::HandLights hand_lights;
	int seen_swings = game.swings();
	gc::SkyParams sky;
	gc::DayCycle day;
	day.time = gc::DayCycle::system_time(); // a new world starts at the computer's clock
	if (save) {
		if (const gc::Json &t = save->field("time"); t.is_number()) {
			day.time = std::fmod(t.get<double>(), 1.0);
		}
		if (const gc::Json &l = save->field("day_length"); l.is_number()) {
			day.day_length = std::max(10.0, l.get<double>());
		}
	}
	if (options.time >= 0) {
		day.time = std::fmod(options.time, 1.0);
	}
	float sleeping = -1; // seconds asleep (-1: awake)
	// Block shadows (the Godot version's default sun shadows), from where the sun is.
	world->set_sun_angle(day.angle());
	world->set_block_shadows(true);

	auto save_all = [&] {
		if (save) {
			const int chunks_saved = world->save_modified();
			gc::Json state = player_state(player);
			state.update(game.save_player());
			save->set_field("block_entities", game.block_entities().serialize());
			save->set_field("time", day.time);
			save->set_field("plants", game.save_plants());
			if (const auto &spawn = game.world_spawn()) {
				save->set_field("spawn", {(*spawn)[0], (*spawn)[1], (*spawn)[2]});
			}
			save->save(state);
			gc::log("Saved the world (%d chunks)", chunks_saved);
		}
	};

	using clock = std::chrono::steady_clock;
	const auto start = clock::now();
	auto last = start, last_save = start;
	float accumulator = 0;
	double tick_accumulator = 0;
	constexpr double TICK = 0.05; // mods' tick event: 20 a second
	long frame = 0;
	End end = End::QUIT;
	bool quit_requested = false;
	while (true) {
		if (!window.pump()) {
			end = End::QUIT;
			break;
		}
		const auto now = clock::now();
		const float dt = std::min(0.25f, std::chrono::duration<float>(now - last).count());
		last = now;
		if (window.take_resized()) {
			renderer.resize(window.width(), window.height());
		}
		const bool left = window.take_pressed(gc::Window::LEFT);
		const bool right = window.take_pressed(gc::Window::RIGHT);
		const bool middle = window.take_pressed(gc::Window::MIDDLE);
		const float wheel = window.take_wheel();
		// Screens: E opens the inventory; E or Esc closes it (what the mouse
		// held goes back in, or is dropped). While one is open the mouse is free.
		const bool key_e = controls.pressed(window, 'E');
		const bool key_esc = controls.pressed(window, VK_ESCAPE);
		auto close_screen = [&] {
			const gc::Stack left_over = game.inventory().close_cursor();
			if (!left_over.empty()) {
				float look[3];
				player.look(look);
				const float at[3] = {player.position[0] + look[0], player.position[1] + 1.2f, player.position[2] + look[2]};
				game.drop_item(left_over.id, left_over.count, at);
			}
			screen = Screen::NONE;
			window.set_mouse_captured(true);
		};
		if (screen == Screen::NONE) {
			if (key_e && window.mouse_captured()) {
				screen = Screen::INVENTORY;
				window.set_mouse_captured(false);
			} else if (key_esc && !game.dead()) {
				screen = Screen::PAUSE;
				window.set_mouse_captured(false);
			}
		} else if (screen == Screen::PAUSE) {
			if (key_esc) {
				screen = Screen::NONE;
				window.set_mouse_captured(true);
			}
		} else if (key_e || key_esc) {
			close_screen();
		}
		const bool paused = screen == Screen::PAUSE; // (single player: the world waits)
		const bool captured = window.mouse_captured() && screen == Screen::NONE;
		if (left && !window.mouse_captured() && screen == Screen::NONE && !ImGui::GetIO().WantCaptureMouse) {
			window.set_mouse_captured(true); // (that click only captures)
		}
		if (window.minimized()) {
			Sleep(16); // nothing to present to
			continue;
		}

		// Looking around, then fixed physics steps; the camera is drawn between
		// the last two steps, so movement is smooth at any frame rate.
		float mx, my;
		window.take_mouse_delta(mx, my);
		const float sensitivity = 0.003f; // radians per mouse count (the Godot version's default)
		player.yaw -= mx * sensitivity;
		player.pitch = std::clamp(player.pitch - my * sensitivity, -1.55f, 1.55f);
		const bool fly_key = captured && window.key_down('F');
		if (fly_key && !controls.fly_key_was_down) {
			player.flying = !player.flying;
			player.velocity[1] = 0;
		}
		controls.fly_key_was_down = fly_key;
		const bool f3 = window.key_down(VK_F3);
		if (f3 && !controls.f3_was_down) {
			controls.details = !controls.details;
		}
		controls.f3_was_down = f3;
		const gc::PlayerInput input = read_input(window);
		accumulator += paused ? 0.0f : dt;
		while (accumulator >= gc::Player::STEP) {
			std::copy(player.position, player.position + 3, previous_position);
			if (!game.frozen()) {
				player.step(input, *world, rules);
				game.after_move(gc::Player::STEP);
			}
			game.physics_step(gc::Player::STEP);
			accumulator -= gc::Player::STEP;
		}
		const float alpha = accumulator / gc::Player::STEP;

		// Hotbar, mining, breaking, placing and picking.
		gc::GameInput gin;
		gin.captured = captured;
		gin.break_held = captured && window.key_down(VK_LBUTTON);
		gin.break_pressed = captured && left;
		gin.use_pressed = captured && right;
		gin.pick_pressed = captured && middle;
		gin.sneak = window.key_down(VK_SHIFT);
		gin.wheel = -int(wheel); // (rolled towards you: the next slot)
		gin.number_key = number_pressed(window, controls);
		if (!paused) {
			game.update(dt, gin);
			game.tick(dt);
			day.update(dt);
		}
		// A bed used at night: sleep (the screen fades out), then it's morning.
		if (game.take_bed_used()) {
			if (day.day_amount() > 0.3f) {
				game.show_notice("You can only sleep at night (respawn point set)");
			} else {
				sleeping = 0;
			}
		}
		if (sleeping >= 0) {
			sleeping += dt;
			if (sleeping >= 2.5f) {
				day.time = gc::DayCycle::MORNING;
				sleeping = -1;
			}
		}
		if (auto opened = game.take_opened()) {
			container = *opened;
			screen = Screen::CONTAINER;
			window.set_mouse_captured(false);
		}
		tick_accumulator += paused ? 0.0 : dt;
		while (tick_accumulator >= TICK) {
			mods.tick(TICK);
			tick_accumulator -= TICK;
		}

		camera.position = {previous_position[0] + (player.position[0] - previous_position[0]) * alpha,
				previous_position[1] + (player.position[1] - previous_position[1]) * alpha + gc::Player::EYE_HEIGHT,
				previous_position[2] + (player.position[2] - previous_position[2]) * alpha};
		camera.yaw = player.yaw;
		camera.pitch = player.pitch;

		// The arm swings with every hit, place, use and meal, and keeps
		// swinging while mining; it bobs while walking.
		if (game.swings() != seen_swings) {
			seen_swings = game.swings();
			hand.swing();
		}
		const bool walking = player.on_floor && std::hypot(player.velocity[0], player.velocity[2]) > 0.5f;
		hand.update(paused ? 0.0f : dt, game.mining_progress() > 0.0f, walking);
		// A light-giving block in hand (a torch) lights the world around
		// (the Godot version's main._update_hand_light).
		std::vector<std::array<float, 4>> held_lights;
		if (auto it = content.blocks.find(game.selected_item()); !game.dead() && it != content.blocks.end()) {
			const float level = float(it->second.value("light", 0.0));
			if (level > 0) {
				held_lights.push_back({camera.position.x, camera.position.y, camera.position.z, level});
			}
		}
		const float eye[3] = {camera.position.x, camera.position.y, camera.position.z};
		const bool hand_light_changed = hand_lights.update(*world, held_lights, eye);

		world->set_sun_angle(day.angle()); // (shadows follow it in steps)
		world->update(player.position[0], player.position[1], player.position[2]);
		for (const gc::ChunkPos &p : world->take_unloaded()) {
			chunks.remove(p);
		}
		for (const gc::ChunkMesh &m : world->take_meshes()) {
			chunks.upload(renderer.device(), m);
		}
		if (save && std::chrono::duration<float>(now - last_save).count() >= 60.0f) {
			save_all(); // autosave
			last_save = now;
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		if (screen == Screen::NONE) {
			draw_ui(renderer, player, *world, chunks, save ? &*save : nullptr, seed,
					game, mods, window.mouse_captured(), day.time, controls.details);
		}
		const gc::UiIcons icons{ImTextureID(chunks.ui_atlas()), &table, &content};
		if (game.dead()) {
			if (window.mouse_captured()) {
				window.set_mouse_captured(false);
			}
			if (death_screen()) {
				game.respawn();
				window.set_mouse_captured(true);
			}
		} else if (screen == Screen::INVENTORY && !gc::inventory_screen(game, icons)) {
			close_screen();
		} else if (screen == Screen::CONTAINER && !gc::container_screen(game, container, icons)) {
			close_screen();
		} else if (screen == Screen::PAUSE) {
			switch (pause_menu(save.has_value(), from_menu)) {
				case PauseChoice::RESUME:
					screen = Screen::NONE;
					window.set_mouse_captured(true);
					break;
				case PauseChoice::QUIT:
					end = from_menu ? End::MENU : End::QUIT;
					quit_requested = true;
					break;
				default:
					break;
			}
		}
		draw_hud(game, table, chunks, rules, captured);
		if (sleeping >= 0) {
			ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize,
					IM_COL32(0, 0, 0, int(230 * std::min(1.0f, sleeping / 1.5f))));
		}
		gc::draw_held(game.inventory().cursor, icons);
		ImGui::Render();

		sky.time = std::chrono::duration<float>(now - start).count();
		if (!options.screenshot.empty()) {
			// The clouds (and their shadows) as 3 seconds in, however long
			// loading took: the same in every screenshot (and in
			// tests/data/godot_screenshot.gd's).
			day.cloud_offset[0] = 0.35f * 3.0f;
			day.cloud_offset[1] = 0.12f * 3.0f;
		}
		apply_day(day, float(options.view), sky);
		const float clear[4] = {sky.fog_color.x, sky.fog_color.y, sky.fog_color.z, 1.0f};
		renderer.begin_frame(clear);
		const float aspect = float(renderer.width()) / float(std::max(1, renderer.height()));
		chunks.set_hand_light(renderer.context(), hand_lights.volume(), hand_lights.origin(), hand_lights.count(),
				hand_light_changed);
		chunks.draw_sky(renderer.context(), camera, aspect, sky);
		chunks.draw(renderer.context(), camera, aspect, sky);
		// Dropped items, and the hand: lit like the terrain around them
		// (never quite black: a ninja's sword in the dark is a faint shape).
		const gc::Rgb tint = day.sky_tint();
		const float sky_light[3] = {tint.r * day.daylight(), tint.g * day.daylight(), tint.b * day.daylight()};
		constexpr float NIGHT_CREATURE_LIGHT = 0.07f;
		{
			std::vector<gc::WorldVertex> drop_mesh;
			game.drops().build_mesh(item_models, [&](const float at[3], float out[3]) {
				gc::light_for(*world, hand_lights, sky_light, at, NIGHT_CREATURE_LIGHT, out);
			}, drop_mesh);
			chunks.draw_items(renderer.context(), drop_mesh, camera.view() * camera.projection(aspect), camera.position,
					sky, true);
		}
		if (const gc::RayHit hit = player.target(*world); hit.hit) {
			std::vector<gocube::Box> boxes;
			world->block_boxes(hit.position.x, hit.position.y, hit.position.z, hit.block, false, boxes);
			chunks.draw_outline(renderer.context(), boxes);
		}
		if (!game.dead() && sleeping < 0) {
			// Over everything (it never pokes into walls), the item over the arm.
			float lit[3];
			gc::light_for(*world, hand_lights, sky_light, eye, NIGHT_CREATURE_LIGHT, lit);
			std::vector<gc::WorldVertex> arm, held;
			hand.build(item_models, game.selected_item(), lit, arm, held);
			const DirectX::XMMATRIX projection = camera.projection(aspect);
			chunks.clear_depth(renderer.context());
			chunks.draw_items(renderer.context(), arm, projection, {0, 0, 0}, sky, false);
			chunks.clear_depth(renderer.context());
			chunks.draw_items(renderer.context(), held, projection, {0, 0, 0}, sky, true, true);
		}

		// With a screenshot, count frames once the world around has loaded.
		const gc::WorldStats ws = world->stats();
		bool counting = options.screenshot.empty() || ws.settled;
		if (!counting && sky.time > 60.0f) {
			static bool warned = false;
			if (!warned) {
				gc::log("The world hasn't finished loading after 60 s (%d jobs left); taking the screenshot anyway", ws.queued);
				warned = true;
			}
			counting = true;
		}
		const bool last_frame = options.frames > 0 && counting && ++frame >= options.frames;
		if (!renderer.end_frame(last_frame && !options.screenshot.empty() ? options.screenshot.c_str() : nullptr)) {
			end = End::FAILED;
			break;
		}
		if (last_frame || quit_requested) {
			break;
		}
	}

	save_all();
	mods.attach(nullptr, nullptr);
	window.set_mouse_captured(false);
	// (The world's workers stop when it goes, after the game, which uses it:
	// both at the end of this scope, in that order.)
	return end;
}

int main(int argc, char **argv) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const Options options = parse(argc, argv);

	gc::Window window;
	if (!window.create(L"GoCube", 1280, 720, !options.hidden)) {
		gc::log("Couldn't create the window");
		return 1;
	}
	gc::Renderer renderer;
	if (!renderer.init(window.hwnd(), window.width(), window.height(), options.renderer)) {
		gc::log("Couldn't start Direct3D 11");
		return 1;
	}
	renderer.set_render_scale(options.render_scale);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr; // no imgui.ini next to the game
	ImGui::StyleColorsDark();
	ImGui::GetStyle().DisabledAlpha = 0.35f; // (what can't be done now looks it)
	ImGui_ImplWin32_Init(window.hwnd());
	if (!renderer.init_imgui()) {
		gc::log("Couldn't start Dear ImGui's Direct3D 11 backend");
		return 1;
	}

	bool ok = true;
	// Started for a world (or for testing): straight into it.
	const bool direct = !options.menu && (!options.world_name.empty() || !options.world_dir.empty() || options.hidden ||
			options.frames > 0 || options.has_pos);
	if (direct) {
		WorldChoice choice;
		choice.dir = options.world_dir;
		if (choice.dir.empty() && !options.world_name.empty()) {
			choice.dir = user_dir() / "worlds" / utf8_path(options.world_name.c_str());
		}
		choice.seed = options.seed;
		choice.mode = options.mode;
		std::string problem;
		ok = play(options, choice, false, window, renderer, problem) != End::FAILED;
	} else {
		TitleMenu menu;
		std::string problem;
		while (true) {
			std::optional<WorldChoice> choice;
			if (!title_menu(options, window, renderer, menu, problem, choice)) {
				break; // the window closed
			}
			problem.clear();
			const End end = play(options, *choice, true, window, renderer, problem);
			if (end == End::QUIT) {
				break;
			}
			if (end == End::FAILED && problem.empty()) {
				problem = "That world couldn't be opened (see the log).";
			}
			menu.refresh = true;
		}
	}

	const int messages = renderer.debug_messages();
	renderer.shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	if (messages > 0) {
		gc::log("The Direct3D 11 debug layer reported %d errors or warnings", messages);
	}
	return ok && messages == 0 ? 0 : 1;
}
