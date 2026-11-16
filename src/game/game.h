#pragma once
// Playing: the player's inventory and hotbar, mining, breaking (with drops)
// and placing, in survival or creative. The rules (mining time, what a block
// drops, starting items) come from the mods (Mods::rule); built-in defaults
// stand in if no mod gives one.

#include "content/content.h"
#include "game/block_entities.h"
#include "game/blocks.h"
#include "game/drops.h"
#include "game/inventory.h"
#include "game/player.h"
#include "mods/mods.h"
#include "world/world.h"

#include <array>
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
	~Game();
	Game(const Game &) = delete;
	Game &operator=(const Game &) = delete;

	bool creative() const { return creative_; }
	Inventory &inventory() { return inventory_; }
	const Inventory &inventory() const { return inventory_; }
	int selected() const { return selected_; }
	const std::string &selected_item() const { return inventory_.slots[selected_].id; }
	float mining_progress() const { return mining_progress_; } // 0..1 while mining
	// Goes up with every swing of the arm: breaking, placing or using, eating,
	// a click at nothing (mining swings on its own: mining_progress).
	int swings() const { return swings_; }
	const ItemDrops &drops() const { return drops_; }
	BlockEntities &block_entities() { return entities_; }
	// A container the player right-clicked this frame (the caller opens its screen).
	std::optional<BlockPos> take_opened() { return std::exchange(opened_, std::nullopt); }
	// A bed the player right-clicked this frame (sleeping is the caller's: it knows the time).
	bool take_bed_used() { return std::exchange(bed_used_, false); }

	// A brand-new player: survival gets the starting items, creative the
	// placeable blocks.
	void start_new_player();
	void load_player(const Json &saved); // world.json's player
	Json save_player() const;            // the fields this adds to it

	// Once a frame: hotbar, mining, breaking, placing, picking.
	void update(float dt, const GameInput &input);
	// Once a frame: furnaces, notices, telling mods about block changes.
	void tick(double dt);
	// The world's generator (trees grow in its shapes).
	void set_generator(const gocube::Generator *generator) { generator_ = generator; }
	// A sapling at `pos` grows into tree `tree_id`, as the generator would
	// make it there. False (nothing changed) without room or loaded ground.
	bool grow_tree(const BlockPos &pos, const std::string &tree_id);
	// Growing crops and saplings (the mods' save_plants/load_plants rules),
	// for world.json's "plants".
	Json save_plants();
	void load_plants(const Json &data);

	// --- Health (survival) ---
	static constexpr int MAX_HEALTH = 20;
	static constexpr float MAX_BREATH = 10.0f; // seconds underwater before drowning starts
	int health() const { return health_; }
	float breath() const { return breath_; }
	bool dead() const { return dead_; }
	// The player shouldn't move: dead, or waiting for ground to spawn on.
	bool frozen() const { return dead_ || spawn_pending_; }
	// After each physics step: falls, burning, drowning, healing.
	void after_move(float dt);
	void damage(int amount, const std::string &source = "");
	// Back to life: at the bed if it's still there, else at the world spawn.
	void respawn();
	// Seconds since the last hurt (for the red flash), and a short message.
	float since_hurt() const { return since_hurt_; }
	const std::string &notice() const { return notice_; }
	float notice_time() const { return notice_time_; }
	void show_notice(const std::string &text) {
		notice_ = text;
		notice_time_ = 3.5f;
	}

	// A new world: put the player on dry ground near the origin once it's
	// loaded (that becomes the world spawn).
	void spawn_on_ground() { spawn_pending_ = true; }
	const std::optional<std::array<float, 3>> &world_spawn() const { return world_spawn_; }
	void set_world_spawn(const Json &saved); // world.json's "spawn"
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
	// Right click with the selected item through the use_item rule (hoes,
	// seeds); false if the rule did nothing.
	bool use_item(const RayHit &hit);

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
	BlockEntities entities_;
	std::optional<BlockPos> opened_;
	bool bed_used_ = false;
	const gocube::Generator *generator_ = nullptr;
	std::vector<std::pair<BlockPos, gocube::BlockId>> changed_; // for the mods' block_changed, next tick
	std::optional<std::vector<Recipe>> recipes_;
	bool try_place_on_ground();
	void unstuck();

	int health_ = MAX_HEALTH;
	float breath_ = MAX_BREATH;
	bool dead_ = false;
	float fall_peak_ = 0;
	bool was_on_floor_ = true;
	float since_damage_ = 0, regen_timer_ = 0, drown_timer_ = 0, burn_timer_ = 0.5f;
	float since_hurt_ = 99;
	std::optional<BlockPos> bed_;
	bool spawn_pending_ = false;
	std::optional<std::array<float, 3>> world_spawn_;
	std::string notice_;
	float notice_time_ = 0;
	BlockPos mining_target_{1 << 30, 0, 0};
	float mining_progress_ = 0;
	int swings_ = 0;
	float mining_seconds_ = 1;
};

} // namespace gc
