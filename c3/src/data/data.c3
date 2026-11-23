// The game's dynamic data value: what a JSON file (data/json.c3) holds and
// what goes over the network. Null, a boolean, an integer, a float, a
// string, a list or an object (string keys, kept in the order they were
// added). (src/data/data.*)
//
// A Data owns what it holds (on the heap): free() it. Values handed to
// set(), push() and add() are taken over (don't free them after); copy()
// makes an independent copy. Reading never fails: a missing field or a value
// of the wrong type gives the fallback (or null).
module gocube::data;
import gocube::util::vec, std::math;

enum DataType : char
{
	NULL,
	BOOLEAN,
	INTEGER,
	FLOAT,
	STRING,
	ARRAY,
	OBJECT,
}

struct Member
{
	String key;
	Data value;
}

alias DataList = Vec{Data};
alias MemberList = Vec{Member};

struct Data
{
	DataType type;
	union
	{
		bool b;
		long i;
		double f;
		String s;
		void* ptr; // DataList* (ARRAY), MemberList* (OBJECT)
	}
}

const Data NULL_DATA = {};

fn DataList* Data.dl(&self) @inline => (DataList*)self.ptr;
fn MemberList* Data.ml(&self) @inline => (MemberList*)self.ptr;

// --- Making values -----------------------------------------------------------------

fn Data boolean(bool b) => { .type = BOOLEAN, .b = b };
fn Data integer(long i) => { .type = INTEGER, .i = i };
fn Data number(double f) => { .type = FLOAT, .f = f };
fn Data string(String s) => { .type = STRING, .s = s.copy(mem) };
fn Data array() => { .type = ARRAY, .ptr = mem::new(DataList) };
fn Data object() => { .type = OBJECT, .ptr = mem::new(MemberList) };

// --- What it is --------------------------------------------------------------------

fn bool Data.is_null(&self) @inline => self.type == NULL;
fn bool Data.is_boolean(&self) @inline => self.type == BOOLEAN;
fn bool Data.is_number(&self) @inline => self.type == INTEGER || self.type == FLOAT;
fn bool Data.is_integer(&self) @inline => self.type == INTEGER;
fn bool Data.is_string(&self) @inline => self.type == STRING;
fn bool Data.is_array(&self) @inline => self.type == ARRAY;
fn bool Data.is_object(&self) @inline => self.type == OBJECT;
fn bool Data.is_structured(&self) @inline => self.type == ARRAY || self.type == OBJECT;

// --- Owning it ---------------------------------------------------------------------

fn void Data.free(&self)
{
	switch (self.type)
	{
		case STRING:
			free(self.s.ptr);
		case ARRAY:
			foreach (&e : *self.dl()) e.free();
			self.dl().free();
			free(self.ptr);
		case OBJECT:
			foreach (&m : *self.ml())
			{
				free(m.key.ptr);
				m.value.free();
			}
			self.ml().free();
			free(self.ptr);
		default:
	}
	*self = {};
}

fn Data Data.copy(&self)
{
	switch (self.type)
	{
		case STRING:
			return string(self.s);
		case ARRAY:
			Data out = array();
			foreach (&e : *self.dl()) out.dl().push(e.copy());
			return out;
		case OBJECT:
			Data out = object();
			foreach (&m : *self.ml()) out.ml().push({ m.key.copy(mem), m.value.copy() });
			return out;
		default:
			return *self;
	}
}

// --- Reading it --------------------------------------------------------------------

// A list's or object's entries; 0 for null; 1 for anything else.
fn sz Data.len(&self)
{
	switch (self.type)
	{
		case NULL: return 0;
		case ARRAY: return self.dl().len();
		case OBJECT: return self.ml().len();
		default: return 1;
	}
}

fn bool Data.is_empty(&self) => self.len() == 0;

// A list's element, or null if this isn't a list or i is out of range.
fn Data* Data.at(&self, sz i)
{
	if (self.type != ARRAY || i >= self.dl().len()) return null;
	return self.dl().get_ref(i);
}

// An object's field, or null if this isn't an object or it's missing.
fn Data* Data.get(&self, String key)
{
	if (self.type != OBJECT) return null;
	foreach (&m : *self.ml())
	{
		if (m.key == key) return &m.value;
	}
	return null;
}

fn bool Data.has(&self, String key) => self.get(key) != null;

// A list's elements (empty for anything else).
fn Data[] Data.items(&self) => self.type == ARRAY ? self.dl().array_view() : (Data[]){};
// An object's fields in order (empty for anything else).
fn Member[] Data.fields(&self) => self.type == OBJECT ? self.ml().array_view() : (Member[]){};

fn bool Data.as_bool(&self, bool fallback = false) => self.type == BOOLEAN ? self.b : fallback;

fn double Data.as_double(&self, double fallback = 0)
{
	switch (self.type)
	{
		case INTEGER: return (double)self.i;
		case FLOAT: return self.f;
		default: return fallback;
	}
}

fn long Data.as_long(&self, long fallback = 0)
{
	switch (self.type)
	{
		case INTEGER: return self.i;
		case FLOAT: return math::is_nan(self.f) ? fallback : (long)self.f;
		default: return fallback;
	}
}

fn int Data.as_int(&self, int fallback = 0) => self.is_number() ? (int)self.as_long() : fallback;
fn float Data.as_float(&self, float fallback = 0) => self.is_number() ? (float)self.as_double() : fallback;
// The string (a view of it), or `fallback` if this isn't one.
fn String Data.as_string(&self, String fallback = "") => self.type == STRING ? self.s : fallback;

// A field's value as each type, or `fallback` if this isn't an object, the
// field is missing or isn't of that type.
fn bool Data.flag(&self, String key, bool fallback = false)
{
	Data* v = self.get(key);
	return v ? v.as_bool(fallback) : fallback;
}

fn double Data.num(&self, String key, double fallback = 0)
{
	Data* v = self.get(key);
	return v ? v.as_double(fallback) : fallback;
}

fn long Data.long_of(&self, String key, long fallback = 0)
{
	Data* v = self.get(key);
	return v ? v.as_long(fallback) : fallback;
}

fn int Data.int_of(&self, String key, int fallback = 0)
{
	Data* v = self.get(key);
	return v ? v.as_int(fallback) : fallback;
}

fn float Data.float_of(&self, String key, float fallback = 0)
{
	Data* v = self.get(key);
	return v ? v.as_float(fallback) : fallback;
}

fn String Data.str(&self, String key, String fallback = "")
{
	Data* v = self.get(key);
	return v ? v.as_string(fallback) : fallback;
}

// --- Changing it -------------------------------------------------------------------

// Sets an object's field (taking `value` over); null becomes an empty object first.
fn void Data.set(&self, String key, Data value)
{
	if (self.type == NULL) *self = object();
	if (self.type != OBJECT)
	{
		value.free();
		return;
	}
	if (Data* old = self.get(key))
	{
		old.free();
		*old = value;
		return;
	}
	self.ml().push({ key.copy(mem), value });
}

// Adds a field without looking for one with the same key (the caller knows it's new).
fn void Data.add(&self, String key, Data value)
{
	if (self.type == NULL) *self = object();
	if (self.type != OBJECT)
	{
		value.free();
		return;
	}
	self.ml().push({ key.copy(mem), value });
}

// Shorthands: set a field to a value of each type.
fn void Data.set_bool(&self, String key, bool v) => self.set(key, boolean(v));
fn void Data.set_long(&self, String key, long v) => self.set(key, integer(v));
fn void Data.set_double(&self, String key, double v) => self.set(key, number(v));
fn void Data.set_string(&self, String key, String v) => self.set(key, string(v));

// Appends to a list (taking `value` over); null becomes an empty list first.
fn void Data.push(&self, Data value)
{
	if (self.type == NULL) *self = array();
	if (self.type != ARRAY)
	{
		value.free();
		return;
	}
	self.dl().push(value);
}

fn void Data.push_long(&self, long v) => self.push(integer(v));
fn void Data.push_double(&self, double v) => self.push(number(v));
fn void Data.push_string(&self, String v) => self.push(string(v));

// Removes a field; whether there was one.
fn bool Data.remove(&self, String key)
{
	if (self.type != OBJECT) return false;
	foreach (i, &m : *self.ml())
	{
		if (m.key == key)
		{
			free(m.key.ptr);
			m.value.free();
			self.ml().remove_at(i);
			return true;
		}
	}
	return false;
}

// Deep equality; an object's field order doesn't matter, and 1 == 1.0.
fn bool Data.same_as(&self, Data* other)
{
	if (self.is_number() && other.is_number())
	{
		if (self.type == INTEGER && other.type == INTEGER) return self.i == other.i;
		return self.as_double() == other.as_double();
	}
	if (self.type != other.type) return false;
	switch (self.type)
	{
		case NULL: return true;
		case BOOLEAN: return self.b == other.b;
		case STRING: return self.s == other.s;
		case ARRAY:
			if (self.dl().len() != other.dl().len()) return false;
			foreach (i, &e : *self.dl())
			{
				if (!e.same_as(other.dl().get_ref(i))) return false;
			}
			return true;
		case OBJECT:
			if (self.ml().len() != other.ml().len()) return false;
			foreach (&m : *self.ml())
			{
				Data* w = other.get(m.key);
				if (!w || !m.value.same_as(w)) return false;
			}
			return true;
		default:
			return false;
	}
}
