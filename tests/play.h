#pragma once
// The base game loaded (its mods kept running: the rules are Lua functions)
// and a world (seed 4242) around the origin, played without a window: for the
// tests, and the two-process multiplayer test (which defines PLAY_REQUIRE).

#include "content/content.h"
#include "game/game.h"
#include "mods/mods.h"
#include "world/world.h"

#ifndef PLAY_REQUIRE
#include <doctest/doctest.h>
#define PLAY_REQUIRE(x) REQUIRE(x)
#endif

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct Play {
	struct Setup {
		bool creative = false;
		uint32_t seed = 4242;
		int generator = gc::GENERATOR_VERSION;
		std::vector<std::string> block_ids; // (a client: the host's)
		float x = 0.5f, z = 0.5f;           // loaded around here
		int view = 32;
		std::vector<std::filesystem::path> more_mods; // folders of mod folders, after the base game's
		std::vector<std::string> only_mods;           // (Mods::load_all's `only`)
	};

	gc::Content content;
	gc::Mods mods{content};
	gc::BlockTable table;
	std::unique_ptr<gc::BlockRules> rules;
	gocube::Generator generator;
	std::unique_ptr<gc::World> world;
	gc::Player player;
	std::unique_ptr<gc::Game> game;

	explicit Play(bool creative = false) : Play(Setup{creative}) {}

	explicit Play(const Setup &s) {
		std::vector<std::filesystem::path> dirs = {std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"};
		dirs.insert(dirs.end(), s.more_mods.begin(), s.more_mods.end());
		mods.load_all(dirs, s.only_mods.empty() ? nullptr : &s.only_mods);
		PLAY_REQUIRE(mods.problems().empty());
		content.freeze(s.block_ids);
		table = gc::build_block_table(content);
		rules = std::make_unique<gc::BlockRules>(content);
		gc::configure_generator(generator, content, s.seed, s.generator);
		world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator);
		world->set_view_distance(s.view);
		wait_loaded(s.x, s.z);
		mods.attach(world.get(), rules.get());
		game = std::make_unique<gc::Game>(content, table, *rules, *world, mods, player, s.creative);
		game->set_generator(&generator);
		game->start_new_player();
	}
	~Play() {
		game.reset();
		mods.attach(nullptr, nullptr);
	}

	// Until the chunks around (x, z) are loaded.
	void wait_loaded(float x, float z) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!(world->is_loaded(int(std::floor(x)), 0, int(std::floor(z))) && world->stats().queued == 0)) {
			world->update(x, 10.0f, z);
			PLAY_REQUIRE(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	gocube::BlockId id(const char *name) const { return gocube::BlockId(content.block_index(name)); }
	std::string block(int x, int y, int z) const { return content.block_ids()[world->block_at(x, y, z)]; }
	std::string block(const gc::BlockPos &p) const { return block(p.x, p.y, p.z); }
};
