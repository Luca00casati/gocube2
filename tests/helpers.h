#pragma once
// Shared by the tests.

#include "content/content.h"
#include "mods/mods.h"

#include "check.h"

#include <filesystem>
#include <fstream>
#include <string>

// Writes a file (and the folders on the way to it).
inline void write(const std::filesystem::path &p, const std::string &text) {
	if (p.has_parent_path()) {
		std::filesystem::create_directories(p.parent_path());
	}
	std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

// Loads the base game (mods/core, as built: its content and core.dll) into
// `content`; with the kit test mod too (tests/data/kit_mods and kit.dll: what the base
// game no longer has, such as a slab) if `with_kit`.
inline void load_base_game(gc::Content &content, bool with_kit = false) {
	gc::Mods mods(content);
	if (with_kit) {
		mods.load_all({GOCUBE_MODS_DIR, GOCUBE_TEST_MODS_DIR "/kit_mods"});
	} else {
		mods.load_all({GOCUBE_MODS_DIR});
	}
	REQUIRE(mods.problems().empty());
	REQUIRE(mods.loaded().size() == (with_kit ? 2u : 1u));
}
