#pragma once
// A steady clock in seconds (from an arbitrary start): for timeouts and rates.

#include <chrono>

namespace gc {

inline double now() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace gc
