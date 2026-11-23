// A growable array on the heap. (The standard library's List and HashMap
// start on the temporary allocator when not initialized, which frees what
// they hold at the end of the enclosing @pool; these always use the heap
// unless told otherwise, so a zero value is ready to use.) free() it.
module gocube::util::vec <Type>;
import std::math;

struct Vec
{
	Type* items;
	sz count;
	sz capacity;
}

fn sz Vec.len(&self) @operator(len) @inline => self.count;
fn bool Vec.is_empty(&self) @inline => self.count == 0;

macro Type Vec.@item(&self, sz i) @operator([])
{
	assert(i >= 0 && i < self.count, "index %d out of %d", i, self.count);
	return self.items[i];
}

fn Type* Vec.get_ref(&self, sz i) @operator(&[]) @inline
{
	assert(i >= 0 && i < self.count, "index %d out of %d", i, self.count);
	return &self.items[i];
}

fn void Vec.set(&self, sz i, Type value) @operator([]=) @inline
{
	assert(i >= 0 && i < self.count, "index %d out of %d", i, self.count);
	self.items[i] = value;
}

fn Type[] Vec.array_view(&self) @inline => self.items ? self.items[:self.count] : (Type[]){};

fn void Vec.reserve(&self, sz wanted)
{
	if (wanted <= self.capacity) return;
	sz cap = math::max(self.capacity * 2, math::max(wanted, (sz)8));
	self.items = (Type*)realloc(self.items, (usz)(cap * Type::size));
	self.capacity = cap;
}

fn void Vec.push(&self, Type value)
{
	if (self.count == self.capacity) self.reserve(self.count + 1);
	self.items[self.count++] = value;
}

fn void Vec.push_all(&self, Type[] values)
{
	if (!values.len) return;
	self.reserve(self.count + values.len);
	mem::copy(self.items + self.count, values.ptr, (usz)(values.len * Type::size));
	self.count += values.len;
}

// Removes and returns the last one (there must be one).
fn Type Vec.pop(&self)
{
	assert(self.count > 0);
	return self.items[--self.count];
}

fn Type* Vec.last(&self) => self.count ? &self.items[self.count - 1] : null;

fn void Vec.clear(&self) @inline => self.count = 0;

// Grows (with zeroes) or shrinks to n.
fn void Vec.resize(&self, sz n)
{
	if (n > self.count)
	{
		self.reserve(n);
		mem::clear(self.items + self.count, (usz)((n - self.count) * Type::size));
	}
	self.count = n;
}

fn void Vec.remove_at(&self, sz i)
{
	assert(i >= 0 && i < self.count);
	if (i + 1 < self.count) mem::move(self.items + i, self.items + i + 1, (usz)((self.count - i - 1) * Type::size));
	self.count--;
}

// Removes the first n.
fn void Vec.remove_first(&self, sz n)
{
	n = math::min(n, self.count);
	if (n == 0) return;
	if (n < self.count) mem::move(self.items, self.items + n, (usz)((self.count - n) * Type::size));
	self.count -= n;
}

// Removes one, moving the last into its place (order not kept).
fn void Vec.swap_remove(&self, sz i)
{
	assert(i >= 0 && i < self.count);
	self.items[i] = self.items[self.count - 1];
	self.count--;
}

fn void Vec.insert_at(&self, sz i, Type value)
{
	assert(i >= 0 && i <= self.count);
	self.reserve(self.count + 1);
	if (i < self.count) mem::move(self.items + i + 1, self.items + i, (usz)((self.count - i) * Type::size));
	self.items[i] = value;
	self.count++;
}

// A copy (its own memory).
fn Vec Vec.copy(&self)
{
	Vec out;
	out.push_all(self.array_view());
	return out;
}

fn void Vec.free(&self)
{
	if (self.items) free(self.items);
	*self = {};
}
