#include "data/json.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace gc {

namespace {

// --- Reading ---------------------------------------------------------------------

int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

void put_utf8(std::string &out, uint32_t c) {
	if (c < 0x80) {
		out += char(c);
	} else if (c < 0x800) {
		out += char(0xC0 | (c >> 6));
		out += char(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		out += char(0xE0 | (c >> 12));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	} else {
		out += char(0xF0 | (c >> 18));
		out += char(0x80 | ((c >> 12) & 0x3F));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	}
}

class Parser {
public:
	Parser(std::string_view text, const std::string &name) : s_(text), name_(name) {
		if (s_.substr(0, 3) == "\xEF\xBB\xBF") {
			i_ = 3; // (a UTF-8 byte order mark)
		}
	}

	bool parse(Data &out, std::string &error) {
		try {
			out = value(0);
			skip();
			if (i_ < s_.size()) {
				fail("unexpected text after the value");
			}
			return true;
		} catch (const std::runtime_error &e) {
			error = e.what();
			return false;
		}
	}

private:
	std::string_view s_;
	const std::string &name_;
	size_t i_ = 0;

	[[noreturn]] void fail(const std::string &why) const {
		int line = 1;
		for (size_t k = 0; k < i_ && k < s_.size(); k++) {
			line += s_[k] == '\n';
		}
		throw std::runtime_error(name_ + ":" + std::to_string(line) + ": " + why);
	}

	char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

	void skip() {
		while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
			i_++;
		}
	}

	void expect(char c) {
		skip();
		if (peek() != c) {
			fail(i_ >= s_.size() ? std::string("the text ends too soon (expected '") + c + "')"
								 : std::string("expected '") + c + "'");
		}
		i_++;
	}

	bool word(std::string_view w) {
		if (s_.substr(i_, w.size()) != w) {
			return false;
		}
		i_ += w.size();
		return true;
	}

	uint32_t hex4() {
		uint32_t v = 0;
		for (int k = 0; k < 4; k++) {
			const int d = hex_digit(peek());
			if (d < 0) {
				fail("\\u needs four hex digits");
			}
			v = v * 16 + uint32_t(d);
			i_++;
		}
		return v;
	}

	std::string string() {
		i_++; // "
		std::string out;
		for (;;) {
			if (i_ >= s_.size()) {
				fail("a string isn't closed");
			}
			const char c = s_[i_++];
			if (c == '"') {
				return out;
			}
			if (static_cast<unsigned char>(c) < 0x20) {
				fail("a control character in a string (write it as \\n, \\t, \\u0000...)");
			}
			if (c != '\\') {
				out += c;
				continue;
			}
			const char e = peek();
			i_++;
			switch (e) {
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u': {
					uint32_t c32 = hex4();
					if (c32 >= 0xD800 && c32 <= 0xDBFF) { // a surrogate pair
						if (!word("\\u")) {
							fail("a lone surrogate in \\u");
						}
						const uint32_t low = hex4();
						if (low < 0xDC00 || low > 0xDFFF) {
							fail("a lone surrogate in \\u");
						}
						c32 = 0x10000 + ((c32 - 0xD800) << 10) + (low - 0xDC00);
					} else if (c32 >= 0xDC00 && c32 <= 0xDFFF) {
						fail("a lone surrogate in \\u");
					}
					put_utf8(out, c32);
					break;
				}
				default:
					fail(std::string("unknown escape \\") + e);
			}
		}
	}

	Data number() {
		const size_t start = i_;
		bool is_float = false;
		auto digits = [&] {
			const size_t from = i_;
			while (peek() >= '0' && peek() <= '9') {
				i_++;
			}
			return i_ > from;
		};
		if (peek() == '-') {
			i_++;
		}
		if (peek() == '0') {
			i_++;
		} else if (!digits()) {
			fail("a malformed number");
		}
		if (peek() == '.') {
			is_float = true;
			i_++;
			if (!digits()) {
				fail("a malformed number");
			}
		}
		if (peek() == 'e' || peek() == 'E') {
			is_float = true;
			i_++;
			if (peek() == '+' || peek() == '-') {
				i_++;
			}
			if (!digits()) {
				fail("a malformed number");
			}
		}
		const char *b = s_.data() + start, *e = s_.data() + i_;
		if (!is_float) {
			int64_t v = 0;
			const auto r = std::from_chars(b, e, v);
			if (r.ec == std::errc() && r.ptr == e) {
				return v;
			}
			// (too large for an integer: a float)
		}
		double d = 0;
		const auto r = std::from_chars(b, e, d);
		if (r.ec != std::errc() || r.ptr != e || !std::isfinite(d)) {
			fail("a number too large for data");
		}
		return d;
	}

	Data value(int depth) {
		skip();
		const char c = peek();
		if (c == '{' || c == '[') {
			if (depth > MAX_DATA_DEPTH) {
				fail("data nested too deeply");
			}
			i_++;
			const char close = c == '{' ? '}' : ']';
			Data out = c == '{' ? Data::object() : Data::array();
			skip();
			if (peek() == close) {
				i_++;
				return out;
			}
			for (;;) {
				if (c == '{') {
					skip();
					if (peek() != '"') {
						fail("expected a key (a string)");
					}
					std::string key = string();
					expect(':');
					Data v = value(depth + 1);
					if (out.contains(key)) {
						out[key] = std::move(v); // (a key given twice: the last wins)
					} else {
						out.add_member(std::move(key), std::move(v));
					}
				} else {
					out.push_back(value(depth + 1));
				}
				skip();
				if (peek() == ',') {
					i_++;
					continue;
				}
				expect(close);
				return out;
			}
		}
		if (c == '"') {
			return string();
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return number();
		}
		if (word("true")) {
			return true;
		}
		if (word("false")) {
			return false;
		}
		if (word("null")) {
			return nullptr;
		}
		if (i_ >= s_.size()) {
			fail("the text ends too soon");
		}
		fail(std::string("unexpected '") + c + "'");
	}
};

// --- Writing ---------------------------------------------------------------------

void put_string(std::string &out, const std::string &s) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20 || c == 0x7F) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += char(c);
				}
		}
	}
	out += '"';
}

void put_number(std::string &out, double d) {
	if (!std::isfinite(d)) {
		throw std::invalid_argument("data can't hold nan or inf");
	}
	char buf[64];
	const auto r = std::to_chars(buf, buf + sizeof(buf), d); // shortest form that reads back the same
	std::string s(buf, r.ptr);
	if (s.find_first_of(".eE") == std::string::npos) {
		s += ".0"; // (stays a float)
	}
	out += s;
}

// An object's fields, sorted by key.
std::vector<const Data::Member *> sorted(const Data &o) {
	std::vector<const Data::Member *> f;
	for (const Data::Member &m : o.items()) {
		f.push_back(&m);
	}
	std::sort(f.begin(), f.end(), [](const auto *a, const auto *b) { return a->first < b->first; });
	return f;
}

void put(std::string &out, const Data &v, int depth, bool compact);

void put_scalar(std::string &out, const Data &v) {
	if (v.is_null()) {
		out += "null";
	} else if (v.is_boolean()) {
		out += v.get<bool>() ? "true" : "false";
	} else if (v.is_number_integer()) {
		out += std::to_string(v.get<int64_t>());
	} else if (v.is_number()) {
		put_number(out, v.get<double>());
	} else {
		put_string(out, v.str());
	}
}

// The whole value on one line, if it's short and holds no structures of structures.
bool put_inline(std::string &out, const Data &v, int depth) {
	std::string line;
	if (v.is_array()) {
		line = "[";
		bool first = true;
		for (const Data &e : v) {
			if (e.is_structured() && !e.empty()) {
				if (e.is_object() || std::any_of(e.begin(), e.end(), [](const Data &x) { return x.is_structured(); })) {
					return false;
				}
			}
			line += first ? "" : ", ";
			first = false;
			put(line, e, depth + 1, false);
		}
		line += "]";
	} else {
		line = "{";
		bool first = true;
		for (const Data::Member *m : sorted(v)) {
			if (m->second.is_structured() && !m->second.empty()) {
				return false;
			}
			line += first ? "" : ", ";
			first = false;
			put_string(line, m->first);
			line += ": ";
			put(line, m->second, depth + 1, false);
		}
		line += "}";
	}
	if (line.size() + size_t(depth) * 4 > 100) {
		return false;
	}
	out += line;
	return true;
}

void put(std::string &out, const Data &v, int depth, bool compact) {
	if (!v.is_structured()) {
		put_scalar(out, v);
		return;
	}
	if (v.empty()) {
		out += v.is_array() ? "[]" : "{}";
		return;
	}
	if (compact) {
		out += v.is_array() ? '[' : '{';
		bool first = true;
		if (v.is_array()) {
			for (const Data &e : v) {
				out += first ? "" : ",";
				first = false;
				put(out, e, depth + 1, true);
			}
		} else {
			for (const Data::Member *m : sorted(v)) {
				out += first ? "" : ",";
				first = false;
				put_string(out, m->first);
				out += ':';
				put(out, m->second, depth + 1, true);
			}
		}
		out += v.is_array() ? ']' : '}';
		return;
	}
	if (put_inline(out, v, depth)) {
		return;
	}
	const std::string pad(size_t(depth) + 1, '\t');
	out += v.is_array() ? "[\n" : "{\n";
	size_t n = 0;
	if (v.is_array()) {
		for (const Data &e : v) {
			out += pad;
			put(out, e, depth + 1, false);
			out += ++n < v.size() ? ",\n" : "\n";
		}
	} else {
		for (const Data::Member *m : sorted(v)) {
			out += pad;
			put_string(out, m->first);
			out += ": ";
			put(out, m->second, depth + 1, false);
			out += ++n < v.size() ? ",\n" : "\n";
		}
	}
	out += std::string(size_t(depth), '\t') + (v.is_array() ? "]" : "}");
}

} // namespace

bool parse_json(std::string_view text, const std::string &name, Data &out, std::string &error) {
	return Parser(text, name).parse(out, error);
}

bool read_json_file(const std::filesystem::path &path, Data &out, std::string &error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "can't read it";
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	const std::u8string file = path.filename().u8string();
	return parse_json(ss.str(), std::string(file.begin(), file.end()), out, error);
}

std::string write_json(const Data &data, bool compact) {
	std::string out;
	put(out, data, 0, compact);
	if (!compact) {
		out += "\n";
	}
	return out;
}

bool save_json_file(const std::filesystem::path &path, const Data &data) {
	std::string text;
	try {
		text = write_json(data);
	} catch (const std::invalid_argument &) {
		return false;
	}
	std::filesystem::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f || !(f << text) || !f.flush()) {
			return false;
		}
	}
	// (Replaces the old file in one step.)
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace gc
