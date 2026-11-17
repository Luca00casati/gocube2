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
//   --spawn=KIND[=BLOCK],...  creatures in front of the player, e.g.
//                    block_eater=core:stone (for trying things out)
//   --warp           render on WARP, Windows' software adapter (no GPU needed)
//   --hidden         don't show the window (tests)
//   --frames=N       quit after N frames; the exit code is 1 if the debug layer
//                    reported errors or warnings. With --screenshot, counted
//                    from when the world around has finished loading.
//   --screenshot=F   save the last frame as a PNG
//   --render-scale=F draw the world at this part of the window's resolution
//                    (0.25..1, stretched over it; the Godot version offers
//                    1, 0.85, 0.75 and 0.5). --view and --render-scale win
//                    over the settings (%APPDATA%\GoCube\settings.lua, which
//                    --hidden doesn't read: tests play with the defaults)
//   --screen=inventory|settings  a screen open at the start (screenshots)
//   --host[=PORT]    open the world to other players (default port 24680)
//   --join=ADDRESS[:PORT]  play in someone else's world
//   --name=NAME      the name others see (default "Player")
//   --accept-mods    joining a host with other mods: download them without asking
//   --no-sound       play without sound (--hidden always does)
//   --sound          with --hidden: sound running but at volume 0 (tests)
//   --no-vsync       present as fast as possible
//   --debug / --no-debug   force the Direct3D 11 debug layer on or off
//                    (default: on in debug builds)

#include "audio/audio.h"
#include "content/content.h"
#include "data/lua_data.h"
#include "game/blocks.h"
#include "game/creature_models.h"
#include "game/day_cycle.h"
#include "game/game.h"
#include "game/hand.h"
#include "game/player.h"
#include "game/settings.h"
#include "log.h"
#include "mods/mods.h"
#include "net/session.h"
#include "platform/fine_sleep.h"
#include "platform/window.h"
#include "render/chunk_renderer.h"
#include "render/renderer.h"
#include "ui/screens.h"
#include "ui/settings_screen.h"
#include "world/save.h"
#include "world/world.h"

#include "ui/gui.h"
#include "ui/gui_d2d.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
	gc::RendererOptions renderer;
	bool hidden = false;
	bool no_sound = false;
	bool sound = false;
	long frames = 0; // 0: until the window is closed
	std::string screenshot;
	std::string world_name;
	std::filesystem::path world_dir;
	uint32_t seed = 777;
	int generator = gc::GENERATOR_VERSION;
	int view = 0;              // --view (0: the settings')
	float render_scale = 0.0f; // --render-scale (0: the settings')
	bool no_vsync = false;
	bool has_pos = false;
	float pos[3] = {0, 0, 0};
	bool has_look = false;
	float look[2] = {0, 0};
	std::string mode = "survival"; // for a new world
	std::vector<std::string> drop; // items dropped in front of the player at the start
	std::vector<std::string> spawn; // creatures ("kind" or "kind:variant") put in front of the player
	std::string screen;            // a screen open at the start ("inventory")
	double time = -1;              // time of day to start at (0..1; -1: the world's)
	bool menu = false;             // the title menu even with testing options (--frames, --screenshot: of it)
	int host_port = 0;             // --host: open to others on this port (0: not)
	std::string join;              // --join: the host's address
	int join_port = gc::Session::DEFAULT_PORT;
	std::string name = "Player";
	bool accept_mods = false;      // --accept-mods: play with a host's other mods without asking (tests)
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
		} else if ((v = value("--spawn="))) {
			std::string list = v;
			for (size_t from = 0, to; from <= list.size(); from = to + 1) {
				to = list.find(',', from);
				to = to == std::string::npos ? list.size() : to;
				if (to > from) {
					o.spawn.push_back(list.substr(from, to - from));
				}
			}
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
		} else if (!strcmp(a, "--host")) {
			o.host_port = gc::Session::DEFAULT_PORT;
		} else if ((v = value("--host="))) {
			o.host_port = int(strtol(v, nullptr, 10));
		} else if ((v = value("--join="))) {
			o.join = v;
			if (const size_t colon = o.join.rfind(':'); colon != std::string::npos) {
				o.join_port = int(strtol(o.join.c_str() + colon + 1, nullptr, 10));
				o.join.resize(colon);
			}
		} else if ((v = value("--name="))) {
			o.name = v;
		} else if (!strcmp(a, "--accept-mods")) {
			o.accept_mods = true;
		} else if (!strcmp(a, "--no-sound")) {
			o.no_sound = true;
		} else if (!strcmp(a, "--sound")) {
			o.sound = true;
		} else if (!strcmp(a, "--no-vsync")) {
			o.renderer.vsync = false;
			o.no_vsync = true;
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

// The player's settings (%APPDATA%\GoCube\settings.lua; tests run with the
// defaults: --hidden doesn't read them).
gc::Settings settings;
std::filesystem::path settings_file() {
	return user_dir() / "settings.lua";
}

// What the game draws and plays with: the settings, unless an option says otherwise.
int view_distance(const Options &o) {
	return o.view > 0 ? o.view : settings.view_distance;
}
float render_scale(const Options &o) {
	return o.render_scale > 0 ? o.render_scale : settings.render_scale;
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

enum class Screen { NONE, INVENTORY, CONTAINER, PAUSE, SETTINGS };

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

// The UI: the game's own (ui/gui.h), drawn with Direct2D (ui/gui_d2d.h).
struct Ui {
	gc::GuiD2D d2d;
	gc::ui::Gui gui{[this](std::string_view text, float size, float wrap) { return d2d.measure(text, size, wrap); }};
	std::vector<gc::ui::DrawCmd> cmds;
	std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
Ui *the_ui = nullptr;

gc::ui::Gui &gui() {
	return the_ui->gui;
}

// Starts a UI frame with what the window got (nothing for the UI while the
// mouse is captured for looking around).
void begin_ui(gc::Window &window) {
	const auto now = std::chrono::steady_clock::now();
	gc::ui::Input in;
	in.dt = std::clamp(std::chrono::duration<float>(now - the_ui->last).count(), 0.0001f, 0.25f);
	the_ui->last = now;
	in.width = float(window.width());
	in.height = float(window.height());
	float x, y;
	if (window.mouse_position(x, y)) {
		in.mouse = {x, y};
	}
	gc::Window::UiEvents e = window.take_ui_events();
	for (int b = 0; b < 3; b++) {
		in.down[b] = !window.mouse_captured() && window.button_down(gc::Window::Button(b));
		in.clicks[b] = e.clicks[b];
	}
	in.wheel = window.mouse_captured() ? 0 : e.wheel;
	in.text = std::move(e.text);
	in.keys = std::move(e.keys);
	in.ctrl = window.key_down(VK_CONTROL);
	gui().begin_frame(in);
}

void end_ui() {
	the_ui->cmds = gui().end_frame();
}

// For Renderer::end_frame: the UI, over the frame.
std::function<void()> ui_overlay(gc::Renderer &renderer) {
	return [&renderer] { the_ui->d2d.draw(renderer.swap_chain(), renderer.context(), the_ui->cmds); };
}

std::string format(const char *f, ...) {
	char out[512];
	va_list args;
	va_start(args, f);
	std::vsnprintf(out, sizeof(out), f, args);
	va_end(args);
	return out;
}

gc::ui::Rect screen_rect() {
	return {0, 0, gui().input().width, gui().input().height};
}

// Behind the menus with no world: an evening sky, for the glass panels to sit on.
void sky_backdrop() {
	const gc::ui::Rect r = screen_rect();
	const float a = std::floor(r.y1 * 0.5f), b = std::floor(r.y1 * 0.68f);
	const gc::ui::Color top = gc::ui::rgba(12, 20, 42), sky = gc::ui::rgba(66, 92, 132),
			glow = gc::ui::rgba(170, 136, 124), ground = gc::ui::rgba(20, 22, 30);
	gc::ui::DrawList &bg = gui().background();
	bg.gradient({r.x0, r.y0, r.x1, a}, top, sky, true);
	bg.gradient({r.x0, a, r.x1, b}, sky, glow, true);
	bg.gradient({r.x0, b, r.x1, r.y1}, glow, ground, true);
}

// The death screen; true when Respawn is pressed.
bool death_screen() {
	gc::ui::Gui &g = gui();
	g.background().fill(screen_rect(), gc::ui::rgba(90, 0, 0, 140));
	gc::ui::WindowOptions o;
	o.pos = {g.input().width * 0.5f, g.input().height * 0.45f};
	o.pivot = {0.5f, 0.5f};
	o.background = false;
	g.begin_window("Dead", o);
	g.text("You died", gc::ui::rgba(255, 255, 255), 2.5f);
	g.space(10);
	const bool respawn = g.button("Respawn", {std::max(160.0f, g.text_size("You died", 2.5f).x), 36});
	g.end_window();
	return respawn;
}

// Text with a black outline, as the Godot version's HUD draws it.
void outlined_text(gc::ui::DrawList &dl, gc::ui::Vec2 at, gc::ui::Color col, const std::string &text) {
	dl.outlined_text(at, col, text, gui().theme.font);
}

// A bar with its text on it (the Godot version's Hotbar._bar).
void status_bar(gc::ui::DrawList &dl, gc::ui::Vec2 a, gc::ui::Vec2 size, float fraction, gc::ui::Color fill,
		gc::ui::Color highlight, const char *text) {
	const gc::ui::Rect r{a.x, a.y, a.x + size.x, a.y + size.y};
	dl.fill(r, gc::ui::rgba(0, 0, 0, 140));
	const float fx = a.x + size.x * std::clamp(fraction, 0.0f, 1.0f);
	dl.fill({a.x, a.y, fx, r.y1}, fill);
	dl.fill({a.x, a.y, fx, a.y + 3}, highlight);
	dl.stroke(r, gc::ui::rgba(255, 255, 255, 128));
	const gc::ui::Vec2 ts = gui().text_size(text);
	outlined_text(dl, {std::floor(a.x + (size.x - ts.x) / 2), std::floor(a.y + (size.y - ts.y) / 2)},
			gc::ui::rgba(255, 255, 255), text);
}

// Health as the HUD shows it: down at once when hurt, up no faster than the
// game heals (a point every 2.5 s), so the red fades evenly rather than in
// steps; a jump of more than a point (respawning, food) comes at once.
float shown_health(int health, float dt) {
	static float shown = float(gc::Game::MAX_HEALTH);
	const float target = float(health);
	if (target <= shown) {
		shown = target;
	} else {
		shown = std::min(target, std::max(shown, target - 1.0f) + dt / 2.5f);
	}
	return shown;
}

// Hurt: the screen reddens the lower health is, mostly at the edges.
void draw_wounds(gc::ui::DrawList &dl, gc::ui::Vec2 size, float health) {
	const float missing = std::clamp(1.0f - health / float(gc::Game::MAX_HEALTH), 0.0f, 1.0f);
	if (missing <= 0.0f) {
		return;
	}
	const int tint = int(110.0f * missing * missing), edge = int(220.0f * missing);
	dl.fill({0, 0, size.x, size.y}, gc::ui::rgba(150, 0, 0, tint));
	const gc::ui::Color red = gc::ui::rgba(170, 0, 0, edge), clear = gc::ui::rgba(170, 0, 0, 0);
	const float band = 0.35f * std::min(size.x, size.y) * (0.5f + missing);
	dl.gradient({0, 0, size.x, band}, red, clear, true);
	dl.gradient({0, size.y - band, size.x, size.y}, clear, red, true);
	dl.gradient({0, 0, band, size.y}, red, clear, false);
	dl.gradient({size.x - band, 0, size.x, size.y}, clear, red, false);
}

// The HUD, as the Godot version's: the crosshair, the mining progress under
// it, the 9 hotbar slots, breath (and armour) bars above them in survival, the
// held item's name, notices. Health has no bar: the screen reddens instead.
void draw_hud(const gc::Game &game, const gc::BlockTable &table, const gc::BlockRules &rules, bool captured) {
	gc::ui::Gui &g = gui();
	const gc::ui::Vec2 display{g.input().width, g.input().height};
	gc::ui::DrawList &dl = g.foreground();
	const gc::ui::Vec2 c{std::floor(display.x * 0.5f), std::floor(display.y * 0.5f)};
	const gc::ui::Color white = gc::ui::rgba(255, 255, 255);
	// Hurt: red, the more the lower health is (under the rest of the HUD, which stays readable).
	draw_wounds(dl, display, shown_health(game.health(), g.input().dt));
	if (captured) { // two bars, 16 x 2
		const gc::ui::Color col = gc::ui::rgba(255, 255, 255, 191);
		dl.fill({c.x - 8, c.y - 1, c.x + 8, c.y + 1}, col);
		dl.fill({c.x - 1, c.y - 8, c.x + 1, c.y - 1}, col);
		dl.fill({c.x - 1, c.y + 1, c.x + 1, c.y + 8}, col);
	}
	if (game.mining_progress() > 0) { // 120 x 8, under the crosshair
		const gc::ui::Rect r{c.x - 60, c.y + 22, c.x + 60, c.y + 30};
		dl.fill(r, gc::ui::rgba(26, 26, 26, 153), 3);
		dl.fill({r.x0, r.y0, r.x0 + 120 * game.mining_progress(), r.y1}, gc::ui::rgba(178, 196, 222), 3);
	}
	const float slot = 52, gap = 4;
	const float total = gc::Inventory::HOTBAR * slot + (gc::Inventory::HOTBAR - 1) * gap;
	const float x0 = std::floor((display.x - total) / 2), y0 = display.y - slot - 12;
	for (int i = 0; i < gc::Inventory::HOTBAR; i++) {
		const gc::ui::Rect r{x0 + i * (slot + gap), y0, x0 + i * (slot + gap) + slot, y0 + slot};
		dl.fill(r, gc::ui::rgba(0, 0, 0, 115));
		const gc::Stack &s = game.inventory().slots[i];
		if (!s.empty()) {
			dl.image({r.x0 + 8, r.y0 + 8, r.x1 - 8, r.y1 - 8}, table.icon(s.id));
			if (!game.creative() && s.count > 1) {
				const std::string n = std::to_string(s.count);
				const gc::ui::Vec2 size = g.text_size(n);
				outlined_text(dl, {r.x1 - 6 - size.x, r.y1 - 6 - size.y}, white, n);
			}
		}
		if (i == game.selected()) {
			dl.stroke(r, gc::ui::rgba(255, 255, 255, 242), 0, 3);
		} else {
			dl.stroke(r, gc::ui::rgba(255, 255, 255, 64));
		}
	}
	// Survival: breath on the right above the hotbar (while it's being used),
	// armour (if a mod adds some) on the left.
	const int armor = game.inventory().armor_points();
	if (!game.creative()) {
		const float width = (total - 12.0f) / 2.0f, y = y0 - 20;
		if (game.breath() < gc::Game::MAX_BREATH) {
			status_bar(dl, {x0 + total - width, y}, {width, 14}, game.breath() / gc::Game::MAX_BREATH,
					gc::ui::rgba(89, 166, 242), gc::ui::rgba(150, 199, 246), "Air");
		}
		if (armor > 0) { // full iron armour is 15 points
			status_bar(dl, {x0, y}, {width, 12}, armor / 15.0f, gc::ui::rgba(158, 163, 179), gc::ui::rgba(193, 196, 206),
					format("Armour %d", armor).c_str());
		}
	}
	// Hurt: a flash at the blow. A notice at the top.
	if (game.since_hurt() < 0.4f) {
		dl.fill({0, 0, display.x, display.y}, gc::ui::rgba(200, 0, 0, int(90 * (1.0f - game.since_hurt() / 0.4f))));
	}
	if (game.notice_time() > 0) {
		const gc::ui::Vec2 size = g.text_size(game.notice());
		const int alpha = int(255 * std::min(1.0f, game.notice_time()));
		outlined_text(dl, {std::floor(c.x - size.x / 2), 60}, gc::ui::rgba(255, 255, 255, alpha), game.notice());
	}
	// The held item's name above the hotbar (above the bars in survival).
	const gc::Stack &held = game.inventory().slots[game.selected()];
	if (!held.empty()) {
		const std::string name = gc::display_name(rules.content(), held.id);
		const gc::ui::Vec2 size = g.text_size(name);
		float bottom = y0 - 10;
		if (!game.creative() && (armor > 0 || game.breath() < gc::Game::MAX_BREATH)) {
			bottom -= 18.0f; // (above the bars)
		}
		outlined_text(dl, {std::floor(c.x - size.x / 2), bottom - size.y}, white, name);
	}
}

gc::PlayerInput read_input(const gc::Window &window) {
	gc::PlayerInput in;
	if (!window.mouse_captured()) {
		return in;
	}
	auto down = [&](const char *action) { return window.key_down(settings.key(action)); };
	in.forward = float(down("move_forward")) - float(down("move_back"));
	in.right = float(down("move_right")) - float(down("move_left"));
	in.jump = down("jump");
	in.sneak = down("sneak");
	in.sprint = down("sprint");
	return in;
}

// Top left: the Godot version's status line (frame rate, the time, mode,
// flying), and with F3 the details (its debug screen); problems with mods
// always.
void draw_ui(const gc::Renderer &renderer, const gc::Player &player, const gc::World &world,
		const gc::ChunkRenderer &chunks, const gc::WorldSave *save, uint32_t seed, const gc::Game &game,
		const gc::Mods &mods, bool captured, double day_time, bool details, const std::string &net_status) {
	gc::ui::Gui &g = gui();
	gc::ui::DrawList &fg = g.foreground();
	char clock[16];
	gc::DayCycle::clock_text(day_time, settings.clock_24h, clock, sizeof(clock));
	outlined_text(fg, {8, 8}, gc::ui::rgba(255, 255, 255),
			format("%.0f FPS · %s · %s · %s · E inventory%s", g.framerate(), clock,
					game.creative() ? "creative" : "survival", player.flying ? "flying (F)" : "walking (F to fly)",
					net_status.c_str()));
	float y = 8 + g.line_height() + 4;
	for (const auto &[id, why] : mods.problems()) {
		outlined_text(fg, {8, y}, gc::ui::rgba(255, 160, 110), "Mod " + id + ": " + why);
		y += g.line_height() + 2;
	}
	if (!captured) {
		outlined_text(fg, {8, y}, gc::ui::rgba(220, 220, 220),
				"Click to play. W A S D, Space, Shift, Ctrl, F to fly. Left click breaks, right click places, "
				"1-9 or the wheel choose a slot. F3: details");
		y += g.line_height() + 2;
	}
	if (!details) {
		return;
	}
	gc::ui::WindowOptions o;
	o.pos = {8, y + 4};
	o.alpha = 0.6f;
	o.inputs = false;
	g.begin_window("Info", o);
	const gc::WorldStats ws = world.stats();
	const gc::ChunkRenderStats &cs = chunks.stats();
	g.text(format("GoCube %s  -  %s", GOCUBE_VERSION, renderer.adapter_name().c_str()));
	g.text(format("%.0f fps (%.2f ms)  -  %s", g.framerate(), 1000.0f / std::max(g.framerate(), 0.001f), clock));
	g.text(format("Position %.1f, %.1f, %.1f%s  -  seed %u", player.position[0], player.position[1], player.position[2],
			player.flying ? "  (flying)" : "", seed));
	g.text(format("Chunks: %d loaded, %d meshed, %d drawn, %d jobs waiting", ws.loaded, ws.meshed, cs.drawn, ws.queued));
	if (save) {
		g.text(format("World: %s  (%d edited chunks not saved yet)", utf8_string(save->dir().filename()).c_str(), ws.dirty));
	} else {
		g.text_dim("World not saved (use --world=NAME), or someone else's");
	}
	g.text(format("%s  -  %d items lying around", game.creative() ? "Creative" : "Survival",
			int(game.drops().drops().size())));
	std::string names;
	for (const gc::ModInfo &m : mods.loaded()) {
		names += (names.empty() ? "" : ", ") + m.id + " " + m.version;
	}
	g.text("Mods: " + names);
	g.end_window();
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

gc::Data player_state(const gc::Player &p) {
	gc::Data s = gc::Data::object();
	s["position"] = {p.position[0], p.position[1], p.position[2]};
	s["rotation"] = {p.pitch, p.yaw};
	s["flying"] = p.flying;
	return s;
}

void restore_player(gc::Player &p, const gc::Data &s) {
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

enum class End { QUIT, MENU, FAILED, RESTART }; // RESTART: again, with the host's mods

// What to play: a world folder (empty: not saved), and for a new one its seed
// and mode; or someone else's world (join).
struct WorldChoice {
	std::filesystem::path dir;
	uint32_t seed = 777;
	std::string mode = "survival";
	int host_port = 0;    // open it to others on this port (0: not)
	std::string join;     // play in the world of the host at this address
	std::vector<std::string> only_mods; // joining again with the host's mods (Session::host_mods)
	int join_port = gc::Session::DEFAULT_PORT;
	std::string name = "Player";
};

enum class PauseChoice { NONE, RESUME, SETTINGS, QUIT };

// The pause menu (single player: the world waits behind it).
PauseChoice pause_menu(bool saved, bool from_menu) {
	gc::ui::Gui &g = gui();
	g.background().fill(screen_rect(), gc::ui::rgba(0, 0, 0, 120));
	gc::ui::WindowOptions o;
	o.pos = {g.input().width * 0.5f, g.input().height * 0.5f};
	o.pivot = {0.5f, 0.5f};
	g.begin_window("Paused", o);
	g.text("Paused", 1.4f);
	g.separator();
	g.space(2);
	PauseChoice choice = PauseChoice::NONE;
	if (g.button("Resume", {260, 36})) {
		choice = PauseChoice::RESUME;
	}
	if (g.button("Settings", {260, 36})) {
		choice = PauseChoice::SETTINGS;
	}
	const char *quit = from_menu ? (saved ? "Save and quit to title" : "Quit to title") : (saved ? "Save and quit" : "Quit");
	if (g.button(quit, {260, 36})) {
		choice = PauseChoice::QUIT;
	}
	g.text_dim("Esc to resume");
	g.end_window();
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
	std::string new_name = "New World";
	std::string new_seed;
	int new_mode = 0; // 0 survival, 1 creative
	std::string player_name = "Player";
	bool open_to_others = false;
	std::string join_address = "127.0.0.1";
	bool loaded_names = false;
	bool in_settings = false; // the settings screen, over the menu
	gc::SettingsScreen settings_screen;
	bool esc_was_down = false;
};

// Where the settings screen finds mods.
gc::SettingsScreen::ModFolders mod_folders() {
	return {find_mods(), user_dir() / "mods", user_dir() / "server_mods"};
}

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
		gc::Data meta;
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
		if (!menu.loaded_names) { // (the last ones used)
			menu.loaded_names = true;
			menu.player_name = settings.player_name.substr(0, 31);
			menu.join_address = settings.last_address.substr(0, 63);
		}
		begin_ui(window);
		const bool esc_down = window.key_down(VK_ESCAPE);
		const bool esc = esc_down && !menu.esc_was_down;
		menu.esc_was_down = esc_down;
		if (menu.in_settings) {
			menu.in_settings = menu.settings_screen.draw(gui(), settings, window, esc, settings_file());
			renderer.set_vsync(!options.no_vsync && settings.vsync);
			renderer.set_render_scale(render_scale(options));
			sky_backdrop();
			end_ui();
			const float clear[4] = {0.18f, 0.32f, 0.62f, 1.0f};
			renderer.begin_frame(clear);
			renderer.end_frame(ui_overlay(renderer));
			continue;
		}
		gc::ui::Gui &g = gui();
		const gc::ui::Color warn = gc::ui::rgba(255, 153, 102);
		gc::ui::WindowOptions o;
		o.pos = {g.input().width * 0.5f, g.input().height * 0.5f};
		o.pivot = {0.5f, 0.5f};
		o.size = {std::min(760.0f, g.input().width - 40), std::min(680.0f, g.input().height - 40)};
		g.begin_window("GoCube", o);
		g.text("GoCube", 2.0f);
		g.text_dim(GOCUBE_VERSION);
		g.same_line(g.window_width() - 2 * g.theme.padding.x - 110);
		if (g.button("Settings", {110, 0})) {
			// (The mods as they'd load now, for its list of them.)
			gc::Content content;
			gc::Mods mods(content);
			mods.set_disabled(settings.disabled_mods);
			mods.load_all({find_mods(), user_dir() / "mods"});
			menu.settings_screen.open(mod_folders(), mods, false);
			menu.in_settings = true;
		}
		g.separator();
		g.begin_child("menu", {0, 0}, false); // (scrolls, on a small window)

		g.text("Worlds");
		g.begin_child("worlds", {0, 140}, true);
		for (int i = 0; i < int(menu.worlds.size()); i++) {
			const WorldEntry &w = menu.worlds[i];
			g.push_id(i);
			if (g.selectable(w.name + "##world", menu.selected == i)) {
				menu.selected = i;
				menu.confirm_delete = false;
				if (g.double_clicked()) {
					choice = WorldChoice{w.dir, 0, w.mode};
				}
			}
			g.same_line(300);
			g.text(w.mode + "  -  seed " + w.seed + "  -  " + w.saved, g.theme.text_dim);
			g.pop_id();
		}
		if (menu.worlds.empty()) {
			g.text_dim("No worlds yet: create one below.");
		}
		g.end_child();
		const bool has = menu.selected >= 0 && menu.selected < int(menu.worlds.size());
		g.begin_disabled(!has);
		if (g.button("Play", {120, 32}) && has) {
			choice = WorldChoice{menu.worlds[menu.selected].dir, 0, menu.worlds[menu.selected].mode};
		}
		g.same_line();
		if (!menu.confirm_delete) {
			if (g.button("Delete...", {120, 32})) {
				menu.confirm_delete = true;
			}
		} else if (has) {
			g.align_text_to_frame();
			g.text("Move \"" + menu.worlds[menu.selected].name + "\" to the Recycle Bin?", warn);
			g.same_line();
			if (g.button("Delete")) {
				if (!to_recycle_bin(menu.worlds[menu.selected].dir)) {
					problem = "The world couldn't be moved to the Recycle Bin.";
				}
				menu.confirm_delete = false;
				menu.refresh = true;
			}
			g.same_line();
			if (g.button("Keep it")) {
				menu.confirm_delete = false;
			}
		}
		g.end_disabled();

		g.space(8);
		g.separator();
		g.text("New world");
		g.next_width(260);
		g.input_text("Name", menu.new_name, 63);
		g.next_width(260);
		g.input_text("Seed", menu.new_seed, 20, "random", true);
		g.next_width(260);
		g.combo("Mode", menu.new_mode, {"Survival", "Creative"});
		const std::string why = name_problem(menu.new_name);
		g.begin_disabled(!why.empty());
		if (g.button("Create and play", {200, 32}) && why.empty()) {
			WorldChoice c;
			c.dir = user_dir() / "worlds" / utf8_path(menu.new_name.c_str());
			c.seed = !menu.new_seed.empty() ? uint32_t(strtoull(menu.new_seed.c_str(), nullptr, 10))
			                                : uint32_t(std::random_device{}());
			c.mode = menu.new_mode == 1 ? "creative" : "survival";
			choice = c;
		}
		g.end_disabled();
		if (!why.empty()) {
			g.same_line();
			g.align_text_to_frame();
			g.text_dim(why);
		}

		g.space(8);
		g.separator();
		g.text("Playing with others");
		g.next_width(260);
		g.input_text("Your name", menu.player_name, 31);
		g.checkbox("Open the world I play to others (port 24680)", menu.open_to_others);
		g.next_width(260);
		g.input_text("Address##join", menu.join_address, 63);
		g.same_line();
		g.begin_disabled(menu.join_address.empty());
		if (g.button("Join their world", {160, 0}) && !menu.join_address.empty()) {
			WorldChoice c;
			c.join = menu.join_address;
			if (const size_t colon = c.join.rfind(':'); colon != std::string::npos) {
				c.join_port = int(strtol(c.join.c_str() + colon + 1, nullptr, 10));
				c.join.resize(colon);
			}
			choice = c;
		}
		g.end_disabled();
		if (!problem.empty()) {
			g.text_wrapped(problem, warn);
		}
		g.end_child();
		g.end_window();
		sky_backdrop();
		end_ui();
		const float clear[4] = {0.18f, 0.32f, 0.62f, 1.0f};
		renderer.begin_frame(clear);
		const bool last_frame = options.frames > 0 && ++frame >= options.frames; // (testing)
		renderer.end_frame(ui_overlay(renderer), last_frame && !options.screenshot.empty() ? options.screenshot.c_str() : nullptr);
		if (choice) {
			choice->name = menu.player_name;
			settings.player_name = menu.player_name; // (kept for next time)
			if (!choice->join.empty()) {
				settings.last_address = menu.join_address;
			}
			if (!options.hidden) {
				settings.save(settings_file());
			}
			if (choice->join.empty() && menu.open_to_others) {
				choice->host_port = gc::Session::DEFAULT_PORT;
			}
			return true;
		}
		if (last_frame) {
			return false;
		}
	}
	return false;
}

// Plays a world until the player quits (to the title menu, or the game).
End play(const Options &options, WorldChoice &choice, bool from_menu, gc::Window &window,
		gc::Renderer &renderer, std::string &problem) {
	// Content: the mods (the base game first).
	const std::filesystem::path builtin_mods = find_mods();
	if (builtin_mods.empty()) {
		gc::log("Couldn't find the mods folder (with the base game, mods\\core)");
		return End::FAILED;
	}
	gc::Content content;
	gc::Mods mods(content);
	// (Joining with the host's mods: exactly those, downloaded ones included.)
	mods.set_disabled(settings.disabled_mods); // (not the host's, when it gives them)
	if (choice.only_mods.empty()) {
		mods.load_all({builtin_mods, user_dir() / "mods"});
	} else {
		mods.load_all({builtin_mods, user_dir() / "mods", user_dir() / "server_mods"}, &choice.only_mods);
	}
	if (mods.loaded().empty() || mods.loaded().front().id != "core") {
		auto it = mods.problems().find("core");
		gc::log("Couldn't load the base game: %s", it != mods.problems().end() ? it->second.c_str() : "it's missing");
		return End::FAILED;
	}

	// Multiplayer: someone else's world comes from its host (after a welcome).
	gc::Session session;
	const bool joining = !choice.join.empty();
	if (joining) {
		if (std::string e = session.join(choice.join, choice.join_port, choice.name, gc::Session::mod_list(mods.loaded()));
				!e.empty()) {
			problem = e;
			return End::FAILED;
		}
		session.set_local_mods(gc::mod_copies({user_dir() / "mods", user_dir() / "server_mods"}));
		session.set_restarted_with(choice.only_mods);
		bool cancelled = false, downloading = false;
		while (!session.welcomed() && session.ended().empty() && !cancelled) {
			if (downloading && session.mods_ready()) {
				// Every mod of the host's is here: again, with exactly those.
				choice.only_mods = session.host_mods();
				return End::RESTART;
			}
			if (!window.pump()) {
				return End::QUIT;
			}
			if (window.take_resized()) {
				renderer.resize(window.width(), window.height());
			}
			session.poll();
			begin_ui(window);
			gc::ui::Gui &g = gui();
			gc::ui::WindowOptions o;
			o.pos = {g.input().width * 0.5f, g.input().height * 0.5f};
			o.pivot = {0.5f, 0.5f};
			g.begin_window("Joining", o);
			if (!session.mods_offered()) {
				g.text("Connecting to " + choice.join + "...");
			} else if (!downloading) {
				// Same game, other mods: play with the host's (downloading what we lack)?
				size_t bytes = 0;
				for (const auto &[entry, size] : session.missing_mods()) {
					bytes += size;
				}
				g.text(choice.join + " plays with other mods:");
				for (const std::string &e : session.host_mods()) {
					const bool missing = session.missing_mods().count(e) > 0;
					g.bullet(e.substr(0, e.find('#')) + (missing ? "  (to download)" : ""));
				}
				g.text_dim("Mods run in the game's sandbox. They're kept in %APPDATA%\\GoCube\\server_mods.");
				const std::string label = session.missing_mods().empty() ? "Play with these mods"
						: "Download (" + std::to_string(std::max<size_t>(1, bytes >> 10)) + " KB) and play with them";
				if (g.button(label, {320, 32}) || options.accept_mods) {
					session.download_mods(user_dir() / "server_mods");
					downloading = true;
				}
				g.same_line();
			} else {
				g.text(format("Downloading the server's mods: %zu KB...", session.downloaded_bytes() >> 10));
			}
			cancelled = g.button("Cancel", {160, 32});
			g.end_window();
			sky_backdrop();
			end_ui();
			const float clear[4] = {0.18f, 0.32f, 0.62f, 1.0f};
			renderer.begin_frame(clear);
			renderer.end_frame(ui_overlay(renderer));
		}
		if (!session.welcomed()) {
			problem = cancelled ? "" : session.ended();
			return cancelled ? End::MENU : End::FAILED;
		}
	}
	const gc::Data &welcome = session.welcome();

	// The world: its folder decides the seed, generator and block ids.
	std::optional<gc::WorldSave> save;
	const std::filesystem::path &dir = choice.dir;
	uint32_t seed = choice.seed;
	int generator_version = options.generator;
	if (joining) {
		seed = uint32_t(welcome.value("seed", 0.0));
		generator_version = int(welcome.value("generator", double(gc::GENERATOR_VERSION)));
		std::vector<std::string> ids;
		for (const gc::Data &id : welcome.value("block_ids", gc::Data::array())) {
			ids.push_back(id.is_string() ? id.get<std::string>() : "");
		}
		content.freeze(ids); // (the host's block ids)
		if (content.block_ids().size() < ids.size() || !std::equal(ids.begin(), ids.end(), content.block_ids().begin())) {
			problem = "The server's blocks differ from ours.";
			return End::FAILED;
		}
		gc::log("Joined %s: seed %u, generator %d", choice.join.c_str(), seed, generator_version);
	} else if (!dir.empty()) {
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
	the_ui->d2d.set_icons(table.atlas); // (item icons)
	if (!chunks.init(renderer.device(), table.atlas)) {
		gc::log("Couldn't set up the terrain renderer");
		return End::FAILED;
	}

	auto world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator,
			save ? utf8_string(save->chunks_dir()) : std::string(), settings.worker_threads);
	world->set_view_distance(view_distance(options));
	mods.attach(world.get(), &rules);

	gc::Player player;
	player.position[0] = 0.5f;
	player.position[1] = float(generator.height_at(0, 0)) + 1.0f; // on the generated ground
	player.position[2] = 0.5f;
	if (save) {
		restore_player(player, save->player());
	}
	if (joining) { // where the host put us
		const gc::Data spawn = welcome.value("spawn", gc::Data::array());
		if (spawn.is_array() && spawn.size() == 3 && spawn[0].is_number()) {
			player.position[0] = spawn[0].get<float>();
			player.position[1] = spawn[1].get<float>() - gc::Player::EYE_HEIGHT;
			player.position[2] = spawn[2].get<float>();
		}
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
	const bool creative = (joining ? welcome.value("mode", "survival") : save ? save->mode() : choice.mode) == "creative";
	gc::Game game(content, table, rules, *world, mods, player, creative);
	game.set_generator(&generator);
	if (save) {
		game.load_player(save->player());
		game.block_entities().deserialize(save->field("block_entities"));
		game.set_world_spawn(save->field("spawn"));
		game.load_plants(save->field("plants"));
		const gc::Data &first_night = save->field("first_night");
		game.set_first_night(first_night.is_string() ? first_night.get<std::string>() : "over");
	} else {
		game.start_new_player();
	}
	if (joining) {
		game.set_world_spawn(welcome.value("world_spawn", gc::Data()));
		game.set_first_night(welcome.value("first_night", "over"));
	}
	if (!options.has_pos && !joining && !(save && save->player().contains("position"))) {
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
	for (size_t i = 0; i < options.spawn.size(); i++) { // (for trying things out)
		// "block_eater" or "block_eater=core:stone": a core kind, and what it's made of.
		std::string kind = options.spawn[i], variant;
		if (const size_t eq = kind.find('='); eq != std::string::npos) {
			variant = kind.substr(eq + 1);
			kind = kind.substr(0, eq);
		}
		if (kind.find(':') == std::string::npos) {
			kind = "core:" + kind;
		}
		float look[3];
		player.look(look);
		const float flat = std::max(0.2f, std::hypot(look[0], look[2]));
		const float side = (float(i) - float(options.spawn.size() - 1) * 0.5f) * 1.5f;
		const float fx = look[0] / flat, fz = look[2] / flat;
		const float at[3] = {player.position[0] + fx * 5.0f - fz * side, player.position[1],
				player.position[2] + fz * 5.0f + fx * side};
		if (game.creatures().spawn(content, kind, at, variant) == 0) {
			gc::log("--spawn: no creature '%s'", kind.c_str());
		}
	}

	gc::Camera camera;
	camera.far_z = float(view_distance(options)) * 2.0f;
	gc::SettingsScreen settings_screen;
	if (screen == Screen::NONE && options.screen == "settings") { // (screenshots)
		settings_screen.open(mod_folders(), mods, !choice.only_mods.empty());
		screen = Screen::SETTINGS;
		window.set_mouse_captured(false);
	}
	gc::ItemModels item_models(content, table);
	gc::Audio audio; // (silent in tests: --hidden)
	if (!options.no_sound && (!options.hidden || options.sound)) {
		audio.init(content, options.hidden ? 0.0f : 1.0f);
	}
	gc::Hand hand;
	struct Puff {
		float pos[3], vel[3], size, age;
	};
	std::vector<Puff> puffs;
	std::mt19937 puff_random{std::random_device{}()};
	gc::HandLights hand_lights;
	int seen_swings = game.swings();
	gc::SkyParams sky;
	gc::DayCycle day;
	day.time = gc::DayCycle::system_time(); // a new world starts at the computer's clock
	if (save) {
		if (const gc::Data &t = save->field("time"); t.is_number()) {
			day.time = std::fmod(t.get<double>(), 1.0);
		}
		if (const gc::Data &l = save->field("day_length"); l.is_number()) {
			day.day_length = std::max(10.0, l.get<double>());
		}
	}
	if (options.time >= 0) {
		day.time = std::fmod(options.time, 1.0);
	}
	if (joining) {
		day.time = welcome.value("time", day.time);
		day.day_length = std::max(10.0, welcome.value("day_length", day.day_length));
	}
	// Multiplayer: as a client (the host does what we do to the world), or
	// the host (open to others).
	gc::Session::Attach attach{&game, world.get(), &player, &content, &rules, &mods, &day};
	std::string net_problem;
	if (joining) {
		session.attach(attach);
	} else if (choice.host_port > 0) {
		if (std::string e = session.host(choice.host_port, choice.name, gc::Session::mod_list(mods.loaded())); !e.empty()) {
			net_problem = e;
			gc::log("%s", e.c_str());
		} else {
			session.set_world_info({{"seed", seed}, {"generator", generator_version}, {"block_ids", content.block_ids()}});
			std::map<std::string, std::filesystem::path> folders; // (players with other mods may download ours)
			for (const gc::ModInfo &m : mods.loaded()) {
				folders[gc::mod_entry(m)] = m.dir;
			}
			session.set_mod_folders(std::move(folders));
			if (save) {
				session.set_saved_players(save->field("players"));
			}
			session.attach(attach);
		}
	}
	const bool online = session.mode() != gc::Session::Mode::OFFLINE;
	float sleeping = -1; // seconds asleep (-1: awake)
	double frame_time_total = 0; // (mining: the arm keeps swinging for others to see)
	// Block shadows (the Godot version's default sun shadows), from where the sun is.
	world->set_sun_angle(day.angle());
	world->set_block_shadows(true);

	auto save_all = [&] {
		if (save) {
			const int chunks_saved = world->save_modified();
			gc::Data state = player_state(player);
			state.update(game.save_player());
			save->set_field("block_entities", game.block_entities().serialize());
			save->set_field("time", day.time);
			save->set_field("first_night", game.first_night());
			save->set_field("plants", game.save_plants());
			if (session.is_host()) {
				save->set_field("players", session.saved_players()); // joined players' inventories
			}
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
		session.poll();
		if (joining && !session.ended().empty()) {
			problem = session.ended(); // (the host left, or we lost it)
			end = End::FAILED;
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
		const bool key_e = controls.pressed(window, settings.key("inventory"));
		const bool key_esc = controls.pressed(window, VK_ESCAPE);
		auto close_screen = [&] {
			game.inventory_action(gc::Data::array({"close"})); // (what the mouse held goes back in, or is dropped)
			if (screen == Screen::CONTAINER && joining) {
				session.close_container(container);
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
		} else if (screen == Screen::SETTINGS) {
			// (Esc closes it, back to the pause menu: the screen sees it.)
		} else if (key_e || key_esc) {
			close_screen();
		}
		const bool paused = (screen == Screen::PAUSE || screen == Screen::SETTINGS) && !online; // (single player: the world waits)
		const bool captured = window.mouse_captured() && screen == Screen::NONE;
		if (left && !window.mouse_captured() && screen == Screen::NONE && !gui().wants_mouse()) {
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
		const float sensitivity = settings.mouse_sensitivity; // radians per mouse count
		player.yaw -= mx * sensitivity;
		player.pitch = std::clamp(player.pitch - my * sensitivity, -1.55f, 1.55f);
		const bool fly_key = captured && window.key_down(settings.key("toggle_fly"));
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
		if (auto eye = session.take_correction()) { // the host put us back
			player.position[0] = (*eye)[0];
			player.position[1] = (*eye)[1] - gc::Player::EYE_HEIGHT;
			player.position[2] = (*eye)[2];
			std::fill(player.velocity, player.velocity + 3, 0.0f);
			std::copy(player.position, player.position + 3, previous_position);
		}
		if (auto push = session.take_push()) { // a creature's knock back
			for (int k = 0; k < 3; k++) {
				player.velocity[k] += (*push)[k];
			}
		}
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
			game.set_day(day.day_amount());
			game.update_first_night();
		}
		// A bed used at night: sleep (the screen fades out), then it's morning.
		if (game.take_bed_used()) {
			if (day.day_amount() > 0.3f) {
				game.show_notice("You can only sleep at night (respawn point set)");
			} else {
				sleeping = 0;
				session.set_sleeping(true); // (a client: the host skips the night once everyone sleeps)
			}
		}
		if (sleeping >= 0) {
			sleeping += dt;
			// The night is skipped when everyone is in bed (the host decides;
			// a client gets up when it says so).
			if (sleeping >= 2.5f && !joining && session.peers_sleeping()) {
				day.time = gc::DayCycle::MORNING;
				sleeping = -1;
				if (game.first_night() != "over") {
					game.set_first_night("over"); // slept through it
				}
				session.night_skipped();
			}
			if (online && sleeping >= 2.5f && sleeping < 2.5f + dt) {
				game.show_notice("Waiting for everyone to sleep...");
			}
		}
		if (session.take_woken()) {
			sleeping = -1;
		}
		if (online && sleeping >= 0 && (window.key_down('W') || window.key_down('S') || window.key_down(VK_SPACE))) {
			sleeping = -1; // got up
			session.set_sleeping(false);
		}
		if (auto opened = game.take_opened()) {
			container = *opened;
			screen = Screen::CONTAINER;
			window.set_mouse_captured(false);
			if (joining) {
				session.open_container(container); // (its contents come from the host)
			}
		}
		tick_accumulator += paused ? 0.0 : dt;
		while (tick_accumulator >= TICK) {
			if (!joining) { // (a client: the host's mods run the world)
				mods.tick(TICK);
			}
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
		// How others see us, and theirs: their held lights light the world too.
		gc::LocalPose pose;
		player.eye(pose.eye);
		pose.yaw = player.yaw;
		pose.pitch = player.pitch;
		pose.item = game.selected_item();
		pose.selected = game.selected();
		pose.flags = (window.key_down(VK_SHIFT) && !player.flying ? gc::Session::FLAG_SNEAK : 0) |
				(sleeping >= 0 ? gc::Session::FLAG_SLEEP : 0) | (player.flying ? gc::Session::FLAG_FLY : 0);
		pose.swings = game.swings() + (game.mining_progress() > 0 ? int(frame_time_total * 3.0) : 0);
		pose.light = held_lights.empty() ? 0 : int(held_lights.front()[3]);
		frame_time_total += dt;
		session.update(dt, pose);
		for (const auto &l : session.held_lights()) {
			held_lights.push_back(l);
		}
		const bool hand_light_changed = hand_lights.update(*world, held_lights, eye);

		// Sounds: heard from the camera; muffled with the head in a fluid, which
		// also tints the view (blue in water, orange in lava).
		const gc::Data &eye_block = rules.def(world->block_at(int(std::floor(eye[0])), int(std::floor(eye[1])),
				int(std::floor(eye[2]))));
		const bool in_fluid = eye_block.contains("fluid");
		const bool in_lava = in_fluid && eye_block.value("fluid", "") == "lava";
		const DirectX::XMFLOAT3 facing = camera.forward();
		const float forward[3] = {facing.x, facing.y, facing.z};
		audio.set_listener(eye, forward);
		for (const gc::SoundEvent &e : game.take_sounds()) {
			audio.play(e.set, e.kind, e.at, e.volume_db);
		}
		audio.set_volumes(options.hidden ? 0.0f : settings.master_volume, settings.effects_volume,
				settings.ambience_volume);
		audio.update_ambience(day.day_amount());
		audio.set_underwater(in_fluid);
		audio.update();

		// The settings, as they are now (the settings screen changes them live).
		world->set_view_distance(view_distance(options));
		camera.far_z = float(view_distance(options)) * 2.0f;
		world->set_smooth_lighting(settings.smooth_lighting);
		world->set_block_shadows(settings.shadow_mode == "blocks");
		renderer.set_render_scale(render_scale(options));
		renderer.set_vsync(!options.no_vsync && settings.vsync);
		world->set_sun_angle(day.angle()); // (shadows follow it in steps)
		world->update(player.position[0], player.position[1], player.position[2]);
		for (const gc::ChunkPos &p : world->take_unloaded()) {
			chunks.remove(p);
		}
		for (const gc::ChunkMesh &m : world->take_meshes()) {
			chunks.upload(renderer.device(), m);
		}
		std::string net_status;
		if (session.is_host()) {
			net_status = " \u00b7 open to others (port " + std::to_string(choice.host_port) + ") \u00b7 " +
					std::to_string(session.player_count()) + " playing";
		} else if (joining) {
			net_status = " \u00b7 in " + choice.join + "'s world \u00b7 " + std::to_string(session.player_count()) + " playing";
		} else if (!net_problem.empty()) {
			net_status = " \u00b7 " + net_problem;
		}
		if (save && std::chrono::duration<float>(now - last_save).count() >= 60.0f) {
			save_all(); // autosave
			last_save = now;
		}

		begin_ui(window);
		if (screen == Screen::NONE) {
			draw_ui(renderer, player, *world, chunks, save ? &*save : nullptr, seed,
					game, mods, window.mouse_captured(), day.time, controls.details, net_status);
		}
		const gc::UiIcons icons{&table, &content};
		if (game.dead()) {
			if (window.mouse_captured()) {
				window.set_mouse_captured(false);
			}
			if (death_screen()) {
				game.respawn();
				window.set_mouse_captured(true);
			}
		} else if (screen == Screen::INVENTORY && !gc::inventory_screen(gui(), game, icons)) {
			close_screen();
		} else if (screen == Screen::CONTAINER && !gc::container_screen(gui(), game, container, icons)) {
			close_screen();
		} else if (screen == Screen::PAUSE) {
			switch (pause_menu(save.has_value(), from_menu)) {
				case PauseChoice::RESUME:
					screen = Screen::NONE;
					window.set_mouse_captured(true);
					break;
				case PauseChoice::SETTINGS:
					settings_screen.open(mod_folders(), mods, !choice.only_mods.empty());
					screen = Screen::SETTINGS;
					break;
				case PauseChoice::QUIT:
					end = from_menu ? End::MENU : End::QUIT;
					quit_requested = true;
					break;
				default:
					break;
			}
		} else if (screen == Screen::SETTINGS && !settings_screen.draw(gui(), settings, window, key_esc, settings_file())) {
			screen = Screen::PAUSE; // (saved; back to the pause menu)
		}
		if (in_fluid) { // (under the HUD)
			gui().background().fill(screen_rect(), in_lava ? gc::ui::rgba(230, 89, 13, 191) : gc::ui::rgba(26, 77, 140, 115));
		}
		{ // Name tags over other players' heads.
			const float tag_aspect = float(renderer.width()) / float(std::max(1, renderer.height()));
			const DirectX::XMMATRIX view_projection = camera.view() * camera.projection(tag_aspect);
			const gc::ui::Vec2 display{gui().input().width, gui().input().height};
			for (const gc::Avatar &a : session.avatars()) {
				const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
						DirectX::XMVectorSet(a.shown[0], a.shown[1] + 0.55f, a.shown[2], 1), view_projection);
				const float w = DirectX::XMVectorGetW(clip);
				const float dx = a.shown[0] - camera.position.x, dy = a.shown[1] - camera.position.y,
							dz = a.shown[2] - camera.position.z;
				if (w <= 0.1f || dx * dx + dy * dy + dz * dz > 64.0f * 64.0f) {
					continue; // behind us, or far
				}
				const float sx = (DirectX::XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x;
				const float sy = (0.5f - DirectX::XMVectorGetY(clip) / w * 0.5f) * display.y;
				const gc::ui::Vec2 size = gui().text_size(a.name);
				outlined_text(gui().background(), {std::floor(sx - size.x * 0.5f), std::floor(sy - size.y)},
						gc::ui::rgba(255, 255, 255), a.name);
			}
		}
		if (screen != Screen::SETTINGS) { // (it covers the screen)
			draw_hud(game, table, rules, captured);
		}
		if (sleeping >= 0) {
			gui().foreground().fill(screen_rect(), gc::ui::rgba(0, 0, 0, int(230 * std::min(1.0f, sleeping / 1.5f))));
		}
		gc::draw_held(gui(), game.inventory().cursor, icons);
		end_ui();

		sky.time = std::chrono::duration<float>(now - start).count();
		if (!options.screenshot.empty()) {
			// The clouds (and their shadows) as 3 seconds in, however long
			// loading took: the same in every screenshot (and in
			// tests/data/godot_screenshot.gd's).
			day.cloud_offset[0] = 0.35f * 3.0f;
			day.cloud_offset[1] = 0.12f * 3.0f;
		}
		apply_day(day, float(view_distance(options)), sky);
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
		{ // Creatures, lit by where they are.
			std::vector<gc::CreatureVertex> creature_mesh;
			for (const gc::Creature &c : game.creatures().all()) {
				const float at[3] = {c.pos[0], c.pos[1] + c.light_height, c.pos[2]};
				float lit[3];
				gc::light_for(*world, hand_lights, sky_light, at, NIGHT_CREATURE_LIGHT, lit);
				gc::build_creature(c, content, table, alpha, lit, creature_mesh);
			}
			// Other players: their figures, what they hold, their names.
			std::vector<gc::WorldVertex> held_by_others;
			for (const gc::Avatar &a : session.avatars()) {
				const float at[3] = {a.shown[0], a.shown[1] - 0.6f, a.shown[2]};
				float lit[3];
				gc::light_for(*world, hand_lights, sky_light, at, NIGHT_CREATURE_LIGHT, lit);
				float hand_m[16];
				gc::build_player(a.figure, a.id, a.shown, a.shown_yaw, lit, creature_mesh, hand_m);
				if (!a.item.empty()) {
					// A block: a small cube in the fist; a flat item held by its
					// lower corner, tip forward and up (the Godot version's Avatar).
					const bool block = content.blocks.count(a.item) > 0 && rules.is_placeable(a.item) &&
							rules.def(gocube::BlockId(std::max(0, content.block_index(a.item)))).value("shape", "") != "cross";
					const DirectX::XMMATRIX local = block
							? DirectX::XMMatrixScaling(0.9f, 0.9f, 0.9f) * DirectX::XMMatrixRotationY(0.6f) *
									DirectX::XMMatrixTranslation(0, -0.08f, -0.06f)
							: DirectX::XMMatrixScaling(0.85f, 0.85f, 0.85f) * DirectX::XMMatrixRotationY(1.5708f) *
									DirectX::XMMatrixTranslation(0, 0.1f, -0.12f);
					const DirectX::XMMATRIX m = local * DirectX::XMMATRIX(hand_m);
					for (const gc::ModelVertex &mv : item_models.model(a.item)) {
						gc::WorldVertex w;
						DirectX::XMFLOAT3 p;
						DirectX::XMStoreFloat3(&p, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(mv.pos[0], mv.pos[1], mv.pos[2], 1), m));
						w.pos[0] = p.x;
						w.pos[1] = p.y;
						w.pos[2] = p.z;
						std::copy(mv.normal, mv.normal + 3, w.normal); // (shaded by the model's own, as drops)
						std::copy(mv.uv, mv.uv + 2, w.uv);
						std::copy(lit, lit + 3, w.color);
						w.color[3] = 1;
						held_by_others.push_back(w);
					}
				}
			}
			chunks.draw_creatures(renderer.context(), creature_mesh, camera.view() * camera.projection(aspect),
					camera.position, sky);
			if (!held_by_others.empty()) {
				chunks.draw_items(renderer.context(), held_by_others, camera.view() * camera.projection(aspect),
						camera.position, sky, true);
			}
			// Puffs of smoke (a ninja caught by the sun): a burst of grey cubes
			// rising for 0.9 s (the Godot version's CPUParticles3D).
			for (const gc::Effect &e : game.take_effects()) {
				if (e.name != "smoke") {
					continue;
				}
				std::uniform_real_distribution<float> unit(0.0f, 1.0f);
				for (int i = 0; i < 24; i++) {
					Puff p;
					std::copy(e.at, e.at + 3, p.pos);
					const float spread = 70.0f * 3.14159265f / 180.0f * std::sqrt(unit(puff_random));
					const float turn = unit(puff_random) * 6.2831853f;
					const float speed = 1.0f + 1.5f * unit(puff_random);
					p.vel[0] = std::sin(spread) * std::cos(turn) * speed;
					p.vel[1] = std::cos(spread) * speed;
					p.vel[2] = std::sin(spread) * std::sin(turn) * speed;
					p.size = 0.2f + 0.25f * unit(puff_random);
					p.age = -0.045f * unit(puff_random); // (nearly all at once)
					puffs.push_back(p);
				}
			}
			std::vector<gc::CreatureVertex> puff_mesh;
			const float smoke[3] = {0.82f, 0.82f, 0.86f};
			for (size_t i = 0; i < puffs.size();) {
				Puff &p = puffs[i];
				p.age += paused ? 0.0f : dt;
				if (p.age >= 0.9f) {
					puffs.erase(puffs.begin() + std::ptrdiff_t(i));
					continue;
				}
				if (p.age > 0 && !paused) {
					p.vel[1] += 0.8f * dt; // it rises
					for (int k = 0; k < 3; k++) {
						p.pos[k] += p.vel[k] * dt;
					}
				}
				if (p.age > 0) {
					gc::build_cube(p.pos, p.size, smoke, puff_mesh);
				}
				i++;
			}
			chunks.draw_creatures(renderer.context(), puff_mesh, camera.view() * camera.projection(aspect),
					camera.position, sky);
		}
		if (const gc::RayHit hit = player.target(*world); hit.hit && game.aimed_creature() == 0) {
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
		if (!renderer.end_frame(ui_overlay(renderer), last_frame && !options.screenshot.empty() ? options.screenshot.c_str() : nullptr)) {
			end = End::FAILED;
			break;
		}
		if (last_frame || quit_requested) {
			break;
		}
		if (settings.max_fps > 0) { // the frame rate limit
			std::this_thread::sleep_until(now + std::chrono::microseconds(1000000 / settings.max_fps));
		}
	}

	save_all();
	session.leave();
	mods.attach(nullptr, nullptr);
	window.set_mouse_captured(false);
	// (The world's workers stop when it goes, after the game, which uses it:
	// both at the end of this scope, in that order.)
	return end;
}

int main(int argc, char **argv) {
	const gc::FineSleep fine; // (the frame rate limit sleeps to the millisecond)
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const Options options = parse(argc, argv);
	if (!options.hidden) { // (tests play with the defaults)
		std::string why;
		if (!settings.load(settings_file(), &why)) {
			gc::log("Can't read the settings (%s); using the defaults", why.c_str());
		}
	}

	gc::Window window;
	if (!window.create(L"GoCube", 1280, 720, !options.hidden)) {
		gc::log("Couldn't create the window");
		return 1;
	}
	gc::Renderer renderer;
	gc::RendererOptions renderer_options = options.renderer;
	renderer_options.vsync = !options.no_vsync && settings.vsync;
	if (!renderer.init(window.hwnd(), window.width(), window.height(), renderer_options)) {
		gc::log("Couldn't start Direct3D 11");
		return 1;
	}
	renderer.set_render_scale(render_scale(options));
	Ui ui; // (before the renderer goes: it draws with its device)
	the_ui = &ui;
	if (!ui.d2d.init(renderer.device())) {
		gc::log("Couldn't start Direct2D for the UI");
		return 1;
	}
	ui.gui.clipboard = [&window] { return window.clipboard_text(); };

	bool ok = true;
	// Started for a world (or for testing): straight into it.
	const bool direct = !options.menu && (!options.world_name.empty() || !options.world_dir.empty() || options.hidden ||
			options.frames > 0 || options.has_pos || options.host_port > 0 || !options.join.empty());
	if (direct) {
		WorldChoice choice;
		choice.dir = options.world_dir;
		if (choice.dir.empty() && !options.world_name.empty()) {
			choice.dir = user_dir() / "worlds" / utf8_path(options.world_name.c_str());
		}
		choice.seed = options.seed;
		choice.mode = options.mode;
		choice.host_port = options.host_port;
		choice.join = options.join;
		choice.join_port = options.join_port;
		choice.name = options.name;
		std::string problem;
		End end;
		while ((end = play(options, choice, false, window, renderer, problem)) == End::RESTART) {
		}
		ok = end != End::FAILED;
		if (!problem.empty()) {
			gc::log("%s", problem.c_str());
		}
	} else {
		TitleMenu menu;
		std::string problem;
		while (true) {
			std::optional<WorldChoice> choice;
			if (!title_menu(options, window, renderer, menu, problem, choice)) {
				break; // the window closed
			}
			problem.clear();
			End end;
			while ((end = play(options, *choice, true, window, renderer, problem)) == End::RESTART) {
			}
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
	the_ui = nullptr;
	renderer.shutdown();
	if (messages > 0) {
		gc::log("The Direct3D 11 debug layer reported %d errors or warnings", messages);
	}
	return ok && messages == 0 ? 0 : 1;
}
