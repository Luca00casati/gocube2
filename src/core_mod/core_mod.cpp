// The base game's code (mods/core's entry, core.dll): the rules of the game.
// Its content is data (mods/core/content.json); this sets the rules the game
// asks (mining, drops, health, food, the two recipes, the dye palette, what a
// new player starts with) and runs plants, fluids and creatures from events.
// Other mods change a rule by setting it again (rule_before gives this one).

#include "mods/mod_sdk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using gc::Data;

constexpr const char *AIR = "core:air";
constexpr double PI = std::numbers::pi;

// A cell as one integer (21 bits a coordinate, up to a million blocks out).
using Key = int64_t;
constexpr int64_t SPAN = int64_t(1) << 21;
constexpr int64_t HALF = SPAN >> 1;

Key key(int x, int y, int z) {
	return ((int64_t(x) + HALF) * SPAN + (int64_t(y) + HALF)) * SPAN + (int64_t(z) + HALF);
}

struct Cell {
	int x, y, z;
};

Cell unkey(Key k) {
	const int z = int(k % SPAN - HALF);
	k /= SPAN;
	return {int(k / SPAN - HALF), int(k % SPAN - HALF), z};
}

const int AROUND[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const int SIDES[4][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

bool yes(const Data &d) {
	return d.is_boolean() && d.get<bool>();
}

int floor_int(double v) {
	return int(std::floor(v));
}

// A set of cells that keeps the order they came in.
struct CellQueue {
	std::deque<Key> order;
	std::unordered_set<Key> in;

	void add(Key k) {
		if (in.insert(k).second) {
			order.push_back(k);
		}
	}
	// Up to n of them, taken out.
	std::vector<Key> take(size_t n) {
		std::vector<Key> out;
		while (!order.empty() && out.size() < n) {
			out.push_back(order.front());
			in.erase(order.front());
			order.pop_front();
		}
		return out;
	}
	size_t size() const { return order.size(); }
};

class Core : public gc::sdk::Mod {
public:
	explicit Core(const GcApi *api) : Mod(api) {
		rules();
		plants();
		fluids();
		creatures();
	}

private:
	// --- Looking things up ---------------------------------------------------------

	// Block definitions, looked up once each (they don't change while playing).
	std::unordered_map<std::string, Data> defs_;
	const Data &def(const std::string &id) {
		static const Data none = Data::object();
		if (id.empty()) {
			return none;
		}
		auto it = defs_.find(id);
		if (it == defs_.end()) {
			Data d = get("blocks", id);
			it = defs_.emplace(id, d.is_object() ? d : Data::object()).first;
		}
		return it->second;
	}

	bool is_fluid(const std::string &id) { return !id.empty() && def(id).contains("fluid"); }
	bool is_leaves(const std::string &id) { return !id.empty() && def(id).value("leaves", false); }

	// Every tree's log.
	std::optional<std::set<std::string>> logs_;
	bool is_log(const std::string &id) {
		if (!logs_) {
			logs_.emplace();
			const Data trees_all = all("trees");
			for (const auto &[tree, t] : trees_all.items()) {
				if (t["log"].is_string()) {
					logs_->insert(t["log"].str());
				}
			}
		}
		return logs_->count(id) > 0;
	}

	void drop_item(int x, int y, int z, const std::string &item, int count) const {
		call("world.drop_item", Data::array({x, y, z, item, count}));
	}
	bool grow_tree(int x, int y, int z, const std::string &tree) const {
		return yes(call("world.grow_tree", Data::array({x, y, z, tree})));
	}
	Data raycast_down(double x, double y, double z) const {
		return call("world.raycast", Data::array({x, y, z, 0, -1, 0, 400}));
	}
	double rand_range(double a, double b) const { return a + (b - a) * random(); }

	// --- The rules -------------------------------------------------------------------

	static constexpr double MINE_SECONDS_PER_HARDNESS = 0.375;
	static constexpr double MIN_MINE_SECONDS = 0.1;
	static constexpr double SAFE_FALL = 3.0; // blocks you can fall without damage

	void rules() {
		// Mining: hardness x 0.375 s, whatever is held (no tools).
		set_rule("mine_seconds", [this](const Data &a) {
			const Data &d = def(a[0].is_string() ? a[0].str() : "");
			return Data(std::max(MIN_MINE_SECONDS, d.value("hardness", 1.0) * MINE_SECONDS_PER_HARDNESS));
		});
		// Its "drops" ("" = nothing), else the block itself (a variant: its base
		// block). Plus its "extra_drops" ({{item, count, chance}, ...}), rolled now.
		set_rule("drops", [this](const Data &a) {
			const std::string block = a[0].is_string() ? a[0].str() : "";
			const Data &d = def(block);
			Data out = Data::array();
			const std::string main = d.value("drops", d.value("variant_of", block));
			if (!main.empty()) {
				out.push_back(main);
			}
			for (const Data &e : d["extra_drops"]) {
				if (e.is_array() && e.size() >= 3 && e[0].is_string() && random() < e[2].get<double>()) {
					for (int i = 0; i < e[1].get<int>(); i++) {
						out.push_back(e[0].str());
					}
				}
			}
			return out;
		});

		// Health: falls, falling out of the world, food.
		set_rule("fall_damage", [](const Data &a) {
			const double blocks = a[0].is_number() ? a[0].get<double>() : 0.0;
			return Data(blocks <= SAFE_FALL ? int64_t(0) : int64_t(std::floor(blocks - SAFE_FALL)));
		});
		// Below the world's bottom: damage every half second.
		set_rule("void_damage", [](const Data &) { return Data(4); });
		// Health an item gives when eaten (0: it isn't food).
		set_rule("food", [this](const Data &a) {
			const Data item = a[0].is_string() ? get("items", a[0].str()) : Data();
			return item.is_object() && item["food"].is_number() ? item["food"] : Data(0);
		});

		// The only crafting: planks from logs, and the dye vat from planks.
		set_rule("hand_recipes", [](const Data &) {
			return Data::array({
					Data{{"id", "planks"}, {"output", "core:planks"}, {"count", 4},
							{"ingredients", Data{{"core:log", 1}}}},
					Data{{"id", "dye_vat"}, {"output", "core:dye_vat"}, {"count", 1},
							{"ingredients", Data{{"core:planks", 6}}}},
			});
		});

		// The dye vat's colours: whatever the gems mix to, the nearest of these
		// natural tones (so no mix is garish).
		set_rule("dye_palette", [](const Data &) {
			return Data::array({
					// whites, greys, black
					"#ecebe6", "#b9b8b2", "#807f7b", "#4a4a48", "#232325",
					// reds, pinks
					"#b8392f", "#9c3b2e", "#6e2a2a", "#d98c8c", "#c26a7a", "#e8b8b0",
					// oranges, yellows, creams
					"#cc7a2e", "#a8552d", "#c9a23a", "#e0c872", "#ece0b8",
					// browns
					"#b08a62", "#6b4a2f", "#4a3221",
					// greens
					"#9cba4a", "#7a7f35", "#4e8a3e", "#5f7a3a", "#2f4f33", "#8fa58a", "#3f7f78",
					// blues
					"#4f9cb0", "#7fa8cf", "#3d62a8", "#5b6f86", "#263a63",
					// purples
					"#9b8bbf", "#6d4a8f", "#a04a86", "#5e3352",
			});
		});

		// What a new survival player starts with: a torch, in hand.
		set_rule("starting_items", [](const Data &) {
			return Data::array({Data{{"id", "core:torch"}, {"count", 1}}});
		});
	}

	// --- Plants: growing, falling, support, leaf decay, trees sprouting -----------------
	//
	// Falling: blocks with "falls" (sand, gravel) drop while there's air or a
	// fluid under them.
	// Support: plants ("plant_on") need one of those blocks below; blocks with
	// "needs_support" need a solid block below or behind them (torches, ladders,
	// doors); unsupported blocks pop off and drop their item.
	// Leaf decay: leaves more than LEAF_REACH steps (through leaves) from a log
	// fall after a short random delay, now and then dropping a sapling; placed
	// leaves are a "_placed" variant that never decays.
	// Growth: a block with "grows_into" turns into that block after about
	// "grow_seconds"; a sapling ("grows_tree": a tree id, or {[tree id] = weight})
	// grows into that tree, trying again later when there's no room. What's
	// growing is saved with the world (world.json's "plants"). (The base game has
	// no saplings: a mod's would grow this way.)
	// Trees sprout by themselves: every SPROUT_EVERY seconds, near one player, a
	// random spot SPROUT_MIN-SPROUT_MAX blocks away (out of the way of what they
	// see up close) whose top block has "sprouts_trees" ({[tree id] = weight}:
	// grass) and no log within SPROUT_SPACE grows one of those trees, if there's
	// room.

	static constexpr double PLANT_TICK = 1.0;
	static constexpr double SPROUT_EVERY = 20.0;
	static constexpr double SPROUT_MIN = 24.0, SPROUT_MAX = 48.0;
	static constexpr int SPROUT_SPACE = 4;
	static constexpr double TOP_Y = 200;
	static constexpr int LEAF_REACH = 4;
	static constexpr double SAPLING_CHANCE = 0.05;
	// At most this many cells (and leaves) are looked at a game tick, the rest later.
	static constexpr size_t MAX_CHECKS = 2048;
	static constexpr size_t MAX_LEAF_CHECKS = 128;

	std::map<Key, double> growing_;  // cell -> seconds left
	CellQueue check_;                // cells whose support (or falling) to check
	CellQueue leaf_check_;           // leaves to test for a nearby log
	std::map<Key, double> decaying_; // leaves -> seconds until they fall
	double plant_timer_ = 0, sprout_timer_ = 0;

	// Whether a log is within LEAF_REACH steps, going through leaves.
	bool near_log(int x, int y, int z) {
		std::unordered_map<Key, int> seen{{key(x, y, z), 0}};
		std::deque<Cell> queue{{x, y, z}};
		while (!queue.empty()) {
			const Cell p = queue.front();
			queue.pop_front();
			const int dist = seen[key(p.x, p.y, p.z)];
			for (const auto &a : AROUND) {
				const int nx = p.x + a[0], ny = p.y + a[1], nz = p.z + a[2];
				const Key k = key(nx, ny, nz);
				if (seen.count(k)) {
					continue;
				}
				const std::string b = block(nx, ny, nz);
				if (is_log(b)) {
					return true;
				}
				if (dist + 1 < LEAF_REACH && is_leaves(b)) {
					seen[k] = dist + 1;
					queue.push_back({nx, ny, nz});
				}
			}
		}
		return false;
	}

	// A tree id from "grows_tree": the id itself, or picked by weight.
	std::string pick_tree(const Data &grows) {
		if (grows.is_string()) {
			return grows.str();
		}
		std::vector<std::pair<std::string, double>> ids; // (sorted by id)
		double total = 0;
		for (const auto &[id, w] : grows.items()) {
			const double weight = w.is_number() ? w.get<double>() : 0.0;
			ids.push_back({id, weight});
			total += weight;
		}
		if (ids.empty()) {
			return "";
		}
		std::sort(ids.begin(), ids.end());
		double roll = random() * total;
		for (const auto &[id, w] : ids) {
			roll -= w;
			if (roll <= 0) {
				return id;
			}
		}
		return ids.back().first;
	}

	// A log within SPROUT_SPACE (sideways) of the cell, at its height or one up.
	bool log_near(int x, int y, int z) {
		for (int dx = -SPROUT_SPACE; dx <= SPROUT_SPACE; dx++) {
			for (int dz = -SPROUT_SPACE; dz <= SPROUT_SPACE; dz++) {
				for (int dy = 0; dy <= 1; dy++) {
					if (is_log(block(x + dx, y + dy, z + dz))) {
						return true;
					}
				}
			}
		}
		return false;
	}

	// One try at a tree coming up near a player.
	void sprout() {
		const Data players = call("game.players");
		if (players.empty()) {
			return;
		}
		const Data &p = players[size_t(std::floor(random() * double(players.size())))];
		const double angle = random() * 2 * PI;
		const double dist = SPROUT_MIN + (SPROUT_MAX - SPROUT_MIN) * random();
		const int x = floor_int(p["x"].get<double>() + std::cos(angle) * dist);
		const int z = floor_int(p["z"].get<double>() + std::sin(angle) * dist);
		const Data hit = raycast_down(x + 0.5, TOP_Y, z + 0.5);
		if (!hit.is_object()) {
			return;
		}
		const int y = hit["y"].get<int>() + 1;
		const Data &trees = def(hit["id"].str())["sprouts_trees"];
		if (!trees.is_object() || block(x, y, z) != AIR || log_near(x, y, z)) {
			return;
		}
		grow_tree(x, y, z, pick_tree(trees));
	}

	void plants() {
		on("block_changed", [this](const Data &a) {
			const int x = a[0].get<int>(), y = a[1].get<int>(), z = a[2].get<int>();
			const Data &d = def(a[3].is_string() ? a[3].str() : "");
			const Key k = key(x, y, z);
			if (d.contains("grows_into") || d.contains("grows_tree")) {
				growing_[k] = d.value("grow_seconds", 60.0) * (0.6 + 0.8 * random());
			} else {
				growing_.erase(k);
			}
			check_.add(k);
			for (const auto &o : AROUND) {
				const int nx = x + o[0], ny = y + o[1], nz = z + o[2];
				check_.add(key(nx, ny, nz));
				if (is_leaves(block(nx, ny, nz))) {
					leaf_check_.add(key(nx, ny, nz));
				}
			}
		});

		on("tick", [this](const Data &a) {
			const double dt = a[0].get<double>();
			sprout_timer_ += dt;
			if (sprout_timer_ >= SPROUT_EVERY) {
				sprout_timer_ = 0;
				sprout();
			}
			// Falling and support.
			for (Key k : check_.take(MAX_CHECKS)) {
				const auto [x, y, z] = unkey(k);
				const std::string id = block(x, y, z);
				const Data &d = def(id);
				if (d.value("falls", false)) {
					const std::string below = block(x, y - 1, z);
					if (below == AIR || is_fluid(below)) {
						set_block(x, y, z, AIR);
						set_block(x, y - 1, z, id); // (checked again next tick)
					}
				} else if (!id.empty() && id != AIR && !supported(x, y, z, id)) {
					const std::string drop = d.value("drops", d.value("variant_of", id));
					set_block(x, y, z, AIR); // (its neighbours get checked in turn)
					const bool upper_door = d.value("shape", "") == "door" && d["state"].value("half", "") == "upper";
					if (!drop.empty() && !upper_door) {
						drop_item(x, y, z, drop, 1);
					}
				}
			}
			// Leaves far from any log start to decay.
			for (Key k : leaf_check_.take(MAX_LEAF_CHECKS)) {
				const auto [x, y, z] = unkey(k);
				if (is_leaves(block(x, y, z)) && !decaying_.count(k) && !near_log(x, y, z)) {
					decaying_[k] = 0.3 + 2.7 * random();
				}
			}
			for (auto it = decaying_.begin(); it != decaying_.end();) {
				it->second -= dt;
				if (it->second > 0) {
					++it;
					continue;
				}
				const auto [x, y, z] = unkey(it->first);
				it = decaying_.erase(it);
				const std::string leaf = block(x, y, z);
				if (is_leaves(leaf)) {
					const std::string sapling = def(leaf).value("sapling", "");
					set_block(x, y, z, AIR);
					if (!sapling.empty() && random() < SAPLING_CHANCE) {
						drop_item(x, y, z, sapling, 1);
					}
				}
			}
			// Growth, once a second.
			plant_timer_ += dt;
			if (plant_timer_ < PLANT_TICK) {
				return;
			}
			std::vector<Key> done;
			for (auto &[k, left] : growing_) {
				left -= plant_timer_;
				if (left <= 0) {
					done.push_back(k);
				}
			}
			for (Key k : done) {
				growing_.erase(k);
				const auto [x, y, z] = unkey(k);
				const Data &d = def(block(x, y, z));
				if (d["grows_into"].is_string()) {
					set_block(x, y, z, d["grows_into"].str()); // (tracks the next stage)
				} else if (d.contains("grows_tree") && !grow_tree(x, y, z, pick_tree(d["grows_tree"]))) {
					growing_[k] = d.value("grow_seconds", 60.0) * 0.5; // no room yet: later
				}
			}
			plant_timer_ = 0;
		});

		// world.json's "plants": {{x, y, z, seconds left}, ...}.
		set_rule("save_plants", [this](const Data &) {
			std::vector<std::pair<Cell, double>> list;
			for (const auto &[k, left] : growing_) {
				list.push_back({unkey(k), left});
			}
			std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
				if (a.first.x != b.first.x) {
					return a.first.x < b.first.x;
				}
				if (a.first.y != b.first.y) {
					return a.first.y < b.first.y;
				}
				return a.first.z < b.first.z;
			});
			Data out = Data::array();
			for (const auto &[c, left] : list) {
				out.push_back(Data::array({c.x, c.y, c.z, left}));
			}
			return out;
		});

		set_rule("load_plants", [this](const Data &a) {
			for (const Data &e : a[0]) {
				if (e.is_array() && e.size() == 4 && e[0].is_number() && e[1].is_number() && e[2].is_number() &&
						e[3].is_number()) {
					growing_[key(floor_int(e[0].get<double>()), floor_int(e[1].get<double>()),
							floor_int(e[2].get<double>()))] = e[3].get<double>();
				}
			}
			return Data();
		});
	}

	// --- Fluids: flowing water and lava ------------------------------------------------
	//
	// Only cells near a recent change are looked at (every FLUID_TICK seconds), so
	// a still sea costs nothing. A fluid is a block with "fluid" (its name) and
	// "level": 8 = source, 7..1 = flowing, each block further losing one (or
	// "flow_step": lava 2).
	// Rules for a fluid cell:
	//   - flowing fluid must be fed: from above (-> level 7) or by a sideways
	//     neighbour one step higher; otherwise it shrinks and finally drains away
	//   - it falls into air below (as level 7) before spreading sideways
	//   - otherwise it spreads sideways into air, one step lower
	//   - lava touching water hardens: a source into stone, flowing into
	//     cobblestone; lava and water flowing into each other make cobblestone

	static constexpr double FLUID_TICK = 0.25;
	static constexpr size_t MAX_CELLS_PER_TICK = 1024;
	// A FLUID_TICK's cells are spread over game ticks, this many at a time (a
	// big flood then spreads more slowly; a few blocks of water at once).
	static constexpr size_t CELLS_PER_CALL = 64;

	bool fluids_learned_ = false;
	std::unordered_map<std::string, int> level_of_;              // block id -> level (fluids only)
	std::unordered_map<std::string, std::string> fluid_of_;      // block id -> fluid name
	std::map<std::string, std::map<int, std::string>> block_for_; // fluid name -> level -> block id
	std::map<std::string, int> step_of_;                          // fluid name -> levels lost per block
	std::string stone_ = AIR, cobblestone_ = AIR;

	CellQueue pending_;      // cells to look at next, in the order they came
	std::vector<Key> batch_; // this tick's
	size_t head_ = 0;
	double fluid_timer_ = 0;

	void learn_fluids() {
		fluids_learned_ = true;
		const Data fluids = with("blocks", "fluid");
		for (const auto &[id, d] : fluids.items()) {
			if (!d["fluid"].is_string()) {
				continue;
			}
			const int level = std::clamp(floor_int(d.value("level", 8.0)), 1, 8);
			const std::string fluid = d["fluid"].str();
			level_of_[id] = level;
			fluid_of_[id] = fluid;
			block_for_[fluid][level] = id;
			step_of_[fluid] = std::max(1, floor_int(d.value("flow_step", 1.0)));
		}
		stone_ = get("blocks", "core:stone").is_null() ? AIR : "core:stone";
		cobblestone_ = get("blocks", "core:cobblestone").is_null() ? AIR : "core:cobblestone";
	}

	std::string fluid_of(const std::string &id) const {
		auto it = fluid_of_.find(id);
		return it == fluid_of_.end() ? "" : it->second;
	}
	int level_of(const std::string &id) const {
		auto it = level_of_.find(id);
		return it == level_of_.end() ? 0 : it->second;
	}
	std::string block_for(const std::string &fluid, int level) const {
		auto f = block_for_.find(fluid);
		if (f == block_for_.end()) {
			return "";
		}
		auto it = f->second.find(level);
		return it == f->second.end() ? "" : it->second;
	}

	void set_fluid(int x, int y, int z, const std::string &fluid, int level) {
		const std::string there = fluid_of(block(x, y, z));
		if (!there.empty() && there != fluid) {
			set_block(x, y, z, cobblestone_);
			return;
		}
		if (const std::string id = block_for(fluid, level); !id.empty()) {
			set_block(x, y, z, id);
		}
	}

	void update_fluid(int x, int y, int z) {
		const std::string here = block(x, y, z);
		if (here.empty() || !level_of_.count(here)) {
			return; // (not loaded, or not a fluid)
		}
		const std::string fluid = fluid_of(here);
		const int level = level_of(here);
		const int step = step_of_[fluid];
		auto fluid_at = [&](int bx, int by, int bz) {
			const std::string b = block(bx, by, bz);
			return fluid_of(b.empty() ? AIR : b);
		};
		if (fluid == "lava") {
			for (const auto &a : AROUND) {
				if (a[1] != -1 && fluid_at(x + a[0], y + a[1], z + a[2]) == "water") {
					set_block(x, y, z, level == 8 ? stone_ : cobblestone_);
					return;
				}
			}
		}
		if (level < 8) {
			// Flowing: is something still feeding it?
			int fed = 0;
			if (fluid_at(x, y + 1, z) == fluid) {
				fed = 7;
			}
			for (const auto &a : SIDES) {
				const std::string n = block(x + a[0], y, z + a[2]);
				if (!n.empty() && fluid_of(n) == fluid) {
					fed = std::max(fed, level_of(n) - step);
				}
			}
			if (fed <= 0) {
				set_block(x, y, z, AIR); // drains away
				return;
			}
			if (fed != level) {
				const std::string id = block_for(fluid, fed);
				set_block(x, y, z, id.empty() ? here : id);
				return; // (changing looks at this cell and its neighbours again)
			}
		}
		// Fall first.
		const std::string under = block(x, y - 1, z);
		if (under.empty()) {
			return; // (not loaded)
		}
		const std::string under_fluid = fluid_of(under);
		if (under == AIR || (!under_fluid.empty() && under_fluid != fluid)) {
			set_fluid(x, y - 1, z, fluid, 7);
			return;
		}
		if (under_fluid == fluid) {
			if (level_of(under) < 7) {
				set_fluid(x, y - 1, z, fluid, 7);
			}
			if (level < 8) {
				return; // falling fluid lands on fluid: no sideways spread
			}
		}
		// Then sideways, one step lower.
		const int next_level = level - step;
		if (next_level < 1) {
			return;
		}
		for (const auto &a : SIDES) {
			const int px = x + a[0], pz = z + a[2];
			const std::string n = block(px, y, pz);
			const std::string nf = fluid_of(n);
			if (!n.empty() && (n == AIR || (nf == fluid && level_of(n) < next_level) || (!nf.empty() && nf != fluid))) {
				set_fluid(px, y, pz, fluid, next_level);
			}
		}
	}

	void fluids() {
		on("block_changed", [this](const Data &a) {
			const int x = a[0].get<int>(), y = a[1].get<int>(), z = a[2].get<int>();
			pending_.add(key(x, y, z));
			for (const auto &o : AROUND) {
				pending_.add(key(x + o[0], y + o[1], z + o[2]));
			}
		});

		on("tick", [this](const Data &a) {
			fluid_timer_ += a[0].get<double>();
			if (head_ >= batch_.size()) {
				if (fluid_timer_ < FLUID_TICK || pending_.size() == 0) {
					return;
				}
				fluid_timer_ = 0;
				if (!fluids_learned_) {
					learn_fluids();
				}
				// This tick's cells (at most MAX_CELLS_PER_TICK; the rest next time).
				batch_ = pending_.take(MAX_CELLS_PER_TICK);
				head_ = 0;
			}
			const size_t stop = std::min(batch_.size(), head_ + CELLS_PER_CALL);
			for (; head_ < stop; head_++) {
				const auto [x, y, z] = unkey(batch_[head_]);
				update_fluid(x, y, z);
			}
		});

		// How many cells wait to be looked at (for tests).
		set_rule("fluids_pending", [this](const Data &) {
			return Data(int64_t(pending_.size() + (batch_.size() - head_)));
		});
	}

	// --- Creatures (nothing here is hostile) ---------------------------------------------
	// The game moves their bodies (gravity, steps, turning towards `heading` at
	// `speed`); this decides where they go, spawns and despawns them, and handles
	// hits.
	//
	// Block Eaters are rare: one comes out by day now and then (every
	// SPAWN_EVERY seconds, at most MAX_EATERS) on grass open to the sky, 20-48
	// blocks from a player, and goes when no player is within DESPAWN_DISTANCE.
	// They graze (idle), wander, turn away from water and drops, and run off when
	// hit. Grazing, now and then one eats the block in front of its feet: the
	// block is gone. (One made of a block, which only a mod spawns, drops it when
	// defeated.)

	static constexpr int MAX_EATERS = 2;
	static constexpr double SPAWN_EVERY = 45.0;
	static constexpr double SPAWN_MIN = 20.0, SPAWN_MAX = 48.0;
	static constexpr double DESPAWN_DISTANCE = 80.0;
	static constexpr const char *EATER = "core:block_eater";
	static constexpr int EATER_HEALTH = 8;
	static constexpr double WALK_SPEED = 1.2, FLEE_SPEED = 3.4;
	static constexpr double EAT_CHANCE = 0.3; // each time one starts grazing

	struct Eater {
		std::string state = "idle";
		double time = 0;
		int health = EATER_HEALTH;
		std::optional<double> heading;
		double flee_x = 0, flee_z = 0;
	};
	std::map<int64_t, Eater> eaters_;
	double spawn_timer_ = 0;

	void block_sound(const std::string &id, const char *kind, double x, double y, double z,
			std::optional<double> db = std::nullopt) {
		const Data &d = def(id);
		if (d["sound"].is_string()) {
			Data args = Data::array({d["sound"].str(), kind, x, y, z});
			if (db) {
				args.push_back(*db);
			}
			call("game.play_sound", args);
		}
	}

	// Water at the next step, or a fall of more than 3 blocks.
	bool danger_ahead(const Data &c, double heading) {
		const int ax = floor_int(c["x"].get<double>() - std::sin(heading) * 0.9);
		const int az = floor_int(c["z"].get<double>() - std::cos(heading) * 0.9);
		const int ay = floor_int(c["y"].get<double>());
		for (int dy = 1; dy >= -3; dy--) {
			const std::string b = block(ax, ay + dy, az);
			if (is_fluid(b)) {
				return true;
			}
			if (!b.empty() && b != AIR) {
				return false;
			}
		}
		return true;
	}

	// Whether a Block Eater eats this block: solid, not a fluid, never storage
	// (the contents would spill) nor half of a door or bed.
	bool edible(const std::string &id) {
		if (id.empty() || id == AIR) {
			return false;
		}
		const Data &d = def(id);
		const std::string shape = d.value("shape", "");
		return d.value("solid", true) && !d.contains("fluid") && !d.contains("block_entity") && shape != "door" &&
				shape != "bed";
	}

	// Eats the block in front of its feet: it's gone.
	void try_eat(const Data &c) {
		const double yaw = c["yaw"].get<double>();
		const int x = floor_int(c["x"].get<double>() - std::sin(yaw) * 0.9);
		const int y = floor_int(c["y"].get<double>() - 0.5);
		const int z = floor_int(c["z"].get<double>() - std::cos(yaw) * 0.9);
		const std::string id = block(x, y, z);
		if (edible(id) && set_block(x, y, z, AIR)) {
			block_sound(id, "break", x + 0.5, y + 0.5, z + 0.5);
		}
	}

	void pick_state(Eater &s, const Data &c) {
		if (s.state == "flee" || random() < 0.45) {
			s.state = "idle"; // graze a while
			s.time = rand_range(2.0, 5.0);
			if (random() < EAT_CHANCE) {
				try_eat(c);
			}
		} else {
			s.state = "wander";
			s.time = rand_range(2.5, 6.0);
			s.heading = s.heading.value_or(c["heading"].get<double>()) + rand_range(-1.6, 1.6);
		}
	}

	void eater(int64_t id, Eater &s, const Data &c, double dt) {
		s.time -= dt;
		if (s.time <= 0) {
			pick_state(s, c);
		}
		double speed = 0;
		double heading = s.heading.value_or(c["heading"].get<double>());
		if (s.state == "wander") {
			speed = WALK_SPEED;
			if (c.value("hit_wall", false)) { // blocked: try another way
				heading += PI * 0.5 + random() * PI;
			}
		} else if (s.state == "flee") {
			speed = FLEE_SPEED;
			const double ax = c["x"].get<double>() - s.flee_x, az = c["z"].get<double>() - s.flee_z;
			if (ax * ax + az * az > 0.01) {
				heading = std::atan2(-ax, -az);
			}
		}
		if (speed > 0 && danger_ahead(c, heading)) {
			heading += PI * (0.5 + random()); // water or a drop ahead: turn away
		}
		s.heading = heading;
		call("creatures.set", Data::array({id, Data{{"heading", heading}, {"speed", speed}}}));
	}

	void spawn_eater(const Data &players) {
		const Data &p = players[size_t(std::floor(random() * double(players.size())))];
		const double angle = random() * 2 * PI;
		const double dist = rand_range(SPAWN_MIN, SPAWN_MAX);
		const int x = floor_int(p["x"].get<double>() + std::cos(angle) * dist);
		const int z = floor_int(p["z"].get<double>() + std::sin(angle) * dist);
		const Data hit = raycast_down(x + 0.5, TOP_Y, z + 0.5);
		if (!hit.is_object() || hit["id"] != "core:grass") {
			return; // only on grass, open to the sky
		}
		const int fy = hit["y"].get<int>() + 1;
		if (yes(call("world.box_free", Data::array({x + 0.5, fy, z + 0.5, 0.9, 0.95, 0.9})))) {
			const Data id = call("creatures.spawn", Data::array({EATER, x + 0.5, fy, z + 0.5}));
			if (id.is_number()) {
				Eater e;
				e.heading = random() * 2 * PI;
				eaters_[id.get<int64_t>()] = e;
			}
		}
	}

	void despawn_far(const Data &players) {
		for (const Data &id : call("creatures.all")) {
			const Data c = call("creatures.get", Data::array({id}));
			if (!c.is_object()) {
				continue;
			}
			bool nearby = false;
			for (const Data &p : players) {
				const double dx = p["x"].get<double>() - c["x"].get<double>();
				const double dy = p["y"].get<double>() - c["y"].get<double>();
				const double dz = p["z"].get<double>() - c["z"].get<double>();
				nearby = nearby || dx * dx + dy * dy + dz * dz < DESPAWN_DISTANCE * DESPAWN_DISTANCE;
			}
			if (!nearby || c["y"].get<double>() < -200) {
				call("creatures.remove", Data::array({id}));
				eaters_.erase(id.get<int64_t>());
			}
		}
	}

	int count(const char *kind) {
		int n = 0;
		for (const Data &id : call("creatures.all")) {
			n += call("creatures.get", Data::array({id}))["kind"] == kind;
		}
		return n;
	}

	void creatures() {
		on("tick", [this](const Data &a) {
			const double dt = a[0].get<double>();
			const Data players = call("game.players");
			const double day = call("game.day").get<double>();
			for (const Data &id : call("creatures.all")) {
				const Data c = call("creatures.get", Data::array({id}));
				if (c.is_object() && c["kind"] == EATER) {
					auto [it, made] = eaters_.try_emplace(id.get<int64_t>());
					if (made) {
						it->second.heading = c["heading"].get<double>();
					}
					eater(id.get<int64_t>(), it->second, c, dt);
				}
			}
			spawn_timer_ += dt;
			if (spawn_timer_ < SPAWN_EVERY) {
				return;
			}
			spawn_timer_ = 0;
			despawn_far(players);
			if (players.empty()) {
				return;
			}
			if (day > 0.3 && count(EATER) < MAX_EATERS) {
				spawn_eater(players);
			}
		});

		// Hit by a player: hurt, knocked back, runs off; defeated, it drops what it's made of.
		on("creature_hit", [this](const Data &a) {
			const int64_t id = a[0].get<int64_t>();
			const Data c = call("creatures.get", Data::array({id}));
			if (!c.is_object() || c["kind"] != EATER) {
				return;
			}
			const double fx = a[2].get<double>(), fz = a[4].get<double>();
			const double cx = c["x"].get<double>(), cy = c["y"].get<double>(), cz = c["z"].get<double>();
			Eater &s = eaters_[id];
			const std::string variant = c.value("variant", "");
			if (variant.empty()) {
				call("game.play_sound", Data::array({"core:effects", "hurt", cx, cy, cz, -4.0}));
			} else {
				block_sound(variant, "break", cx, cy, cz);
			}
			s.health -= a[1].get<int>();
			s.state = "flee";
			s.time = 3.0;
			s.flee_x = fx;
			s.flee_z = fz;
			double px = cx - fx, pz = cz - fz;
			const double len = std::sqrt(px * px + pz * pz);
			if (len > 1e-4) {
				px /= len;
				pz /= len;
			} else {
				px = pz = 0;
			}
			call("creatures.set", Data::array({id, Data{{"hurt", true}, {"push", Data::array({px * 4.0, 5.0, pz * 4.0})}}}));
			if (s.health <= 0) {
				if (!variant.empty()) {
					drop_item(floor_int(cx), floor_int(cy + 0.5), floor_int(cz), variant, 1);
				}
				call("creatures.remove", Data::array({id}));
				eaters_.erase(id);
			}
		});
	}
};

} // namespace

GOCUBE_MOD(Core)
