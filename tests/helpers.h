#pragma once
// Shared by the tests.

#include "content/content.h"
#include "mods/mods.h"

#include <doctest/doctest.h>

// Loads the base game (mods/core: its JSON content and main.lua) into `content`.
inline void load_base_game(gc::Content &content) {
	gc::Mods mods(content);
	mods.load_all({GOCUBE_SOURCE_DIR "/mods"});
	REQUIRE(mods.problems().empty());
	REQUIRE(mods.loaded().size() == 1);
}
