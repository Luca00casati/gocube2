#include "profiler.h"

#include <algorithm>
#include <cstdio>

namespace gc {

size_t Profiler::index_of(const char *name, int depth) {
	for (size_t i = 0; i < entries_.size(); i++) {
		if (entries_[i].name == name) {
			return i;
		}
	}
	Entry e;
	e.name = name;
	e.depth = depth;
	entries_.push_back(e);
	Track t;
	t.history.assign(WINDOW, 0.0);
	tracks_.push_back(std::move(t));
	return entries_.size() - 1;
}

void Profiler::add(const char *name, double ms, int depth) {
	Track &t = tracks_[index_of(name, depth)];
	t.now += ms;
}

void Profiler::begin_frame() {
	for (size_t i = 0; i < entries_.size(); i++) {
		Entry &e = entries_[i];
		Track &t = tracks_[i];
		e.average = frame_ == 0 || e.average == 0 ? t.now : e.average * 0.95 + t.now * 0.05;
		t.history[size_t(frame_ % WINDOW)] = t.now;
		e.worst = *std::max_element(t.history.begin(), t.history.end());
		t.now = 0;
	}
	frame_++;
}

Profiler::Scope::Scope(Profiler &p, const char *name)
		: p_(p), name_(name), depth_(p.depth_++), start_(std::chrono::steady_clock::now()) {}

Profiler::Scope::~Scope() {
	p_.depth_--;
	p_.add(name_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(), depth_);
}

std::string Profiler::report() const {
	std::string out;
	char line[160];
	std::snprintf(line, sizeof(line), "%-48s %9s %9s\n", "", "average", "worst");
	out += line;
	for (const Entry &e : entries_) {
		const std::string name = std::string(size_t(e.depth) * 2, ' ') + e.name;
		std::snprintf(line, sizeof(line), "%-48s %6.2f ms %6.2f ms\n", name.c_str(), e.average, e.worst);
		out += line;
	}
	return out;
}

Profiler &profiler() {
	static Profiler p;
	return p;
}

} // namespace gc
