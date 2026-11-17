#include "mods/mod_pack.h"

#include "mods/mods.h"

#include "platform/compression.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace gc {

namespace fs = std::filesystem;

namespace {

void put32(std::vector<uint8_t> &out, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		out.push_back(uint8_t(v >> (8 * i)));
	}
}

bool get32(const std::vector<uint8_t> &in, size_t &at, uint32_t &v) {
	if (in.size() - at < 4 || at > in.size()) {
		return false;
	}
	v = 0;
	for (int i = 0; i < 4; i++) {
		v |= uint32_t(in[at + i]) << (8 * i);
	}
	at += 4;
	return true;
}

// A path a pack may hold: relative, "/"-separated parts of plain names.
bool safe_path(const std::string &p) {
	if (p.empty() || p.size() > 255 || p.front() == '/' || p.back() == '/') {
		return false;
	}
	size_t from = 0;
	while (from <= p.size()) {
		size_t to = p.find('/', from);
		to = to == std::string::npos ? p.size() : to;
		const std::string part = p.substr(from, to - from);
		if (part.empty() || part[0] == '.' || part.back() == '.' || part.back() == ' ' ||
				part.find_first_of("<>:\\\"|?*") != std::string::npos) {
			return false;
		}
		for (unsigned char c : part) {
			if (c < 32) {
				return false;
			}
		}
		// Windows' reserved device names (con, nul, com1...), with or without an extension.
		std::string base = part.substr(0, part.find('.'));
		for (char &c : base) {
			c = char(std::tolower(static_cast<unsigned char>(c)));
		}
		static const char *reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6",
				"com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
		for (const char *r : reserved) {
			if (base == r) {
				return false;
			}
		}
		from = to + 1;
	}
	return true;
}

} // namespace

std::vector<uint8_t> pack_mod(const fs::path &dir) {
	std::vector<uint8_t> raw;
	const std::vector<std::string> files = mod_files(dir);
	put32(raw, uint32_t(files.size()));
	for (const std::string &rel : files) {
		std::ifstream f(dir / fs::path(reinterpret_cast<const char8_t *>(rel.c_str())), std::ios::binary);
		std::ostringstream s;
		s << f.rdbuf();
		const std::string bytes = s.str();
		put32(raw, uint32_t(rel.size()));
		raw.insert(raw.end(), rel.begin(), rel.end());
		put32(raw, uint32_t(bytes.size()));
		raw.insert(raw.end(), bytes.begin(), bytes.end());
	}
	const std::vector<uint8_t> packed = compress_bytes(raw);
	if (packed.empty()) {
		return {};
	}
	std::vector<uint8_t> out;
	put32(out, uint32_t(raw.size()));
	out.insert(out.end(), packed.begin(), packed.end());
	return out;
}

std::string unpack_mod(const std::vector<uint8_t> &pack, const fs::path &dir) {
	size_t at = 0;
	uint32_t raw_size = 0;
	if (!get32(pack, at, raw_size) || raw_size > MAX_MOD_PACK) {
		return "it's too big, or not a mod";
	}
	std::vector<uint8_t> raw;
	if (!decompress_bytes(pack.data() + 4, pack.size() - 4, raw_size, raw)) {
		return "it's damaged";
	}
	// Read it all first: nothing is written unless all of it is fine.
	struct File {
		std::string path;
		size_t from, size;
	};
	std::vector<File> files;
	at = 0;
	uint32_t count = 0;
	if (!get32(raw, at, count) || count > MAX_MOD_FILES) {
		return "it has too many files";
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t len = 0, size = 0;
		if (!get32(raw, at, len) || len > raw.size() - at) {
			return "it's damaged";
		}
		std::string path(reinterpret_cast<const char *>(raw.data() + at), len);
		at += len;
		if (!get32(raw, at, size) || size > raw.size() - at) {
			return "it's damaged";
		}
		if (!safe_path(path)) {
			return "it has a file it may not have (" + path + ")";
		}
		for (const File &f : files) {
			if (f.path == path) {
				return "it has a file twice";
			}
		}
		files.push_back({path, at, size});
		at += size;
	}
	if (at != raw.size()) {
		return "it's damaged";
	}
	std::error_code ec;
	if (fs::exists(dir, ec)) {
		return "its folder is already there";
	}
	for (const File &f : files) {
		const fs::path to = dir / fs::path(reinterpret_cast<const char8_t *>(f.path.c_str()));
		fs::create_directories(to.parent_path(), ec);
		std::ofstream out(to, std::ios::binary);
		out.write(reinterpret_cast<const char *>(raw.data() + f.from), std::streamsize(f.size));
		if (!out) {
			fs::remove_all(dir, ec);
			return "it couldn't be written";
		}
	}
	return "";
}

} // namespace gc
