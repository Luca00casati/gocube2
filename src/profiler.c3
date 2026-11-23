// A small frame profiler (what's slow): named timers around the parts of a
// frame (@profile("name") { ... }), added up per frame, shown on the F3
// screen as an average and the worst frame of the last WINDOW frames. Nested
// timers show under the one around them. Main thread only: work on the
// world's threads isn't timed here.
module gocube::profiler;
import gocube::clock, gocube::util::vec;

const int WINDOW = 120; // frames the worst is taken from (2 s at 60 fps)

struct Entry
{
	String name;    // (a literal)
	int depth;      // how many timers are around it
	double average; // ms a frame (smoothed)
	double worst;   // ms, the worst frame of the last WINDOW
	double now;     // this frame so far
	double[WINDOW] history;
}

struct Profiler
{
	Vec{Entry} entries;
	int frame, depth;
}

// The game's profiler (main thread).
Profiler profiler;

fn void Profiler.free(&self) => self.entries.free();

fn Entry* Profiler.entry(&self, String name, int depth)
{
	foreach (&e : self.entries)
	{
		if (e.name == name) return e;
	}
	self.entries.push({ .name = name, .depth = depth });
	return self.entries.last();
}

// Adds `ms` to `name` in this frame (a timer at `depth`).
fn void Profiler.add(&self, String name, double ms, int depth = 0) => self.entry(name, depth).now += ms;

// A new frame: the last one's times go into the averages and worsts.
fn void Profiler.begin_frame(&self)
{
	foreach (&e : self.entries)
	{
		e.average = self.frame == 0 || e.average == 0 ? e.now : e.average * 0.95 + e.now * 0.05;
		e.history[self.frame % WINDOW] = e.now;
		e.worst = 0;
		foreach (h : e.history) if (h > e.worst) e.worst = h;
		e.now = 0;
	}
	self.frame++;
}

// The table as text (temp): a line a timer (average and worst, ms), indented.
fn String Profiler.report(&self)
{
	DString out;
	out.tinit();
	out.appendf("%-48s %9s %9s\n", "", "average", "worst");
	foreach (&e : self.entries)
	{
		DString name;
		name.tinit();
		for (int i = 0; i < e.depth * 2; i++) name.append_char(' ');
		name.append(e.name);
		out.appendf("%-48s %6.2f ms %6.2f ms\n", name.str_view(), e.average, e.worst);
	}
	return out.str_view();
}

// Times the body as `name`.
macro @profile(String $name; @body())
{
	int depth = profiler.depth++;
	long start = clock::now_us();
	defer
	{
		profiler.depth--;
		profiler.add($name, (double)(clock::now_us() - start) / 1000.0, depth);
	}
	@body();
}
