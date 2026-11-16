#include "game/game.h"

#include <algorithm>
#include <cmath>

namespace gc {

Game::Game(Content &content, const BlockTable &table, const BlockRules &rules, World &world, Mods &mods, Player &player,
		bool creative) :
		content_(content), table_(table), rules_(rules), world_(world), mods_(mods), player_(player),
		creative_(creative), inventory_(content), entities_(content, mods) {
	// Containers come and go with their blocks; what was in one spills out.
	world_.on_block_changed = [this](const BlockPos &p, gocube::BlockId id) {
		const float at[3] = {p.x + 0.5f, p.y + 0.3f, p.z + 0.5f};
		const std::vector<Stack> spilled = entities_.block_changed(p, rules_.id_of(id));
		if (remote_) {
			return; // (a client: the host spills them, and its mods hear of it)
		}
		for (const Stack &s : spilled) {
			drops_.spawn(s.id, s.count, at);
		}
		changed_.push_back({p, id});
	};
	Mods::WorldHooks hooks;
	hooks.drop_item = [this](const BlockPos &p, const std::string &item, int count) {
		const float at[3] = {p.x + 0.5f, p.y + 0.3f, p.z + 0.5f};
		drops_.spawn(item, count, at);
	};
	hooks.grow_tree = [this](const BlockPos &p, const std::string &tree) { return grow_tree(p, tree); };
	hooks.creature_spawn = [this](const std::string &kind, const float at[3], const std::string &variant) {
		return creatures_.spawn(content_, kind, at, variant);
	};
	hooks.creature_remove = [this](int id) { return creatures_.remove(id); };
	hooks.creature_ids = [this] {
		std::vector<int> ids;
		for (const Creature &c : creatures_.all()) {
			ids.push_back(c.id);
		}
		return ids;
	};
	hooks.creature_get = [this](int id) {
		Creature *c = creatures_.find(id);
		return c ? creatures_.describe(*c) : Json();
	};
	hooks.creature_set = [this](int id, const Json &fields) {
		Creature *c = creatures_.find(id);
		if (c) {
			creatures_.apply(*c, fields);
		}
		return c != nullptr;
	};
	hooks.players = [this] {
		Json list = Json::array({Json{{"peer", 0}, {"x", player_.position[0]}, {"y", player_.position[1]},
				{"z", player_.position[2]}, {"survival", !creative_}, {"alive", !dead_}}});
		for (const OtherPlayer &o : others_) {
			list.push_back({{"peer", o.peer}, {"x", o.feet[0]}, {"y", o.feet[1]}, {"z", o.feet[2]},
					{"survival", !creative_}, {"alive", o.alive}});
		}
		return list;
	};
	hooks.day = [this] { return day_; };
	hooks.first_night = [this] { return first_night_; };
	hooks.damage_player = [this](int peer, int amount, const std::string &source, const float push[3]) {
		if (peer != 0) {
			if (on_damage_peer && !creative_) {
				on_damage_peer(peer, amount, source, push);
			}
			return;
		}
		if (creative_ || dead_) {
			return;
		}
		damage(amount, source);
		for (int k = 0; k < 3; k++) {
			player_.velocity[k] += push[k];
		}
	};
	hooks.play_sound = [this](const std::string &set, const std::string &kind, const float at[3], float db) {
		play_sound(set, kind, at, db);
		if (on_shared_sound) {
			on_shared_sound(sounds_.back());
		}
	};
	hooks.effect = [this](const std::string &name, const float at[3]) {
		Effect e;
		e.name = name;
		std::copy(at, at + 3, e.at);
		if (on_effect) {
			on_effect(e);
		}
		effects_.push_back(std::move(e));
	};
	mods_.set_world_hooks(std::move(hooks));
}

Game::~Game() {
	world_.on_block_changed = nullptr;
	mods_.set_world_hooks({});
}

bool Game::grow_tree(const BlockPos &pos, const std::string &tree_id) {
	if (!generator_) {
		return false;
	}
	const std::vector<std::string> order = tree_order(content_);
	auto it = std::find(order.begin(), order.end(), tree_id);
	if (it == order.end()) {
		return false;
	}
	std::vector<gocube::TreeBlock> blocks;
	generator_->tree_blocks(int(it - order.begin()), pos.x, pos.y - 1, pos.z, blocks);
	if (blocks.empty()) {
		return false;
	}
	// Plants and leaves give way to a growing trunk.
	auto makes_room = [&](int block) {
		const Json &d = rules_.def(block);
		return d.value("replaceable", false) || d.value("leaves", false) || d.value("shape", "") == "cross";
	};
	for (const gocube::TreeBlock &b : blocks) {
		if (!world_.is_loaded(b.x, b.y, b.z)) {
			return false; // part of it would land in an unloaded chunk
		}
		const int here = world_.block_at(b.x, b.y, b.z);
		if (!b.only_air && !(BlockPos{b.x, b.y, b.z} == pos) && here != 0 && !makes_room(here)) {
			return false; // no room for its trunk
		}
	}
	for (const gocube::TreeBlock &b : blocks) {
		const int here = world_.block_at(b.x, b.y, b.z);
		if (here == 0 || (!b.only_air && (BlockPos{b.x, b.y, b.z} == pos || makes_room(here)))) {
			world_.set_block({b.x, b.y, b.z}, b.id);
		}
	}
	return true;
}

Json Game::save_plants() {
	const std::optional<Json> r = mods_.rule("save_plants", Json::array());
	return r && r->is_array() ? *r : Json::array();
}

void Game::load_plants(const Json &data) {
	if (data.is_array() && !data.empty()) {
		mods_.rule("load_plants", Json::array({data}));
	}
}

void Game::fill_creative() {
	// One of every placeable block (counts are never used up).
	const auto &ids = rules_.placeable();
	for (int i = 0; i < Inventory::SIZE && i < int(ids.size()); i++) {
		inventory_.slots[i] = {ids[i], 1};
	}
}

void Game::start_new_player() {
	inventory_ = Inventory(content_);
	selected_ = 0;
	if (creative_) {
		fill_creative();
		return;
	}
	give_starting_items(inventory_);
}

void Game::give_starting_items(Inventory &inv) {
	const std::optional<Json> items = mods_.rule("starting_items", Json::array());
	if (items && items->is_array()) {
		int slot = 0;
		for (const Json &s : *items) {
			if (slot < Inventory::SIZE && s.is_object() && s.value("id", "") != "") {
				inv.slots[slot++] = {s["id"].get<std::string>(), std::max(1, int(s.value("count", 1.0)))};
			}
		}
	}
}

void Game::load_player(const Json &saved) {
	if (!saved.contains("inventory")) {
		start_new_player(); // a player who never played here
		return;
	}
	inventory_.deserialize(saved["inventory"]);
	selected_ = std::clamp(int(saved.value("selected", 0.0)), 0, Inventory::HOTBAR - 1);
	health_ = std::clamp(int(saved.value("health", double(MAX_HEALTH))), 1, MAX_HEALTH);
	const Json bed = saved.value("bed", Json());
	if (bed.is_array() && bed.size() == 3 && bed[0].is_number()) {
		bed_ = BlockPos{int(bed[0].get<double>()), int(bed[1].get<double>()), int(bed[2].get<double>())};
	}
	if (creative_ && std::all_of(inventory_.slots.begin(), inventory_.slots.end(), [](const Stack &s) { return s.empty(); })) {
		fill_creative();
	}
}

Json Game::save_player() const {
	Json s = Json::object();
	s["inventory"] = inventory_.serialize();
	s["selected"] = selected_;
	if (!creative_) {
		s["health"] = health_;
	}
	if (bed_) {
		s["bed"] = {bed_->x, bed_->y, bed_->z};
	}
	return s;
}

const std::vector<Recipe> &Game::hand_recipes() {
	if (!recipes_) {
		recipes_.emplace();
		const std::optional<Json> list = mods_.rule("hand_recipes", Json::array());
		if (list && list->is_array()) {
			for (const Json &r : *list) {
				if (!r.is_object() || !r.contains("output") || !r["output"].is_string()) {
					continue;
				}
				Recipe recipe;
				recipe.id = r.value("id", "");
				recipe.output = r["output"].get<std::string>();
				recipe.count = std::max(1, int(r.value("count", 1.0)));
				const Json ingredients = r.value("ingredients", Json::object()); // (a local: items() of a temporary dangles)
				for (const auto &[item, n] : ingredients.items()) {
					if (n.is_number()) {
						recipe.ingredients.push_back({item, std::max(1, int(n.get<double>()))});
					}
				}
				std::sort(recipe.ingredients.begin(), recipe.ingredients.end());
				if (!recipe.ingredients.empty()) {
					recipes_->push_back(std::move(recipe));
				}
			}
		}
	}
	return *recipes_;
}

bool Game::can_craft(const Inventory &inv, const Recipe &r) const {
	if (creative_) {
		return true;
	}
	for (const auto &[item, n] : r.ingredients) {
		if (inv.count_of(item) < n) {
			return false;
		}
	}
	return true;
}

bool Game::craft(Inventory &inv, const Recipe &r) {
	if (creative_) {
		inv.add(r.output, r.count);
		return true;
	}
	if (!can_craft(inv, r)) {
		return false;
	}
	const Inventory before = inv;
	for (const auto &[item, n] : r.ingredients) {
		inv.remove(item, n);
	}
	if (inv.add(r.output, r.count) > 0) {
		inv = before; // no room: undo
		return false;
	}
	return true;
}

void Game::inventory_action(const Json &action) {
	const float at[3] = {player_.position[0], player_.position[1], player_.position[2]};
	if (apply_inventory_action(inventory_, action, at) && remote_ && !creative_) {
		remote_->inventory_action(action); // (the host keeps a survival player's inventory)
	}
}

bool Game::apply_inventory_action(Inventory &inv, const Json &a, const float feet[3]) {
	if (!a.is_array() || a.empty() || !a[0].is_string()) {
		return false;
	}
	const std::string kind = a[0].get<std::string>();
	auto integer = [&](size_t i, int lo, int hi, int &out) {
		if (a.size() <= i || !a[i].is_number_integer()) {
			return false;
		}
		const long long v = a[i].get<long long>();
		if (v < lo || v > hi) {
			return false;
		}
		out = int(v);
		return true;
	};
	int slot = 0;
	if (kind == "click" && integer(1, 0, Inventory::SIZE - 1, slot)) {
		if (creative_ && inv.cursor.empty() && !inv.slots[slot].empty()) {
			inv.cursor = inv.slots[slot]; // creative: rearrange freely, items never run out
			inv.slots[slot] = {};
		} else {
			inv.click(slot);
		}
		return true;
	}
	if (kind == "armor" && integer(1, 0, 3, slot)) {
		inv.cursor = inv.put_armor(slot, inv.cursor);
		return true;
	}
	if (kind == "craft" && a.size() == 2 && a[1].is_string()) {
		for (const Recipe &r : hand_recipes()) {
			if (r.id == a[1].get<std::string>()) {
				return craft(inv, r);
			}
		}
		return false;
	}
	if (kind == "pick" && creative_ && a.size() == 2 && a[1].is_string()) {
		const std::string item = a[1].get<std::string>();
		if (!content_.items.count(item) && !rules_.is_placeable(item)) {
			return false;
		}
		inv.cursor = {item, 1};
		return true;
	}
	int x, y, z;
	if (kind == "container" && integer(1, INT32_MIN, INT32_MAX, x) && integer(2, INT32_MIN, INT32_MAX, y) &&
			integer(3, INT32_MIN, INT32_MAX, z) && integer(4, 0, 255, slot)) {
		BlockEntity *e = entities_.at({x, y, z});
		if (!e || slot >= int(e->slots.size())) {
			return false;
		}
		constexpr float CONTAINER_REACH = 8.0f;
		const float dx = x + 0.5f - feet[0], dy = y + 0.5f - (feet[1] + Player::EYE_HEIGHT), dz = z + 0.5f - feet[2];
		if (dx * dx + dy * dy + dz * dz > CONTAINER_REACH * CONTAINER_REACH) {
			return false;
		}
		inv.cursor = entities_.click({x, y, z}, slot, inv.cursor);
		return true;
	}
	if (kind == "close" && a.size() == 1) {
		const Stack left_over = inv.close_cursor();
		if (!left_over.empty() && !remote_) { // (a client's host drops it)
			const float at[3] = {feet[0], feet[1] + 1.2f, feet[2]};
			drops_.spawn(left_over.id, left_over.count, at);
		}
		return true;
	}
	return false;
}

void Game::set_health_from_host(int health) {
	health = std::clamp(health, 0, MAX_HEALTH);
	if (health < health_) {
		since_hurt_ = 0;
		float eye[3];
		player_.eye(eye);
		play_sound("core:effects", "hurt", eye);
	}
	health_ = health;
	if (health_ == 0 && !dead_) {
		dead_ = true;
		std::fill(player_.velocity, player_.velocity + 3, 0.0f);
	} else if (health_ > 0) {
		dead_ = false;
	}
}

bool Game::use(const Actor &who, bool sneak) {
	const RayHit hit = who.body->target(world_);
	if (!hit.hit) {
		return false;
	}
	// Hoes, seeds: the use_item rule.
	if (!who.item.empty()) {
		const std::optional<Json> r = mods_.rule("use_item", Json::array({who.item, hit.position.x, hit.position.y,
				hit.position.z, hit.normal.x, hit.normal.y, hit.normal.z}));
		if (r && r->is_object()) {
			const int take = int(r->value("take", 0.0));
			for (int i = 0; i < take && !creative_ && who.inventory; i++) {
				who.inventory->take_one(who.selected);
			}
			const Json placed = r->value("placed", Json());
			if (placed.is_array() && placed.size() == 3 && placed[0].is_number() && placed[1].is_number() &&
					placed[2].is_number()) {
				const BlockPos p{int(placed[0].get<double>()), int(placed[1].get<double>()), int(placed[2].get<double>())};
				const std::string id = rules_.id_of(world_.block_at(p.x, p.y, p.z));
				mods_.block_placed(p, id);
				const float center[3] = {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f};
				play_block_sound(id, "place", center);
			}
			return true;
		}
		if (r && !r->is_null()) {
			return true;
		}
	}
	// Placing (or using a door); survival uses the item up. Nobody may be
	// built into a block.
	auto occupied = [&](const BlockPos &cell) {
		if (who.peer != 0 && Player::overlaps_block(player_.position, cell)) {
			return true; // the local player
		}
		for (const OtherPlayer &o : others_) {
			if (o.peer != who.peer && Player::overlaps_block(o.feet, cell)) {
				return true;
			}
		}
		return false;
	};
	std::vector<BlockChange> changes;
	const bool done = who.body->use_or_place(world_, rules_, who.item, sneak, &changes, occupied);
	if (done) {
		// A door opened or closed sounds like the door; a placed block like itself.
		const std::string id = changes.empty() ? rules_.id_of(hit.block) : changes.front().id;
		const BlockPos at = changes.empty() ? hit.position : changes.front().pos;
		const float center[3] = {at.x + 0.5f, at.y + 0.5f, at.z + 0.5f};
		play_block_sound(id, "place", center);
	}
	const bool placed = std::any_of(changes.begin(), changes.end(), [](const BlockChange &c) { return c.placed; });
	if (placed && !creative_ && who.inventory) {
		who.inventory->take_one(who.selected);
	}
	for (const BlockChange &c : changes) {
		mods_.block_placed(c.pos, c.id);
	}
	return done;
}

float Game::mine_seconds(const std::string &block, const std::string &tool) {
	const std::optional<Json> r = mods_.rule("mine_seconds", Json::array({block, tool}));
	if (r && r->is_number()) {
		return std::max(0.05f, float(r->get<double>()));
	}
	const Json &def = rules_.def(block); // default: hardness x 0.75 s
	return std::max(0.1f, float(def.value("hardness", 1.0)) * 0.75f);
}

std::vector<std::string> Game::drops_of(const std::string &block, const std::string &tool) {
	std::vector<std::string> out;
	const std::optional<Json> r = mods_.rule("drops", Json::array({block, tool}));
	if (r && r->is_array()) {
		for (const Json &d : *r) {
			if (d.is_string() && !d.get<std::string>().empty()) {
				out.push_back(d.get<std::string>());
			}
		}
		return out;
	}
	if (!r || r->is_null()) { // default: the block itself (its base block)
		out.push_back(rules_.base_of(block));
	}
	return out;
}

void Game::play_sound(const std::string &set, const std::string &kind, const float at[3], float volume_db) {
	SoundEvent e;
	e.set = set;
	e.kind = kind;
	std::copy(at, at + 3, e.at);
	e.volume_db = volume_db;
	sounds_.push_back(std::move(e));
}

void Game::play_block_sound(const std::string &block, const std::string &kind, const float at[3], float volume_db) {
	const int index = content_.block_index(block);
	if (index < 0) {
		return;
	}
	const Json &def = rules_.def(gocube::BlockId(index));
	if (auto it = def.find("sound"); it != def.end() && it->is_string()) {
		play_sound(it->get<std::string>(), kind, at, volume_db);
	}
}

void Game::break_block(const BlockPos &p, const std::string &tool) {
	const std::string broken = rules_.id_of(world_.block_at(p.x, p.y, p.z));
	if (broken.empty() || broken == "core:air" || rules_.is_fluid(world_.block_at(p.x, p.y, p.z))) {
		return;
	}
	world_.set_block(p, 0);
	std::vector<BlockChange> changes = {{p, broken, false}};
	if (auto other = rules_.other_part(broken, p)) { // a door or bed goes as a whole
		const std::string there = rules_.id_of(world_.block_at(other->x, other->y, other->z));
		if (rules_.base_of(there) == rules_.base_of(broken)) {
			world_.set_block(*other, 0);
			changes.push_back({*other, there, false});
		}
	}
	if (!creative_) {
		const float at[3] = {p.x + 0.5f, p.y + 0.3f, p.z + 0.5f};
		for (const std::string &item : drops_of(broken, tool)) {
			drops_.spawn(item, 1, at);
		}
	}
	for (const BlockChange &c : changes) {
		mods_.block_broken(c.pos, c.id);
	}
	const float center[3] = {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f};
	play_block_sound(broken, "break", center);
	swings_++;
}

void Game::update(float dt, const GameInput &in) {
	if (!in.captured) {
		mining_progress_ = 0;
		mining_target_ = {1 << 30, 0, 0};
		return;
	}
	// Hotbar: number keys and the wheel.
	if (in.number_key >= 1 && in.number_key <= Inventory::HOTBAR) {
		selected_ = in.number_key - 1;
	}
	if (in.wheel != 0) {
		selected_ = ((selected_ + in.wheel) % Inventory::HOTBAR + Inventory::HOTBAR) % Inventory::HOTBAR;
	}

	const RayHit hit = player_.target(world_);
	RayHit mine = hit; // (a creature in the way: nothing to mine)
	const std::string tool = selected_item();
	// A creature in front: a left click hits it (now and then), and it's in
	// the way of mining.
	attack_cooldown_ = std::max(0.0f, attack_cooldown_ - dt);
	if (const int creature = aimed_creature()) {
		if (in.break_pressed && attack_cooldown_ <= 0 && !dead_) {
			attack_cooldown_ = ATTACK_COOLDOWN;
			if (remote_) {
				remote_->hit_creature(creature);
			} else {
				const Json &item = content_.items.count(tool) ? content_.items.at(tool) : Json::object();
				const int damage = int(item.value("damage", 2.0)); // bare hand: 2
				mods_.creature_hit(creature, damage, player_.position, 0);
			}
			swings_++;
		}
		mine.hit = false;
	} else if (in.break_pressed && !hit.hit) {
		swings_++; // at nothing
	}
	// Breaking: at once in creative, by holding the button in survival.
	auto break_it = [&](const BlockPos &p) {
		if (remote_) {
			remote_->request_break(p, selected_);
			swings_++;
		} else {
			break_block(p, tool);
		}
	};
	if (creative_) {
		if (in.break_pressed && mine.hit) {
			break_it(mine.position);
		}
	} else if (in.break_held && mine.hit) {
		if (!(mine.position == mining_target_)) {
			mining_target_ = mine.position;
			mining_progress_ = 0;
			mining_seconds_ = mine_seconds(rules_.id_of(mine.block), tool);
			if (remote_) {
				remote_->start_mining(mine.position); // (the host times it too)
			}
		}
		mining_progress_ += dt / mining_seconds_;
		if (mining_progress_ >= 1.0f) {
			mining_progress_ = 0;
			mining_target_ = {1 << 30, 0, 0}; // (mining the next block starts over)
			break_it(mine.position);
		}
	} else {
		mining_progress_ = 0;
		mining_target_ = {1 << 30, 0, 0};
	}

	// Right click: a container opens, a bed becomes the respawn point (sneak
	// to place against them instead), food is eaten; else placing.
	const Json &target_def = hit.hit ? rules_.def(hit.block) : Json::object();
	int food = 0;
	if (in.use_pressed && !selected_item().empty()) {
		const std::optional<Json> r = mods_.rule("food", Json::array({selected_item()}));
		food = r && r->is_number() ? int(r->get<double>()) : 0;
	}
	if (in.use_pressed && hit.hit && !in.sneak && remote_ && !entities_.at(hit.position) &&
			target_def.contains("block_entity")) {
		// A client: a container that came with the host's chunks. An empty copy
		// for now; the host sends what's in it.
		entities_.block_changed(hit.position, rules_.id_of(hit.block));
	}
	if (in.use_pressed && hit.hit && !in.sneak && entities_.at(hit.position)) {
		opened_ = hit.position;
	} else if (in.use_pressed && hit.hit && !in.sneak && target_def.value("bed", false)) {
		// The head part is the one you lie on.
		BlockPos head = hit.position;
		const Json st = target_def.value("state", Json::object());
		if (st.value("part", "") == "foot") {
			if (auto other = rules_.other_part(rules_.id_of(hit.block), hit.position)) {
				head = *other;
			}
		}
		if (!bed_ || !(*bed_ == head)) {
			bed_ = head;
			show_notice("Respawn point set");
		}
		if (remote_) {
			remote_->set_bed(head);
		}
		bed_used_ = true;
	} else if (in.use_pressed && food > 0) {
		if (!creative_ && health_ < MAX_HEALTH && !dead_) {
			if (remote_) {
				remote_->request_eat(selected_); // (the host heals us, and takes it from its copy)
			} else {
				health_ = std::min(MAX_HEALTH, health_ + food);
			}
			inventory_.take_one(selected_);
			swings_++;
			float eye[3];
			player_.eye(eye);
			play_sound("core:effects", "pickup", eye, -4.0f); // (eating)
		}
	} else if (in.use_pressed && remote_) {
		if (hit.hit && !dead_) { // the host does it as us
			float eye[3], dir[3];
			player_.eye(eye);
			player_.look(dir);
			remote_->request_use(eye, dir, selected_, selected_item(), in.sneak);
			swings_++;
		}
	} else if (in.use_pressed) {
		Actor me;
		me.body = &player_;
		me.inventory = &inventory_;
		me.selected = selected_;
		me.item = selected_item();
		if (use(me, in.sneak)) {
			swings_++;
		}
	}

	// Creative: middle click puts the block aimed at in the hand.
	if (in.pick_pressed && creative_ && hit.hit) {
		const std::string base = rules_.base_of(rules_.id_of(hit.block));
		if (rules_.is_placeable(base)) {
			inventory_.slots[selected_] = {base, 1};
		}
	}
}

void Game::tick(double dt) {
	creatures_.animate(float(dt));
	if (!remote_) { // (a client: the host's furnaces burn, its mods hear of changes)
		entities_.update(dt);
		// Mods hear about block changes a tick later (their own changes too,
		// without re-entering them).
		const auto changes = std::exchange(changed_, {});
		for (const auto &[p, id] : changes) {
			mods_.block_changed(p, rules_.id_of(id));
		}
	} else {
		drops_.age(float(dt));
	}
	notice_time_ = std::max(0.0f, notice_time_ - float(dt));
	since_hurt_ += float(dt);
	if (spawn_pending_ && try_place_on_ground()) {
		spawn_pending_ = false;
		fall_peak_ = player_.position[1];
		if (!world_spawn_) { // where new players appear and the dead respawn
			world_spawn_ = std::array<float, 3>{player_.position[0], player_.position[1], player_.position[2]};
		}
	}
}

void Game::update_first_night() {
	constexpr float NINJA_NIGHT = 0.15f; // daylight below which ninjas come out
	if (first_night_ == "ahead" && day_ < NINJA_NIGHT) {
		first_night_ = "now";
	} else if (first_night_ == "now" && day_ > 0.3f) {
		first_night_ = "over"; // morning: from now on nights have ninjas
	}
}

int Game::aimed_creature() const {
	float eye[3], dir[3], distance;
	player_.eye(eye);
	player_.look(dir);
	const int id = creatures_.pick(eye, dir, ATTACK_REACH, distance);
	if (id == 0) {
		return 0;
	}
	// A block nearer than the creature (by its centre, as the Godot version) is what's aimed at.
	const RayHit hit = player_.target(world_);
	if (hit.hit) {
		const float dx = hit.position.x + 0.5f - eye[0], dy = hit.position.y + 0.5f - eye[1],
					dz = hit.position.z + 0.5f - eye[2];
		if (std::sqrt(dx * dx + dy * dy + dz * dz) < distance - 0.5f) {
			return 0;
		}
	}
	return id;
}

void Game::physics_step(float dt) {
	creatures_.step(dt, world_);
	if (remote_) {
		return; // (a client: the host's drops, shown as it sends them)
	}
	std::vector<ItemDrops::Taker> takers(1);
	std::copy(player_.position, player_.position + 3, takers[0].feet);
	takers[0].alive = !dead_;
	takers[0].inventory = &inventory_;
	for (const OtherPlayer &o : others_) {
		ItemDrops::Taker t;
		t.peer = o.peer;
		std::copy(o.feet, o.feet + 3, t.feet);
		t.alive = o.alive;
		t.inventory = o.inventory;
		takers.push_back(t);
	}
	bool picked = false;
	for (const ItemDrops::Picked &p : drops_.step(dt, world_, takers)) {
		if (p.peer == 0) {
			picked = true;
		} else if (on_pickup) {
			on_pickup(p.peer, p.stack);
		}
	}
	if (picked) {
		float eye[3];
		player_.eye(eye);
		play_sound("core:effects", "pickup", eye, -8.0f);
	}
}

} // namespace gc
