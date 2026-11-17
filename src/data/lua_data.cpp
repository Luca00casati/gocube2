#include "data/lua_data.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace gc {

namespace {

// --- Reading ---------------------------------------------------------------------

constexpr size_t MEMORY_LIMIT = size_t(256) << 20; // a world.lua is far smaller
constexpr int STEPS_PER_HOOK = 10000;
constexpr int MAX_HOOKS = 5000; // 50 million VM steps: a data file needs a few per value

struct Reader {
	size_t used = 0;
	int hooks = 0;
	Data result;
};

void *alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	Reader *r = static_cast<Reader *>(ud);
	const size_t old = ptr ? osize : 0;
	if (nsize == 0) {
		r->used -= old;
		std::free(ptr);
		return nullptr;
	}
	if (nsize > old && r->used - old + nsize > MEMORY_LIMIT) {
		return nullptr; // (Lua raises "not enough memory")
	}
	void *p = std::realloc(ptr, nsize);
	if (p) {
		r->used = r->used - old + nsize;
	}
	return p;
}

Reader *reader_of(lua_State *L) {
	void *ud = nullptr;
	lua_getallocf(L, &ud);
	return static_cast<Reader *>(ud);
}

void hook(lua_State *L, lua_Debug *) {
	if (++reader_of(L)->hooks > MAX_HOOKS) {
		luaL_error(L, "it takes too long to read (a data file only describes data)");
	}
}

// Converts the chunk's result (argument 1) into the reader's result.
int convert(lua_State *L) {
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "it must return a table (return { ... })");
	}
	reader_of(L)->result = lua_to_data(L, 1);
	return 0;
}

// --- Writing ---------------------------------------------------------------------

const char *const KEYWORDS[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto",
		"if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};

bool is_name(const std::string &k) {
	if (k.empty() || !(std::isalpha(static_cast<unsigned char>(k[0])) || k[0] == '_')) {
		return false;
	}
	for (char c : k) {
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
			return false;
		}
	}
	return std::find_if(std::begin(KEYWORDS), std::end(KEYWORDS), [&](const char *w) { return k == w; }) ==
			std::end(KEYWORDS);
}

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
				if (c < 32 || c == 127) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\%03d", c); // (three digits: never runs into the next one)
					out += buf;
				} else {
					out += char(c);
				}
		}
	}
	out += '"';
}

void put_key(std::string &out, const std::string &k) {
	if (is_name(k)) {
		out += k;
	} else {
		out += '[';
		put_string(out, k);
		out += ']';
	}
}

void put_number(std::string &out, double d) {
	if (!std::isfinite(d)) {
		throw std::invalid_argument("data can't hold nan or inf");
	}
	char buf[64];
	const auto r = std::to_chars(buf, buf + sizeof(buf), d); // shortest form that reads back the same
	std::string s(buf, r.ptr);
	if (s.find_first_of(".eEn") == std::string::npos) {
		s += ".0"; // (stays a float)
	}
	out += s;
}

// Keys in order, null fields left out.
std::vector<std::pair<std::string, const Data *>> fields(const Data &o) {
	std::vector<std::pair<std::string, const Data *>> f;
	for (const auto &[k, v] : o.items()) {
		if (!v.is_null()) {
			f.push_back({k, &v});
		}
	}
	std::sort(f.begin(), f.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	return f;
}

void put(std::string &out, const Data &v, int depth);

// The whole value on one line, if it's short and holds no tables of tables.
bool put_inline(std::string &out, const Data &v, int depth) {
	if (!v.is_structured()) {
		put(out, v, depth);
		return true;
	}
	std::string line = "{";
	bool first = true;
	if (v.is_array()) {
		for (const Data &e : v) {
			if (e.is_structured() && !e.empty()) {
				if (e.is_object() || std::any_of(e.begin(), e.end(), [](const Data &x) { return x.is_structured(); })) {
					return false;
				}
			}
			line += first ? "" : ", ";
			first = false;
			put(line, e, depth + 1);
		}
	} else {
		for (const auto &[k, e] : fields(v)) {
			if (e->is_structured() && !e->empty()) {
				return false;
			}
			line += first ? "" : ", ";
			first = false;
			put_key(line, k);
			line += " = ";
			put(line, *e, depth + 1);
		}
	}
	line += "}";
	if (line.size() + size_t(depth) * 4 > 100) {
		return false;
	}
	out += line;
	return true;
}

void put(std::string &out, const Data &v, int depth) {
	if (v.is_null()) {
		throw std::invalid_argument("a list can't hold null in a Lua data file");
	}
	if (v.is_boolean()) {
		out += v.get<bool>() ? "true" : "false";
	} else if (v.is_number_integer()) {
		out += std::to_string(v.get<int64_t>());
	} else if (v.is_number()) {
		put_number(out, v.get<double>());
	} else if (v.is_string()) {
		put_string(out, v.str());
	} else if (v.empty()) {
		out += "{}";
	} else {
		std::string line;
		if (put_inline(line, v, depth)) {
			out += line;
			return;
		}
		const std::string pad(size_t(depth) + 1, '\t');
		out += "{\n";
		if (v.is_array()) {
			for (const Data &e : v) {
				out += pad;
				put(out, e, depth + 1);
				out += ",\n";
			}
		} else {
			for (const auto &[k, e] : fields(v)) {
				out += pad;
				put_key(out, k);
				out += " = ";
				put(out, *e, depth + 1);
				out += ",\n";
			}
		}
		out += std::string(size_t(depth), '\t') + "}";
	}
}

} // namespace

Data lua_to_data(lua_State *L, int idx, int depth) {
	idx = lua_absindex(L, idx);
	switch (lua_type(L, idx)) {
		case LUA_TNIL:
			return nullptr;
		case LUA_TBOOLEAN:
			return bool(lua_toboolean(L, idx));
		case LUA_TNUMBER: {
			if (lua_isinteger(L, idx)) {
				return int64_t(lua_tointeger(L, idx));
			}
			const double d = lua_tonumber(L, idx);
			if (!std::isfinite(d)) {
				luaL_error(L, "data can't hold nan or inf");
			}
			return d;
		}
		case LUA_TSTRING: {
			size_t n;
			const char *s = lua_tolstring(L, idx, &n);
			return std::string(s, n);
		}
		case LUA_TTABLE: {
			if (depth > 32) {
				luaL_error(L, "data nested too deeply");
			}
			luaL_checkstack(L, 4, "data nested too deeply");
			// 1..n with nothing else: a list; otherwise string keys: an object.
			const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
			lua_Integer count = 0;
			lua_pushnil(L);
			while (lua_next(L, idx)) {
				count++;
				lua_pop(L, 1);
			}
			if (n > 0 && count == n) {
				Data a = Data::array();
				for (lua_Integer i = 1; i <= n; i++) {
					lua_rawgeti(L, idx, i);
					a.push_back(lua_to_data(L, -1, depth + 1));
					lua_pop(L, 1);
				}
				return a;
			}
			Data o = Data::object();
			lua_pushnil(L);
			while (lua_next(L, idx)) {
				if (lua_type(L, -2) != LUA_TSTRING) {
					luaL_error(L, "keys must be strings (or a list 1..n)");
				}
				o.add_member(lua_tostring(L, -2), lua_to_data(L, -1, depth + 1)); // (keys are unique)
				lua_pop(L, 1);
			}
			return o;
		}
		default:
			luaL_error(L, "data can't hold a %s", luaL_typename(L, idx));
			return nullptr;
	}
}

void push_data(lua_State *L, const Data &j) {
	luaL_checkstack(L, 4, "data nested too deeply");
	if (j.is_null()) {
		lua_pushnil(L);
	} else if (j.is_boolean()) {
		lua_pushboolean(L, j.get<bool>());
	} else if (j.is_number_integer()) {
		lua_pushinteger(L, j.get<int64_t>());
	} else if (j.is_number()) {
		lua_pushnumber(L, j.get<double>());
	} else if (j.is_string()) {
		const std::string &s = j.str();
		lua_pushlstring(L, s.data(), s.size());
	} else if (j.is_array()) {
		lua_createtable(L, int(j.size()), 0);
		for (size_t i = 0; i < j.size(); i++) {
			push_data(L, j[i]);
			lua_rawseti(L, -2, lua_Integer(i + 1));
		}
	} else {
		lua_createtable(L, 0, int(j.size()));
		for (const auto &[k, v] : j.items()) {
			push_data(L, v);
			lua_setfield(L, -2, k.c_str());
		}
	}
}

bool parse_lua_data(std::string_view text, const std::string &name, Data &out, std::string &error) {
	Reader r;
	lua_State *L = lua_newstate(&alloc, &r);
	if (!L) {
		error = "out of memory";
		return false;
	}
	lua_sethook(L, &hook, LUA_MASKCOUNT, STEPS_PER_HOOK);
	const std::string chunk = "=" + name;
	int status = luaL_loadbufferx(L, text.data(), text.size(), chunk.c_str(), "t"); // text only
	if (status == LUA_OK) {
		status = lua_pcall(L, 0, 1, 0);
	}
	if (status == LUA_OK) {
		lua_pushcfunction(L, &convert);
		lua_insert(L, -2);
		status = lua_pcall(L, 1, 0, 0);
	}
	if (status != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		error = msg ? msg : "it can't be read";
	}
	lua_close(L);
	if (status != LUA_OK) {
		return false;
	}
	out = std::move(r.result);
	return true;
}

bool read_lua_data(const std::filesystem::path &path, Data &out, std::string &error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "can't read it";
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return parse_lua_data(ss.str(), path.filename().string(), out, error);
}

std::string write_lua_data(const Data &data) {
	std::string out = "return ";
	put(out, data, 0);
	out += "\n";
	return out;
}

bool save_lua_data(const std::filesystem::path &path, const Data &data) {
	std::string text;
	try {
		text = write_lua_data(data);
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
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec); // (replaces the old file in one step)
	return !ec;
}

} // namespace gc
