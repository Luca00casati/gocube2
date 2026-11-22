#include "mods/mods.h"

#include "data/json.h"
#include "game/blocks.h"
#include "log.h"
#include "mods/gocube_api.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace gc {

namespace fs = std::filesystem;

namespace {

const char *const EVENTS[] = {"tick", "block_placed", "block_broken", "block_changed", "creature_hit"};

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

bool ends_with(const std::string &s, const char *end) {
	const size_t n = strlen(end);
	return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
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
	if (!ends_with(r, extension)) {
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
		if (std::string why = file_problem(m["entry"], root, ".dll"); !why.empty()) {
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

// --- Calling into a mod's DLL, catching crashes ---------------------------------------
// (Structured exception handling: these functions hold no C++ objects.)

int seh_filter(unsigned long code, unsigned long *out) {
	*out = code;
	return EXCEPTION_EXECUTE_HANDLER;
}

// 0 if it ran (*status: what it returned), else the exception's code.
unsigned long guarded_function(GcFunction fn, void *user, const char *args, const char **result, int *status) {
	unsigned long code = 0;
	__try {
		*status = fn(user, args, result);
	} __except (seh_filter(GetExceptionCode(), &code)) {
		return code ? code : 1;
	}
	return 0;
}

unsigned long guarded_load(GcModLoad load, const GcApi *api, void **instance, const char **error) {
	unsigned long code = 0;
	__try {
		*error = load(api, instance);
	} __except (seh_filter(GetExceptionCode(), &code)) {
		return code ? code : 1;
	}
	return 0;
}

unsigned long guarded_unload(GcModUnload unload, void *instance) {
	unsigned long code = 0;
	__try {
		unload(instance);
	} __except (seh_filter(GetExceptionCode(), &code)) {
		return code ? code : 1;
	}
	return 0;
}

std::string crash_text(unsigned long code) {
	char buf[96];
	if (code == 0xC0000005) {
		std::snprintf(buf, sizeof(buf), "it crashed (an access violation)");
	} else if (code == 0xE06D7363) {
		std::snprintf(buf, sizeof(buf), "it crashed (a C++ exception it didn't catch)");
	} else {
		std::snprintf(buf, sizeof(buf), "it crashed (exception 0x%08lX)", code);
	}
	return buf;
}

std::string windows_error(DWORD e) {
	wchar_t *msg = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
			0, reinterpret_cast<wchar_t *>(&msg), 0, nullptr);
	std::string out = "error " + std::to_string(e);
	if (msg) {
		const int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
		std::string text(size_t(std::max(n, 1)) - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, msg, -1, text.data(), n, nullptr, nullptr);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
			text.pop_back();
		}
		out = text + " (" + out + ")";
		LocalFree(msg);
	}
	return out;
}

} // namespace

} // namespace gc

// The game's handle for a mod (gocube_api.h's opaque GcMod).
struct GcMod {
	gc::Mods::Impl *impl;
	int index;
};

namespace gc {

// --- Mods' code -------------------------------------------------------------------------

struct Mods::Impl {
	struct ModState {
		ModInfo info;
		GcMod handle{};
		GcApi api{};
		HMODULE dll = nullptr;
		GcModUnload unload = nullptr;
		void *instance = nullptr;
		std::string out; // what the game last handed it
		int errors = 0;
		bool disabled = false;
	};
	struct Function {
		int mod = 0;
		GcFunction fn = nullptr;
		void *user = nullptr;
	};
	struct Handler : Function {
		std::string event;
	};
	struct Command : Function {
		ModCommand info;
	};

	Content &content;
	std::map<std::string, std::string> &problems;
	std::vector<std::unique_ptr<ModState>> mods;
	std::vector<Handler> handlers;
	std::map<std::string, std::vector<Function>> game_rules; // by name: the last set wins (the others, to wrap)
	std::map<std::string, Command> commands;
	std::map<std::string, std::vector<std::string>> functions; // "mod:name" -> its command file's lines
	bool registry_open = true;
	std::mt19937_64 random{std::random_device{}()};
	World *world = nullptr;
	const BlockRules *rules = nullptr;
	Mods::WorldHooks hooks;
	std::function<std::vector<std::string>(const std::string &)> run_command; // game.command

	Impl(Content &c, std::map<std::string, std::string> &p) : content(c), problems(p) {}
	~Impl() {
		for (size_t i = mods.size(); i-- > 0;) {
			release(int(i));
		}
	}

	static Impl &of(GcMod *m) { return *m->impl; }

	// --- Calling a mod's functions ---

	// Calls `f` with these arguments (a list): its result (null for nothing),
	// or nullopt and `error`. A crash turns the mod off at once.
	std::optional<Data> invoke(const Function &f, const Data &args, std::string &error) {
		std::string text;
		try {
			text = write_json(args.is_array() ? args : Data::array(), true);
		} catch (const std::exception &e) {
			error = e.what();
			return std::nullopt;
		}
		const char *result = nullptr;
		int status = 0;
		if (const unsigned long code = guarded_function(f.fn, f.user, text.c_str(), &result, &status); code != 0) {
			error = crash_text(code);
			turn_off(f.mod, error);
			return std::nullopt;
		}
		if (status != 0) {
			error = result ? result : "it failed";
			return std::nullopt;
		}
		Data list;
		std::string why;
		if (result && !parse_json(result, mods[size_t(f.mod)]->info.id, list, why)) {
			error = "it returned something that isn't data: " + why;
			return std::nullopt;
		}
		return list.is_array() && !list.empty() ? list[0] : Data();
	}

	void turn_off(int mod, const std::string &why) {
		ModState &m = *mods[size_t(mod)];
		if (m.disabled) {
			return;
		}
		problems[m.info.id] = "turned off: " + why;
		log("Mod '%s' is turned off until the game restarts: %s", m.info.id.c_str(), why.c_str());
		drop(mod);
	}

	// Forgets a mod's functions (it failed or was turned off). Its DLL stays
	// loaded until the end: something of it may still be running.
	void drop(int mod) {
		auto mine = [&](const Function &f) { return f.mod == mod; };
		for (auto &[name, stack] : game_rules) {
			stack.erase(std::remove_if(stack.begin(), stack.end(), mine), stack.end());
		}
		handlers.erase(std::remove_if(handlers.begin(), handlers.end(), mine), handlers.end());
		for (auto it = commands.begin(); it != commands.end();) {
			it = it->second.mod == mod ? commands.erase(it) : std::next(it);
		}
		mods[size_t(mod)]->disabled = true;
	}

	// Unloads a mod's DLL (after its unload function).
	void release(int mod) {
		ModState &m = *mods[size_t(mod)];
		if (m.unload && m.instance) {
			if (const unsigned long code = guarded_unload(m.unload, m.instance); code != 0) {
				log("Mod '%s': unloading it failed: %s", m.info.id.c_str(), crash_text(code).c_str());
			}
		}
		m.instance = nullptr;
		if (m.dll) {
			FreeLibrary(m.dll);
			m.dll = nullptr;
		}
	}

	// Calls every handler of `event`.
	void fire(const char *event, const Data &args) {
		const std::vector<Handler> list = handlers; // (a handler may turn its mod off)
		for (const Handler &hd : list) {
			if (hd.event != event || mods[size_t(hd.mod)]->disabled) {
				continue;
			}
			std::string error;
			if (invoke(hd, args, error)) {
				continue;
			}
			ModState &m = *mods[size_t(hd.mod)];
			log("Mod '%s': its %s handler failed: %s", m.info.id.c_str(), event, error.c_str());
			if (!m.disabled && ++m.errors >= ERRORS_BEFORE_DISABLED) {
				turn_off(hd.mod, "its event handlers kept failing (last: " + error + ")");
			}
		}
	}

	// The rule `name` below mod `mod`'s own (the one it replaced), or null.
	const Function *rule_below(const std::string &name, int mod) const {
		auto it = game_rules.find(name);
		if (it == game_rules.end()) {
			return nullptr;
		}
		const std::vector<Function> &stack = it->second;
		size_t i = stack.size();
		while (i > 0 && stack[i - 1].mod != mod) {
			i--;
		}
		return i >= 2 ? &stack[i - 2] : nullptr;
	}

	// --- The API mods call ---

	World *need_world() const {
		if (!world || !rules) {
			throw std::runtime_error("there is no world yet (use world functions while playing: in event handlers and rules)");
		}
		return world;
	}

	static BlockPos pos(const Data &a) {
		return {a.at(size_t(0)).get<int>(), a.at(size_t(1)).get<int>(), a.at(size_t(2)).get<int>()};
	}
	static float num(const Data &a, size_t i) { return a.at(i).get<float>(); }
	static float opt(const Data &a, size_t i, float fallback) { return a[i].is_number() ? a[i].get<float>() : fallback; }
	static std::string text(const Data &a, size_t i) { return a.at(i).get<std::string>(); }

	std::map<std::string, Data> &section(const std::string &name) {
		std::map<std::string, Data> *t = content.table(name);
		if (!t) {
			throw std::runtime_error("no section '" + name +
					"' (blocks, items, recipes, ores, trees, biomes, sound_sets, creatures)");
		}
		return *t;
	}

	// 1 changed, 0 not, -1 unknown block.
	int set_block(const BlockPos &p, const std::string &id) {
		const int index = content.block_index(undyed(id)); // ("core:stone#ff8800": dyed)
		if (index < 0) {
			return -1;
		}
		return world->set_block(p, gocube::BlockId(index), dye_of(id)) ? 1 : 0;
	}

	// GcApi::call's functions: the result (null for nothing); errors throw.
	Data api_call(int mod, const std::string &fn, const Data &a) {
		ModState &m = *mods[size_t(mod)];
		if (fn == "registry.register") {
			if (!registry_open) {
				throw std::runtime_error("content can only be registered while mods load");
			}
			std::map<std::string, Data> &t = section(text(a, 0));
			const std::string id = text(a, 1);
			const Data &def = a.at(size_t(2));
			if (!def.is_object()) {
				throw std::runtime_error("a definition must be a table with named fields");
			}
			content.register_entry(t, m.info.id, id, def);
			return id.find(':') != std::string::npos ? id : m.info.id + ":" + id;
		}
		if (fn == "registry.get") {
			const std::map<std::string, Data> &t = section(text(a, 0));
			auto it = t.find(text(a, 1));
			return it == t.end() ? Data() : it->second;
		}
		if (fn == "registry.all") {
			Data out = Data::object();
			for (const auto &[id, def] : section(text(a, 0))) {
				out.add_member(id, def);
			}
			return out;
		}
		if (fn == "registry.with") {
			const std::string field = text(a, 1);
			Data out = Data::object();
			for (const auto &[id, def] : section(text(a, 0))) {
				if (def.contains(field)) {
					out.add_member(id, def);
				}
			}
			return out;
		}
		if (fn == "mod.info") {
			return Data{{"id", m.info.id}, {"version", m.info.version}, {"dir", utf8(m.info.dir)}};
		}
		if (fn == "game.random") {
			if (registry_open) {
				throw std::runtime_error("random() only works while playing (what mods register must depend only on their files)");
			}
			return std::uniform_real_distribution<double>(0.0, 1.0)(random);
		}
		if (fn == "game.rule_before") {
			const std::string name = text(a, 0);
			const Function *below = rule_below(name, mod);
			if (!below) {
				return Data();
			}
			const Function f = *below;
			Data rest = Data::array();
			for (size_t i = 1; i < a.size(); i++) {
				rest.push_back(a[i]);
			}
			std::string error;
			std::optional<Data> r = invoke(f, rest, error);
			if (!r) {
				throw std::runtime_error("the " + name + " rule before it failed: " + error);
			}
			return *r;
		}
		// While playing.
		World *w = need_world();
		if (fn == "world.get_block") {
			const BlockPos p = pos(a);
			const int id = w->block_id_at(p.x, p.y, p.z);
			return id < 0 ? Data() : Data(rules->id_of(id));
		}
		if (fn == "world.set_block") {
			const int r = set_block(pos(a), text(a, 3));
			if (r < 0) {
				throw std::runtime_error("no block '" + text(a, 3) + "'");
			}
			return r > 0;
		}
		if (fn == "world.supported") {
			return rules->supported(*w, text(a, 3), pos(a));
		}
		if (fn == "world.drop_item") {
			const std::string item = text(a, 3);
			const int count = a[size_t(4)].is_number() ? a[size_t(4)].get<int>() : 1;
			if (!content.items.count(item) && !content.blocks.count(undyed(item))) {
				throw std::runtime_error("no item '" + item + "'");
			}
			if (hooks.drop_item && count > 0) {
				hooks.drop_item(pos(a), item, std::min(count, 64 * 36));
			}
			return Data();
		}
		if (fn == "world.grow_tree") {
			return bool(hooks.grow_tree && hooks.grow_tree(pos(a), text(a, 3)));
		}
		if (fn == "world.raycast") {
			const float o[3] = {num(a, 0), num(a, 1), num(a, 2)};
			float d[3] = {num(a, 3), num(a, 4), num(a, 5)};
			const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			const float max = std::clamp(num(a, 6), 0.0f, 512.0f);
			if (!(len > 1e-6f) || !std::isfinite(len)) {
				throw std::runtime_error("raycast: the direction must not be zero");
			}
			for (float &v : d) {
				v /= len;
			}
			const RayHit hit = w->raycast(o, d, max);
			if (!hit.hit) {
				return Data();
			}
			return Data{{"x", hit.position.x}, {"y", hit.position.y}, {"z", hit.position.z},
					{"id", rules->id_of(hit.block)}, {"distance", hit.distance}};
		}
		if (fn == "world.box_free") {
			const float p[3] = {num(a, 0), num(a, 1), num(a, 2)};
			const float size[3] = {std::clamp(num(a, 3), 0.0f, 8.0f), std::clamp(num(a, 4), 0.0f, 8.0f),
					std::clamp(num(a, 5), 0.0f, 8.0f)};
			return w->is_box_free(p, size);
		}
		if (fn == "creatures.spawn") {
			std::string kind = text(a, 0);
			if (kind.find(':') == std::string::npos) {
				kind = m.info.id + ":" + kind;
			}
			const float at[3] = {num(a, 1), num(a, 2), num(a, 3)};
			const std::string variant = a[size_t(4)].is_string() ? a[size_t(4)].str() : "";
			const int id = hooks.creature_spawn ? hooks.creature_spawn(kind, at, variant) : 0;
			return id > 0 ? Data(id) : Data();
		}
		if (fn == "creatures.remove") {
			return bool(hooks.creature_remove && hooks.creature_remove(a.at(size_t(0)).get<int>()));
		}
		if (fn == "creatures.all") {
			Data out = Data::array();
			if (hooks.creature_ids) {
				for (int id : hooks.creature_ids()) {
					out.push_back(id);
				}
			}
			return out;
		}
		if (fn == "creatures.get") {
			return hooks.creature_get ? hooks.creature_get(a.at(size_t(0)).get<int>()) : Data();
		}
		if (fn == "creatures.set") {
			const Data &fields = a.at(size_t(1));
			if (!fields.is_object()) {
				throw std::runtime_error("creatures.set: the fields must be a table");
			}
			return bool(hooks.creature_set && hooks.creature_set(a.at(size_t(0)).get<int>(), fields));
		}
		if (fn == "game.players") {
			return hooks.players ? hooks.players() : Data::array();
		}
		if (fn == "game.day") {
			return hooks.day ? double(hooks.day()) : 1.0;
		}
		if (fn == "game.damage_player") {
			const int peer = a.at(size_t(0)).get<int>();
			const int amount = int(std::clamp<int64_t>(a.at(size_t(1)).get<int64_t>(), 0, 1000));
			const std::string source = a[size_t(2)].is_string() ? a[size_t(2)].str() : "";
			const float push[3] = {std::clamp(opt(a, 3, 0), -40.0f, 40.0f), std::clamp(opt(a, 4, 0), -40.0f, 40.0f),
					std::clamp(opt(a, 5, 0), -40.0f, 40.0f)};
			if (hooks.damage_player) {
				hooks.damage_player(peer, amount, source, push);
			}
			return Data();
		}
		if (fn == "game.play_sound") {
			const float at[3] = {num(a, 2), num(a, 3), num(a, 4)};
			if (hooks.play_sound) {
				hooks.play_sound(text(a, 0), text(a, 1), at, std::clamp(opt(a, 5, 0), -80.0f, 12.0f));
			}
			return Data();
		}
		if (fn == "game.effect") {
			const float at[3] = {num(a, 1), num(a, 2), num(a, 3)};
			if (hooks.effect) {
				hooks.effect(text(a, 0), at);
			}
			return Data();
		}
		if (fn == "game.command") {
			Data out = Data::array();
			if (run_command) {
				for (const std::string &line : run_command(text(a, 0))) {
					out.push_back(line);
				}
			}
			return out;
		}
		throw std::runtime_error("no function '" + fn + "'");
	}

	// --- GcApi's functions ---

	static int c_call(GcMod *m, const char *function, const char *args, const char **result) {
		Impl &h = of(m);
		ModState &s = *h.mods[size_t(m->index)];
		const std::string name = function ? function : "";
		try {
			Data a;
			std::string why;
			if (!parse_json(args ? args : "{}", "arguments", a, why)) {
				throw std::runtime_error(why);
			}
			if (!a.is_array()) {
				a = Data::array();
			}
			const Data r = h.api_call(m->index, name, a);
			s.out = write_json(r.is_null() ? Data::array() : Data::array(Data::List{r}), true);
			*result = s.out.c_str();
			return 0;
		} catch (const std::out_of_range &) {
			s.out = name + ": missing arguments";
		} catch (const std::invalid_argument &) {
			s.out = name + ": an argument of the wrong type";
		} catch (const std::exception &e) {
			s.out = e.what();
		}
		*result = s.out.c_str();
		return 1;
	}

	static const char *c_get_block(GcMod *m, int x, int y, int z) {
		Impl &h = of(m);
		if (!h.world || !h.rules) {
			return nullptr;
		}
		const int id = h.world->block_id_at(x, y, z);
		return id < 0 ? nullptr : h.rules->id_of(id).c_str();
	}

	static int c_set_block(GcMod *m, int x, int y, int z, const char *id) {
		Impl &h = of(m);
		if (!h.world || !h.rules || !id) {
			return -1;
		}
		return h.set_block({x, y, z}, id);
	}

	static int c_supported(GcMod *m, int x, int y, int z, const char *id) {
		Impl &h = of(m);
		if (!h.world || !h.rules || !id || h.content.block_index(undyed(id)) < 0) {
			return -1;
		}
		return h.rules->supported(*h.world, id, {x, y, z}) ? 1 : 0;
	}

	static const char *c_on(GcMod *m, const char *event, GcFunction fn, void *user) {
		Impl &h = of(m);
		if (!event || !fn) {
			return "on: no event or function";
		}
		if (std::find_if(std::begin(EVENTS), std::end(EVENTS), [&](const char *e) { return strcmp(event, e) == 0; }) ==
				std::end(EVENTS)) {
			return "no such event (there are: tick, block_placed, block_broken, block_changed, creature_hit)";
		}
		Handler hd;
		hd.mod = m->index;
		hd.fn = fn;
		hd.user = user;
		hd.event = event;
		h.handlers.push_back(hd);
		return nullptr;
	}

	static const char *c_set_rule(GcMod *m, const char *name, GcFunction fn, void *user) {
		Impl &h = of(m);
		if (!name || !fn) {
			return "set_rule: no name or function";
		}
		if (!h.registry_open) {
			return "rules can only be set while mods load";
		}
		std::vector<Function> &stack = h.game_rules[name];
		stack.erase(std::remove_if(stack.begin(), stack.end(), [&](const Function &f) { return f.mod == m->index; }),
				stack.end()); // (a mod setting it again replaces its own)
		Function f;
		f.mod = m->index;
		f.fn = fn;
		f.user = user;
		stack.push_back(f);
		return nullptr;
	}

	static const char *c_add_command(GcMod *m, const char *name_c, const char *def_text, GcFunction fn, void *user) {
		Impl &h = of(m);
		if (!h.registry_open) {
			return "commands can only be registered while mods load";
		}
		const std::string name = name_c ? name_c : "";
		if (!fn || name.empty() || name.size() > 32 ||
				name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
			return "a command's name is a-z 0-9 _ (32 at most)";
		}
		Data def;
		std::string why;
		if (!def_text || !parse_json(def_text, "command", def, why)) {
			def = Data::object();
		}
		Command c;
		c.mod = m->index;
		c.fn = fn;
		c.user = user;
		c.info.name = name;
		c.info.usage = def.is_object() ? def.value("usage", "") : "";
		c.info.help = def.is_object() ? def.value("help", "") : "";
		c.info.op = def.is_object() ? def.value("op", true) : true;
		h.commands[name] = c; // (the last mod to register a name wins)
		return nullptr;
	}

	static void c_log(GcMod *m, const char *line) {
		log("[%s] %s", of(m).mods[size_t(m->index)]->info.id.c_str(), line ? line : "");
	}

	// Loads mod `mod`'s DLL and runs its load function. "" or what went wrong.
	std::string load_code(int mod, const std::string &entry) {
		ModState &m = *mods[size_t(mod)];
		m.handle = {this, mod};
		m.api.version = GOCUBE_API_VERSION;
		m.api.size = sizeof(GcApi);
		m.api.mod = &m.handle;
		m.api.call = &Impl::c_call;
		m.api.get_block = &Impl::c_get_block;
		m.api.set_block = &Impl::c_set_block;
		m.api.supported = &Impl::c_supported;
		m.api.on = &Impl::c_on;
		m.api.set_rule = &Impl::c_set_rule;
		m.api.add_command = &Impl::c_add_command;
		m.api.log = &Impl::c_log;
		const fs::path path = fs::absolute(m.info.dir / utf8_path(entry));
		// (Its own folder first for DLLs it needs, then the system's.)
		m.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!m.dll) {
			return "its DLL can't be loaded: " + windows_error(GetLastError());
		}
		const auto load = reinterpret_cast<GcModLoad>(GetProcAddress(m.dll, "gocube_mod_load"));
		m.unload = reinterpret_cast<GcModUnload>(GetProcAddress(m.dll, "gocube_mod_unload"));
		if (!load) {
			return "its DLL has no gocube_mod_load (see src/mods/gocube_api.h)";
		}
		const char *error = nullptr;
		if (const unsigned long code = guarded_load(load, &m.api, &m.instance, &error); code != 0) {
			m.instance = nullptr; // (whatever it made is lost)
			return crash_text(code);
		}
		if (error) {
			m.instance = nullptr; // (a mod that fails cleans up after itself)
			return error;
		}
		return "";
	}
};

// --- Mods ----------------------------------------------------------------------------

Mods::Mods(Content &content) : impl_(std::make_unique<Impl>(content, problems_)) {}

Mods::~Mods() = default;

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
			if (!fs::exists(dir / "mod.json")) {
				continue;
			}
			Data m;
			std::string why;
			if (!read_json_file(dir / "mod.json", m, why)) {
				why = "it can't be read: " + why;
			} else {
				why = manifest_problem(m, dir);
			}
			if (!why.empty()) {
				fail(utf8(dir.filename()), "invalid mod.json (" + why + "); skipping it");
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
	Content &content = impl_->content;
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
				fail(id, "runs code (a DLL), and your settings only allow server mods without code");
				failed.insert(id);
				continue;
			}
		}
		// What's registered so far, to undo this mod if its code fails.
		const Content before = content;
		ModInfo info;
		info.id = id;
		info.name = f.manifest.value("name", id);
		info.version = f.manifest.value("version", "");
		info.dir = f.dir;
		info.fingerprint = mod_fingerprint(f.dir);
		content.mod_dirs[id] = f.dir;
		const int index = int(impl_->mods.size());
		impl_->mods.push_back(std::make_unique<Impl::ModState>());
		impl_->mods.back()->info = info;
		auto undo = [&](const std::string &why) {
			content = before;
			impl_->drop(index);
			impl_->release(index);
			fail(id, why);
			failed.insert(id);
		};
		// Content as data (no code runs): {blocks = {[id] = def}, items = ..., ...}.
		if (f.manifest.contains("content")) {
			if (const std::string error = load_data_content(f.dir, f.manifest, id, content); !error.empty()) {
				undo(error);
				continue;
			}
		}
		if (f.manifest.contains("entry")) {
			if (const std::string error = impl_->load_code(index, f.manifest["entry"].get<std::string>()); !error.empty()) {
				undo("its code failed, so it was undone: " + error);
				continue;
			}
		}
		load_functions(f.dir, id);
		loaded_.push_back(info);
		log("Mod %s %s loaded (%s)", id.c_str(), info.version.c_str(), info.fingerprint.c_str());
	}
	impl_->registry_open = false;
}

std::optional<Data> Mods::rule(const std::string &name, const Data &args) {
	Impl &h = *impl_;
	auto it = h.game_rules.find(name);
	if (it == h.game_rules.end() || it->second.empty()) {
		return std::nullopt;
	}
	const Impl::Function f = it->second.back();
	std::string error;
	std::optional<Data> r = h.invoke(f, args, error);
	if (!r) {
		log("Mod '%s': its %s rule failed: %s", h.mods[size_t(f.mod)]->info.id.c_str(), name.c_str(), error.c_str());
	}
	return r;
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
	if (!read_json_file(path, data, why)) {
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
		impl_->functions[id + ":" + rel.substr(0, rel.size() - 4)] = std::move(lines);
	}
}

std::vector<ModCommand> Mods::commands() const {
	std::vector<ModCommand> out;
	for (const auto &[name, c] : impl_->commands) {
		out.push_back(c.info);
	}
	return out;
}

std::optional<std::string> Mods::run_command(const std::string &name, const CommandPlayer &who,
		const std::vector<std::string> &args) {
	Impl &h = *impl_;
	auto it = h.commands.find(name);
	if (it == h.commands.end()) {
		return std::nullopt;
	}
	const Impl::Command c = it->second;
	Data words = Data::array();
	for (const std::string &a : args) {
		words.push_back(a);
	}
	const Data player{{"peer", who.peer}, {"name", who.name}, {"x", who.feet[0]}, {"y", who.feet[1]},
			{"z", who.feet[2]}, {"op", who.op}};
	std::string error;
	const std::optional<Data> r = h.invoke(c, Data::array({player, words}), error);
	if (!r) {
		log("Mod '%s': its command /%s failed: %s", h.mods[size_t(c.mod)]->info.id.c_str(), name.c_str(), error.c_str());
		return std::nullopt;
	}
	return r->is_string() ? r->str() : std::string();
}

const std::vector<std::string> *Mods::function(const std::string &name) const {
	auto it = impl_->functions.find(name);
	return it != impl_->functions.end() ? &it->second : nullptr;
}

void Mods::set_command_runner(std::function<std::vector<std::string>(const std::string &line)> run) {
	impl_->run_command = std::move(run);
}

void Mods::attach(World *world, const BlockRules *rules) {
	impl_->world = world;
	impl_->rules = rules;
}

void Mods::set_world_hooks(WorldHooks hooks) {
	impl_->hooks = std::move(hooks);
}

void Mods::set_random_seed(uint64_t seed) {
	impl_->random.seed(seed);
}

void Mods::block_changed(const BlockPos &p, const std::string &id) {
	impl_->fire("block_changed", Data::array({p.x, p.y, p.z, id}));
}

void Mods::tick(double dt) {
	impl_->fire("tick", Data::array({dt}));
}

void Mods::block_placed(const BlockPos &p, const std::string &id) {
	impl_->fire("block_placed", Data::array({p.x, p.y, p.z, id}));
}

void Mods::block_broken(const BlockPos &p, const std::string &id) {
	impl_->fire("block_broken", Data::array({p.x, p.y, p.z, id}));
}

void Mods::creature_hit(int id, int damage, const float from[3], int attacker) {
	impl_->fire("creature_hit", Data::array({id, damage, from[0], from[1], from[2], attacker}));
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
			if (!fs::exists(dir / "mod.json") || !read_json_file(dir / "mod.json", m, why) || !m.is_object() ||
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
