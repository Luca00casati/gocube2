#pragma once
// Reading an object's fields with a fallback: what content definitions use.

#include "data/data.h"

#include <string>

namespace gc {

// The field if it's a string, else `fallback`.
inline std::string str(const Data &j, const char *key, const std::string &fallback = "") {
	return j.value<std::string>(key, fallback);
}

// The field if it's a boolean, else `fallback`.
inline bool flag(const Data &j, const char *key, bool fallback = false) {
	return j.value<bool>(key, fallback);
}

} // namespace gc
