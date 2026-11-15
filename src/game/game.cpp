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
		for (const Stack &s : entities_.block_changed(p, rules_.id_of(id))) {
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
	const std::optional<Json> items = mods_.rule("starting_items", Json::array());
	if (items && items->is_array()) {
		int slot = 0;
		for (const Json &s : *items) {
			if (slot < Inventory::SIZE && s.is_object() && s.value("id", "") != "") {
				inventory_.slots[slot++] = {s["id"].get<std::string>(), std::max(1, int(s.value("count", 1.0)))};
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

bool Game::can_craft(const Recipe &r) const {
	if (creative_) {
		return true;
	}
	for (const auto &[item, n] : r.ingredients) {
		if (inventory_.count_of(item) < n) {
			return false;
		}
	}
	return true;
}

bool Game::craft(const Recipe &r) {
	if (creative_) {
		inventory_.add(r.output, r.count);
		return true;
	}
	if (!can_craft(r)) {
		return false;
	}
	const Inventory before = inventory_;
	for (const auto &[item, n] : r.ingredients) {
		inventory_.remove(item, n);
	}
	if (inventory_.add(r.output, r.count) > 0) {
		inventory_ = before; // no room: undo
		return false;
	}
	return true;
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
	const std::string tool = selected_item();
	// Breaking: at once in creative, by holding the button in survival.
	if (creative_) {
		if (in.break_pressed && hit.hit) {
			break_block(hit.position, tool);
		}
	} else if (in.break_held && hit.hit) {
		if (!(hit.position == mining_target_)) {
			mining_target_ = hit.position;
			mining_progress_ = 0;
			mining_seconds_ = mine_seconds(rules_.id_of(hit.block), tool);
		}
		mining_progress_ += dt / mining_seconds_;
		if (mining_progress_ >= 1.0f) {
			mining_progress_ = 0;
			mining_target_ = {1 << 30, 0, 0}; // (mining the next block starts over)
			break_block(hit.position, tool);
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
		bed_used_ = true;
	} else if (in.use_pressed && food > 0) {
		if (!creative_ && health_ < MAX_HEALTH) {
			health_ = std::min(MAX_HEALTH, health_ + food);
			inventory_.take_one(selected_);
		}
	} else if (in.use_pressed) {
		// Placing (or using a door) with the selected item; survival uses it up.
		std::vector<BlockChange> changes;
		const std::string item = selected_item();
		player_.use_or_place(world_, rules_, item, in.sneak, &changes);
		const bool placed = std::any_of(changes.begin(), changes.end(), [](const BlockChange &c) { return c.placed; });
		if (placed && !creative_) {
			inventory_.take_one(selected_);
		}
		for (const BlockChange &c : changes) {
			mods_.block_placed(c.pos, c.id);
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
	entities_.update(dt);
	// Mods hear about block changes a tick later (their own changes too,
	// without re-entering them).
	const auto changes = std::exchange(changed_, {});
	for (const auto &[p, id] : changes) {
		mods_.block_changed(p, rules_.id_of(id));
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

void Game::physics_step(float dt) {
	drops_.step(dt, world_, player_.position, true, inventory_);
}

} // namespace gc
