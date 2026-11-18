#pragma once
// The settings screen: view distance,
// mouse sensitivity, frame rate limit, vsync, smooth lighting, sun shadows,
// render scale, volumes, the clock, key bindings, and which mods load (and
// the mods downloaded from servers). Changes apply at once (the game reads the
// settings every frame) and are saved when it closes. From the title menu and
// the pause menu.

#include "game/settings.h"
#include "mods/mods.h"
#include "platform/window.h"
#include "ui/gui.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gc {

class SettingsScreen {
public:
	// Where mods are: the game's (the base game), the player's, the ones
	// downloaded from servers; what loaded this time. `server_mods`: playing
	// with a host's mods now (the downloaded ones are in use).
	struct ModFolders {
		std::filesystem::path builtin, user, downloaded;
	};
	void open(const ModFolders &folders, const Mods &mods, bool server_mods);

	// One frame of it; false once it closes (Done, or Esc while not waiting
	// for a key), with the settings saved to `file`.
	bool draw(ui::Gui &gui, Settings &s, const Window &window, bool esc_pressed, const std::filesystem::path &file);
	bool waiting_for_key() const { return !capturing_.empty(); }

	// A key's name ("W", "Space", "Left Shift"...).
	static std::string key_name(int vk);

private:
	struct ModRow {
		std::string id, label, where, status, problem;
	};
	std::vector<ModRow> rows_;
	std::vector<std::string> other_problems_;
	size_t cache_count_ = 0;
	uintmax_t cache_bytes_ = 0;
	bool server_mods_ = false;
	ModFolders folders_;
	std::string capturing_; // the action waiting for a key
	bool was_down_[256] = {};
};

} // namespace gc
