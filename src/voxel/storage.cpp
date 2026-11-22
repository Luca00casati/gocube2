#include "storage.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace gocube {

namespace {

// Paths come as UTF-8; on Windows they must become wide paths, or
// folders with non-ASCII names (a user called Ëlla) can't be opened.
std::filesystem::path native_path(const std::string &utf8) {
	return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Replaces `to` with `from` in one step. std::rename can't on Windows when
// `to` exists (every save after the first).
bool replace_file(const std::string &from, const std::string &to) {
#ifdef _WIN32
	return MoveFileExW(native_path(from).c_str(), native_path(to).c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// File layout (little-endian):
//   "GCC2"                     magic + format version ("GCC1": no dyes)
//   u8   uniform               1 = every block is `value`
//   u16  value                 (uniform only)
//   u32  run_count             (non-uniform only)
//   run_count x { u16 length, u16 id }   runs over ChunkData::index order
//   u32  dye_count
//   dye_count x { u16 index, u8 r, u8 g, u8 b }   dyed blocks, by index
constexpr char CHUNK_MAGIC[4] = { 'G', 'C', 'C', '2' };
constexpr char CHUNK_MAGIC_V1[4] = { 'G', 'C', 'C', '1' };

void put_dyes(std::vector<uint8_t> &out, const ChunkData &chunk) {
	std::vector<std::pair<uint16_t, Dye>> dyes(chunk.dyes.begin(), chunk.dyes.end());
	std::sort(dyes.begin(), dyes.end()); // (the same chunk: the same bytes)
	const uint32_t n = uint32_t(dyes.size());
	for (int k = 0; k < 4; k++) {
		out.push_back(uint8_t(n >> (8 * k)));
	}
	for (const auto &[index, d] : dyes) {
		out.push_back(uint8_t(index & 0xFF));
		out.push_back(uint8_t(index >> 8));
		out.push_back(uint8_t(d >> 16));
		out.push_back(uint8_t(d >> 8));
		out.push_back(uint8_t(d));
	}
}

} // namespace

std::string chunk_file_path(const std::string &dir, int cx, int cy, int cz) {
	char name[64];
	std::snprintf(name, sizeof(name), "/c.%d.%d.%d.bin", cx, cy, cz);
	return dir + name;
}

std::vector<uint8_t> encode_chunk(const ChunkData &chunk) {
	std::vector<uint8_t> out(CHUNK_MAGIC, CHUNK_MAGIC + 4);
	auto put16 = [&](uint16_t v) { out.push_back(v & 0xFF); out.push_back(v >> 8); };
	auto put32 = [&](uint32_t v) { put16(v & 0xFFFF); put16(v >> 16); };
	out.push_back(chunk.uniform ? 1 : 0);
	if (chunk.uniform) {
		put16(chunk.uniform_value);
		put_dyes(out, chunk);
		return out;
	}
	std::vector<uint16_t> runs; // pairs: length, id
	uint16_t current = chunk.blocks[0];
	uint16_t length = 0;
	for (BlockId b : chunk.blocks) {
		if (b == current && length < 0xFFFF) {
			length++;
		} else {
			runs.push_back(length);
			runs.push_back(current);
			current = b;
			length = 1;
		}
	}
	runs.push_back(length);
	runs.push_back(current);
	put32(static_cast<uint32_t>(runs.size() / 2));
	for (uint16_t v : runs) {
		put16(v);
	}
	put_dyes(out, chunk);
	return out;
}

bool decode_chunk(const uint8_t *data, size_t size, ChunkData &chunk) {
	size_t pos = 0;
	auto get16 = [&](uint16_t &v) {
		if (pos + 2 > size) return false;
		v = uint16_t(data[pos] | (data[pos + 1] << 8));
		pos += 2;
		return true;
	};
	const bool v1 = size >= 4 && std::memcmp(data, CHUNK_MAGIC_V1, 4) == 0;
	if (size < 5 || (!v1 && std::memcmp(data, CHUNK_MAGIC, 4) != 0)) {
		return false;
	}
	pos = 4;
	ChunkData result;
	const uint8_t uniform = data[pos++];
	if (uniform) {
		uint16_t value = 0;
		if (!get16(value)) return false;
		result.uniform = true;
		result.uniform_value = value;
	} else {
		uint16_t lo = 0, hi = 0;
		if (!get16(lo) || !get16(hi)) return false;
		const uint32_t count = lo | (uint32_t(hi) << 16);
		if (count == 0 || count > CHUNK_VOLUME) return false;
		result.uniform = false;
		result.blocks.reserve(CHUNK_VOLUME);
		for (uint32_t i = 0; i < count; i++) {
			uint16_t length = 0, id = 0;
			if (!get16(length) || !get16(id) || result.blocks.size() + length > CHUNK_VOLUME) return false;
			result.blocks.insert(result.blocks.end(), length, id);
		}
		if (result.blocks.size() != CHUNK_VOLUME) return false;
	}
	if (!v1) {
		uint16_t lo = 0, hi = 0;
		if (!get16(lo) || !get16(hi)) return false;
		const uint32_t count = lo | (uint32_t(hi) << 16);
		if (count > CHUNK_VOLUME || size - pos < size_t(count) * 5) return false;
		for (uint32_t i = 0; i < count; i++) {
			const uint16_t index = uint16_t(data[pos] | (data[pos + 1] << 8));
			const Dye d = make_dye(data[pos + 2], data[pos + 3], data[pos + 4]);
			pos += 5;
			if (index >= CHUNK_VOLUME) return false;
			const BlockId b = result.uniform ? result.uniform_value : result.blocks[index];
			if (b != AIR) {
				result.dyes[index] = d;
			}
		}
	}
	if (pos != size) return false;
	chunk = std::move(result);
	return true;
}

bool save_chunk_file(const std::string &path, const ChunkData &chunk) {
	const std::vector<uint8_t> bytes = encode_chunk(chunk);
	const std::string tmp = path + ".tmp";
	{
		std::ofstream f(native_path(tmp), std::ios::binary | std::ios::trunc);
		if (!f) {
			return false;
		}
		f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
		f.close();
		if (!f) {
			std::error_code ec;
			std::filesystem::remove(native_path(tmp), ec);
			return false;
		}
	}
	return replace_file(tmp, path);
}

bool load_chunk_file(const std::string &path, ChunkData &chunk) {
	std::ifstream f(native_path(path), std::ios::binary);
	if (!f) {
		return false;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return decode_chunk(bytes.data(), bytes.size(), chunk);
}

std::vector<std::array<int, 3>> list_chunk_files(const std::string &dir) {
	std::vector<std::array<int, 3>> out;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(native_path(dir), ec)) {
		int x, y, z;
		const std::string name = entry.path().filename().string();
		char tail = 0;
		if (sscanf_s(name.c_str(), "c.%d.%d.%d.bi%c", &x, &y, &z, &tail, 1u) == 4 && tail == 'n' &&
				name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0) {
			out.push_back({ x, y, z });
		}
	}
	return out;
}

} // namespace gocube
