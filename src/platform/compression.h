#pragma once
// Compressing bytes with Windows' own Compression API (cabinet.dll: MSZIP,
// the deflate family), for mod packs sent between players.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gc {

// Empty if it fails.
std::vector<uint8_t> compress_bytes(const std::vector<uint8_t> &raw);
// Decompresses `size` bytes of `data` into exactly `raw_size` bytes: false if
// it's damaged or comes out any other size (never more than `raw_size` is made).
bool decompress_bytes(const uint8_t *data, size_t size, size_t raw_size, std::vector<uint8_t> &out);

} // namespace gc
