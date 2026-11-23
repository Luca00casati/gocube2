// Random numbers (splitmix64): seeded, so the same seed gives the same
// numbers (tests); seed_random() for a different run each time.
module gocube::util::rand;
import gocube::clock;

struct Rng
{
	ulong state;
}

fn Rng seeded(ulong seed) => { seed };

// Seeded from the clock (and the struct's address).
fn Rng seed_random()
{
	Rng r;
	r.state = (ulong)clock::now_us() * 0x9E3779B97F4A7C15 ^ (ulong)(uptr)&r;
	return r;
}

fn ulong Rng.next(&self)
{
	ulong z = (self.state += 0x9E3779B97F4A7C15);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
	return z ^ (z >> 31);
}

// 0 <= x < 1.
fn double Rng.unit(&self) => (double)(self.next() >> 11) / (double)(1ul << 53);

fn float Rng.range(&self, float lo, float hi) => lo + (hi - lo) * (float)self.unit();
fn double Rng.range_d(&self, double lo, double hi) => lo + (hi - lo) * self.unit();
