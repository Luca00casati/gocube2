// Playing: the player's inventory and hotbar, mining, breaking (with drops)
// and placing, in survival or creative; health; dropped items, containers,
// dye vats; and the base game's behaviour (game::rules, and core_*.c3:
// plants, fluids, creatures). (src/game/game.*, and src/core_mod's code)
module gocube::game;
import gocube::game::blocks, gocube::game::player, gocube::game::inventory, gocube::game::drops, gocube::game::block_entities;
import gocube::game::creatures, gocube::game::rules, gocube::game::vitals, gocube::game::dye;
import gocube::content, gocube::data, gocube::world, gocube::voxel, gocube::maths;
import gocube::util::vec, gocube::util::map, gocube::util::rand, gocube::util::intern;
import std::math, std::sort;

// A sound to play: sound set ("core:grass"), kind ("step", "break",
// "place", "hurt"...), where, and how loud. (Strings interned.)
struct SoundEvent
{
	String set, kind;
	float[3] at;
	float volume_db;
}

// A one-off sight the game shows: "smoke".
struct Effect
{
	String name; // (interned)
	float[3] at;
}

// Someone acting in the world: the local player, or (the host) a joined
// player doing what its client asked.
struct Actor
{
	int peer;             // 0: the local player
	Player* body;         // where it stands and looks
	Inventory* inventory; // survival: what is used up comes from here
	int selected;         // the hotbar slot in hand
	String item;          // what it holds
}

// The host: another player in the game (for creatures, dropped items and
// placing). `inventory`: the host's copy of a survival player's, else null.
struct OtherPlayer
{
	int peer;
	float[3] feet;
	bool alive;
	Inventory* inventory;
}

// A client: what the player does to the world goes to the host, which does
// it and sends back what changed (blocks, the inventory, health).
interface Remote
{
	fn void start_mining(BlockPos p);
	fn void request_break(BlockPos p, int slot);
	// A right click that isn't opening a container, a bed or eating: placing, a door.
	fn void request_use(float[3] eye, float[3] dir, int slot, String item, bool sneak);
	fn void request_eat(int slot);
	// Q: throwing what is in hand (one, or the whole stack).
	fn void request_throw(float[3] eye, float[3] dir, int slot, String item, bool whole);
	fn void hit_creature(int id);
	// Survival: a click in a screen (Game.inventory_action), done here too (taken over).
	fn void inventory_action(Data action);
	fn void set_bed(BlockPos head);
	fn void respawned();
}

// The host: what happens to other players here (a creature hits one, it
// picks something up), and the sounds and sights the game makes (for
// everyone to hear and see).
interface GameEvents
{
	fn void damage_peer(int peer, int amount, String source, float[3] push) @optional;
	fn void picked_up(int peer, Stack stack) @optional;
	fn void shared_sound(SoundEvent* e) @optional;
	fn void effect(Effect* e) @optional;
}

struct GameInput
{
	bool captured;      // playing (not in a menu, mouse held by the game)
	bool break_held;    // left button down
	bool break_pressed;
	bool use_pressed;   // right click
	bool pick_pressed;
	bool sneak;
	int wheel;          // notches (positive: towards the next slot)
	int number_key;     // 1..9 pressed this frame, else 0
	bool throw_pressed; // Q: throws one of what is in hand
	bool throw_stack;   // (with Ctrl: the whole stack)
}

struct BlockChanged
{
	BlockPos pos;
	BlockId id;
}

const float ATTACK_REACH = 4.0f;
const float ATTACK_COOLDOWN = 0.4f;
const int MAX_HEALTH = vitals::MAX_HEALTH;
const float MAX_BREATH = vitals::MAX_BREATH; // seconds underwater before drowning starts
const float THROW_SPEED = 7.0f;
// The dye vat: each gem thrown in dyes DYE_PER_GEM blocks; it holds
// VAT_MAX_GEMS gems at most (more bounce off). Blocks thrown in come out dyed
// (one charge each), as many as it has charge for.
const int DYE_PER_GEM = 8;
const int VAT_MAX_GEMS = 16;

struct Game
{
	Content* content;
	BlockTable* table;
	BlockRules* rules;
	World* world;
	Player* player;
	bool creative;
	Inventory inventory;
	int selected;
	ItemDrops drops;
	BlockEntities entities;
	bool has_opened;
	BlockPos opened;
	bool bed_used;
	Generator* generator;
	Vec{BlockChanged} changed; // for the base game's block_changed, next tick
	Remote remote;             // a client's (null: this game is the world's)
	GameEvents events;         // the host's (null: none)
	Vec{OtherPlayer} others;
	Rng random;

	int health;
	float breath;
	bool dead;
	float fall_peak;
	bool was_on_floor;
	float since_damage, regen_timer, drown_timer, burn_timer, void_timer;
	float since_hurt;
	bool has_bed;
	BlockPos bed;
	bool spawn_pending;
	bool has_world_spawn;
	float[3] world_spawn;
	DString notice_text;
	float notice_time;
	BlockPos mining_target;
	float mining_progress; // 0..1 while mining
	int swings;            // goes up with every swing of the arm
	Vec{SoundEvent} sounds;
	Vec{Effect} effects_;
	Creatures creatures;
	float day;
	float attack_cooldown;
	float step_distance;   // walked since the last footstep
	float[3] last_feet;
	bool feet_known;
	bool was_in_water;
	float last_vy;
	float mining_seconds;
	Core core;
}

const BlockPos NOWHERE @private = { 1 << 30, 0, 0 };

fn void Game.init(&self, Content* content, BlockTable* table, BlockRules* rules, World* world, Player* player, bool creative)
{
	*self = { .content = content, .table = table, .rules = rules, .world = world, .player = player, .creative = creative,
		.inventory = inventory::new(content), .entities = { .content = content }, .random = rand::seed_random(),
		.health = MAX_HEALTH, .breath = MAX_BREATH, .was_on_floor = true, .burn_timer = 0.5f, .void_timer = 0.5f,
		.since_hurt = 99, .mining_target = NOWHERE, .day = 1, .mining_seconds = 1 };
	self.drops.init();
	self.notice_text.init(mem);
	// Containers come and go with their blocks; what was in one spills out.
	world.on_block_changed = &on_world_block_changed;
	world.on_block_changed_context = self;
}

fn void Game.free(&self)
{
	if (self.world && self.world.on_block_changed_context == self)
	{
		self.world.on_block_changed = null;
		self.world.on_block_changed_context = null;
	}
	self.drops.free();
	self.entities.free();
	self.changed.free();
	self.others.free();
	self.notice_text.free();
	self.sounds.free();
	self.effects_.free();
	self.creatures.free();
	self.core.free();
	*self = {};
}

fn void on_world_block_changed(void* context, BlockPos p, BlockId id) @private
{
	Game* self = context;
	Vec{Stack} spilled;
	defer spilled.free();
	self.entities.block_changed(p, self.rules.id_of(id), &spilled);
	if (self.remote) return; // (a client: the host spills them)
	foreach (s : spilled) self.drops.spawn(s.id, s.count, { p.x + 0.5f, p.y + 0.3f, p.z + 0.5f });
	self.changed.push({ p, id });
}

// Starts random() over from `seed`: the same seed, the same numbers (tests).
fn void Game.set_random_seed(&self, ulong seed) => self.random = rand::seeded(seed);

fn String Game.selected_item(&self) => self.inventory.slots[self.selected].id;

// A container the player right-clicked this frame (the caller opens its screen).
fn bool Game.take_opened(&self, BlockPos* out)
{
	if (!self.has_opened) return false;
	*out = self.opened;
	self.has_opened = false;
	return true;
}

// A bed the player right-clicked this frame (sleeping is the caller's: it knows the time).
fn bool Game.take_bed_used(&self)
{
	bool used = self.bed_used;
	self.bed_used = false;
	return used;
}

// The sounds the game made since the last call (footsteps, breaking,
// placing, splashes, pickups, getting hurt), for the audio to play; the
// caller frees the list.
fn Vec{SoundEvent} Game.take_sounds(&self)
{
	Vec{SoundEvent} out = self.sounds;
	self.sounds = {};
	return out;
}

fn Vec{Effect} Game.take_effects(&self)
{
	Vec{Effect} out = self.effects_;
	self.effects_ = {};
	return out;
}

fn void Game.add_effect(&self, Effect e) => self.effects_.push(e);

fn String Game.notice(&self) => self.notice_text.str_view();

fn void Game.show_notice(&self, String text)
{
	self.notice_text.clear();
	self.notice_text.append(text);
	self.notice_time = 3.5f;
}

// The player shouldn't move: dead, or waiting for ground to spawn on.
fn bool Game.frozen(&self) => self.dead || self.spawn_pending;

// A new world: put the player on dry ground near the origin once it's loaded
// (that becomes the world spawn).
fn void Game.spawn_on_ground(&self) => self.spawn_pending = true;

// world.json's "spawn".
fn void Game.set_world_spawn(&self, Data* saved)
{
	if (saved && saved.is_array() && saved.len() == 3 && saved.at(0).is_number())
	{
		self.world_spawn = { saved.at(0).as_float(), saved.at(1).as_float(), saved.at(2).as_float() };
		self.has_world_spawn = true;
	}
}

fn void Game.drop_item(&self, String item, int count, float[3] at) => self.drops.spawn(item, count, at);

// A sound set's sound.
fn void Game.play_sound(&self, String set, String kind, float[3] at, float volume_db = 0)
{
	self.sounds.push({ intern::intern(set), intern::intern(kind), at, volume_db });
}

// A block's sound of `kind` ("step", "break", "place"), from its "sound".
fn void Game.play_block_sound(&self, String block, String kind, float[3] at, float volume_db = 0)
{
	if (self.content.block_index(block) < 0) return;
	Data* def = self.rules.def(block);
	if (Data* s = def.get("sound"))
	{
		if (s.is_string()) self.play_sound(s.s, kind, at, volume_db);
	}
}

// A sound everyone hears (the host shares it).
fn void Game.play_shared_sound(&self, String set, String kind, float[3] at, float volume_db = 0)
{
	self.play_sound(set, kind, at, volume_db);
	if (self.events && &self.events.shared_sound) self.events.shared_sound(self.sounds.last());
}

// A sight everyone sees.
fn void Game.effect(&self, String name, float[3] at)
{
	Effect e = { intern::intern(name), at };
	if (self.events && &self.events.effect) self.events.effect(&e);
	self.effects_.push(e);
}

// Changes the game mode (/gamemode); the inventory stays as it is.
fn void Game.set_creative(&self, bool on)
{
	if (on == self.creative) return;
	self.creative = on;
	self.mining_progress = 0;
	if (!self.creative)
	{
		self.health = MAX_HEALTH;
		self.breath = MAX_BREATH;
		self.fall_peak = self.player.position[1];
	}
}

// A brand-new player: survival gets the starting items, creative starts
// empty (the inventory screen's palette has everything).
fn void Game.start_new_player(&self)
{
	self.inventory = inventory::new(self.content);
	self.selected = 0;
	if (!self.creative) self.give_starting_items(&self.inventory);
}

fn void Game.give_starting_items(&self, Inventory* inv)
{
	inv.slots[0] = inventory::stack(rules::STARTING_ITEM, rules::STARTING_COUNT);
}

// world.json's player.
fn void Game.load_player(&self, Data* saved)
{
	if (!saved || !saved.has("inventory"))
	{
		self.start_new_player(); // a player who never played here
		return;
	}
	self.inventory.deserialize(saved.get("inventory"));
	self.selected = math::clamp((int)saved.num("selected", 0), 0, inventory::HOTBAR - 1);
	self.health = math::clamp((int)saved.num("health", MAX_HEALTH), 1, MAX_HEALTH);
	Data* bed = saved.get("bed");
	if (bed && bed.is_array() && bed.len() == 3 && bed.at(0).is_number())
	{
		self.bed = { bed.at(0).as_int(), bed.at(1).as_int(), bed.at(2).as_int() };
		self.has_bed = true;
	}
}

// The fields this adds to world.json's player.
fn Data Game.save_player(&self)
{
	Data s = data::object();
	s.set("inventory", self.inventory.serialize());
	s.set_long("selected", self.selected);
	if (!self.creative) s.set_long("health", self.health);
	if (self.has_bed)
	{
		Data b = data::array();
		b.push_long(self.bed.x);
		b.push_long(self.bed.y);
		b.push_long(self.bed.z);
		s.set("bed", b);
	}
	return s;
}

// A sapling at `pos` grows into tree `tree_id`, as the generator would make
// it there. False (nothing changed) without room or loaded ground.
fn bool Game.grow_tree(&self, BlockPos pos, String tree_id)
{
	if (!self.generator) return false;
	Vec{String} order;
	defer order.free();
	self.content.tree_order(&order);
	int kind = -1;
	foreach (i, t : order)
	{
		if (t == tree_id) kind = (int)i;
	}
	if (kind < 0) return false;
	TreeBlockList blocks;
	defer blocks.free();
	self.generator.tree_blocks(kind, pos.x, pos.y - 1, pos.z, &blocks);
	if (blocks.is_empty()) return false;
	foreach (b : blocks)
	{
		if (!self.world.is_loaded(b.x, b.y, b.z)) return false; // part of it would land in an unloaded chunk
		int here = self.world.block_at(b.x, b.y, b.z);
		if (!b.only_air && !((BlockPos){ b.x, b.y, b.z } == pos) && here != 0 && !self.makes_room(here)) return false; // no room for its trunk
	}
	foreach (b : blocks)
	{
		int here = self.world.block_at(b.x, b.y, b.z);
		if (here == 0 || (!b.only_air && ((BlockPos){ b.x, b.y, b.z } == pos || self.makes_room(here)))) self.world.set_block({ b.x, b.y, b.z }, b.id);
	}
	return true;
}

// Plants and leaves give way to a growing trunk.
fn bool Game.makes_room(&self, int block)
{
	Data* d = self.rules.def_of(block);
	return d.flag("replaceable") || d.flag("leaves") || d.str("shape") == "cross";
}

// Hand crafting.
fn Recipe[] Game.hand_recipes(&self) => &rules::HAND_RECIPES;

fn bool Game.can_craft(&self, Inventory* inv, Recipe* r)
{
	if (self.creative) return true;
	for (int i = 0; i < r.ingredients; i++)
	{
		if (inv.count_of(r.ingredient_ids[i]) < r.ingredient_counts[i]) return false;
	}
	return true;
}

// Takes the ingredients and adds the output; false (nothing changed) if
// there aren't enough or the output doesn't fit. Creative: just the output.
fn bool Game.craft(&self, Inventory* inv, Recipe* r)
{
	if (self.creative)
	{
		inv.add(r.output, r.count);
		return true;
	}
	if (!self.can_craft(inv, r)) return false;
	Inventory before = *inv;
	for (int i = 0; i < r.ingredients; i++) inv.remove(r.ingredient_ids[i], r.ingredient_counts[i]);
	if (inv.add(r.output, r.count) > 0)
	{
		*inv = before; // no room: undo
		return false;
	}
	return true;
}

// A click in a screen (what the screens do, so a client's host can do it
// too): ["click", slot], ["armor", k], ["craft", recipe id], ["pick", item]
// (creative: take one from the palette), ["container", x, y, z, slot],
// ["close"] (what the mouse holds goes back, the rest is dropped). Taken over.
fn void Game.inventory_action(&self, Data action)
{
	if (self.apply_inventory_action(&self.inventory, &action, self.player.position) && self.remote)
	{
		self.remote.inventory_action(action); // (the host keeps our inventory)
		return;
	}
	action.free();
}

fn bool action_int(Data* a, sz i, long lo, long hi, long* out) @private
{
	Data* v = a.at(i);
	if (!v || !v.is_integer() || v.i < lo || v.i > hi) return false;
	*out = v.i;
	return true;
}

// The same on any inventory (the host: a joined player's copy, standing at
// `feet`); false if the action isn't a valid one.
fn bool Game.apply_inventory_action(&self, Inventory* inv, Data* a, float[3] feet)
{
	if (!a.is_array() || a.is_empty() || !a.at(0).is_string()) return false;
	String kind = a.at(0).s;
	long slot;
	switch (kind)
	{
		case "click":
			if (!action_int(a, 1, 0, inventory::SIZE - 1, &slot)) return false;
			if (self.creative && inv.cursor.is_empty() && !inv.slots[slot].is_empty())
			{
				inv.cursor = inv.slots[slot]; // creative: rearrange freely, items never run out
				inv.slots[slot] = {};
			}
			else
			{
				inv.click((int)slot);
			}
			return true;
		case "armor":
			if (!action_int(a, 1, 0, 3, &slot)) return false;
			inv.cursor = inv.put_armor((int)slot, inv.cursor);
			return true;
		case "craft":
			if (a.len() != 2 || !a.at(1).is_string()) return false;
			foreach (&r : self.hand_recipes())
			{
				if (r.id == a.at(1).s) return self.craft(inv, r);
			}
			return false;
		case "pick":
			if (!self.creative || a.len() != 2 || !a.at(1).is_string()) return false;
			String item = a.at(1).s;
			if (!self.content.items.has(item) && !self.rules.is_placeable(item)) return false;
			inv.cursor = inventory::stack(item, 1);
			return true;
		case "container":
			long x;
			long y;
			long z;
			if (!action_int(a, 1, int::min, int::max, &x) || !action_int(a, 2, int::min, int::max, &y) ||
				!action_int(a, 3, int::min, int::max, &z) || !action_int(a, 4, 0, 255, &slot))
			{
				return false;
			}
			BlockPos p = { (int)x, (int)y, (int)z };
			BlockEntity* e = self.entities.at(p);
			if (!e || slot >= e.slots.len()) return false;
			const float CONTAINER_REACH = 8.0f;
			float dx = p.x + 0.5f - feet[0];
			float dy = p.y + 0.5f - (feet[1] + player::EYE_HEIGHT);
			float dz = p.z + 0.5f - feet[2];
			if (dx * dx + dy * dy + dz * dz > CONTAINER_REACH * CONTAINER_REACH) return false;
			inv.cursor = self.entities.click(p, (int)slot, inv.cursor);
			return true;
		case "close":
			if (a.len() != 1) return false;
			Stack left_over = inv.close_cursor();
			if (!left_over.is_empty() && !self.remote) // (a client's host drops it)
			{
				self.drops.spawn(left_over.id, left_over.count, { feet[0], feet[1] + 1.2f, feet[2] });
			}
			return true;
		default:
			return false;
	}
}

// Shorthand: an action made of a word and numbers.
fn Data action(String kind, long[] numbers = {})
{
	Data a = data::array();
	a.push_string(kind);
	foreach (n : numbers) a.push_long(n);
	return a;
}

// A client in survival: health as the host has it (hurt shows as a flash).
fn void Game.set_health_from_host(&self, int health)
{
	health = math::clamp(health, 0, MAX_HEALTH);
	if (health < self.health)
	{
		self.since_hurt = 0;
		self.play_sound("core:effects", "hurt", self.player.eye());
	}
	self.health = health;
	if (self.health == 0 && !self.dead)
	{
		self.dead = true;
		self.player.velocity = { 0, 0, 0 };
	}
	else if (self.health > 0)
	{
		self.dead = false;
	}
}

struct Occupancy @private
{
	Game* game;
	int peer;
}

fn bool occupied_cell(void* context, BlockPos cell) @private
{
	Occupancy* o = context;
	if (o.peer != 0 && player::overlaps_block(o.game.player.position, cell)) return true; // the local player
	foreach (&other : o.game.others)
	{
		if (other.peer != o.peer && player::overlaps_block(other.feet, cell)) return true;
	}
	return false;
}

// A right click doing something to the world (a dye vat, a door, placing)
// by `who`; false if nothing happened. Survival takes what was used from its
// inventory.
fn bool Game.use(&self, Actor* who, bool sneak)
{
	RayHit hit = who.body.target(self.world);
	if (!hit.hit) return false;
	// A dye vat, with an empty hand: its dye is tipped out.
	if (who.item.len == 0 && !sneak && self.rules.def_of(hit.block).flag("dye_vat"))
	{
		self.empty_vat(hit.position);
		return true;
	}
	// Placing (or using a door); survival uses the item up. Nobody may be
	// built into a block.
	Occupancy occupancy = { self, who.peer };
	BlockChanges changes;
	defer changes.free();
	bool done = who.body.use_or_place(self.world, self.rules, who.item, sneak, &changes, &occupied_cell, &occupancy);
	if (done)
	{
		// A door opened or closed sounds like the door; a placed block like itself.
		String id = changes.is_empty() ? self.rules.id_of(hit.block) : changes[0].id;
		BlockPos at = changes.is_empty() ? hit.position : changes[0].pos;
		self.play_block_sound(id, "place", { at.x + 0.5f, at.y + 0.5f, at.z + 0.5f });
	}
	bool placed = false;
	foreach (c : changes) placed = placed || c.placed;
	if (placed && !self.creative && who.inventory) who.inventory.take_one(who.selected);
	return done;
}

// Seconds to break `block` (whatever is held).
fn float Game.mine_seconds_of(&self, String block) => math::max(0.05f, rules::mine_seconds(self.content.blocks.get(content::undyed(block))));

// Breaks the block at `p` as the player (drops, sounds). For tests too.
fn void Game.break_block(&self, BlockPos p)
{
	int broken_index = self.world.block_at(p.x, p.y, p.z);
	String broken = intern::intern(self.rules.id_of(broken_index));
	if (broken.len == 0 || broken == "core:air" || self.rules.is_fluid(broken_index)) return;
	Dye dye = self.world.dye_at(p.x, p.y, p.z);
	self.world.set_block(p, 0);
	BlockPos other;
	if (self.rules.other_part(broken, p, &other)) // a door or bed goes as a whole
	{
		String there = self.rules.id_of(self.world.block_at(other.x, other.y, other.z));
		if (self.rules.base_of(there) == self.rules.base_of(broken)) self.world.set_block(other, 0);
	}
	if (!self.creative)
	{
		float[3] at = { p.x + 0.5f, p.y + 0.3f, p.z + 0.5f };
		bool keeps_dye = dye != voxel::NO_DYE && !self.rules.def(broken).flag("dye_vat"); // (a vat's is its dye's)
		Vec{String} items;
		defer items.free();
		rules::drops(self.content.blocks.get(broken), broken, &self.random, &items);
		@pool()
		{
			foreach (item : items)
			{
				self.drops.spawn(keeps_dye && self.rules.is_placeable(item) ? content::dyed(tmem, item, dye) : item, 1, at); // (dyed stays dyed)
			}
		};
	}
	self.play_block_sound(broken, "break", { p.x + 0.5f, p.y + 0.5f, p.z + 0.5f });
	self.swings++;
}

// The creature aimed at, within attacking reach and nearer than the block
// aimed at (0: none).
fn int Game.aimed_creature(&self)
{
	float[3] eye = self.player.eye();
	float[3] dir = self.player.look();
	float distance;
	int id = self.creatures.pick(eye, dir, ATTACK_REACH, &distance);
	if (id == 0) return 0;
	// A block nearer than the creature is what's aimed at.
	RayHit hit = self.player.target(self.world);
	if (hit.hit)
	{
		float dx = hit.position.x + 0.5f - eye[0];
		float dy = hit.position.y + 0.5f - eye[1];
		float dz = hit.position.z + 0.5f - eye[2];
		if (math::sqrt(dx * dx + dy * dy + dz * dz) < distance - 0.5f) return 0;
	}
	return id;
}

fn void Game.break_or_ask(&self, BlockPos p)
{
	if (self.remote)
	{
		self.remote.request_break(p, self.selected);
		self.swings++;
	}
	else
	{
		self.break_block(p);
	}
}

// Once a frame: hotbar, mining, breaking, placing, picking.
fn void Game.update(&self, float dt, GameInput in) => @pool()
{
	if (!in.captured)
	{
		self.mining_progress = 0;
		self.mining_target = NOWHERE;
		return;
	}
	// Hotbar: number keys and the wheel.
	if (in.number_key >= 1 && in.number_key <= inventory::HOTBAR) self.selected = in.number_key - 1;
	if (in.wheel != 0) self.selected = ((self.selected + in.wheel) % inventory::HOTBAR + inventory::HOTBAR) % inventory::HOTBAR;

	RayHit hit = self.player.target(self.world);
	RayHit mine = hit; // (a creature in the way: nothing to mine)
	String tool = self.selected_item();
	// A creature in front: a left click hits it (now and then), and it's in
	// the way of mining.
	self.attack_cooldown = math::max(0.0f, self.attack_cooldown - dt);
	if (int creature = self.aimed_creature())
	{
		if (in.break_pressed && self.attack_cooldown <= 0 && !self.dead)
		{
			self.attack_cooldown = ATTACK_COOLDOWN;
			if (self.remote)
			{
				self.remote.hit_creature(creature);
			}
			else
			{
				Data* item = self.content.items.get(tool);
				int damage = item ? (int)item.num("damage", 2.0) : 2; // bare hand: 2
				self.creature_hit(creature, damage, self.player.position, 0);
			}
			self.swings++;
		}
		mine.hit = false;
	}
	else if (in.break_pressed && !hit.hit)
	{
		self.swings++; // at nothing
	}
	// Breaking: at once in creative, by holding the button in survival.
	if (self.creative)
	{
		if (in.break_pressed && mine.hit) self.break_or_ask(mine.position);
	}
	else if (in.break_held && mine.hit)
	{
		if (!(mine.position == self.mining_target))
		{
			self.mining_target = mine.position;
			self.mining_progress = 0;
			self.mining_seconds = self.mine_seconds_of(self.rules.id_of(mine.block));
			if (self.remote) self.remote.start_mining(mine.position); // (the host times it too)
		}
		self.mining_progress += dt / self.mining_seconds;
		if (self.mining_progress >= 1.0f)
		{
			self.mining_progress = 0;
			self.mining_target = NOWHERE; // (mining the next block starts over)
			self.break_or_ask(mine.position);
		}
	}
	else
	{
		self.mining_progress = 0;
		self.mining_target = NOWHERE;
	}

	// Right click: a container opens, a bed becomes the respawn point (sneak
	// to place against them instead), food is eaten; else placing.
	Data empty;
	Data* target_def = hit.hit ? self.rules.def_of(hit.block) : &empty;
	int food = in.use_pressed && self.selected_item().len ? rules::food(self.content, self.selected_item()) : 0;
	if (in.use_pressed && hit.hit && !in.sneak && self.remote && !self.entities.at(hit.position) && target_def.has("block_entity"))
	{
		// A client: a container that came with the host's chunks. An empty copy
		// for now; the host sends what's in it.
		Vec{Stack} none;
		defer none.free();
		self.entities.block_changed(hit.position, self.rules.id_of(hit.block), &none);
	}
	if (in.use_pressed && hit.hit && !in.sneak && self.entities.at(hit.position) && !target_def.flag("dye_vat"))
	{
		self.opened = hit.position;
		self.has_opened = true;
	}
	else if (in.use_pressed && hit.hit && !in.sneak && target_def.flag("bed"))
	{
		// The head part is the one you lie on.
		BlockPos head = hit.position;
		Data* st = target_def.get("state");
		if (st && st.str("part") == "foot")
		{
			BlockPos other;
			if (self.rules.other_part(self.rules.id_of(hit.block), hit.position, &other)) head = other;
		}
		if (!self.has_bed || !(self.bed == head))
		{
			self.bed = head;
			self.has_bed = true;
			self.show_notice("Respawn point set");
		}
		if (self.remote) self.remote.set_bed(head);
		self.bed_used = true;
	}
	else if (in.use_pressed && food > 0)
	{
		if (!self.creative && self.health < MAX_HEALTH && !self.dead)
		{
			if (self.remote)
			{
				self.remote.request_eat(self.selected); // (the host heals us, and takes it from its copy)
			}
			else
			{
				self.health = math::min(MAX_HEALTH, self.health + food);
			}
			self.inventory.take_one(self.selected);
			self.swings++;
			self.play_sound("core:effects", "pickup", self.player.eye(), -4.0f); // (eating)
		}
	}
	else if (in.use_pressed && self.remote)
	{
		if (hit.hit && !self.dead) // the host does it as us
		{
			self.remote.request_use(self.player.eye(), self.player.look(), self.selected, self.selected_item(), in.sneak);
			self.swings++;
		}
	}
	else if (in.use_pressed)
	{
		Actor me = { .body = self.player, .inventory = &self.inventory, .selected = self.selected, .item = self.selected_item() };
		if (self.use(&me, in.sneak)) self.swings++;
	}

	// Creative: middle click puts the block aimed at in the hand (dyed too).
	if (in.pick_pressed && self.creative && hit.hit)
	{
		String base = self.rules.base_of(self.rules.id_of(hit.block));
		if (self.rules.is_placeable(base))
		{
			Dye dye = target_def.flag("dye_vat") ? voxel::NO_DYE : self.world.dye_at(hit.position.x, hit.position.y, hit.position.z);
			self.inventory.slots[self.selected] = inventory::stack(content::dyed(tmem, base, dye), 1);
		}
	}

	// Q: throwing what is in hand (Ctrl: the whole stack).
	if (in.throw_pressed && !self.dead && self.selected_item().len)
	{
		float[3] eye = self.player.eye();
		float[3] dir = self.player.look();
		if (self.remote)
		{
			self.remote.request_throw(eye, dir, self.selected, self.selected_item(), in.throw_stack);
			Stack* s = &self.inventory.slots[self.selected]; // (the host sends our inventory as it then is)
			s.count -= in.throw_stack ? s.count : 1;
			if (s.count <= 0) *s = {};
		}
		else
		{
			Actor me = { .body = self.player, .inventory = &self.inventory, .selected = self.selected, .item = self.selected_item() };
			self.throw_item(&me, eye, dir, in.throw_stack);
		}
		self.swings++;
	}
}

// Once a frame: containers, notices, the base game hearing about block changes.
fn void Game.tick(&self, double dt)
{
	self.creatures.animate((float)dt);
	if (!self.remote)
	{
		// The base game hears about block changes a tick later (its own changes
		// too, without re-entering it).
		Vec{BlockChanged} changes = self.changed;
		self.changed = {};
		defer changes.free();
		foreach (c : changes) self.core_block_changed(c.pos, c.id);
		self.core_tick(dt);
	}
	else
	{
		self.drops.age((float)dt);
	}
	self.notice_time = math::max(0.0f, self.notice_time - (float)dt);
	self.since_hurt += (float)dt;
	if (self.spawn_pending && self.try_place_on_ground())
	{
		self.spawn_pending = false;
		self.fall_peak = self.player.position[1];
		if (!self.has_world_spawn) // where new players appear and the dead respawn
		{
			self.world_spawn = self.player.position;
			self.has_world_spawn = true;
		}
	}
}

// Once a physics step: creatures' bodies, dropped items.
fn void Game.physics_step(&self, float dt)
{
	self.creatures.step(dt, self.world);
	if (self.remote) return; // (a client: the host's drops, shown as it sends them)
	Vec{Taker} takers;
	defer takers.free();
	takers.push({ .feet = self.player.position, .alive = !self.dead, .inventory = &self.inventory });
	foreach (&o : self.others) takers.push({ .peer = o.peer, .feet = o.feet, .alive = o.alive, .inventory = o.inventory });
	Vec{Picked} got;
	defer got.free();
	self.drops.step(dt, self.world, takers.array_view(), &got);
	self.dye_in_vats();
	bool picked = false;
	foreach (p : got)
	{
		if (p.peer == 0)
		{
			picked = true;
		}
		else if (self.events && &self.events.picked_up)
		{
			self.events.picked_up(p.peer, p.stack);
		}
	}
	if (picked) self.play_sound("core:effects", "pickup", self.player.eye(), -8.0f);
}

// The host: the other players (set every frame).
fn void Game.set_others(&self, OtherPlayer[] others)
{
	self.others.clear();
	self.others.push_all(others);
}

// world.json's "plants".
fn Data Game.save_plants(&self) => self.core.save_plants();
fn void Game.load_plants(&self, Data* d) => self.core.load_plants(d);
