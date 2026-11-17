#pragma once
// Sleeping to the millisecond. Windows wakes sleeping threads on its timer
// tick, 15.6 ms by default, so sleep_for(1 ms) and the frame rate limit's
// sleep_until take up to 16 ms; this asks for a 1 ms tick while it lives (per
// process since Windows 10 2004). One in main(), for the game and the tests.
// (ENet used to ask for it as a side effect of starting, only once a world
// was hosted or joined.)

#include <windows.h>
#include <timeapi.h>

namespace gc {

struct FineSleep {
	FineSleep() { timeBeginPeriod(1); }
	~FineSleep() { timeEndPeriod(1); }
	FineSleep(const FineSleep &) = delete;
	FineSleep &operator=(const FineSleep &) = delete;
};

} // namespace gc
