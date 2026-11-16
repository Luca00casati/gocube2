#pragma once
// A mod folder packed for sending to another player (multiplayer: a host's
// mods a joining player lacks): the files its fingerprint covers (mod_files),
// each as a path and its bytes, compressed with zlib. Unpacking checks every
// path (inside the folder, no "..", no drive or absolute paths, nothing
// hidden) and size before writing anything, so a pack can't write outside
// its folder or fill the disk.
//
//   u32 raw size, then zlib(compress2) of:
//   u32 file count, then per file: u32 path length, path (UTF-8, "/"), u32 size, bytes

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gc {

constexpr size_t MAX_MOD_PACK = 64u << 20; // unpacked, all files together
constexpr size_t MAX_MOD_FILES = 4096;

std::vector<uint8_t> pack_mod(const std::filesystem::path &dir);
// Unpacks into `dir` (made; it must not exist yet). "" or what's wrong
// (then nothing is left behind).
std::string unpack_mod(const std::vector<uint8_t> &pack, const std::filesystem::path &dir);

} // namespace gc
