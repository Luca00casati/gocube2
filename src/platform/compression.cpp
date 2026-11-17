#include "platform/compression.h"

#include <windows.h>

#include <compressapi.h>

namespace gc {

std::vector<uint8_t> compress_bytes(const std::vector<uint8_t> &raw) {
	COMPRESSOR_HANDLE c = nullptr;
	if (!CreateCompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &c)) {
		return {};
	}
	std::vector<uint8_t> out;
	SIZE_T needed = 0;
	// Asked with no buffer first: how big the result is.
	if (!Compress(c, raw.data(), raw.size(), nullptr, 0, &needed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
		out.resize(needed);
		if (Compress(c, raw.data(), raw.size(), out.data(), out.size(), &needed)) {
			out.resize(needed);
		} else {
			out.clear();
		}
	}
	CloseCompressor(c);
	return out;
}

bool decompress_bytes(const uint8_t *data, size_t size, size_t raw_size, std::vector<uint8_t> &out) {
	DECOMPRESSOR_HANDLE d = nullptr;
	if (size == 0 || !CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &d)) {
		return false;
	}
	out.assign(raw_size, 0);
	SIZE_T got = 0;
	const bool ok = Decompress(d, data, size, out.data(), out.size(), &got) && got == raw_size;
	CloseDecompressor(d);
	if (!ok) {
		out.clear();
	}
	return ok;
}

} // namespace gc
