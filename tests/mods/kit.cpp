// The kit test mod's code (tests/data/kit_mods/kit): crafting from its
// recipes, smelting, fuel, a hostile creature and planting, as a mod would add
// them (the base game has none of these; tests/test_survival.cpp plays with them).

#include "mods/mod_sdk.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace {

using gc::Data;

class Kit : public gc::sdk::Mod {
public:
	explicit Kit(const GcApi *api) : Mod(api) {
		set_rule("hand_recipes", [this](const Data &) { return hand_recipes(); });

		set_rule("smelting", [this](const Data &a) {
			const Data recipes_all = all("recipes");
			for (const auto &[id, r] : recipes_all.items()) {
				if (r.value("type", "") == "smelting" && r["input"] == a[0] && r["output"]["item"].is_string()) {
					return Data{{"item", r["output"]["item"]}, {"count", r["output"].value("count", 1)}};
				}
			}
			return Data();
		});

		set_rule("fuel_seconds", [](const Data &a) {
			const std::string id = a[0].is_string() ? a[0].str() : "";
			return Data(id == "core:coal" ? 40.0 : id == "kit:stick" ? 2.5 : 0.0);
		});

		on("tick", [this](const Data &a) { stalkers(a[0].get<double>()); });

		// Seeds planted on dirt.
		set_rule("use_item", [this](const Data &a) {
			const Data item = get("items", a[0].str());
			const int x = a[1].get<int>(), y = a[2].get<int>(), z = a[3].get<int>(), ny = a[5].get<int>();
			if (!item.is_object() || !item["plants"].is_string() || ny != 1 || block(x, y + 1, z) != "core:air") {
				return Data();
			}
			bool ok = false;
			for (const Data &b : item["plant_on"]) {
				ok = ok || b == block(x, y, z);
			}
			if (!ok) {
				return Data();
			}
			set_block(x, y + 1, z, item["plants"].str());
			return Data{{"take", 1}, {"placed", Data::array({x, y + 1, z})}};
		});
	}

private:
	std::map<int64_t, double> cooldown_;

	// {{id, ingredients = {[item] = count}, output = item, count = n}}, by output.
	Data hand_recipes() const {
		struct Recipe {
			std::string id, output;
			Data ingredients;
			int count;
		};
		std::vector<Recipe> list;
		const Data recipes_all = all("recipes");
		for (const auto &[id, r] : recipes_all.items()) {
			std::map<std::string, int> ingredients;
			bool ok = true;
			const std::string type = r.value("type", "");
			if (type == "shaped") {
				for (const Data &row : r["pattern"]) {
					for (char ch : row.str()) {
						if (ch != ' ') {
							const std::string item = r["key"].value(std::string(1, ch), "");
							ok = ok && !item.empty();
							ingredients[item]++;
						}
					}
				}
			} else if (type == "shapeless") {
				for (const auto &[item, count] : r["ingredients"].items()) {
					ingredients[item] = count.get<int>();
				}
			} else {
				ok = false;
			}
			const Data &output = r["output"];
			if (ok && !ingredients.empty() && output["item"].is_string()) {
				Data in = Data::object();
				for (const auto &[item, count] : ingredients) {
					in[item] = count;
				}
				list.push_back({id, output["item"].str(), in, output.value("count", 1)});
			}
		}
		std::sort(list.begin(), list.end(), [](const Recipe &a, const Recipe &b) {
			return a.output != b.output ? a.output < b.output : a.id < b.id;
		});
		Data out = Data::array();
		for (const Recipe &r : list) {
			out.push_back(Data{{"id", r.id}, {"ingredients", r.ingredients}, {"output", r.output}, {"count", r.count}});
		}
		return out;
	}

	// At night a stalker runs at the nearest survival player and strikes; in
	// daylight under open sky it's gone in a puff of smoke.
	void stalkers(double dt) {
		const Data players = call("game.players");
		const double day = call("game.day").get<double>();
		for (const Data &idv : call("creatures.all")) {
			const Data c = call("creatures.get", Data::array({idv}));
			if (!c.is_object() || c["kind"] != "kit:stalker") {
				continue;
			}
			const int64_t id = idv.get<int64_t>();
			const double cx = c["x"].get<double>(), cy = c["y"].get<double>(), cz = c["z"].get<double>();
			double &cool = cooldown_[id];
			cool = std::max(0.0, cool - dt);
			const Data *target = nullptr;
			double best = 24.0;
			for (const Data &p : players) {
				if (p.value("survival", false) && p.value("alive", false)) {
					const double dx = p["x"].get<double>() - cx, dz = p["z"].get<double>() - cz;
					const double d = std::sqrt(dx * dx + dz * dz);
					if (d < best) {
						target = &p;
						best = d;
					}
				}
			}
			if (target) {
				const double tx = (*target)["x"].get<double>() - cx, tz = (*target)["z"].get<double>() - cz;
				call("creatures.set", Data::array({id, Data{{"heading", std::atan2(-tx, -tz)}, {"speed", best > 1.2 ? 3.6 : 0.0}}}));
				if (best < 1.5 && std::abs((*target)["y"].get<double>() - cy) < 1.6 && cool <= 0) {
					cool = 1.0;
					call("creatures.set", Data::array({id, Data{{"attack", true}}}));
					double nx = 0, nz = 0;
					if (best > 1e-4) {
						nx = tx / best;
						nz = tz / best;
					}
					call("game.damage_player", Data::array({(*target)["peer"], 3, "mob", nx * 6.0, 4.0, nz * 6.0}));
				}
			}
			if (day >= 0.35 && call("world.raycast", Data::array({cx, cy + 1.9, cz, 0, 1, 0, 96})).is_null()) {
				call("game.effect", Data::array({"smoke", cx, cy + 0.9, cz}));
				call("creatures.remove", Data::array({id}));
				cooldown_.erase(id);
			}
		}
	}
};

} // namespace

GOCUBE_MOD(Kit)
