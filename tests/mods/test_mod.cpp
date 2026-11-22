// Small mods for tests/test_mods.cpp, one DLL each. nob.c compiles this file
// once and links it into every one (test_mods\dlls\NAME.dll), exporting that
// mod's functions under the names the game looks for: test_NAME_load as
// gocube_mod_load, test_NAME_unload as gocube_mod_unload.

#include "mods/mod_sdk.h"

#include <stdexcept>
#include <string>

namespace {

using gc::Data;

// Throws unless `f` throws.
template <class F>
void expect_error(F f, const char *what) {
	bool threw = false;
	try {
		f();
	} catch (const std::exception &) {
		threw = true;
	}
	if (!threw) {
		throw std::runtime_error(std::string("expected an error: ") + what);
	}
}

// What every test mod keeps.
struct TestMod : gc::sdk::Mod {
	int ticks = 0;
	int counted = 0;
	using Mod::Mod;
};

struct Fine : TestMod {
	explicit Fine(const GcApi *api) : TestMod(api) {
		// Registers a block; that's all.
		register_entry("blocks", "ok", {{"color", "#00ff00"}});
	}
};

struct Failing : TestMod {
	explicit Failing(const GcApi *api) : TestMod(api) {
		// Registers things, then fails: all of it is undone.
		register_entry("blocks", "half", {{"color", "#ffffff"}});
		register_entry("items", "thing", Data::object());
		on("tick", [](const Data &) { throw std::runtime_error("a handler of an undone mod ran"); });
		volatile bool fail = true; // (as far as the compiler knows, it might not)
		if (fail) {
			throw std::runtime_error("boom");
		}
	}
};

struct Crasher : TestMod {
	explicit Crasher(const GcApi *api) : TestMod(api) {
		// Registers a block, then crashes (an access violation): caught, undone.
		register_entry("blocks", "wreck", Data::object());
		volatile int *nothing = nullptr;
		*nothing = 1;
	}
};

struct Counter : TestMod {
	explicit Counter(const GcApi *api) : TestMod(api) {
		// A handler that fails once (registering too late), then goes on.
		on("tick", [this](const Data &) {
			if (++ticks == 3) {
				register_entry("blocks", "late", Data::object()); // (too late: an error)
			}
		});
	}
};

struct Flaky : TestMod {
	explicit Flaky(const GcApi *api) : TestMod(api) {
		// A handler that always fails: the mod is turned off after a few.
		on("tick", [](const Data &) { throw std::runtime_error("flaky"); });
	}
};

struct Crashy : TestMod {
	explicit Crashy(const GcApi *api) : TestMod(api) {
		// A handler that crashes: the mod is turned off at once.
		on("tick", [](const Data &) {
			volatile int *nothing = nullptr;
			*nothing = 1;
		});
	}
};

struct Builder : TestMod {
	explicit Builder(const GcApi *api) : TestMod(api) {
		// Sees the world: stone placed in the air gets glass on top; broken blocks grow back as planks.
		on("block_placed", [this](const Data &a) {
			const int x = a[0].get<int>(), y = a[1].get<int>(), z = a[2].get<int>();
			if (a[3] == "core:stone" && block(x, y + 1, z) == "core:air") {
				set_block(x, y + 1, z, "core:glass");
			}
		});
		on("block_broken", [this](const Data &a) {
			call("world.set_block", Data::array({a[0], a[1], a[2], "core:planks"}));
		});
	}
};

struct Greeter : TestMod {
	explicit Greeter(const GcApi *api) : TestMod(api) {
		// Commands, a rule that runs a command, wrapping a rule, and what
		// can't be done while loading.
		add_command("greet", {{"usage", "<name>"}, {"help", "says hello"}, {"op", false}},
				[](const Data &who, const Data &args) {
					return "hello " + (args.empty() ? std::string("?") : args[0].str()) + " from " + who["name"].str() +
							(who.value("op", false) ? " (op)" : "");
				});
		add_command("broken", Data::object(), [](const Data &, const Data &) -> std::string {
			throw std::runtime_error("oops");
		});
		add_command("count", Data::object(), [this](const Data &, const Data &) {
			return std::to_string(++counted); // (this mod's own count: each world's)
		});
		expect_error([&] { add_command("Bad Name", Data::object(), [](const Data &, const Data &) { return ""; }); },
				"a bad command name was taken");
		expect_error([&] { call("world.get_block", Data::array({0, 0, 0})); }, "world.get_block while loading");
		expect_error([&] { random(); }, "random() while loading");
		expect_error([&] { call("no.such.function"); }, "an unknown function");
		expect_error([&] { register_entry("spells", "fire", Data::object()); }, "an unknown section");
		set_rule("try_command", [this](const Data &a) { return call("game.command", a); });
		// Falls hurt twice as much as the base game says.
		set_rule("fall_damage", [this](const Data &a) { return Data(rule_before("fall_damage", a).get<int>() * 2); });
		const Data info = call("mod.info");
		register_entry("blocks", "named_" + info["id"].str(), Data::object());
	}
};

} // namespace

GOCUBE_MOD_FUNCTIONS(Fine, test_fine_load, test_fine_unload)
GOCUBE_MOD_FUNCTIONS(Failing, test_failing_load, test_failing_unload)
GOCUBE_MOD_FUNCTIONS(Crasher, test_crasher_load, test_crasher_unload)
GOCUBE_MOD_FUNCTIONS(Counter, test_counter_load, test_counter_unload)
GOCUBE_MOD_FUNCTIONS(Flaky, test_flaky_load, test_flaky_unload)
GOCUBE_MOD_FUNCTIONS(Crashy, test_crashy_load, test_crashy_unload)
GOCUBE_MOD_FUNCTIONS(Builder, test_builder_load, test_builder_unload)
GOCUBE_MOD_FUNCTIONS(Greeter, test_greeter_load, test_greeter_unload)
