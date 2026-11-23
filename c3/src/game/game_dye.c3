// Throwing items (Q) and the dye vat: gems thrown in make its dye, blocks
// thrown in come out that colour.
module gocube::game;
import gocube::game::blocks, gocube::game::player, gocube::game::inventory, gocube::game::drops, gocube::game::block_entities;
import gocube::game::creatures, gocube::game::rules, gocube::game::vitals, gocube::game::dye;
import gocube::content, gocube::data, gocube::world, gocube::voxel, gocube::maths;
import gocube::util::vec, gocube::util::map, gocube::util::rand, gocube::util::intern;
import std::math, std::sort;

// Throws what `who` holds (one, or the whole stack) from `eye` towards `dir`,
// out of its inventory (in creative too). False if it holds nothing.
fn bool Game.throw_item(&self, Actor* who, float[3] eye, float[3] dir, bool whole)
{
	String item;
	int count = 1;
	if (who.inventory) // out of its inventory (creative too: throwing gives it away)
	{
		if (who.selected < 0 || who.selected >= inventory::HOTBAR) return false;
		Stack* s = &who.inventory.slots[who.selected];
		if (s.is_empty()) return false;
		item = s.id;
		count = whole ? s.count : 1;
		s.count -= count;
		if (s.count <= 0) *s = {};
	}
	else // (no inventory: what it says it holds)
	{
		item = who.item;
		if (item.len == 0 || (!self.content.items.has(item) && !self.rules.is_placeable(item))) return false;
	}
	float len = math::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len < 1e-4f) return false;
	float[3] d = { dir[0] / len, dir[1] / len, dir[2] / len };
	self.drops.throw_item(item, count, { eye[0] + d[0] * 0.4f, eye[1] - 0.35f + d[1] * 0.4f, eye[2] + d[2] * 0.4f },
		{ d[0] * THROW_SPEED, d[1] * THROW_SPEED + 2.0f, d[2] * THROW_SPEED });
	return true;
}

// The dye in the vat at `p` (NO_DYE: empty).
fn Dye Game.vat_dye(&self, BlockPos p)
{
	BlockEntity* e = self.entities.at(p);
	if (!e || e.type != "vat" || e.charge <= 0) return voxel::NO_DYE;
	Vec{Pigment} pigments;
	defer pigments.free();
	foreach (s : e.slots)
	{
		Data* def = s.is_empty() ? null : self.content.items.get(s.id);
		Dye color;
		if (def && content::parse_dye(def.str("dye"), &color)) pigments.push({ color, s.count });
	}
	return dye::snap_dye(dye::mix_dye(pigments.array_view()), &rules::DYE_PALETTE);
}

// Empty or full, the dye's colour.
fn void Game.set_vat_block(&self, BlockPos p, BlockEntity* e)
{
	String here = self.rules.id_of(self.world.block_at(p.x, p.y, p.z));
	String empty = self.rules.base_of(here);
	String full = self.rules.def(empty).str("full");
	bool filled = e.charge > 0 && full.len;
	int index = self.content.block_index(filled ? full : empty);
	if (index > 0) self.world.set_block(p, (BlockId)index, filled ? self.vat_dye(p) : voxel::NO_DYE);
}

// Right click with an empty hand: the dye is tipped out.
fn void Game.empty_vat(&self, BlockPos p)
{
	BlockEntity* e = self.entities.at(p);
	if (!e || e.type != "vat") return;
	foreach (&s : e.slots) *s = {};
	e.charge = 0;
	self.set_vat_block(p, e);
	self.play_sound("core:effects", "pickup", { p.x + 0.5f, p.y + 0.8f, p.z + 0.5f }, -4.0f);
}

struct LeftOver @private
{
	String item;
	int count;
	float[3] at;
}

// Each physics step: what fell into a vat.
fn void Game.dye_in_vats(&self) => @pool()
{
	Vec{LeftOver} left_over; // (added after: the drops mustn't grow while walked)
	defer left_over.free();
	foreach (&d : self.drops.drops)
	{
		if (d.count <= 0) continue;
		BlockPos cell = { maths::floor_i(d.pos[0]), maths::floor_i(d.pos[1] + 0.05f), maths::floor_i(d.pos[2]) };
		int block = self.world.block_id_at(cell.x, cell.y, cell.z);
		if (block <= 0 || !self.rules.def_of(block).flag("dye_vat")) continue;
		BlockEntity* e = self.entities.at(cell);
		if (!e) // (a vat from before it had one)
		{
			Vec{Stack} none;
			defer none.free();
			self.entities.block_changed(cell, self.rules.id_of(block), &none);
			e = self.entities.at(cell);
		}
		if (!e || e.type != "vat") continue;
		float[3] at = { cell.x + 0.5f, cell.y + 0.8f, cell.z + 0.5f };
		// A gem: into the dye.
		Data* def = self.content.items.get(d.item);
		Dye pigment;
		if (def && content::parse_dye(def.str("dye"), &pigment))
		{
			int held = 0;
			foreach (s : e.slots) held += s.is_empty() ? 0 : s.count;
			int n = math::min(d.count, VAT_MAX_GEMS - held);
			sz slot = -1;
			foreach (i, s : e.slots)
			{
				if (s.id == d.item && slot < 0) slot = i;
			}
			if (slot < 0)
			{
				foreach (i, s : e.slots)
				{
					if (s.is_empty() && slot < 0) slot = i;
				}
			}
			if (n <= 0 || slot < 0) continue; // full: it stays there, to be picked up again
			Stack* sl = e.slots.get_ref(slot);
			*sl = { d.item, (sl.is_empty() ? 0 : sl.count) + n };
			e.charge += n * DYE_PER_GEM;
			d.count -= n;
			self.set_vat_block(cell, e);
			self.play_sound("core:effects", "pickup", at, -4.0f);
			continue;
		}
		// A block: dyed, as many as there is dye for (not the vat itself).
		Dye mix = self.vat_dye(cell);
		if (mix == voxel::NO_DYE || !self.rules.is_placeable(d.item) || self.rules.def(d.item).flag("dye_vat") || content::dye_of(d.item) == mix)
		{
			continue;
		}
		int n = math::min(d.count, e.charge);
		if (n < d.count) left_over.push({ d.item, d.count - n, d.pos });
		d.item = intern::intern(content::dyed(tmem, d.item, mix));
		d.count = n;
		d.delay = 0; // (it comes back to whoever is near)
		e.charge -= n;
		if (e.charge <= 0)
		{
			foreach (&s : e.slots) *s = {};
		}
		self.set_vat_block(cell, e);
		self.play_sound("core:effects", "pickup", at, -2.0f);
	}
	for (sz i = 0; i < self.drops.drops.len();)
	{
		if (self.drops.drops[i].count <= 0)
		{
			self.drops.drops.remove_at(i);
		}
		else
		{
			i++;
		}
	}
	foreach (l : left_over) self.drops.throw_item(l.item, l.count, l.at, { 0, 0, 0 });
}
