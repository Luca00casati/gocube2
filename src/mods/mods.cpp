#include "mods/mods.h"

#include "game/blocks.h"
#include "log.h"

// Lua is compiled as C++ (CMakeLists.txt): its errors are C++ exceptions, so
// C++ objects in the functions below are destroyed properly when a mod's code
// raises one. (Hence lua.h directly, not the extern "C" lua.hpp.)
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <sstream>

namespace gc {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

const char *const EVENTS[] = {"tick", "block_placed", "block_broken"};
const char *const STOPPED_TIME = "it ran too long";
const char *const STOPPED_MEMORY = "it used too much memory";

bool valid_id(const std::string &id) {
	if (id.empty()) {
		return false;
	}
	for (char c : id) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	return true;
}

std::string utf8(const fs::path &p) {
	const std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

bool read_file(const fs::path &p, std::string &out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) {
		return false;
	}
	std::ostringstream s;
	s << f.rdbuf();
	out = s.str();
	return true;
}

// A path inside the mod folder to an existing file with this extension, or what's wrong.
std::string file_problem(const Json &rel, const fs::path &root, const char *extension) {
	if (!rel.is_string() || rel.get<std::string>().empty()) {
		return "path must be a string";
	}
	const std::string r = rel.get<std::string>();
	const fs::path p(reinterpret_cast<const char8_t *>(r.c_str()));
	bool outside = p.is_absolute() || r[0] == '/' || r.find('\\') != std::string::npos || r.find(':') != std::string::npos;
	for (const auto &part : p) {
		outside = outside || part == "..";
	}
	if (outside) {
		return "'" + r + "' must be a path inside the mod folder";
	}
	if (r.size() < strlen(extension) || r.compare(r.size() - strlen(extension), std::string::npos, extension) != 0) {
		return "'" + r + "' must be a " + extension + " file";
	}
	std::error_code ec;
	if (!fs::is_regular_file(root / p, ec)) {
		return "'" + r + "' doesn't exist";
	}
	return "";
}

// What is wrong with a mod.json for a mod in `root`, or "" (checked before
// anything of the mod loads, so a broken mod is skipped as a whole).
std::string manifest_problem(const Json &m, const fs::path &root) {
	if (!m.is_object()) {
		return "not a JSON object";
	}
	if (!m.contains("id") || !m["id"].is_string() || !valid_id(m["id"].get<std::string>())) {
		return "\"id\" must be a string of a-z, 0-9 and _";
	}
	for (const char *key : {"name", "version"}) {
		if (m.contains(key) && !m[key].is_string()) {
			return std::string("\"") + key + "\" must be a string";
		}
	}
	if (m.contains("api_version") && !m["api_version"].is_number()) {
		return "\"api_version\" must be a number";
	}
	const Json deps = m.value("dependencies", Json::array());
	if (!deps.is_array()) {
		return "\"dependencies\" must be a list";
	}
	for (const Json &d : deps) {
		if (!d.is_string() || !valid_id(d.get<std::string>())) {
			return "dependency " + d.dump() + " isn't a mod id";
		}
	}
	const Json content = m.value("content", Json::array());
	if (!content.is_array()) {
		return "\"content\" must be a list";
	}
	for (const Json &rel : content) {
		if (std::string why = file_problem(rel, root, ".json"); !why.empty()) {
			return "content " + why;
		}
	}
	if (m.contains("entry")) {
		if (std::string why = file_problem(m["entry"], root, ".lua"); !why.empty()) {
			return "entry " + why;
		}
	}
	return "";
}

// What is wrong with a parsed content file, or "".
std::string content_problem(const Json &data) {
	if (!data.is_object()) {
		return "isn't a JSON object";
	}
	for (const auto &[section, entries] : data.items()) {
		if (!entries.is_object()) {
			return "has \"" + section + "\" not mapping ids to objects";
		}
		for (const auto &[id, def] : entries.items()) {
			if (!def.is_object()) {
				return "has \"" + section + "\" not mapping ids to objects";
			}
		}
	}
	return "";
}

std::vector<fs::path> visible_subdirs(const fs::path &dir) {
	std::vector<fs::path> out;
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(dir, ec)) {
		const std::string name = utf8(e.path().filename());
		if (e.is_directory(ec) && !name.empty() && name[0] != '.') {
			out.push_back(e.path());
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

} // namespace

// --- The Lua VM and sandbox --------------------------------------------------------

struct Mods::Lua {
	struct ModState {
		ModInfo info;
		int env_ref = LUA_NOREF;     // its environment table
		int modules_ref = LUA_NOREF; // require()'s cache
		int errors = 0;              // event handler failures
		bool disabled = false;
	};
	struct Handler {
		int mod;
		std::string event;
		int ref;
	};
	struct Rule {
		int mod;
		int ref;
	};

	Content &content;
	ModLimits limits;
	lua_State *L = nullptr;
	size_t used = 0;
	std::vector<ModState> mods;
	std::vector<Handler> handlers;
	std::map<std::string, Rule> game_rules; // game.set_rule
	bool registry_open = true;
	std::mt19937_64 random{std::random_device{}()};
	World *world = nullptr;
	const BlockRules *rules = nullptr;

	// The mod whose code is running (-1: none) and its limits; why it was
	// stopped ("" while it may go on).
	int current = -1;
	Clock::time_point deadline;
	size_t memory_cap = 0;
	const char *stopped = nullptr;

	Lua(Content &c, ModLimits l) : content(c), limits(l) {
		L = lua_newstate(&Lua::alloc, this);
		*static_cast<Lua **>(lua_getextraspace(L)) = this;
		lua_sethook(L, &Lua::hook, LUA_MASKCOUNT, 1000);
		// The libraries mods get copies of (never io, os, debug, package, coroutine).
		luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
		luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 0);
		luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 0);
		luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 0);
		luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 0);
		lua_pop(L, 5);
		// Strings' metatable leads to the shared string library: hide it.
		lua_pushliteral(L, "");
		lua_getmetatable(L, -1);
		lua_pushliteral(L, "string");
		lua_setfield(L, -2, "__metatable");
		lua_pop(L, 2);
	}
	~Lua() { lua_close(L); }

	static Lua *of(lua_State *L) { return *static_cast<Lua **>(lua_getextraspace(L)); }

	// Every allocation of the VM. Mod code may not grow past its cap (or the
	// VM's total); the game's own use of the VM is never refused.
	static void *alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
		Lua *h = static_cast<Lua *>(ud);
		const size_t old = ptr ? osize : 0;
		if (nsize == 0) {
			if (ptr) {
				std::free(ptr);
				h->used -= old;
			}
			return nullptr;
		}
		if (nsize > old && h->current >= 0) {
			const size_t cap = std::min(h->memory_cap, h->limits.total_memory);
			if (h->used + (nsize - old) > cap) {
				if (!h->stopped) {
					h->stopped = STOPPED_MEMORY;
				}
				return nullptr;
			}
		}
		void *p = std::realloc(ptr, nsize);
		if (p) {
			h->used = h->used - old + nsize;
		}
		return p;
	}

	// Every 1000 instructions of Lua code: past the deadline (or stopped
	// already), the mod's code stops.
	static void hook(lua_State *L, lua_Debug *) {
		Lua *h = of(L);
		if (h->current < 0) {
			return;
		}
		if (!h->stopped && Clock::now() > h->deadline) {
			h->stopped = STOPPED_TIME;
		}
		if (h->stopped) {
			luaL_error(L, "stopped: %s", h->stopped);
		}
	}

	static int traceback(lua_State *L) {
		const char *msg = lua_tostring(L, 1);
		luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
		return 1;
	}

	// Calls the function under `nargs` arguments as mod `mod`'s code, with a
	// time budget; `nresults` results are left on the stack if it succeeds.
	// "" or what went wrong.
	std::string call(int mod, int nargs, double budget_ms, int nresults = 0) {
		current = mod;
		stopped = nullptr;
		deadline = Clock::now() + std::chrono::microseconds(int64_t(budget_ms * 1000));
		memory_cap = used + limits.memory;
		const int base = lua_gettop(L) - nargs;
		lua_pushcfunction(L, &Lua::traceback);
		lua_insert(L, base);
		const int status = lua_pcall(L, nargs, nresults, base);
		std::string error;
		if (status != LUA_OK) {
			if (stopped == STOPPED_TIME) {
				error = std::string(STOPPED_TIME) + " (more than " + std::to_string(int(budget_ms)) + " ms)";
			} else if (stopped == STOPPED_MEMORY || status == LUA_ERRMEM) {
				error = std::string(STOPPED_MEMORY) + " (more than " + std::to_string(limits.memory >> 20) + " MB)";
			} else {
				const char *msg = lua_tostring(L, -1);
				error = msg ? msg : "an error";
			}
			lua_pop(L, 1);
		}
		lua_remove(L, base);
		current = -1;
		stopped = nullptr;
		if (status != LUA_OK) {
			lua_gc(L, LUA_GCCOLLECT); // whatever it left behind
		}
		return error;
	}

	// --- Basics for mod code ---

	// pcall/xpcall that can't catch the sandbox stopping the code.
	static int safe_pcall(lua_State *L) {
		luaL_checkany(L, 1);
		const int status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
		if (of(L)->stopped) {
			return lua_error(L);
		}
		lua_pushboolean(L, status == LUA_OK);
		lua_insert(L, 1);
		return lua_gettop(L);
	}

	static int safe_xpcall(lua_State *L) {
		const int n = lua_gettop(L);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_pushvalue(L, 1);
		lua_pushvalue(L, 2);
		lua_replace(L, 1); // handler first, then the function and its arguments
		lua_replace(L, 2);
		const int status = lua_pcall(L, n - 2, LUA_MULTRET, 1);
		if (of(L)->stopped) {
			return lua_error(L);
		}
		lua_pushboolean(L, status == LUA_OK);
		lua_replace(L, 1);
		return lua_gettop(L);
	}

	// setmetatable without __gc: a finalizer would run whenever the collector
	// does, outside any time limit.
	static int safe_setmetatable(lua_State *L) {
		if (lua_type(L, 2) == LUA_TTABLE && lua_getfield(L, 2, "__gc") != LUA_TNIL) {
			return luaL_error(L, "__gc metamethods aren't allowed in mods");
		}
		lua_settop(L, 2);
		lua_getglobal(L, "setmetatable");
		lua_insert(L, 1);
		lua_call(L, 2, 1);
		return 1;
	}

	static int mod_print(lua_State *L) {
		Lua *h = of(L);
		std::string line;
		for (int i = 1; i <= lua_gettop(L); i++) {
			line += (i > 1 ? "\t" : "") + std::string(luaL_tolstring(L, i, nullptr));
			lua_pop(L, 1);
		}
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		log("[%s] %s", h->mods[mod].info.id.c_str(), line.c_str());
		return 0;
	}

	// Loads a .lua file of mod `mod` as text, with the mod's environment; pushes the chunk.
	static void load_chunk(lua_State *L, int mod, const fs::path &path, const std::string &name) {
		Lua *h = of(L);
		std::string code;
		if (!read_file(path, code)) {
			luaL_error(L, "can't read %s", name.c_str());
		}
		if (!code.empty() && code[0] == LUA_SIGNATURE[0]) {
			luaL_error(L, "%s is compiled Lua (bytecode), which mods can't use", name.c_str());
		}
		const std::string chunk = "@" + h->mods[mod].info.id + "/" + name;
		if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) {
			lua_error(L);
		}
		lua_rawgeti(L, LUA_REGISTRYINDEX, h->mods[mod].env_ref);
		lua_setupvalue(L, -2, 1); // its _ENV
	}

	// require("name") / require("folder.name"): name.lua inside the mod, run once.
	static int mod_require(lua_State *L) {
		Lua *h = of(L);
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		const std::string name = luaL_checkstring(L, 1);
		bool ok = !name.empty() && name.front() != '.' && name.back() != '.' && name.find("..") == std::string::npos;
		std::string rel;
		for (char c : name) {
			ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.');
			rel += c == '.' ? '/' : c;
		}
		if (!ok) {
			return luaL_error(L, "require: '%s' isn't a module name (letters, digits, _ and . between folders)", name.c_str());
		}
		rel += ".lua";
		lua_rawgeti(L, LUA_REGISTRYINDEX, h->mods[mod].modules_ref);
		if (lua_getfield(L, -1, name.c_str()) != LUA_TNIL) {
			return 1;
		}
		lua_pop(L, 1);
		load_chunk(L, mod, h->mods[mod].info.dir / fs::path(reinterpret_cast<const char8_t *>(rel.c_str())), rel);
		lua_pushstring(L, name.c_str());
		lua_call(L, 1, 1);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_pushboolean(L, 1);
		}
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, name.c_str());
		return 1;
	}

	// --- Converting between Lua values and JSON ---

	static Json to_json(lua_State *L, int idx, int depth) {
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
					luaL_error(L, "definitions can't hold nan or inf");
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
					luaL_error(L, "definition nested too deeply");
				}
				// 1..n with nothing else: a list; otherwise string keys: an object.
				const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
				lua_Integer count = 0;
				lua_pushnil(L);
				while (lua_next(L, idx)) {
					count++;
					lua_pop(L, 1);
				}
				if (n > 0 && count == n) {
					Json a = Json::array();
					for (lua_Integer i = 1; i <= n; i++) {
						lua_rawgeti(L, idx, i);
						a.push_back(to_json(L, -1, depth + 1));
						lua_pop(L, 1);
					}
					return a;
				}
				Json o = Json::object();
				lua_pushnil(L);
				while (lua_next(L, idx)) {
					if (lua_type(L, -2) != LUA_TSTRING) {
						luaL_error(L, "definition keys must be strings (or a list 1..n)");
					}
					o[lua_tostring(L, -2)] = to_json(L, -1, depth + 1);
					lua_pop(L, 1);
				}
				return o;
			}
			default:
				luaL_error(L, "definitions can't hold a %s", luaL_typename(L, idx));
				return nullptr;
		}
	}

	static void push_json(lua_State *L, const Json &j) {
		luaL_checkstack(L, 4, "definition nested too deeply");
		if (j.is_null()) {
			lua_pushnil(L);
		} else if (j.is_boolean()) {
			lua_pushboolean(L, j.get<bool>());
		} else if (j.is_number_integer() || j.is_number_unsigned()) {
			lua_pushinteger(L, j.get<int64_t>());
		} else if (j.is_number()) {
			lua_pushnumber(L, j.get<double>());
		} else if (j.is_string()) {
			const std::string &s = j.get_ref<const std::string &>();
			lua_pushlstring(L, s.data(), s.size());
		} else if (j.is_array()) {
			lua_createtable(L, int(j.size()), 0);
			for (size_t i = 0; i < j.size(); i++) {
				push_json(L, j[i]);
				lua_rawseti(L, -2, lua_Integer(i + 1));
			}
		} else {
			lua_createtable(L, 0, int(j.size()));
			for (const auto &[k, v] : j.items()) {
				push_json(L, v);
				lua_setfield(L, -2, k.c_str());
			}
		}
	}

	// --- registry ---

	// registry.register_<section>(id, def); upvalues: mod index, section name.
	static int registry_register(lua_State *L) {
		Lua *h = of(L);
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		const std::string section = lua_tostring(L, lua_upvalueindex(2));
		if (!h->registry_open) {
			return luaL_error(L, "content can only be registered while mods load");
		}
		const std::string id = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		Json def = to_json(L, 2, 0);
		if (!def.is_object()) {
			return luaL_error(L, "a definition must be a table with named fields");
		}
		const std::string &mod_id = h->mods[mod].info.id;
		const std::string full = id.find(':') != std::string::npos ? id : mod_id + ":" + id;
		h->content.register_entry(*h->content.table(section), mod_id, id, def);
		lua_pushstring(L, full.c_str());
		return 1;
	}

	// registry.get_block(id) / get_item(id): a copy of the definition, or nil.
	static int registry_get(lua_State *L) {
		Lua *h = of(L);
		const std::string section = lua_tostring(L, lua_upvalueindex(1));
		const std::string id = luaL_checkstring(L, 1);
		const auto *t = h->content.table(section);
		auto it = t->find(id);
		if (it == t->end()) {
			lua_pushnil(L);
		} else {
			push_json(L, it->second);
		}
		return 1;
	}

	// --- events and world ---

	static int events_on(lua_State *L) {
		Lua *h = of(L);
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		const std::string event = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		if (std::find_if(std::begin(EVENTS), std::end(EVENTS), [&](const char *e) { return event == e; }) == std::end(EVENTS)) {
			return luaL_error(L, "no event '%s' (there are: tick, block_placed, block_broken)", event.c_str());
		}
		lua_pushvalue(L, 2);
		h->handlers.push_back({mod, event, luaL_ref(L, LUA_REGISTRYINDEX)});
		return 0;
	}

	// game.set_rule(name, fn): while loading.
	static int game_set_rule(lua_State *L) {
		Lua *h = of(L);
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		const std::string name = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		if (!h->registry_open) {
			return luaL_error(L, "rules can only be set while mods load");
		}
		lua_pushvalue(L, 2);
		auto it = h->game_rules.find(name);
		if (it != h->game_rules.end()) {
			luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref); // (a wrapper keeps its own reference)
		}
		h->game_rules[name] = {mod, luaL_ref(L, LUA_REGISTRYINDEX)};
		return 0;
	}

	// game.rule(name): the function set for it so far, or nil.
	static int game_rule(lua_State *L) {
		Lua *h = of(L);
		auto it = h->game_rules.find(luaL_checkstring(L, 1));
		if (it == h->game_rules.end()) {
			lua_pushnil(L);
		} else {
			lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
		}
		return 1;
	}

	static int game_random(lua_State *L) {
		Lua *h = of(L);
		if (h->registry_open) {
			return luaL_error(L, "random() only works while playing (what mods register must depend only on their files)");
		}
		lua_pushnumber(L, std::uniform_real_distribution<double>(0.0, 1.0)(h->random));
		return 1;
	}

	static World *need_world(lua_State *L) {
		Lua *h = of(L);
		if (!h->world || !h->rules) {
			luaL_error(L, "there is no world yet (use world functions in event handlers)");
		}
		return h->world;
	}

	static int world_get_block(lua_State *L) {
		World *w = need_world(L);
		const int id = w->block_id_at(int(luaL_checkinteger(L, 1)), int(luaL_checkinteger(L, 2)), int(luaL_checkinteger(L, 3)));
		if (id < 0) {
			lua_pushnil(L); // not loaded
		} else {
			lua_pushstring(L, of(L)->rules->id_of(id).c_str());
		}
		return 1;
	}

	static int world_set_block(lua_State *L) {
		World *w = need_world(L);
		const BlockPos p{int(luaL_checkinteger(L, 1)), int(luaL_checkinteger(L, 2)), int(luaL_checkinteger(L, 3))};
		const char *id = luaL_checkstring(L, 4);
		const int index = of(L)->content.block_index(id);
		if (index < 0) {
			return luaL_error(L, "no block '%s'", id);
		}
		lua_pushboolean(L, w->set_block(p, gocube::BlockId(index)));
		return 1;
	}

	// --- A mod's environment ---

	// Pushes a copy of the library `lib` (from the loaded-libraries table)
	// without the fields in `leave_out`: a mod changing its copy changes no
	// one else's.
	void push_library_copy(const char *lib, std::initializer_list<const char *> leave_out) {
		lua_newtable(L);
		luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
		lua_getfield(L, -1, lib);
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			const char *k = lua_tostring(L, -2);
			bool keep = k != nullptr;
			for (const char *x : leave_out) {
				keep = keep && strcmp(k, x) != 0;
			}
			if (keep) {
				lua_pushvalue(L, -2);
				lua_insert(L, -2);
				lua_rawset(L, -6);
			} else {
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 2);
	}

	void push_closure(lua_CFunction f, int mod) {
		lua_pushinteger(L, mod);
		lua_pushcclosure(L, f, 1);
	}

	// Makes mod `mod`'s environment and require() cache.
	void make_env(int mod) {
		lua_newtable(L); // env
		lua_getglobal(L, LUA_GNAME);
		for (const char *name : {"assert", "error", "ipairs", "next", "pairs", "select", "tonumber", "tostring", "type",
					 "rawequal", "rawget", "rawset", "rawlen", "getmetatable", "_VERSION"}) {
			lua_getfield(L, -1, name);
			lua_setfield(L, -3, name);
		}
		lua_pop(L, 1);
		lua_pushcfunction(L, &Lua::safe_pcall);
		lua_setfield(L, -2, "pcall");
		lua_pushcfunction(L, &Lua::safe_xpcall);
		lua_setfield(L, -2, "xpcall");
		lua_pushcfunction(L, &Lua::safe_setmetatable);
		lua_setfield(L, -2, "setmetatable");
		push_closure(&Lua::mod_print, mod);
		lua_setfield(L, -2, "print");
		push_closure(&Lua::mod_require, mod);
		lua_setfield(L, -2, "require");
		push_library_copy(LUA_STRLIBNAME, {"dump"}); // (bytecode)
		lua_setfield(L, -2, "string");
		push_library_copy(LUA_TABLIBNAME, {});
		lua_setfield(L, -2, "table");
		push_library_copy(LUA_MATHLIBNAME, {"random", "randomseed"}); // (results must depend only on the files)
		lua_setfield(L, -2, "math");
		push_library_copy(LUA_UTF8LIBNAME, {});
		lua_setfield(L, -2, "utf8");

		lua_newtable(L); // registry
		for (const char *section : {"block", "item", "recipe", "ore", "tree", "biome", "sound_set"}) {
			lua_pushinteger(L, mod);
			lua_pushstring(L, (std::string(section) + "s").c_str());
			lua_pushcclosure(L, &Lua::registry_register, 2);
			lua_setfield(L, -2, (std::string("register_") + section).c_str());
		}
		for (const char *section : {"block", "item"}) {
			lua_pushstring(L, (std::string(section) + "s").c_str());
			lua_pushcclosure(L, &Lua::registry_get, 1);
			lua_setfield(L, -2, (std::string("get_") + section).c_str());
		}
		lua_setfield(L, -2, "registry");

		lua_newtable(L); // events
		push_closure(&Lua::events_on, mod);
		lua_setfield(L, -2, "on");
		lua_setfield(L, -2, "events");

		lua_newtable(L); // world
		lua_pushcfunction(L, &Lua::world_get_block);
		lua_setfield(L, -2, "get_block");
		lua_pushcfunction(L, &Lua::world_set_block);
		lua_setfield(L, -2, "set_block");
		lua_setfield(L, -2, "world");

		lua_newtable(L); // game
		push_closure(&Lua::game_set_rule, mod);
		lua_setfield(L, -2, "set_rule");
		lua_pushcfunction(L, &Lua::game_rule);
		lua_setfield(L, -2, "rule");
		lua_pushcfunction(L, &Lua::game_random);
		lua_setfield(L, -2, "random");
		lua_setfield(L, -2, "game");

		lua_newtable(L); // mod
		lua_pushstring(L, mods[mod].info.id.c_str());
		lua_setfield(L, -2, "id");
		lua_pushstring(L, mods[mod].info.version.c_str());
		lua_setfield(L, -2, "version");
		lua_setfield(L, -2, "mod");

		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "_G");
		mods[mod].env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_newtable(L);
		mods[mod].modules_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	// The body of loading a mod's entry script (as mod code): upvalues mod index, entry path.
	static int run_entry(lua_State *L) {
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		const std::string entry = lua_tostring(L, lua_upvalueindex(2));
		Lua *h = of(L);
		load_chunk(L, mod, h->mods[mod].info.dir / fs::path(reinterpret_cast<const char8_t *>(entry.c_str())), entry);
		lua_call(L, 0, 0);
		return 0;
	}

	// Runs mod `mod`'s entry script. "" or what went wrong.
	std::string run_entry_script(int mod, const std::string &entry) {
		make_env(mod);
		lua_pushinteger(L, mod);
		lua_pushstring(L, entry.c_str());
		lua_pushcclosure(L, &Lua::run_entry, 2);
		return call(mod, 0, limits.load_ms);
	}

	// Forgets everything of a mod that failed or was turned off.
	void drop(int mod) {
		for (auto it = game_rules.begin(); it != game_rules.end();) {
			if (it->second.mod == mod) {
				luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
				it = game_rules.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = handlers.begin(); it != handlers.end();) {
			if (it->mod == mod) {
				luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
				it = handlers.erase(it);
			} else {
				++it;
			}
		}
		luaL_unref(L, LUA_REGISTRYINDEX, mods[mod].env_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, mods[mod].modules_ref);
		mods[mod].env_ref = mods[mod].modules_ref = LUA_NOREF;
		mods[mod].disabled = true;
		lua_gc(L, LUA_GCCOLLECT);
	}

	// Calls every handler of `event` with the arguments `push` pushes.
	void fire(const char *event, int nargs, const std::function<void()> &push, std::map<std::string, std::string> &problems) {
		const std::vector<Handler> list = handlers; // (a handler may turn its mod off)
		for (const Handler &hd : list) {
			if (hd.event != event || mods[hd.mod].disabled) {
				continue;
			}
			lua_rawgeti(L, LUA_REGISTRYINDEX, hd.ref);
			push();
			const std::string error = call(hd.mod, nargs, limits.callback_ms);
			if (error.empty()) {
				continue;
			}
			ModState &m = mods[hd.mod];
			log("Mod '%s': its %s handler failed: %s", m.info.id.c_str(), event, error.c_str());
			if (++m.errors >= ERRORS_BEFORE_DISABLED) {
				problems[m.info.id] = "turned off: its event handlers kept failing (last: " + error + ")";
				log("Mod '%s' is turned off until the game restarts", m.info.id.c_str());
				drop(hd.mod);
			}
		}
	}
};

// --- Mods ----------------------------------------------------------------------------

Mods::Mods(Content &content, ModLimits limits) : lua_(std::make_unique<Lua>(content, limits)) {}

Mods::~Mods() = default;

size_t Mods::memory_in_use() const {
	return lua_->used;
}

void Mods::load_all(const std::vector<fs::path> &dirs) {
	struct Found {
		Json manifest;
		fs::path dir;
	};
	std::map<std::string, Found> found;
	auto fail = [&](const std::string &id, const std::string &why) {
		problems_[id] = why;
		log("Mod '%s': %s", id.c_str(), why.c_str());
	};
	for (const fs::path &root : dirs) {
		for (const fs::path &dir : visible_subdirs(root)) {
			if (!fs::exists(dir / "mod.json")) {
				continue;
			}
			std::ifstream f(dir / "mod.json", std::ios::binary);
			const Json m = Json::parse(f, nullptr, false);
			if (std::string why = m.is_discarded() ? "it isn't valid JSON" : manifest_problem(m, dir); !why.empty()) {
				fail(utf8(dir.filename()), "invalid mod.json (" + why + "); skipping it");
				continue;
			}
			const std::string id = m["id"].get<std::string>();
			if (m.value("api_version", 1.0) > API_VERSION) {
				fail(id, "needs a newer game (mod API " + std::to_string(int(m["api_version"].get<double>())) +
								 ", this game has " + std::to_string(API_VERSION) + "); skipping it");
				continue;
			}
			if (found.count(id)) {
				log("Duplicate mod id '%s' in %s; using the one in %s", id.c_str(), utf8(dir).c_str(),
						utf8(found[id].dir).c_str());
				continue;
			}
			found[id] = {m, dir};
		}
	}

	// Dependency order; core always first, then by id.
	std::vector<std::string> order;
	std::map<std::string, int> state; // 1 visiting, 2 done, -1 failed
	std::function<bool(const std::string &)> visit = [&](const std::string &id) -> bool {
		const int s = state[id];
		if (s == 2) {
			return true;
		}
		if (s == 1) {
			fail(id, "is part of a dependency cycle");
			return false;
		}
		if (s == -1) {
			return false;
		}
		if (!found.count(id)) {
			fail(id, "is missing");
			state[id] = -1;
			return false;
		}
		state[id] = 1;
		std::vector<std::string> deps;
		if (id != "core") {
			deps.push_back("core");
		}
		for (const Json &d : found[id].manifest.value("dependencies", Json::array())) {
			deps.push_back(d.get<std::string>());
		}
		for (const std::string &dep : deps) {
			if (!visit(dep)) {
				fail(id, "skipped: dependency '" + dep + "' unavailable");
				state[id] = -1;
				return false;
			}
		}
		state[id] = 2;
		order.push_back(id);
		return true;
	};
	visit("core");
	for (const auto &[id, f] : found) {
		visit(id);
	}

	std::set<std::string> failed;
	Content &content = lua_->content;
	for (const std::string &id : order) {
		const Found &f = found[id];
		bool skip = false;
		for (const Json &d : f.manifest.value("dependencies", Json::array())) {
			if (failed.count(d.get<std::string>())) {
				fail(id, "skipped: dependency '" + d.get<std::string>() + "' failed to load");
				skip = true;
				break;
			}
		}
		if (skip || (id != "core" && failed.count("core"))) {
			failed.insert(id);
			continue;
		}
		// Every content file is read and checked before anything of the mod is registered.
		std::vector<Json> files;
		std::string why;
		for (const Json &rel : f.manifest.value("content", Json::array())) {
			const std::string r = rel.get<std::string>();
			std::ifstream in(f.dir / fs::path(reinterpret_cast<const char8_t *>(r.c_str())), std::ios::binary);
			Json data = Json::parse(in, nullptr, false);
			why = data.is_discarded() ? "isn't valid JSON" : content_problem(data);
			if (!why.empty()) {
				why = "content file " + r + " " + why;
				break;
			}
			files.push_back(std::move(data));
		}
		if (!why.empty()) {
			fail(id, why + "; skipping it");
			failed.insert(id);
			continue;
		}

		// What's registered so far, to undo this mod if its entry fails.
		const Content before = content;
		ModInfo info;
		info.id = id;
		info.name = f.manifest.value("name", id);
		info.version = f.manifest.value("version", "");
		info.dir = f.dir;
		info.fingerprint = mod_fingerprint(f.dir);
		content.mod_dirs[id] = f.dir;
		for (const Json &data : files) {
			content.register_content(id, data);
		}
		const int index = int(lua_->mods.size());
		lua_->mods.push_back({info});
		if (f.manifest.contains("entry")) {
			const std::string error = lua_->run_entry_script(index, f.manifest["entry"].get<std::string>());
			if (!error.empty()) {
				content = before;
				lua_->drop(index);
				fail(id, "its code failed, so it was undone: " + error);
				failed.insert(id);
				continue;
			}
		}
		loaded_.push_back(info);
		log("Mod %s %s loaded (%s)", id.c_str(), info.version.c_str(), info.fingerprint.c_str());
	}
	lua_->registry_open = false;
}

std::optional<Json> Mods::rule(const std::string &name, const Json &args) {
	Lua &h = *lua_;
	auto it = h.game_rules.find(name);
	if (it == h.game_rules.end() || h.mods[it->second.mod].disabled) {
		return std::nullopt;
	}
	const int mod = it->second.mod;
	lua_State *L = h.L;
	const int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
	for (const Json &a : args) {
		Lua::push_json(L, a);
	}
	const std::string error = h.call(mod, int(args.size()), h.limits.callback_ms, 1);
	if (!error.empty()) {
		log("Mod '%s': its %s rule failed: %s", h.mods[mod].info.id.c_str(), name.c_str(), error.c_str());
		lua_settop(L, top);
		return std::nullopt;
	}
	std::optional<Json> out;
	// (Converting can raise a Lua error for a value that can't be data: protected.)
	lua_pushcfunction(L, [](lua_State *L) -> int {
		auto *result = static_cast<std::optional<Json> *>(lua_touserdata(L, 2));
		*result = Lua::to_json(L, 1, 0);
		return 0;
	});
	lua_insert(L, -2);
	lua_pushlightuserdata(L, &out);
	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		log("Mod '%s': its %s rule returned something that isn't data", h.mods[mod].info.id.c_str(), name.c_str());
		out.reset();
	}
	lua_settop(L, top);
	return out;
}

void Mods::attach(World *world, const BlockRules *rules) {
	lua_->world = world;
	lua_->rules = rules;
}

void Mods::tick(double dt) {
	lua_State *L = lua_->L;
	lua_->fire("tick", 1, [&] { lua_pushnumber(L, dt); }, problems_);
}

void Mods::block_placed(const BlockPos &p, const std::string &id) {
	lua_State *L = lua_->L;
	lua_->fire("block_placed", 4, [&] {
		lua_pushinteger(L, p.x);
		lua_pushinteger(L, p.y);
		lua_pushinteger(L, p.z);
		lua_pushstring(L, id.c_str());
	}, problems_);
}

void Mods::block_broken(const BlockPos &p, const std::string &id) {
	lua_State *L = lua_->L;
	lua_->fire("block_broken", 4, [&] {
		lua_pushinteger(L, p.x);
		lua_pushinteger(L, p.y);
		lua_pushinteger(L, p.z);
		lua_pushstring(L, id.c_str());
	}, problems_);
}

// --- Fingerprints ----------------------------------------------------------------------

namespace {

// Relative paths ('/' between folders) of the files under root, without
// dot-names, sorted: as the Godot version's ModLoader._files_under.
void files_under(const fs::path &root, const std::string &rel, std::vector<std::string> &out) {
	const fs::path dir = rel.empty() ? root : root / fs::path(reinterpret_cast<const char8_t *>(rel.c_str()));
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(dir, ec)) {
		const std::string name = utf8(e.path().filename());
		if (name.empty() || name[0] == '.') {
			continue;
		}
		const std::string path = rel.empty() ? name : rel + "/" + name;
		if (e.is_directory(ec)) {
			files_under(root, path, out);
		} else {
			out.push_back(path);
		}
	}
}

} // namespace

std::string mod_fingerprint(const fs::path &root) {
	std::vector<std::string> files;
	files_under(root, "", files);
	std::sort(files.begin(), files.end());
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
	auto update = [&](const void *data, size_t n) {
		if (n) {
			BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(n), 0);
		}
	};
	for (const std::string &rel : files) {
		update(rel.data(), rel.size());
		const unsigned char zero = 0;
		update(&zero, 1);
		std::string bytes;
		read_file(root / fs::path(reinterpret_cast<const char8_t *>(rel.c_str())), bytes);
		update(bytes.data(), bytes.size());
	}
	unsigned char digest[32];
	BCryptFinishHash(hash, digest, sizeof(digest), 0);
	BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(alg, 0);
	static const char *hex = "0123456789abcdef";
	std::string out;
	for (int i = 0; i < 6; i++) { // 12 hex digits
		out += hex[digest[i] >> 4];
		out += hex[digest[i] & 15];
	}
	return out;
}

} // namespace gc
