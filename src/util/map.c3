// A hash map on the heap (open addressing, linear probing); a zero value is
// ready to use (see vec.c3 for why not the standard library's). Keys are
// compared with == and hashed with .hash(); the map doesn't own what keys or
// values point to. free() it.
module gocube::util::map <Key, Value>;
import std::math;

struct Slot
{
	Key key;
	Value value;
	char state; // 0 empty, 1 used, 2 removed
}

struct Map
{
	Slot* slots;
	sz cap;    // a power of 2 (or 0)
	sz count;  // used
	sz filled; // used or removed
}

fn sz Map.len(&self) @inline => self.count;
fn bool Map.is_empty(&self) @inline => self.count == 0;

fn sz Map.find(&self, Key key)
{
	if (!self.cap) return -1;
	sz mask = self.cap - 1;
	sz i = (sz)((usz)key.hash() & (usz)mask);
	while (true)
	{
		Slot* s = &self.slots[i];
		if (s.state == 0) return -1;
		if (s.state == 1 && s.key == key) return i;
		i = (i + 1) & mask;
	}
}

fn Value* Map.get_ref(&self, Key key)
{
	sz i = self.find(key);
	return i < 0 ? null : &self.slots[i].value;
}

fn Value? Map.get(&self, Key key) @operator([])
{
	sz i = self.find(key);
	if (i < 0) return NOT_FOUND~;
	return self.slots[i].value;
}

fn Value Map.get_or(&self, Key key, Value fallback)
{
	sz i = self.find(key);
	return i < 0 ? fallback : self.slots[i].value;
}

fn bool Map.has(&self, Key key) => self.find(key) >= 0;

fn void Map.grow(&self)
{
	Slot* old = self.slots;
	sz old_cap = self.cap;
	sz cap = old_cap ? old_cap * 2 : 16;
	// (Mostly removed slots: same size, cleaned.)
	if (old_cap && self.count * 4 < old_cap) cap = old_cap;
	self.slots = (Slot*)calloc((usz)(cap * Slot::size));
	self.cap = cap;
	self.count = 0;
	self.filled = 0;
	for (sz i = 0; i < old_cap; i++)
	{
		if (old[i].state == 1) self.set(old[i].key, old[i].value);
	}
	if (old) free(old);
}

// The value's slot for `key`, made (zeroed) if missing; `made` says which.
fn Value* Map.slot(&self, Key key, bool* made = null)
{
	if ((self.filled + 1) * 10 > self.cap * 7) self.grow();
	sz mask = self.cap - 1;
	sz i = (sz)((usz)key.hash() & (usz)mask);
	sz removed = -1;
	while (true)
	{
		Slot* s = &self.slots[i];
		if (s.state == 0) break;
		if (s.state == 2)
		{
			if (removed < 0) removed = i;
		}
		else if (s.key == key)
		{
			if (made) *made = false;
			return &s.value;
		}
		i = (i + 1) & mask;
	}
	if (removed >= 0)
	{
		i = removed;
	}
	else
	{
		self.filled++;
	}
	Slot* s = &self.slots[i];
	s.key = key;
	s.value = {};
	s.state = 1;
	self.count++;
	if (made) *made = true;
	return &s.value;
}

fn void Map.set(&self, Key key, Value value) @operator([]=)
{
	*self.slot(key) = value;
}

// Removes `key`; whether it was there.
fn bool Map.remove(&self, Key key)
{
	sz i = self.find(key);
	if (i < 0) return false;
	self.slots[i].state = 2;
	self.slots[i].value = {};
	self.count--;
	return true;
}

fn void Map.clear(&self)
{
	if (self.slots) mem::clear(self.slots, (usz)(self.cap * Slot::size));
	self.count = 0;
	self.filled = 0;
}

fn void Map.free(&self)
{
	if (self.slots) free(self.slots);
	*self = {};
}

// Goes through every entry (in no particular order): @each(map; key, value).
// Don't add or remove during it.
macro void Map.@each(&self; @body(key, value))
{
	for (sz i = 0; i < self.cap; i++)
	{
		if (self.slots[i].state == 1) @body(self.slots[i].key, self.slots[i].value);
	}
}

// The same, with a pointer to each value.
macro void Map.@each_ref(&self; @body(key, value_ref))
{
	for (sz i = 0; i < self.cap; i++)
	{
		if (self.slots[i].state == 1) @body(self.slots[i].key, &self.slots[i].value);
	}
}
