#pragma once
// The player's settings (the Godot version's Settings): view distance, mouse
// sensitivity, frame rate limit, vsync, smooth lighting, sun shadows, render
// scale, the clock, volumes, key bindings, mods turned off, and the title
// menu's name and last address. Saved in %APPDATA%\GoCube\settings.lua (a Lua
// data table); what isn't there, or isn't valid, is the default. The settings
// screen (ui/settings_screen.h) changes them live; the game reads them every
// frame.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gc {

struct Settings {
	static constexpr int VIEW_DISTANCES[] = {64, 96, 128, 160, 192, 256};
	static constexpr int FPS_LIMITS[] = {30, 60, 120, 144, 0}; // 0: unlimited
	// The world is drawn at this part of the window's resolution and
	// stretched over it (menus and the HUD stay sharp): less uses less GPU.
	static constexpr float RENDER_SCALES[] = {1.0f, 0.85f, 0.75f, 0.5f};
	// Sun shadows: "blocks" (each block face in sun or shadow, worked out as
	// the sun moves) or "off". (The Godot version's "smooth", its shadow map,
	// isn't here.)
	static constexpr const char *SHADOW_MODES[] = {"blocks", "off"};

	// A rebindable action: its id (saved), what the screen calls it, its
	// default key (a Windows virtual-key code).
	struct Action {
		const char *id, *label;
		int key;
	};
	static const std::vector<Action> &actions();

	int view_distance = 128;
	float mouse_sensitivity = 0.003f; // radians per mouse count
	int max_fps = 0;
	bool vsync = true;
	bool smooth_lighting = true; // corner shading (ambient occlusion) and soft light
	std::string shadow_mode = "blocks";
	float render_scale = 1.0f;
	bool clock_24h = false;      // 07:19 instead of 7:19 AM
	int worker_threads = 0;      // chunk threads (0: as many as cores, less one); file only
	float master_volume = 1.0f, effects_volume = 1.0f, ambience_volume = 0.7f; // 0..1
	std::map<std::string, int> keys; // action -> key
	std::vector<std::string> disabled_mods; // ids not loaded (the next time the game starts)
	std::string player_name = "Player";
	std::string last_address = "127.0.0.1";

	Settings(); // the defaults

	int key(const std::string &action) const;
	void reset_keys();
	// Actions bound to this key (more than one: a clash).
	std::vector<std::string> actions_using(int key) const;

	// False (and the defaults kept) if the file is there but can't be read.
	bool load(const std::filesystem::path &path, std::string *error = nullptr);
	bool save(const std::filesystem::path &path) const;
};

} // namespace gc
