#pragma once
// The precompiled header: the standard library headers most files use,
// parsed once per configuration instead of once per file. nob.c includes it
// in every C++ file it compiles (/FI), so no source includes it: each still
// includes what it uses. Windows' headers aren't here: some files need
// <winsock2.h> before <windows.h>, and their macros would reach every file.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
