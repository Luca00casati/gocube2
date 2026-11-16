#pragma once
// Playing: the player's inventory and hotbar, mining, breaking (with drops)
// and placing, in survival or creative. The rules (mining time, what a block
// drops, starting items) come from the mods (Mods::rule); built-in defaults
// stand in if no mod gives one.

#include "content/content.h"
#include "game/block_entities.h"
#include "game/creatures.h"
#include "game/blocks.h"
#include "game/drops.h"
#include "game/inventory.h"
#include "game/player.h"
#include "mods/mods.h"
#include "world/world.h"

#include <array>
#include <functional>
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

// A sound to play (the Godot version's Sounds.play): sound set ("core:grass"),
// kind ("step", "break", "place", "hurt"...), where, and how loud.
struct SoundEvent {
	std::string set, kind;
	float at[3] = {0, 0, 0};
	float volume_db = 0;
};

// A one-off sight the game shows (game.effect from the mods): "smoke".
struct Effect {
	std::string name;
	float at[3] = {0, 0, 0};
};

// Someone acting in the world: the local player, or (the host) a joined
// player doing what its client asked.
struct Actor {
	int peer = 0;                    // 0: the local player
	const Player *body = nullptr;    // where it stands and looks
	Inventory *inventory = nullptr;  // survival: what is used up comes from here
	int selected = 0;                // the hotbar slot in hand
	std::string item;                // what it holds
};

// The host: another player in the game (for creatures, dropped items and
// placing). `inventory`: the host's copy of a survival player's, else nullptr.
struct OtherPlayer {
	int peer = 0;
	float feet[3] = {0, 0, 0};
	bool alive = true;
	Inventory *inventory = nullptr;
};

// A client: what the player does to the world goes to the host, which does
// it and sends back what changed (blocks, the inventory, health).
class Remote {
public:
	virtual ~Remote() = default;
	virtual void start_mining(const BlockPos &p) = 0;
	virtual void request_break(const BlockPos &p, int slot) = 0;
	// A right click that isn't opening a container, a bed or eating: placing,
	// a door, using an item (a hoe, seeds).
	virtual void request_use(const float eye[3], const float dir[3], int slot, const std::string &item, bool sneak) = 0;
	virtual void request_eat(int slot) = 0;
	virtual void hit_creature(int id) = 0;
	// Survival: a click in a screen (Game::inventory_action), done here too.
	virtual void inventory_action(const Json &action) = 0;
	virtual void set_bed(const BlockPos &head) = 0;
	virtual void respawned() = 0;
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
	// The sounds the game made since the last call (footsteps, breaking,
	// placing, splashes, pickups, getting hurt), for the audio to play.
	std::vector<SoundEvent> take_sounds() { return std::exchange(sounds_, {}); }
	std::vector<Effect> take_effects() { return std::exchange(effects_, {}); }
	// Creatures (their behaviour is the mods'); the one aimed at, within
	// attacking reach and nearer than the block aimed at (0: none).
	const Creatures &creatures() const { return creatures_; }
	Creatures &creatures() { return creatures_; }
	int aimed_creature() const;
	static constexpr float ATTACK_REACH = 4.0f, ATTACK_COOLDOWN = 0.4f;
	// The daylight (0 night .. 1 day): the mods ask (game.day).
	void set_day(float day) { day_ = day; }
	// A new world's first night is safe: no ninjas until a morning has followed
	// a night, or the player slept through it. "ahead" -> "now" -> "over"
	// (world.lua's "first_night"; worlds without one count as over).
	const std::string &first_night() const { return first_night_; }
	void set_first_night(const std::string &stage) { first_night_ = stage; }
	void update_first_night(); // (from the daylight; each frame)
	// A sound set's sound (as the Godot version: e.g. "core:effects", "splash").
	void play_sound(const std::string &set, const std::string &kind, const float at[3], float volume_db = 0);
	// A block's sound of `kind` ("step", "break", "place"), from its "sound".
	void play_block_sound(const std::string &block, const std::string &kind, const float at[3], float volume_db = 0);

	// A brand-new player: survival gets the starting items, creative the
	// placeable blocks.
	void start_new_player();
	void give_starting_items(Inventory &inv); // (the starting_items rule)
	void load_player(const Json &saved); // world.lua's player
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
	// for world.lua's "plants".
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
	void set_world_spawn(const Json &saved); // world.lua's "spawn"
	// Once a physics step: dropped items.
	void physics_step(float dt);
	void drop_item(const std::string &item, int count, const float at[3]) { drops_.spawn(item, count, at); }
	// Breaks the block at `p` as the player (drops, events). For tests too.
	void break_block(const BlockPos &p, const std::string &tool);

	// Hand crafting (the hand_recipes rule, asked once).
	const std::vector<Recipe> &hand_recipes();
	bool can_craft(const Recipe &r) const { return can_craft(inventory_, r); }
	bool can_craft(const Inventory &inv, const Recipe &r) const;
	// Takes the ingredients and adds the output; false (nothing changed) if
	// there aren't enough or the output doesn't fit. Creative: just the output.
	bool craft(const Recipe &r) { return craft(inventory_, r); }
	bool craft(Inventory &inv, const Recipe &r);

	// A click in a screen (what the screens do, so a client's host can do it
	// too): ["click", slot], ["armor", k], ["craft", recipe id], ["pick", item]
	// (creative: take one from the palette), ["container", x, y, z, slot],
	// ["close"] (what the mouse holds goes back, the rest is dropped).
	void inventory_action(const Json &action);
	// The same on any inventory (the host: a joined player's copy, standing
	// at `feet`); false if the action isn't a valid one.
	bool apply_inventory_action(Inventory &inv, const Json &action, const float feet[3]);

	// --- Multiplayer ---
	// A client: the world's changes go through the host.
	void set_remote(Remote *remote) { remote_ = remote; }
	bool remote() const { return remote_ != nullptr; }
	// A client in survival: health as the host has it (hurt shows as a flash).
	void set_health_from_host(int health);
	// A right click doing something to the world (use_item rule, a door,
	// placing) by `who`; false if nothing happened. Survival takes what was
	// used from its inventory.
	bool use(const Actor &who, bool sneak);
	// The host: the other players (set every frame), and what happens to them
	// here (a creature hits one, it picks something up), and the sounds and
	// sights mods make (for everyone to hear and see).
	void set_others(std::vector<OtherPlayer> others) { others_ = std::move(others); }
	std::function<void(int peer, int amount, const std::string &source, const float push[3])> on_damage_peer;
	std::function<void(int peer, const Stack &stack)> on_pickup;
	std::function<void(const SoundEvent &)> on_shared_sound;
	std::function<void(const Effect &)> on_effect;
	const std::optional<BlockPos> &bed() const { return bed_; }
	// A client: the host's dropped items, and a sight it shows.
	void set_drops(std::vector<ItemDrops::Drop> drops) { drops_.set_all(std::move(drops)); }
	void add_effect(Effect e) { effects_.push_back(std::move(e)); }

	float mine_seconds(const std::string &block, const std::string &tool);
	std::vector<std::string> drops_of(const std::string &block, const std::string &tool);

private:
	void fill_creative();
	Remote *remote_ = nullptr;
	std::vector<OtherPlayer> others_;

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
	std::vector<SoundEvent> sounds_;
	std::vector<Effect> effects_;
	Creatures creatures_;
	float day_ = 1;
	std::string first_night_ = "over";
	float attack_cooldown_ = 0;
	float step_distance_ = 0;       // walked since the last footstep
	float last_feet_[3] = {0, 0, 0};
	bool feet_known_ = false;
	bool was_in_water_ = false;
	float last_vy_ = 0;
	float mining_seconds_ = 1;
};

} // namespace gc
