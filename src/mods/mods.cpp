#include "mods/mods.h"

#include "data/lua_data.h"
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

const char *const EVENTS[] = {"tick", "block_placed", "block_broken", "block_changed", "creature_hit"};
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

fs::path utf8_path(const std::string &s) {
	return fs::path(std::u8string(s.begin(), s.end()));
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
std::string file_problem(const Data &rel, const fs::path &root, const char *extension) {
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

// What is wrong with a mod.lua for a mod in `root`, or "" (checked before
// anything of the mod loads, so a broken mod is skipped as a whole).
std::string manifest_problem(const Data &m, const fs::path &root) {
	if (!m.is_object()) {
		return "it must return a table of named fields";
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
	const Data deps = m.value("dependencies", Data::array());
	if (!deps.is_array() && !(deps.is_object() && deps.empty())) { // ({} reads as an empty object)
		return "\"dependencies\" must be a list";
	}
	for (const Data &d : deps) {
		if (!d.is_string() || !valid_id(d.get<std::string>())) {
			return "\"dependencies\" must list mod ids";
		}
	}
	if (m.contains("entry")) {
		if (std::string why = file_problem(m["entry"], root, ".lua"); !why.empty()) {
			return "entry " + why;
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
	struct Command {
		int mod;
		int ref;
		ModCommand info;
	};
	std::map<std::string, Command> commands; // commands.register
	std::map<std::string, std::vector<std::string>> functions; // "mod:name" -> its command file's lines
	bool registry_open = true;
	std::mt19937_64 random{std::random_device{}()};
	World *world = nullptr;
	const BlockRules *rules = nullptr;
	Mods::WorldHooks hooks;
	std::function<std::vector<std::string>(const std::string &)> run_command; // game.command

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
		Data def = lua_to_data(L, 2, 0);
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
			push_data(L, it->second);
		}
		return 1;
	}

	// registry.all(section): a copy of a whole section, {[id] = def}.
	static int registry_all(lua_State *L) {
		Lua *h = of(L);
		const std::map<std::string, Data> *t = h->content.table(luaL_checkstring(L, 1));
		if (!t) {
			return luaL_error(L, "no section '%s' (blocks, items, recipes, ores, trees, biomes, sound_sets, creatures)", lua_tostring(L, 1));
		}
		lua_createtable(L, 0, int(t->size()));
		for (const auto &[id, def] : *t) {
			push_data(L, def);
			lua_setfield(L, -2, id.c_str());
		}
		return 1;
	}

	// registry.recipes(): a copy of every recipe, {[id] = def}.
	static int registry_recipes(lua_State *L) {
		Lua *h = of(L);
		lua_createtable(L, 0, int(h->content.recipes.size()));
		for (const auto &[id, def] : h->content.recipes) {
			push_data(L, def);
			lua_setfield(L, -2, id.c_str());
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
			return luaL_error(L, "no event '%s' (there are: tick, block_placed, block_broken, block_changed)", event.c_str());
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

	// commands.register(name, {usage=, help=, op=}, function(who, args) -> reply)
	static int commands_register(lua_State *L) {
		Lua *h = of(L);
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		std::string name = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		luaL_checktype(L, 3, LUA_TFUNCTION);
		if (!h->registry_open) {
			return luaL_error(L, "commands can only be registered while mods load");
		}
		if (name.empty() || name.size() > 32 ||
				name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
			return luaL_error(L, "a command's name is a-z 0-9 _ (32 at most): '%s'", name.c_str());
		}
		const Data def = lua_to_data(L, 2, 0);
		ModCommand info;
		info.name = name;
		info.usage = def.is_object() ? def.value("usage", "") : "";
		info.help = def.is_object() ? def.value("help", "") : "";
		info.op = def.is_object() ? def.value("op", true) : true;
		lua_pushvalue(L, 3);
		auto it = h->commands.find(name);
		if (it != h->commands.end()) {
			luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref); // (the last mod to register a name wins)
		}
		h->commands[name] = {mod, luaL_ref(L, LUA_REGISTRYINDEX), info};
		return 0;
	}

	// game.command(line) -> {reply lines}: runs a command as the world itself (an operator).
	static int game_command(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const std::string line = luaL_checkstring(L, 1);
		Data out = Data::array();
		if (h->run_command) {
			for (const std::string &s : h->run_command(line)) {
				out.push_back(s);
			}
		}
		push_data(L, out);
		return 1;
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
		const int index = of(L)->content.block_index(undyed(id)); // ("core:stone#ff8800": dyed)
		if (index < 0) {
			return luaL_error(L, "no block '%s'", id);
		}
		lua_pushboolean(L, w->set_block(p, gocube::BlockId(index), dye_of(id)));
		return 1;
	}

	static BlockPos check_pos(lua_State *L) {
		return {int(luaL_checkinteger(L, 1)), int(luaL_checkinteger(L, 2)), int(luaL_checkinteger(L, 3))};
	}

	static int world_supported(lua_State *L) {
		World *w = need_world(L);
		const BlockPos p = check_pos(L);
		lua_pushboolean(L, of(L)->rules->supported(*w, luaL_checkstring(L, 4), p));
		return 1;
	}

	static int world_drop_item(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const BlockPos p = check_pos(L);
		const std::string item = luaL_checkstring(L, 4);
		const int count = int(luaL_optinteger(L, 5, 1));
		if (!h->content.items.count(item) && !h->content.blocks.count(undyed(item))) {
			return luaL_error(L, "no item '%s'", item.c_str());
		}
		if (h->hooks.drop_item && count > 0) {
			h->hooks.drop_item(p, item, std::min(count, 64 * 36));
		}
		return 0;
	}

	static int world_grow_tree(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const BlockPos p = check_pos(L);
		lua_pushboolean(L, h->hooks.grow_tree && h->hooks.grow_tree(p, luaL_checkstring(L, 4)));
		return 1;
	}

	// world.raycast(x, y, z, dx, dy, dz, max): the first block a ray meets
	// (fluids seen through), {x, y, z, id, distance}, or nil.
	static int world_raycast(lua_State *L) {
		World *w = need_world(L);
		const float o[3] = {float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3))};
		float d[3] = {float(luaL_checknumber(L, 4)), float(luaL_checknumber(L, 5)), float(luaL_checknumber(L, 6))};
		const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		const float max = std::clamp(float(luaL_checknumber(L, 7)), 0.0f, 512.0f);
		if (!(len > 1e-6f) || !std::isfinite(len)) {
			return luaL_error(L, "raycast: the direction must not be zero");
		}
		for (float &v : d) {
			v /= len;
		}
		const RayHit hit = w->raycast(o, d, max);
		if (!hit.hit) {
			lua_pushnil(L);
			return 1;
		}
		push_data(L, Data{{"x", hit.position.x}, {"y", hit.position.y}, {"z", hit.position.z},
							 {"id", of(L)->rules->id_of(hit.block)}, {"distance", hit.distance}});
		return 1;
	}

	// world.box_free(x, y, z, width, height, depth): nothing solid in the box
	// standing at x, y, z (its bottom centre).
	static int world_box_free(lua_State *L) {
		World *w = need_world(L);
		const float p[3] = {float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3))};
		const float size[3] = {std::clamp(float(luaL_checknumber(L, 4)), 0.0f, 8.0f),
				std::clamp(float(luaL_checknumber(L, 5)), 0.0f, 8.0f), std::clamp(float(luaL_checknumber(L, 6)), 0.0f, 8.0f)};
		lua_pushboolean(L, w->is_box_free(p, size));
		return 1;
	}

	// --- creatures ---

	// creatures.spawn(kind, x, y, z [, variant]) -> id, or nil (unknown kind, too many).
	static int creatures_spawn(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const int mod = int(lua_tointeger(L, lua_upvalueindex(1)));
		std::string kind = luaL_checkstring(L, 1);
		if (kind.find(':') == std::string::npos) {
			kind = h->mods[mod].info.id + ":" + kind;
		}
		const float at[3] = {float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))};
		const std::string variant = luaL_optstring(L, 5, "");
		const int id = h->hooks.creature_spawn ? h->hooks.creature_spawn(kind, at, variant) : 0;
		if (id > 0) {
			lua_pushinteger(L, id);
		} else {
			lua_pushnil(L);
		}
		return 1;
	}

	static int creatures_remove(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		lua_pushboolean(L, h->hooks.creature_remove && h->hooks.creature_remove(int(luaL_checkinteger(L, 1))));
		return 1;
	}

	// creatures.all() -> {id, ...}
	static int creatures_all(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const std::vector<int> ids = h->hooks.creature_ids ? h->hooks.creature_ids() : std::vector<int>();
		lua_createtable(L, int(ids.size()), 0);
		for (size_t i = 0; i < ids.size(); i++) {
			lua_pushinteger(L, ids[i]);
			lua_rawseti(L, -2, lua_Integer(i + 1));
		}
		return 1;
	}

	// creatures.get(id) -> {id, kind, variant, x, y, z, yaw, heading, speed,
	// on_floor, hit_wall, walking}, or nil.
	static int creatures_get(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const Data c = h->hooks.creature_get ? h->hooks.creature_get(int(luaL_checkinteger(L, 1))) : Data();
		push_data(L, c);
		return 1;
	}

	// creatures.set(id, {heading, speed, variant, yaw, push = {x, y, z}, hurt,
	// flash, attack}) -> false if there's no such creature.
	static int creatures_set(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const int id = int(luaL_checkinteger(L, 1));
		luaL_checktype(L, 2, LUA_TTABLE);
		const Data fields = lua_to_data(L, 2);
		lua_pushboolean(L, h->hooks.creature_set && h->hooks.creature_set(id, fields));
		return 1;
	}

	// --- game, while playing ---

	// game.players() -> {{peer, x, y, z, survival, alive}, ...}
	static int game_players(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		push_data(L, h->hooks.players ? h->hooks.players() : Data::array());
		return 1;
	}

	// game.day() -> 0 at night .. 1 in full day
	static int game_day(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		lua_pushnumber(L, h->hooks.day ? h->hooks.day() : 1.0);
		return 1;
	}

	// game.damage_player(peer, amount, source [, push_x, push_y, push_z])
	static int game_damage_player(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const int peer = int(luaL_checkinteger(L, 1));
		const int amount = int(std::clamp<lua_Integer>(luaL_checkinteger(L, 2), 0, 1000));
		const std::string source = luaL_optstring(L, 3, "");
		const float push[3] = {std::clamp(float(luaL_optnumber(L, 4, 0)), -40.0f, 40.0f),
				std::clamp(float(luaL_optnumber(L, 5, 0)), -40.0f, 40.0f), std::clamp(float(luaL_optnumber(L, 6, 0)), -40.0f, 40.0f)};
		if (h->hooks.damage_player) {
			h->hooks.damage_player(peer, amount, source, push);
		}
		return 0;
	}

	// game.play_sound(set, kind, x, y, z [, volume_db])
	static int game_play_sound(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const float at[3] = {float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4)), float(luaL_checknumber(L, 5))};
		if (h->hooks.play_sound) {
			h->hooks.play_sound(luaL_checkstring(L, 1), luaL_checkstring(L, 2), at,
					std::clamp(float(luaL_optnumber(L, 6, 0)), -80.0f, 12.0f));
		}
		return 0;
	}

	// game.effect(name, x, y, z): "smoke" (a puff).
	static int game_effect(lua_State *L) {
		need_world(L);
		Lua *h = of(L);
		const float at[3] = {float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))};
		if (h->hooks.effect) {
			h->hooks.effect(luaL_checkstring(L, 1), at);
		}
		return 0;
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
		for (const char *section : {"block", "item", "recipe", "ore", "tree", "biome", "sound_set", "creature"}) {
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
		lua_pushcfunction(L, &Lua::registry_recipes);
		lua_setfield(L, -2, "recipes");
		lua_pushcfunction(L, &Lua::registry_all);
		lua_setfield(L, -2, "all");
		lua_setfield(L, -2, "registry");

		lua_newtable(L); // commands
		push_closure(&Lua::commands_register, mod);
		lua_setfield(L, -2, "register");
		lua_setfield(L, -2, "commands");

		lua_newtable(L); // events
		push_closure(&Lua::events_on, mod);
		lua_setfield(L, -2, "on");
		lua_setfield(L, -2, "events");

		lua_newtable(L); // world
		lua_pushcfunction(L, &Lua::world_get_block);
		lua_setfield(L, -2, "get_block");
		lua_pushcfunction(L, &Lua::world_set_block);
		lua_setfield(L, -2, "set_block");
		lua_pushcfunction(L, &Lua::world_supported);
		lua_setfield(L, -2, "supported");
		lua_pushcfunction(L, &Lua::world_drop_item);
		lua_setfield(L, -2, "drop_item");
		lua_pushcfunction(L, &Lua::world_grow_tree);
		lua_setfield(L, -2, "grow_tree");
		lua_pushcfunction(L, &Lua::world_raycast);
		lua_setfield(L, -2, "raycast");
		lua_pushcfunction(L, &Lua::world_box_free);
		lua_setfield(L, -2, "box_free");
		lua_setfield(L, -2, "world");

		lua_newtable(L); // creatures
		push_closure(&Lua::creatures_spawn, mod);
		lua_setfield(L, -2, "spawn");
		lua_pushcfunction(L, &Lua::creatures_remove);
		lua_setfield(L, -2, "remove");
		lua_pushcfunction(L, &Lua::creatures_all);
		lua_setfield(L, -2, "all");
		lua_pushcfunction(L, &Lua::creatures_get);
		lua_setfield(L, -2, "get");
		lua_pushcfunction(L, &Lua::creatures_set);
		lua_setfield(L, -2, "set");
		lua_setfield(L, -2, "creatures");

		lua_newtable(L); // game
		push_closure(&Lua::game_set_rule, mod);
		lua_setfield(L, -2, "set_rule");
		lua_pushcfunction(L, &Lua::game_rule);
		lua_setfield(L, -2, "rule");
		lua_pushcfunction(L, &Lua::game_command);
		lua_setfield(L, -2, "command");
		lua_pushcfunction(L, &Lua::game_random);
		lua_setfield(L, -2, "random");
		lua_pushcfunction(L, &Lua::game_players);
		lua_setfield(L, -2, "players");
		lua_pushcfunction(L, &Lua::game_day);
		lua_setfield(L, -2, "day");
		lua_pushcfunction(L, &Lua::game_damage_player);
		lua_setfield(L, -2, "damage_player");
		lua_pushcfunction(L, &Lua::game_play_sound);
		lua_setfield(L, -2, "play_sound");
		lua_pushcfunction(L, &Lua::game_effect);
		lua_setfield(L, -2, "effect");
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
		for (auto it = commands.begin(); it != commands.end();) {
			if (it->second.mod == mod) {
				luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
				it = commands.erase(it);
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

void Mods::load_all(const std::vector<fs::path> &dirs, const std::vector<std::string> *only) {
	struct Found {
		Data manifest;
		fs::path dir;
	};
	std::map<std::string, Found> found;
	auto fail = [&](const std::string &id, const std::string &why) {
		problems_[id] = why;
		log("Mod '%s': %s", id.c_str(), why.c_str());
	};
	for (const fs::path &root : dirs) {
		for (const fs::path &dir : visible_subdirs(root)) {
			if (!fs::exists(dir / "mod.lua")) {
				continue;
			}
			Data m;
			std::string why;
			if (!read_lua_data(dir / "mod.lua", m, why)) {
				why = "it can't be read: " + why;
			} else {
				why = manifest_problem(m, dir);
			}
			if (!why.empty()) {
				fail(utf8(dir.filename()), "invalid mod.lua (" + why + "); skipping it");
				continue;
			}
			const std::string id = m["id"].get<std::string>();
			if (!only && id != "core" && std::find(disabled_.begin(), disabled_.end(), id) != disabled_.end()) {
				log("Mod '%s' is turned off", id.c_str());
				continue;
			}
			if (only) {
				ModInfo info;
				info.id = id;
				info.version = m.value("version", "");
				info.fingerprint = mod_fingerprint(dir);
				if (std::find(only->begin(), only->end(), mod_entry(info)) == only->end()) {
					continue; // (not one of the host's: another copy may be)
				}
			}
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
		for (const Data &d : found[id].manifest.value("dependencies", Data::array())) {
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
		for (const Data &d : f.manifest.value("dependencies", Data::array())) {
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
		// Mods from where they may not run code (server mods, if the player said so).
		if (f.manifest.contains("entry")) {
			bool forbidden = false;
			for (const fs::path &folder : code_forbidden_) {
				const std::string a = utf8(fs::weakly_canonical(folder)), b = utf8(fs::weakly_canonical(f.dir));
				forbidden = forbidden || (b.size() > a.size() && b.compare(0, a.size(), a) == 0);
			}
			if (forbidden) {
				fail(id, "runs code (Lua), and your settings only allow server mods without code");
				failed.insert(id);
				continue;
			}
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
		const int index = int(lua_->mods.size());
		lua_->mods.push_back({info});
		// Content as data (no code runs): {blocks = {[id] = def}, items = ..., ...}.
		if (f.manifest.contains("content")) {
			const std::string error = load_data_content(f.dir, f.manifest, id, content);
			if (!error.empty()) {
				content = before;
				lua_->drop(index);
				fail(id, error);
				failed.insert(id);
				continue;
			}
		}
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
		load_functions(f.dir, id);
		loaded_.push_back(info);
		log("Mod %s %s loaded (%s)", id.c_str(), info.version.c_str(), info.fingerprint.c_str());
	}
	lua_->registry_open = false;
}

std::optional<Data> Mods::rule(const std::string &name, const Data &args) {
	Lua &h = *lua_;
	auto it = h.game_rules.find(name);
	if (it == h.game_rules.end() || h.mods[it->second.mod].disabled) {
		return std::nullopt;
	}
	const int mod = it->second.mod;
	lua_State *L = h.L;
	const int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
	for (const Data &a : args) {
		push_data(L, a);
	}
	const std::string error = h.call(mod, int(args.size()), h.limits.callback_ms, 1);
	if (!error.empty()) {
		log("Mod '%s': its %s rule failed: %s", h.mods[mod].info.id.c_str(), name.c_str(), error.c_str());
		lua_settop(L, top);
		return std::nullopt;
	}
	std::optional<Data> out;
	// (Converting can raise a Lua error for a value that can't be data: protected.)
	lua_pushcfunction(L, [](lua_State *L) -> int {
		auto *result = static_cast<std::optional<Data> *>(lua_touserdata(L, 2));
		*result = lua_to_data(L, 1, 0);
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

// A mod's `content` file: a data table read without running anything.
std::string Mods::load_data_content(const fs::path &dir, const Data &manifest, const std::string &id, Content &content) {
	const Data &file = manifest["content"];
	if (!file.is_string()) {
		return "its \"content\" isn't a file name";
	}
	const fs::path path = dir / utf8_path(file.get<std::string>());
	const std::string a = utf8(fs::weakly_canonical(dir)), b = utf8(fs::weakly_canonical(path));
	if (b.size() <= a.size() || b.compare(0, a.size(), a) != 0) {
		return "its content file must be inside the mod";
	}
	Data data;
	std::string why;
	if (!read_lua_data(path, data, why)) {
		return "its content file can't be read: " + why;
	}
	if (!data.is_object()) {
		return "its content file must be a table of sections (blocks = {...}, items = {...})";
	}
	for (const auto &[section, entries] : data.items()) {
		std::map<std::string, Data> *table = content.table(section);
		if (!table || !entries.is_object()) {
			return "its content file has an unknown section \"" + section + "\" (blocks, items, recipes, ores, "
					"trees, biomes, sound_sets, creatures)";
		}
		for (const auto &[entry_id, def] : entries.items()) {
			if (!def.is_object()) {
				return "its content file's " + section + "." + entry_id + " isn't a table of named fields";
			}
			content.register_entry(*table, id, entry_id, def);
		}
	}
	return "";
}

// A mod's command files: functions/NAME.txt (in folders too) -> "mod:NAME".
void Mods::load_functions(const fs::path &dir, const std::string &id) {
	std::error_code ec;
	const fs::path root = dir / "functions";
	if (!fs::is_directory(root, ec)) {
		return;
	}
	for (const std::string &rel : mod_files(root)) {
		if (rel.size() <= 4 || rel.compare(rel.size() - 4, 4, ".txt") != 0) {
			continue;
		}
		std::string text;
		if (!read_file(root / utf8_path(rel), text)) {
			continue;
		}
		std::vector<std::string> lines;
		size_t from = 0;
		while (from <= text.size() && lines.size() < 10000) {
			size_t to = text.find('\n', from);
			to = to == std::string::npos ? text.size() : to;
			std::string line = text.substr(from, to - from);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			lines.push_back(line);
			from = to + 1;
		}
		lua_->functions[id + ":" + rel.substr(0, rel.size() - 4)] = std::move(lines);
	}
}

std::vector<ModCommand> Mods::commands() const {
	std::vector<ModCommand> out;
	for (const auto &[name, c] : lua_->commands) {
		if (!lua_->mods[c.mod].disabled) {
			out.push_back(c.info);
		}
	}
	return out;
}

std::optional<std::string> Mods::run_command(const std::string &name, const CommandPlayer &who,
		const std::vector<std::string> &args) {
	Lua &h = *lua_;
	auto it = h.commands.find(name);
	if (it == h.commands.end() || h.mods[it->second.mod].disabled) {
		return std::nullopt;
	}
	const int mod = it->second.mod;
	lua_State *L = h.L;
	const int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
	push_data(L, Data{{"peer", who.peer}, {"name", who.name}, {"x", who.feet[0]}, {"y", who.feet[1]}, {"z", who.feet[2]},
						 {"op", who.op}});
	Data list = Data::array();
	for (const std::string &a : args) {
		list.push_back(a);
	}
	push_data(L, list);
	const std::string error = h.call(mod, 2, h.limits.callback_ms, 1);
	if (!error.empty()) {
		log("Mod '%s': its command /%s failed: %s", h.mods[mod].info.id.c_str(), name.c_str(), error.c_str());
		lua_settop(L, top);
		return std::nullopt;
	}
	std::string reply;
	if (lua_type(L, -1) == LUA_TSTRING) {
		reply = lua_tostring(L, -1);
	}
	lua_settop(L, top);
	return reply;
}

const std::vector<std::string> *Mods::function(const std::string &name) const {
	auto it = lua_->functions.find(name);
	return it != lua_->functions.end() ? &it->second : nullptr;
}

void Mods::set_command_runner(std::function<std::vector<std::string>(const std::string &line)> run) {
	lua_->run_command = std::move(run);
}

void Mods::attach(World *world, const BlockRules *rules) {
	lua_->world = world;
	lua_->rules = rules;
}

void Mods::set_world_hooks(WorldHooks hooks) {
	lua_->hooks = std::move(hooks);
}

void Mods::block_changed(const BlockPos &p, const std::string &id) {
	lua_State *L = lua_->L;
	lua_->fire("block_changed", 4, [&] {
		lua_pushinteger(L, p.x);
		lua_pushinteger(L, p.y);
		lua_pushinteger(L, p.z);
		lua_pushstring(L, id.c_str());
	}, problems_);
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

void Mods::creature_hit(int id, int damage, const float from[3], int attacker) {
	lua_State *L = lua_->L;
	lua_->fire("creature_hit", 6, [&] {
		lua_pushinteger(L, id);
		lua_pushinteger(L, damage);
		lua_pushnumber(L, from[0]);
		lua_pushnumber(L, from[1]);
		lua_pushnumber(L, from[2]);
		lua_pushinteger(L, attacker);
	}, problems_);
}

// --- Fingerprints ----------------------------------------------------------------------

namespace {

// Relative paths ('/' between folders) of the files under root, without
// dot-names, sorted.
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

std::vector<std::string> mod_files(const fs::path &root) {
	std::vector<std::string> files;
	files_under(root, "", files);
	std::sort(files.begin(), files.end());
	return files;
}

std::string mod_entry(const ModInfo &m) {
	return m.id + "@" + m.version + "#" + m.fingerprint;
}

std::map<std::string, fs::path> mod_copies(const std::vector<fs::path> &dirs) {
	std::map<std::string, fs::path> out;
	for (const fs::path &root : dirs) {
		for (const fs::path &dir : visible_subdirs(root)) {
			Data m;
			std::string why;
			if (!fs::exists(dir / "mod.lua") || !read_lua_data(dir / "mod.lua", m, why) || !m.is_object() ||
					!m.contains("id") || !m["id"].is_string()) {
				continue;
			}
			ModInfo info;
			info.id = m["id"].get<std::string>();
			info.version = m.value("version", "");
			info.fingerprint = mod_fingerprint(dir);
			out.emplace(mod_entry(info), dir);
		}
	}
	return out;
}

std::string mod_fingerprint(const fs::path &root) {
	const std::vector<std::string> files = mod_files(root);
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
