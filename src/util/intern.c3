// Interned strings: one copy of each, kept for the whole run, so ids (items,
// blocks, sound sets...) can be passed around and stored as plain String
// views with nothing to free, and compared by content as usual. Thread-safe.
// (Kept outside the game's heap, on the C allocator: they're meant to stay.)
module gocube::util::intern;
import std::thread, libc;

String* slots @private; // open addressing; .ptr null: empty
usz cap @private;
usz count @private;
Mutex mutex @private;

fn void init() @init @private => mutex.init()!!;

fn usz hash_of(String s) @private
{
	usz h = 14695981039346656037;
	foreach (c : s) h = (h ^ c) * 1099511628211;
	return h;
}

fn void grow() @private
{
	usz new_cap = cap ? cap * 2 : 1024;
	String* fresh = (String*)libc::calloc(new_cap, String::size);
	for (usz i = 0; i < cap; i++)
	{
		String s = slots[i];
		if (!s.ptr) continue;
		usz j = hash_of(s) & (new_cap - 1);
		while (fresh[j].ptr) j = (j + 1) & (new_cap - 1);
		fresh[j] = s;
	}
	if (slots) libc::free(slots);
	slots = fresh;
	cap = new_cap;
}

// The kept copy of `s` ("" for an empty one).
fn String intern(String s)
{
	if (s.len == 0) return "";
	mutex.lock();
	defer mutex.unlock();
	if ((count + 1) * 2 > cap) grow();
	usz i = hash_of(s) & (cap - 1);
	while (slots[i].ptr)
	{
		if (slots[i] == s) return slots[i];
		i = (i + 1) & (cap - 1);
	}
	char* copy = (char*)libc::malloc((usz)s.len);
	mem::copy(copy, s.ptr, (usz)s.len);
	slots[i] = (String)copy[:s.len];
	count++;
	return slots[i];
}
