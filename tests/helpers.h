#pragma once
// Shared by the tests.

#include "content/content.h"
#include "mods/mods.h"

#include <doctest/doctest.h>

// Loads the base game (mods/core: main.lua, which registers its content) into
// `content`; with the kit test mod too (tests/data/kit_mods: what the base
// game no longer has, such as a slab) if `with_kit`.
inline void load_base_game(gc::Content &content, bool with_kit = false) {
	gc::Mods mods(content);
	if (with_kit) {
		mods.load_all({GOCUBE_SOURCE_DIR "/mods", GOCUBE_SOURCE_DIR "/tests/data/kit_mods"});
	} else {
		mods.load_all({GOCUBE_SOURCE_DIR "/mods"});
	}
	REQUIRE(mods.problems().empty());
	REQUIRE(mods.loaded().size() == (with_kit ? 2u : 1u));
}
