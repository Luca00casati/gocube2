#pragma once
// Playing: the player's inventory and hotbar, mining, breaking (with drops)
// and placing, in survival or creative. The rules (mining time, what a block
// drops, starting items) come from the mods (Mods::rule); built-in defaults
// stand in if no mod gives one.

#include "content/content.h"
#include "game/blocks.h"
#include "game/drops.h"
#include "game/inventory.h"
#include "game/player.h"
#include "mods/mods.h"
#include "world/world.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gc {

struct Recipe {
	std::string id;
	std::vector<std::pair<std::string, int>> ingredients;
	std::string output;
	int count = 1;
};

struct GameInput {
	bool captured = false;    // playing (not in a menu, mouse held by the game)
	bool break_held = false;  // left button down
	bool break_pressed = false;
	bool use_pressed = false; // right click
	bool pick_pressed = false;
	bool sneak = false;
	int wheel = 0;            // notches (positive: towards the next slot)
	int number_key = 0;       // 1..9 pressed this frame, else 0
};

class Game {
public:
	Game(Content &content, const BlockTable &table, const BlockRules &rules, World &world, Mods &mods, Player &player,
			bool creative);

	bool creative() const { return creative_; }
	Inventory &inventory() { return inventory_; }
	const Inventory &inventory() const { return inventory_; }
	int selected() const { return selected_; }
	const std::string &selected_item() const { return inventory_.slots[selected_].id; }
	float mining_progress() const { return mining_progress_; } // 0..1 while mining
	const ItemDrops &drops() const { return drops_; }

	// A brand-new player: survival gets the starting items, creative the
	// placeable blocks.
	void start_new_player();
	void load_player(const Json &saved); // world.json's player
	Json save_player() const;            // the fields this adds to it

	// Once a frame: hotbar, mining, breaking, placing, picking.
	void update(float dt, const GameInput &input);
	// Once a physics step: dropped items.
	void physics_step(float dt);
	void drop_item(const std::string &item, int count, const float at[3]) { drops_.spawn(item, count, at); }
	// Breaks the block at `p` as the player (drops, events). For tests too.
	void break_block(const BlockPos &p, const std::string &tool);

	// Hand crafting (the hand_recipes rule, asked once).
	const std::vector<Recipe> &hand_recipes();
	bool can_craft(const Recipe &r) const;
	// Takes the ingredients and adds the output; false (nothing changed) if
	// there aren't enough or the output doesn't fit. Creative: just the output.
	bool craft(const Recipe &r);

	float mine_seconds(const std::string &block, const std::string &tool);
	std::vector<std::string> drops_of(const std::string &block, const std::string &tool);

private:
	void fill_creative();

	Content &content_;
	const BlockTable &table_;
	const BlockRules &rules_;
	World &world_;
	Mods &mods_;
	Player &player_;
	bool creative_;
	Inventory inventory_;
	int selected_ = 0;
	ItemDrops drops_;
	std::optional<std::vector<Recipe>> recipes_;
	BlockPos mining_target_{1 << 30, 0, 0};
	float mining_progress_ = 0;
	float mining_seconds_ = 1;
};

} // namespace gc
