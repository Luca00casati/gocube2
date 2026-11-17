#include "game/block_entities.h"

#include "mods/mods.h"

#include <algorithm>
#include <cstdio>

namespace gc {

namespace {

std::string kind_of(const Content &content, const std::string &block) {
	auto it = content.blocks.find(block);
	if (it == content.blocks.end()) {
		return "";
	}
	const Data &def = it->second;
	auto k = def.find("block_entity");
	return k != def.end() && k->is_string() ? k->get<std::string>() : "";
}

} // namespace

BlockEntity BlockEntities::make(const std::string &type) {
	BlockEntity e;
	e.type = type;
	e.slots.resize(type == "chest" ? CHEST_SLOTS : 3);
	return e;
}

BlockEntity *BlockEntities::at(const BlockPos &p) {
	auto it = entities_.find({p.x, p.y, p.z});
	return it == entities_.end() ? nullptr : &it->second;
}

std::vector<Stack> BlockEntities::block_changed(const BlockPos &p, const std::string &block) {
	std::vector<Stack> spilled;
	const std::string kind = kind_of(content_, block);
	auto it = entities_.find({p.x, p.y, p.z});
	if (it != entities_.end() && it->second.type != kind) {
		for (const Stack &s : it->second.slots) {
			if (!s.empty()) {
				spilled.push_back(s);
			}
		}
		entities_.erase(it);
		it = entities_.end();
	}
	if (!kind.empty() && it == entities_.end()) {
		entities_[{p.x, p.y, p.z}] = make(kind);
	}
	return spilled;
}

double BlockEntities::fuel_seconds(const std::string &item) {
	if (item.empty()) {
		return 0;
	}
	const std::optional<Data> r = mods_.rule("fuel_seconds", Data::array({item}));
	return r && r->is_number() ? std::max(0.0, r->get<double>()) : 0.0;
}

std::optional<Stack> BlockEntities::smelting(const std::string &input) {
	if (input.empty()) {
		return std::nullopt;
	}
	const std::optional<Data> r = mods_.rule("smelting", Data::array({input}));
	if (!r || !r->is_object() || !r->contains("item") || !(*r)["item"].is_string()) {
		return std::nullopt;
	}
	return Stack{(*r)["item"].get<std::string>(), std::max(1, int(r->value("count", 1.0)))};
}

Stack BlockEntities::click(const BlockPos &p, int slot, const Stack &held) {
	BlockEntity *e = at(p);
	if (!e || slot < 0 || slot >= int(e->slots.size())) {
		return held;
	}
	if (e->type == "furnace") {
		if (slot == FURNACE_OUTPUT && !held.empty()) {
			return held;
		}
		if (slot == FURNACE_FUEL && !held.empty() && fuel_seconds(held.id) <= 0) {
			return held;
		}
	}
	Stack &here = e->slots[slot];
	if (!held.empty() && here.id == held.id) {
		const Inventory limits(content_);
		const int n = std::max(0, std::min(held.count, limits.max_stack(held.id) - here.count));
		here.count += n;
		Stack rest = held;
		rest.count -= n;
		return rest.count <= 0 ? Stack{} : rest;
	}
	const Stack was = here;
	here = held;
	return was;
}

void BlockEntities::update(double dt) {
	timer_ += dt;
	if (timer_ < TICK) {
		return;
	}
	for (auto &[key, e] : entities_) {
		if (e.type == "furnace") {
			tick_furnace(e, timer_);
		}
	}
	timer_ = 0;
}

void BlockEntities::tick_furnace(BlockEntity &e, double dt) {
	Stack &input = e.slots[FURNACE_INPUT];
	Stack &fuel = e.slots[FURNACE_FUEL];
	Stack &out = e.slots[FURNACE_OUTPUT];
	if (input.empty() && e.burn <= 0 && e.progress <= 0) {
		return; // (idle: no need to ask the rules)
	}
	const std::optional<Stack> result = smelting(input.id);
	const Inventory limits(content_);
	const bool room = result && (out.empty() || (out.id == result->id && out.count + result->count <= limits.max_stack(result->id)));
	if (e.burn <= 0 && room && !fuel.empty()) {
		e.burn_total = fuel_seconds(fuel.id);
		e.burn = e.burn_total;
		if (--fuel.count <= 0) {
			fuel = {};
		}
	}
	if (e.burn > 0) {
		e.burn = std::max(0.0, e.burn - dt);
		if (room) {
			e.progress += dt;
			if (e.progress >= SMELT_SECONDS) {
				e.progress = 0;
				if (--input.count <= 0) {
					input = {};
				}
				if (out.empty()) {
					out = *result;
				} else {
					out.count += result->count;
				}
			}
		} else {
			e.progress = 0;
		}
	} else if (e.progress > 0) {
		e.progress = std::max(0.0, e.progress - dt * 2.0); // cools down without fuel
	}
}

void BlockEntities::furnace_status(const BlockEntity &e, float &burn, float &progress) {
	burn = e.burn_total > 0 ? float(e.burn / e.burn_total) : 0.0f;
	progress = float(e.progress / SMELT_SECONDS);
}

Data BlockEntities::serialize() const {
	Data out = Data::object();
	for (const auto &[key, e] : entities_) {
		const auto &[x, y, z] = key;
		Data slots = Data::array();
		for (const Stack &s : e.slots) {
			slots.push_back(s.empty() ? Data(false) : Data::array({s.id, s.count}));
		}
		out[std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z)] = {
				{"type", e.type}, {"slots", slots}, {"burn", e.burn}, {"burn_total", e.burn_total}, {"progress", e.progress}};
	}
	return out;
}

Data BlockEntities::entity_data(const BlockPos &p) const {
	auto it = entities_.find({p.x, p.y, p.z});
	if (it == entities_.end()) {
		return Data();
	}
	Data slots = Data::array();
	for (const Stack &s : it->second.slots) {
		slots.push_back(s.empty() ? Data(false) : Data::array({s.id, s.count}));
	}
	return {{"type", it->second.type}, {"slots", slots}, {"burn", it->second.burn},
			{"burn_total", it->second.burn_total}, {"progress", it->second.progress}};
}

void BlockEntities::set_entity_data(const BlockPos &p, const Data &d) {
	if (!d.is_object()) {
		entities_.erase({p.x, p.y, p.z});
		return;
	}
	Data one = Data::object();
	one["0,0,0"] = d;
	const auto saved = std::move(entities_);
	deserialize(one);
	auto it = entities_.find({0, 0, 0});
	std::optional<BlockEntity> e;
	if (it != entities_.end()) {
		e = std::move(it->second);
	}
	entities_ = std::move(saved);
	if (e) {
		entities_[{p.x, p.y, p.z}] = std::move(*e);
	}
}

void BlockEntities::deserialize(const Data &data) {
	entities_.clear();
	if (!data.is_object()) {
		return;
	}
	for (const auto &[key, d] : data.items()) {
		int x, y, z;
		if (sscanf_s(key.c_str(), "%d,%d,%d", &x, &y, &z) != 3 || !d.is_object()) {
			continue;
		}
		const std::string type = d.value("type", "chest");
		if (type != "chest" && type != "furnace") {
			continue;
		}
		BlockEntity e = make(type);
		const Data saved = d.value("slots", Data::array());
		for (size_t i = 0; i < saved.size() && i < e.slots.size(); i++) {
			const Data &s = saved[i];
			if (s.is_array() && s.size() == 2 && s[0].is_string() && s[1].is_number() &&
					(content_.items.count(s[0].get<std::string>()) || content_.blocks.count(s[0].get<std::string>()))) {
				e.slots[i] = {s[0].get<std::string>(), int(s[1].get<double>())};
			}
		}
		e.burn = d.value("burn", 0.0);
		e.burn_total = d.value("burn_total", 0.0);
		e.progress = d.value("progress", 0.0);
		entities_[{x, y, z}] = std::move(e);
	}
}

} // namespace gc
