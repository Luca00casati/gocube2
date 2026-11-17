#pragma once
// Messages on the wire: little-endian numbers, strings and byte strings with
// a length in front, and data values (Data) as Lua data tables
// (data/lua_data.h: read with a step and memory limit, and Lua's own nesting
// limit, whatever a peer sends). A Reader never reads past
// its end: what isn't there reads as zero or empty and marks it bad (`ok()`),
// and strings and data have a size limit, so a peer's message can't make us
// read or allocate without bound.

#include "content/content.h"
#include "data/lua_data.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gc::net {

class Writer {
public:
	explicit Writer(uint8_t type) { u8(type); }
	std::vector<uint8_t> bytes;

	Writer &u8(uint8_t v) {
		bytes.push_back(v);
		return *this;
	}
	Writer &u16(uint16_t v) { return raw(&v, 2); }
	Writer &u32(uint32_t v) { return raw(&v, 4); }
	Writer &i32(int32_t v) { return raw(&v, 4); }
	Writer &f32(float v) { return raw(&v, 4); }
	Writer &f64(double v) { return raw(&v, 8); }
	Writer &boolean(bool v) { return u8(v ? 1 : 0); }
	Writer &vec3(const float v[3]) { return f32(v[0]).f32(v[1]).f32(v[2]); }
	Writer &str(const std::string &s) {
		u32(uint32_t(s.size()));
		return raw(s.data(), s.size());
	}
	Writer &blob(const std::vector<uint8_t> &b) {
		u32(uint32_t(b.size()));
		return raw(b.data(), b.size());
	}
	Writer &data(const Data &j) { return str(write_lua_data(j)); } // (no null in a list)

private:
	Writer &raw(const void *p, size_t n) {
		const auto *b = static_cast<const uint8_t *>(p);
		bytes.insert(bytes.end(), b, b + n);
		return *this;
	}
};

class Reader {
public:
	static constexpr size_t MAX_STRING = 4096;

	Reader(const uint8_t *data, size_t size) : p_(data), n_(size) {}
	bool ok() const { return ok_; }
	bool at_end() const { return i_ == n_; }

	uint8_t u8() {
		uint8_t v = 0;
		raw(&v, 1);
		return v;
	}
	uint16_t u16() {
		uint16_t v = 0;
		raw(&v, 2);
		return v;
	}
	uint32_t u32() {
		uint32_t v = 0;
		raw(&v, 4);
		return v;
	}
	int32_t i32() {
		int32_t v = 0;
		raw(&v, 4);
		return v;
	}
	// Floats that aren't finite read as 0 and mark the message bad.
	float f32() {
		float v = 0;
		raw(&v, 4);
		if (!std::isfinite(v)) {
			ok_ = false;
			v = 0;
		}
		return v;
	}
	double f64() {
		double v = 0;
		raw(&v, 8);
		if (!std::isfinite(v)) {
			ok_ = false;
			v = 0;
		}
		return v;
	}
	bool boolean() { return u8() != 0; }
	void vec3(float out[3]) {
		out[0] = f32();
		out[1] = f32();
		out[2] = f32();
	}
	std::string str(size_t max = MAX_STRING) {
		const uint32_t len = u32();
		if (!ok_ || len > max || len > n_ - i_) {
			ok_ = false;
			return {};
		}
		std::string s(reinterpret_cast<const char *>(p_ + i_), len);
		i_ += len;
		return s;
	}
	std::vector<uint8_t> blob(size_t max) {
		const uint32_t len = u32();
		if (!ok_ || len > max || len > n_ - i_) {
			ok_ = false;
			return {};
		}
		std::vector<uint8_t> b(p_ + i_, p_ + i_ + len);
		i_ += len;
		return b;
	}
	// A data value (null and a bad message if it isn't one).
	Data data(size_t max = 1 << 20) {
		const std::string text = str(max);
		Data j;
		std::string error;
		if (!ok_ || !parse_lua_data(text, "message", j, error)) {
			ok_ = false;
			return Data();
		}
		return j;
	}

private:
	void raw(void *out, size_t n) {
		if (!ok_ || n > n_ - i_) {
			ok_ = false;
			return;
		}
		std::memcpy(out, p_ + i_, n);
		i_ += n;
	}
	const uint8_t *p_;
	size_t n_, i_ = 0;
	bool ok_ = true;
};

} // namespace gc::net
