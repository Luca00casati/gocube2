#include "game/inventory.h"

#include <algorithm>

namespace gc {

const Data *Inventory::item_def(const std::string &id) const {
	if (auto it = content_->items.find(id); it != content_->items.end()) {
		return &it->second;
	}
	if (auto it = content_->blocks.find(id); it != content_->blocks.end()) {
		return &it->second;
	}
	return nullptr;
}

int Inventory::max_stack(const std::string &id) const {
	const Data *def = item_def(id);
	if (def && def->contains("max_stack") && (*def)["max_stack"].is_number()) {
		return std::max(1, int((*def)["max_stack"].get<double>()));
	}
	return DEFAULT_MAX_STACK;
}

int Inventory::add(const std::string &id, int count) {
	if (count <= 0 || id.empty()) {
		return 0;
	}
	const int limit = max_stack(id);
	for (Stack &s : slots) {
		if (count > 0 && s.id == id && s.count < limit) {
			const int n = std::min(count, limit - s.count);
			s.count += n;
			count -= n;
		}
	}
	for (Stack &s : slots) {
		if (count > 0 && s.empty()) {
			const int n = std::min(count, limit);
			s = {id, n};
			count -= n;
		}
	}
	return count;
}

bool Inventory::can_fit(const std::string &id) const {
	const int limit = max_stack(id);
	for (const Stack &s : slots) {
		if (s.empty() || (s.id == id && s.count < limit)) {
			return true;
		}
	}
	return false;
}

int Inventory::count_of(const std::string &id) const {
	int total = 0;
	for (const Stack &s : slots) {
		total += s.id == id ? s.count : 0;
	}
	return total;
}

bool Inventory::remove(const std::string &id, int count) {
	if (count_of(id) < count) {
		return false;
	}
	for (int i = SIZE - 1; i >= 0 && count > 0; i--) {
		Stack &s = slots[i];
		if (s.id == id) {
			const int n = std::min(count, s.count);
			s.count -= n;
			count -= n;
			if (s.count <= 0) {
				s = {};
			}
		}
	}
	return true;
}

std::string Inventory::take_one(int slot) {
	if (slot < 0 || slot >= SIZE || slots[slot].empty()) {
		return "";
	}
	const std::string id = slots[slot].id;
	if (--slots[slot].count <= 0) {
		slots[slot] = {};
	}
	return id;
}

Stack Inventory::put(int slot, const Stack &held) {
	if (slot < 0 || slot >= SIZE) {
		return held;
	}
	Stack here = slots[slot];
	if (!held.empty() && here.id == held.id) {
		const int n = std::min(held.count, max_stack(held.id) - here.count);
		slots[slot].count += std::max(0, n);
		Stack rest = held;
		rest.count -= std::max(0, n);
		return rest.count <= 0 ? Stack{} : rest;
	}
	slots[slot] = held;
	return here;
}

Stack Inventory::put_armor(int slot, const Stack &held) {
	if (slot < 0 || slot >= 4) {
		return held;
	}
	if (!held.empty()) {
		const Data *def = item_def(held.id);
		const Data worn = def ? def->value("armor", Data::object()) : Data::object();
		if (!worn.is_object() || worn.value("slot", "") != ARMOR_SLOTS[slot]) {
			return held; // doesn't go there
		}
	}
	const Stack was = armor[slot];
	armor[slot] = held.empty() ? Stack{} : Stack{held.id, 1};
	Stack rest = held;
	rest.count -= 1;
	// (The Godot version wears one and keeps nothing else held: armour doesn't stack.)
	return was.empty() ? (rest.count > 0 ? rest : Stack{}) : was;
}

int Inventory::armor_points() const {
	int total = 0;
	for (const Stack &s : armor) {
		if (!s.empty()) {
			const Data *def = item_def(s.id);
			const Data a = def ? def->value("armor", Data::object()) : Data::object();
			if (a.is_object() && a.contains("points") && a["points"].is_number()) {
				total += int(a["points"].get<double>());
			}
		}
	}
	return total;
}

Stack Inventory::close_cursor() {
	const Stack held = cursor;
	cursor = {};
	const int left = held.empty() ? 0 : add(held.id, held.count);
	return left == 0 ? Stack{} : Stack{held.id, left};
}

Data Inventory::serialize() const {
	Data out = Data::array();
	for (const Stack &s : slots) {
		out.push_back(s.empty() ? Data(false) : Data::array({s.id, s.count}));
	}
	Data worn = Data::array();
	for (const Stack &s : armor) {
		worn.push_back(s.empty() ? "" : s.id);
	}
	out.push_back({{"armor", worn}});
	return out;
}

void Inventory::deserialize(const Data &data) {
	slots.fill({});
	armor.fill({});
	cursor = {};
	if (!data.is_array()) {
		return;
	}
	size_t n = data.size();
	if (n > 0 && data.back().is_object()) {
		const Data worn = data.back().value("armor", Data::array());
		for (size_t i = 0; i < worn.size() && i < 4; i++) {
			if (worn[i].is_string() && content_->items.count(worn[i].get<std::string>())) {
				armor[i] = {worn[i].get<std::string>(), 1};
			}
		}
		n--;
	}
	for (size_t i = 0; i < n && i < SIZE; i++) {
		const Data &s = data[i];
		// Items whose mod is gone are dropped; everything else comes back.
		if (s.is_array() && s.size() == 2 && s[0].is_string() && s[1].is_number() && item_def(s[0].get<std::string>())) {
			slots[i] = {s[0].get<std::string>(), int(s[1].get<double>())};
		}
	}
}

} // namespace gc
