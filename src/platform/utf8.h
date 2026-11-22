#pragma once
// Windows' UTF-16 text as UTF-8. Needs <windows.h> first.

#include <string>

namespace gc {

inline std::string narrow(const wchar_t *w) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	}
	return s;
}

} // namespace gc
