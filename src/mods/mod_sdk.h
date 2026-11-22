#pragma once
// Writing a mod's code in C++ (a DLL, mods/gocube_api.h): derive a class from
// gc::sdk::Mod, register in its constructor, and name it with GOCUBE_MOD.
// Build the DLL with src/data/data.cpp and src/data/json.cpp (the game's data
// value and JSON: data/data.h, data/json.h); nob.c's mod_dll does.
//
//   #include "mods/mod_sdk.h"
//   struct MyMod : gc::sdk::Mod {
//       explicit MyMod(const GcApi *api) : Mod(api) {
//           register_entry("blocks", "ruby_ore", {{"hardness", 3}, {"color", "#c02040"}});
//           on("block_broken", [this](const gc::Data &a) { log("broken: " + a[3].str()); });
//           set_rule("fall_damage", [](const gc::Data &a) { return gc::Data(0); });
//       }
//   };
//   GOCUBE_MOD(MyMod)
//
// Everything the mod keeps belongs in its object: the game may load the DLL
// for two worlds at once. An exception thrown in a handler, rule or command
// is that function failing (the game logs it); thrown from the constructor,
// the mod failing to load (it's undone).

#include "data/data.h"
#include "data/json.h"
#include "mods/gocube_api.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gc::sdk {

class Mod {
public:
	explicit Mod(const GcApi *api) : api_(api) {
		if (!api || api->version < GOCUBE_API_VERSION || api->size < sizeof(GcApi)) {
			throw std::runtime_error("this game's modding API is too old for this mod");
		}
	}
	virtual ~Mod() = default;
	Mod(const Mod &) = delete;
	Mod &operator=(const Mod &) = delete;

	// --- The game's functions (gocube_api.h, mods/mods.h) --------------------------

	// Any of them by name, with a list of arguments: its result (null for
	// nothing). Throws std::runtime_error if it fails.
	Data call(const char *function, const Data &args = Data::array()) const {
		const std::string text = write_json(args.is_null() ? Data::array() : args, true);
		const char *result = nullptr;
		if (api_->call(api_->mod, function, text.c_str(), &result) != 0) {
			throw std::runtime_error(std::string(function) + ": " + (result ? result : "failed"));
		}
		return unwrap(result);
	}

	// The block at a position, "" where nothing is loaded.
	std::string block(int x, int y, int z) const {
		const char *id = api_->get_block(api_->mod, x, y, z);
		return id ? id : "";
	}
	// Whether it changed; throws for an unknown block or when there's no world.
	bool set_block(int x, int y, int z, const std::string &id) const {
		const int r = api_->set_block(api_->mod, x, y, z, id.c_str());
		if (r < 0) {
			throw std::runtime_error("set_block: no block '" + id + "' (or no world yet)");
		}
		return r > 0;
	}

	// Whether block `id` could stand there; throws for an unknown block or when there's no world.
	bool supported(int x, int y, int z, const std::string &id) const {
		const int r = api_->supported(api_->mod, x, y, z, id.c_str());
		if (r < 0) {
			throw std::runtime_error("supported: no block '" + id + "' (or no world yet)");
		}
		return r > 0;
	}

	void log(const std::string &line) const { api_->log(api_->mod, line.c_str()); }

	// The registry: an id without "mod:" gets this mod's. Registering only
	// works while mods load (in the constructor).
	std::string register_entry(const char *section, const std::string &id, const Data &def) const {
		return call("registry.register", Data::array({section, id, def})).str();
	}
	Data get(const char *section, const std::string &id) const { return call("registry.get", Data::array({section, id})); }
	Data all(const char *section) const { return call("registry.all", Data::array({section})); }
	// Those with this field (cheaper than all of them).
	Data with(const char *section, const char *field) const {
		return call("registry.with", Data::array({section, field}));
	}

	// 0..1, only while playing.
	double random() const { return call("game.random").get<double>(); }
	// What the rule this mod replaced gives (null if there was none).
	Data rule_before(const std::string &name, const Data &args = Data::array()) const {
		Data a = Data::array({name});
		for (const Data &v : args) {
			a.push_back(v);
		}
		return call("game.rule_before", a);
	}

	// --- Registering functions (in the constructor) --------------------------------

	// An event handler: args as the event gives them (a list).
	void on(const char *event, std::function<void(const Data &args)> fn) {
		Function *f = keep([fn = std::move(fn)](const Data &a) {
			fn(a);
			return Data();
		});
		check(api_->on(api_->mod, event, &Mod::trampoline, f));
	}
	// A rule the game asks: its result (null for nothing).
	void set_rule(const char *name, std::function<Data(const Data &args)> fn) {
		check(api_->set_rule(api_->mod, name, &Mod::trampoline, keep(std::move(fn))));
	}
	// A chat command /name: `who` {peer, name, x, y, z, op}, `args` its words; it answers a line ("" for nothing).
	void add_command(const char *name, const Data &def,
			std::function<std::string(const Data &who, const Data &args)> fn) {
		Function *f = keep([fn = std::move(fn)](const Data &a) { return Data(fn(a[0], a[1])); });
		const std::string text = write_json(def.is_null() ? Data::object() : def, true);
		check(api_->add_command(api_->mod, name, text.c_str(), &Mod::trampoline, f));
	}

private:
	struct Function {
		std::function<Data(const Data &)> fn;
		std::string out; // (the result handed to the game, kept until the next call)
	};

	const GcApi *api_;
	std::vector<std::unique_ptr<Function>> functions_;

	Function *keep(std::function<Data(const Data &)> fn) {
		functions_.push_back(std::make_unique<Function>(Function{std::move(fn), {}}));
		return functions_.back().get();
	}

	static void check(const char *error) {
		if (error) {
			throw std::runtime_error(error);
		}
	}

	static Data unwrap(const char *text) {
		Data list;
		std::string error;
		if (!text || !parse_json(text, "result", list, error)) {
			return Data();
		}
		return list.is_array() && !list.empty() ? list[0] : Data();
	}

	static int trampoline(void *user, const char *args, const char **result) {
		Function *f = static_cast<Function *>(user);
		try {
			Data a;
			std::string error;
			if (!parse_json(args ? args : "{}", "arguments", a, error)) {
				throw std::runtime_error(error);
			}
			if (!a.is_array()) {
				a = Data::array();
			}
			const Data r = f->fn(a);
			f->out = write_json(r.is_null() ? Data::array() : Data::array(Data::List{r}), true);
			*result = f->out.c_str();
			return 0;
		} catch (const std::exception &e) {
			f->out = e.what();
		} catch (...) {
			f->out = "an unknown exception";
		}
		*result = f->out.c_str();
		return 1;
	}
};

} // namespace gc::sdk

// The load and unload functions for a class made from a const GcApi *, with
// these names (not exported: GOCUBE_MOD exports them; or the linker can, under
// the names the game looks for: /EXPORT:gocube_mod_load=LOAD, which lets one
// object hold several mods, a DLL each).
#define GOCUBE_MOD_FUNCTIONS(Type, load, unload)                                                             \
	extern "C" const char *load(const GcApi *api, void **instance) {                                         \
		static thread_local std::string gocube_error;                                                      \
		try {                                                                                              \
			*instance = new Type(api);                                                                     \
			return nullptr;                                                                                \
		} catch (const std::exception &e) {                                                                \
			gocube_error = e.what();                                                                       \
		} catch (...) {                                                                                    \
			gocube_error = "an unknown exception";                                                         \
		}                                                                                                  \
		return gocube_error.c_str();                                                                       \
	}                                                                                                      \
	extern "C" void unload(void *instance) {                                                               \
		delete static_cast<Type *>(instance);                                                              \
	}

// The DLL's exports, for a class made from a const GcApi *.
#define GOCUBE_MOD(Type)                                                                                     \
	extern "C" __declspec(dllexport) const char *gocube_mod_load(const GcApi *api, void **instance);         \
	extern "C" __declspec(dllexport) void gocube_mod_unload(void *instance);                                 \
	GOCUBE_MOD_FUNCTIONS(Type, gocube_mod_load, gocube_mod_unload)
