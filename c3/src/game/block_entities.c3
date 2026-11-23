// Data attached to some blocks: chests (27 slots) and dye vats (the gems in
// its dye, one slot per kind, and how many blocks it can still dye:
// "charge"). A block gets one when it appears, if its definition has
// "block_entity": "chest" or "vat"; when it goes, its contents spill. Saved in
// world.json's block_entities:
//   {"x,y,z": {type, slots: [[id, count] or false, ...], charge}}
// (No furnace: its smelting was only ever a mod's.)
module gocube::game::block_entities;
import gocube::game::inventory, gocube::content, gocube::data, gocube::world, gocube::util::vec, gocube::util::map, gocube::util::intern;
import std::math;

const int CHEST_SLOTS = 27;
const int VAT_KINDS = 8; // kinds of gem in one dye

struct BlockEntity
{
	String type;       // "chest", "vat" (interned)
	Vec{Stack} slots;
	int charge;        // vats: blocks it can still dye
}

fn void BlockEntity.free(&self) => self.slots.free();

fn BlockEntity make(String type) @private
{
	BlockEntity e = { .type = intern::intern(type) };
	e.slots.resize(type == "chest" ? CHEST_SLOTS : VAT_KINDS);
	return e;
}

fn bool known_type(String type) @private => type == "chest" || type == "vat";

struct BlockEntities
{
	Content* content;
	Map{long, BlockEntity*} entities; // by world::key(x, y, z)
}

fn void BlockEntities.free(&self)
{
	self.entities.@each(; long k, BlockEntity* e)
	{
		e.free();
		free(e);
	};
	self.entities.free();
}

fn void BlockEntities.clear(&self)
{
	self.free();
	self.entities = {};
}

fn sz BlockEntities.len(&self) => self.entities.len();

fn BlockEntity* BlockEntities.at(&self, BlockPos p) => self.entities.get_or(world::key(p.x, p.y, p.z), null);

fn String BlockEntities.kind_of(&self, String block)
{
	Data* def = self.content.blocks.get(block);
	String kind = def ? def.str("block_entity") : "";
	return known_type(kind) ? kind : "";
}

fn void BlockEntities.remove(&self, long k)
{
	if (BlockEntity* e = self.entities.get_or(k, null))
	{
		e.free();
		free(e);
		self.entities.remove(k);
	}
}

// A block at `p` became `block`: an entity is made, or the old one goes and
// its contents are added to `spilled` (to drop).
fn void BlockEntities.block_changed(&self, BlockPos p, String block, Vec{Stack}* spilled)
{
	String kind = self.kind_of(block);
	long k = world::key(p.x, p.y, p.z);
	BlockEntity* e = self.entities.get_or(k, null);
	if (e && e.type != kind)
	{
		foreach (s : e.slots)
		{
			if (!s.is_empty()) spilled.push(s);
		}
		self.remove(k);
		e = null;
	}
	if (kind.len && !e)
	{
		BlockEntity* made = mem::new(BlockEntity);
		*made = make(kind);
		self.entities[k] = made;
	}
}

// Mouse-style click on a slot while holding `held`; returns what is held now.
// (A vat's gems are its dye: not taken by hand.)
fn Stack BlockEntities.click(&self, BlockPos p, int slot, Stack held)
{
	BlockEntity* e = self.at(p);
	if (!e || slot < 0 || slot >= e.slots.len() || e.type == "vat") return held;
	Stack* here = e.slots.get_ref(slot);
	if (!held.is_empty() && here.id == held.id)
	{
		Inventory limits = inventory::new(self.content);
		int n = math::max(0, math::min(held.count, limits.max_stack(held.id) - here.count));
		here.count += n;
		Stack rest = held;
		rest.count -= n;
		return rest.count <= 0 ? (Stack){} : rest;
	}
	Stack was = *here;
	*here = held;
	return was;
}

fn Data slots_data(BlockEntity* e) @private
{
	Data slots = data::array();
	foreach (s : e.slots)
	{
		if (s.is_empty())
		{
			slots.push(data::boolean(false));
			continue;
		}
		Data pair = data::array();
		pair.push_string(s.id);
		pair.push_long(s.count);
		slots.push(pair);
	}
	return slots;
}

fn Data entity_to_data(BlockEntity* e) @private
{
	Data d = data::object();
	d.set_string("type", e.type);
	d.set("slots", slots_data(e));
	d.set_long("charge", e.charge);
	return d;
}

fn Data BlockEntities.serialize(&self) => @pool()
{
	Data out = data::object();
	self.entities.@each(; long k, BlockEntity* e)
	{
		ChunkPos p = world::pos_of(k);
		out.set(string::tformat("%d,%d,%d", p.x, p.y, p.z), entity_to_data(e));
	};
	return out;
}

// One entity in the same form (null: none there).
fn Data BlockEntities.entity_data(&self, BlockPos p)
{
	BlockEntity* e = self.at(p);
	return e ? entity_to_data(e) : data::NULL_DATA;
}

fn BlockEntity* BlockEntities.entity_from(&self, Data* d)
{
	String type = d.str("type", "chest");
	if (!known_type(type)) return null;
	BlockEntity* e = mem::new(BlockEntity);
	*e = make(type);
	Data* saved = d.get("slots");
	if (saved)
	{
		for (sz i = 0; i < saved.len() && i < e.slots.len(); i++)
		{
			Data* s = saved.at(i);
			if (s && s.is_array() && s.len() == 2 && s.at(0).is_string() && s.at(1).is_number() &&
				(self.content.items.has(s.at(0).s) || self.content.blocks.has(content::undyed(s.at(0).s))))
			{
				e.slots[i] = inventory::stack(s.at(0).s, (int)s.at(1).as_double());
			}
		}
	}
	e.charge = math::max(0, (int)d.num("charge", 0));
	return e;
}

// Setting null removes it. (A client's copies of the host's.)
fn void BlockEntities.set_entity_data(&self, BlockPos p, Data* d)
{
	long k = world::key(p.x, p.y, p.z);
	self.remove(k);
	if (!d || !d.is_object()) return;
	if (BlockEntity* e = self.entity_from(d)) self.entities[k] = e;
}

fn void BlockEntities.deserialize(&self, Data* d)
{
	self.clear();
	if (!d || !d.is_object()) return;
	foreach (&f : d.fields())
	{
		String[] parts = f.key.tsplit(",");
		if (parts.len != 3 || !f.value.is_object()) continue;
		int? x = parts[0].to_int();
		int? y = parts[1].to_int();
		int? z = parts[2].to_int();
		if (catch x) continue;
		if (catch y) continue;
		if (catch z) continue;
		if (BlockEntity* e = self.entity_from(&f.value)) self.entities[world::key(x, y, z)] = e;
	}
}
