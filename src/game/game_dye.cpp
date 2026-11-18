// Throwing items (Q) and the dye vat: gems thrown in make its dye, blocks
// thrown in come out that colour (game.h).

#include "game/dye.h"
#include "game/game.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

int floor_int(float v) {
	return int(std::floor(v));
}

} // namespace

bool Game::throw_item(const Actor &who, const float eye[3], const float dir[3], bool whole) {
	std::string item;
	int count = 1;
	if (who.inventory) { // out of its inventory (creative too: throwing gives it away)
		if (who.selected < 0 || who.selected >= Inventory::HOTBAR) {
			return false;
		}
		Stack &s = who.inventory->slots[size_t(who.selected)];
		if (s.empty()) {
			return false;
		}
		item = s.id;
		count = whole ? s.count : 1;
		s.count -= count;
		if (s.count <= 0) {
			s = {};
		}
	} else { // (no inventory: what it says it holds)
		item = who.item;
		if (item.empty() || (!content_.items.count(item) && !rules_.is_placeable(item))) {
			return false;
		}
	}
	const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len < 1e-4f) {
		return false;
	}
	const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
	const float at[3] = {eye[0] + d[0] * 0.4f, eye[1] - 0.35f + d[1] * 0.4f, eye[2] + d[2] * 0.4f};
	const float vel[3] = {d[0] * THROW_SPEED, d[1] * THROW_SPEED + 2.0f, d[2] * THROW_SPEED};
	drops_.throw_item(item, count, at, vel);
	return true;
}

gocube::Dye Game::vat_dye(const BlockPos &p) {
	const BlockEntity *e = entities_.at(p);
	if (!e || e->type != "vat" || e->charge <= 0) {
		return gocube::NO_DYE;
	}
	std::vector<Pigment> pigments;
	for (const Stack &s : e->slots) {
		auto it = s.empty() ? content_.items.end() : content_.items.find(s.id);
		gocube::Dye color = gocube::NO_DYE;
		if (it != content_.items.end() && parse_dye(it->second.value("dye", ""), color)) {
			pigments.push_back({color, s.count});
		}
	}
	if (!dye_palette_) { // the natural tones mixes snap to (none: any colour)
		dye_palette_.emplace();
		const std::optional<Data> list = mods_.rule("dye_palette", Data::array());
		for (const Data &c : list && list->is_array() ? *list : Data::array()) {
			gocube::Dye d = gocube::NO_DYE;
			if (c.is_string() && parse_dye(c.get<std::string>(), d)) {
				dye_palette_->push_back(d);
			}
		}
	}
	return snap_dye(mix_dye(pigments), *dye_palette_);
}

void Game::set_vat_block(const BlockPos &p, const BlockEntity &e) {
	const std::string here = rules_.id_of(world_.block_at(p.x, p.y, p.z));
	const std::string empty = rules_.base_of(here);
	const std::string full = rules_.def(empty).value("full", "");
	const bool filled = e.charge > 0 && !full.empty();
	const int index = content_.block_index(filled ? full : empty);
	if (index > 0) {
		world_.set_block(p, gocube::BlockId(index), filled ? vat_dye(p) : gocube::NO_DYE);
	}
}

void Game::empty_vat(const BlockPos &p) {
	BlockEntity *e = entities_.at(p);
	if (!e || e->type != "vat") {
		return;
	}
	std::fill(e->slots.begin(), e->slots.end(), Stack{});
	e->charge = 0;
	set_vat_block(p, *e);
	const float at[3] = {p.x + 0.5f, p.y + 0.8f, p.z + 0.5f};
	play_sound("core:effects", "pickup", at, -4.0f);
}

void Game::dye_in_vats() {
	std::vector<ItemDrops::Drop> &all = drops_.all();
	struct Left {
		std::string item;
		int count;
		float at[3];
	};
	std::vector<Left> left_over; // (added after: `all` mustn't grow while walked)
	for (ItemDrops::Drop &d : all) {
		if (d.count <= 0) {
			continue;
		}
		const BlockPos cell{floor_int(d.pos[0]), floor_int(d.pos[1] + 0.05f), floor_int(d.pos[2])};
		const int block = world_.block_id_at(cell.x, cell.y, cell.z);
		if (block <= 0 || !rules_.def(gocube::BlockId(block)).value("dye_vat", false)) {
			continue;
		}
		BlockEntity *e = entities_.at(cell);
		if (!e) { // (a vat from before it had one)
			entities_.block_changed(cell, rules_.id_of(gocube::BlockId(block)));
			e = entities_.at(cell);
		}
		if (!e || e->type != "vat") {
			continue;
		}
		const float at[3] = {cell.x + 0.5f, cell.y + 0.8f, cell.z + 0.5f};
		// A gem: into the dye.
		auto def = content_.items.find(d.item);
		gocube::Dye pigment = gocube::NO_DYE;
		if (def != content_.items.end() && parse_dye(def->second.value("dye", ""), pigment)) {
			int held = 0;
			for (const Stack &s : e->slots) {
				held += s.empty() ? 0 : s.count;
			}
			const int n = std::min(d.count, VAT_MAX_GEMS - held);
			auto slot = std::find_if(e->slots.begin(), e->slots.end(), [&](const Stack &s) { return s.id == d.item; });
			if (slot == e->slots.end()) {
				slot = std::find_if(e->slots.begin(), e->slots.end(), [](const Stack &s) { return s.empty(); });
			}
			if (n <= 0 || slot == e->slots.end()) {
				continue; // full: it stays there, to be picked up again
			}
			*slot = {d.item, (slot->empty() ? 0 : slot->count) + n};
			e->charge += n * DYE_PER_GEM;
			d.count -= n;
			set_vat_block(cell, *e);
			play_sound("core:effects", "pickup", at, -4.0f);
			continue;
		}
		// A block: dyed, as many as there is dye for (not the vat itself).
		const gocube::Dye mix = vat_dye(cell);
		if (mix == gocube::NO_DYE || !rules_.is_placeable(d.item) || rules_.def(d.item).value("dye_vat", false) ||
				dye_of(d.item) == mix) {
			continue;
		}
		const int n = std::min(d.count, e->charge);
		if (n < d.count) {
			left_over.push_back({d.item, d.count - n, {d.pos[0], d.pos[1], d.pos[2]}});
		}
		d.item = dyed(d.item, mix);
		d.count = n;
		d.delay = 0; // (it comes back to whoever is near)
		e->charge -= n;
		if (e->charge <= 0) {
			std::fill(e->slots.begin(), e->slots.end(), Stack{});
		}
		set_vat_block(cell, *e);
		play_sound("core:effects", "pickup", at, -2.0f);
	}
	all.erase(std::remove_if(all.begin(), all.end(), [](const ItemDrops::Drop &d) { return d.count <= 0; }), all.end());
	for (const Left &l : left_over) {
		const float still[3] = {0, 0, 0};
		drops_.throw_item(l.item, l.count, l.at, still);
	}
}

} // namespace gc
