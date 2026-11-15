#pragma once
// Logging to stderr (the console in debug builds) and the debugger's output window.

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace gc {

inline void log(const char *fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	fprintf(stderr, "%s\n", buf);
	OutputDebugStringA(buf);
	OutputDebugStringA("\n");
}

} // namespace gc
