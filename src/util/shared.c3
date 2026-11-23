// A value on the heap shared by several owners (threads too), counted: the
// last release() frees it (calling its free() if it has one). For snapshots
// worker threads read while the main thread moves on to a new one.
module gocube::util::shared <Type>;
import std::atomic;

struct Shared
{
	Atomic{int} refs;
	Type value;
}

// A new one (one reference: the caller's), its value zeroed.
fn Shared* new()
{
	Shared* s = mem::new(Shared);
	s.refs.store(1);
	return s;
}

fn Shared* Shared.retain(&self)
{
	self.refs.add(1);
	return self;
}

fn void Shared.release(&self)
{
	if (self.refs.sub(1) != 1) return;
	$if $defined(Type.free):
		self.value.free();
	$endif
	free(self);
}
