// Block slots: 0..8 are the hotbar. A slot is empty or a stack {id, count}
// (placing one uses it up). Also what the mouse holds in an open screen (the
// cursor). Saved in world.json's player.inventory:
//   [[id, count] or false, ... (36)]
// (Older saves end with {"armor": [...]}: skipped.)
module gocube::game::inventory;
import gocube::content, gocube::data, gocube::util::intern, std::math;

struct Stack
{
	String id; // interned; "" = empty
	int count;
}

fn bool Stack.is_empty(self) @inline => self.id.len == 0 || self.count <= 0;
fn bool Stack.same(self, Stack o) => self.id == o.id && self.count == o.count;
fn Stack stack(String id, int count) => { intern::intern(id), count };

const int SIZE = 36;
const int HOTBAR = 9;
const int DEFAULT_MAX_STACK = 64;

struct Inventory
{
	Content* content;
	Stack[SIZE] slots;
	Stack cursor;       // held by the mouse in an open screen
}

fn Inventory new(Content* content) => { .content = content };

// The block's definition (null: not a block of the game's).
fn Data* Inventory.item_def(&self, String id) => self.content.blocks.get(id);

fn int Inventory.max_stack(&self, String id)
{
	Data* def = self.item_def(id);
	if (def && def.get("max_stack") && def.get("max_stack").is_number()) return math::max(1, (int)def.num("max_stack"));
	return DEFAULT_MAX_STACK;
}

// Adds items, filling existing stacks first, then empty slots (hotbar first).
// Returns how many didn't fit.
fn int Inventory.add(&self, String id, int count)
{
	if (count <= 0 || id.len == 0) return 0;
	id = intern::intern(id);
	int limit = self.max_stack(id);
	foreach (&s : self.slots)
	{
		if (count > 0 && s.id == id && s.count < limit)
		{
			int n = math::min(count, limit - s.count);
			s.count += n;
			count -= n;
		}
	}
	foreach (&s : self.slots)
	{
		if (count > 0 && s.is_empty())
		{
			int n = math::min(count, limit);
			*s = { id, n };
			count -= n;
		}
	}
	return count;
}

// At least one more fits.
fn bool Inventory.can_fit(&self, String id)
{
	int limit = self.max_stack(id);
	foreach (s : self.slots)
	{
		if (s.is_empty() || (s.id == id && s.count < limit)) return true;
	}
	return false;
}

fn int Inventory.count_of(&self, String id)
{
	int total = 0;
	foreach (s : self.slots) total += s.id == id ? s.count : 0;
	return total;
}

// Removes `count` from wherever they are (the back first, keeping the
// hotbar). False, and nothing removed, if there aren't enough.
fn bool Inventory.remove(&self, String id, int count)
{
	if (self.count_of(id) < count) return false;
	for (int i = SIZE - 1; i >= 0 && count > 0; i--)
	{
		Stack* s = &self.slots[i];
		if (s.id == id)
		{
			int n = math::min(count, s.count);
			s.count -= n;
			count -= n;
			if (s.count <= 0) *s = {};
		}
	}
	return true;
}

// Uses up one item from a slot; returns its id ("" if it was empty).
fn String Inventory.take_one(&self, int slot)
{
	if (slot < 0 || slot >= SIZE || self.slots[slot].is_empty()) return "";
	String id = self.slots[slot].id;
	if (--self.slots[slot].count <= 0) self.slots[slot] = {};
	return id;
}

// Mouse-style: puts `held` into a slot, returns what is held now (same item
// stacks up to the limit, otherwise the two swap).
fn Stack Inventory.put(&self, int slot, Stack held)
{
	if (slot < 0 || slot >= SIZE) return held;
	Stack here = self.slots[slot];
	if (!held.is_empty() && here.id == held.id)
	{
		int n = math::min(held.count, self.max_stack(held.id) - here.count);
		self.slots[slot].count += math::max(0, n);
		Stack rest = held;
		rest.count -= math::max(0, n);
		return rest.count <= 0 ? (Stack){} : rest;
	}
	self.slots[slot] = held;
	return here;
}

// Click on a slot in a screen: put down or pick up.
fn void Inventory.click(&self, int slot) => self.cursor = self.put(slot, self.cursor);

// A screen closed: what the mouse held goes back in; returns what didn't fit.
fn Stack Inventory.close_cursor(&self)
{
	Stack held = self.cursor;
	self.cursor = {};
	int left = held.is_empty() ? 0 : self.add(held.id, held.count);
	return left == 0 ? (Stack){} : (Stack){ held.id, left };
}

fn Data Inventory.serialize(&self)
{
	Data out = data::array();
	foreach (s : self.slots)
	{
		if (s.is_empty())
		{
			out.push(data::boolean(false));
			continue;
		}
		Data pair = data::array();
		pair.push_string(s.id);
		pair.push_long(s.count);
		out.push(pair);
	}
	return out;
}

// Unknown blocks and items (no longer in the game) are dropped.
fn void Inventory.deserialize(&self, Data* d)
{
	self.slots[..] = {};
	self.cursor = {};
	if (!d || !d.is_array()) return;
	sz n = d.len();
	if (n > 0 && d.at(n - 1).is_object()) n--; // (worn armour, in older saves)
	for (sz i = 0; i < n && i < SIZE; i++)
	{
		Data* s = d.at(i);
		if (s.is_array() && s.len() == 2 && s.at(0).is_string() && s.at(1).is_number() && self.item_def(s.at(0).s))
		{
			self.slots[i] = stack(s.at(0).s, (int)s.at(1).as_double());
		}
	}
}
