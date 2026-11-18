#pragma once
// A small frame profiler (what's slow): named timers around the parts of a
// frame (GC_PROFILE("name") times to the end of the scope), added up per
// frame, shown on the F3 screen as an average and the worst frame of the last
// WINDOW frames. Nested timers show under the one around them. Main thread
// only: work on the world's threads isn't timed here (the F3 screen shows
// how many jobs wait).

#include <chrono>
#include <string>
#include <vector>

namespace gc {

class Profiler {
public:
	static constexpr int WINDOW = 120; // frames the worst is taken from (2 s at 60 fps)

	struct Entry {
		std::string name;
		int depth = 0;       // how many timers are around it
		double average = 0;  // ms a frame (smoothed)
		double worst = 0;    // ms, the worst frame of the last WINDOW
	};

	// A new frame: the last one's times go into the averages and worsts.
	void begin_frame();
	// Adds `ms` to `name` in this frame (a timer at `depth`).
	void add(const char *name, double ms, int depth = 0);
	// Every timer seen, in the order first seen.
	const std::vector<Entry> &entries() const { return entries_; }
	// The table as text: a line a timer (average and worst, ms), indented.
	std::string report() const;

	// Times from its making to the end of its scope.
	class Scope {
	public:
		Scope(Profiler &p, const char *name);
		~Scope();
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;

	private:
		Profiler &p_;
		const char *name_;
		int depth_;
		std::chrono::steady_clock::time_point start_;
	};

private:
	struct Track {
		double now = 0; // this frame so far
		std::vector<double> history; // the last WINDOW frames (a ring)
	};
	std::vector<Entry> entries_;
	std::vector<Track> tracks_;
	int frame_ = 0, depth_ = 0;
	size_t index_of(const char *name, int depth);
};

// The game's profiler (main thread). Also reads the GPU's times
// (Renderer::gpu_times) as "GPU: ..." timers.
Profiler &profiler();

} // namespace gc

#define GC_PROFILE_CAT2(a, b) a##b
#define GC_PROFILE_CAT(a, b) GC_PROFILE_CAT2(a, b)
#define GC_PROFILE(name) ::gc::Profiler::Scope GC_PROFILE_CAT(gc_profile_, __LINE__)(::gc::profiler(), name)
