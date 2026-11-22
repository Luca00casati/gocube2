// Chat commands (game/commands.h) on a fake world: who may run what, the
// arguments (relative coordinates, ids, players), the limits, mods' commands
// and command files.

#include "game/commands.h"

#include "check.h"

#include <map>
#include <string>
#include <vector>

using gc::CommandPlayer;
using gc::Commands;

namespace {

struct Fake {
	std::vector<CommandPlayer> players;
	std::map<std::tuple<int, int, int>, std::string> blocks;
	std::map<int, std::array<float, 3>> moved;
	std::map<int, std::map<std::string, int>> given;
	std::vector<std::pair<int, std::string>> told;
	std::vector<std::string> said, ops_changed;
	double time = 0.3;
	bool creative = false;
	std::map<std::string, std::vector<std::string>> functions;
	std::vector<std::string> mod_calls;

	gc::CommandWorld world() {
		gc::CommandWorld w;
		w.players = [this] { return players; };
		w.is_block = [](const std::string &id) { return id == "core:stone" || id == "core:air"; };
		w.is_item = [](const std::string &id) { return id == "core:torch"; };
		w.is_creature = [](const std::string &id) { return id == "core:block_eater"; };
		w.teleport = [this](int peer, const float f[3]) {
			moved[peer] = {f[0], f[1], f[2]};
			return true;
		};
		w.give = [this](int peer, const std::string &item, int count) {
			given[peer][item] += count;
			return count;
		};
		w.time = [this] { return time; };
		w.set_time = [this](double t) { time = t; };
		w.creative = [this] { return creative; };
		w.set_creative = [this](bool on) { creative = on; };
		w.set_block = [this](const gc::BlockPos &p, const std::string &id) {
			blocks[{p.x, p.y, p.z}] = id;
			return true;
		};
		w.tell = [this](int peer, const std::string &line) { told.push_back({peer, line}); };
		w.say_all = [this](const std::string &line) { said.push_back(line); };
		w.set_op = [this](const std::string &name, bool on) { ops_changed.push_back(name + (on ? "+" : "-")); };
		w.seed = 777;
		w.mod_commands = [] {
			return std::vector<gc::ModCommand>{{"heal", "[player]", "heals", true}, {"wave", "", "waves", false}};
		};
		w.run_mod_command = [this](const std::string &name, const CommandPlayer &who, const std::vector<std::string> &args) {
			mod_calls.push_back(name + " by " + who.name + " " + std::to_string(args.size()));
			return std::optional<std::string>(name == "wave" ? "o/" : "");
		};
		w.function = [this](const std::string &name) -> const std::vector<std::string> * {
			auto it = functions.find(name);
			return it != functions.end() ? &it->second : nullptr;
		};
		return w;
	}
};

bool says(const std::vector<std::string> &out, const std::string &part) {
	for (const std::string &line : out) {
		if (line.find(part) != std::string::npos) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("commands: words, quotes") {
	CHECK(Commands::split("give  Ann \"core:torch\" 5") == std::vector<std::string>{"give", "Ann", "core:torch", "5"});
	CHECK(Commands::split("msg Ann \"hello there\"") == std::vector<std::string>{"msg", "Ann", "hello there"});
	CHECK(Commands::split("   ").empty());
}

TEST_CASE("commands: operators change the world, everyone else only talks") {
	Fake f;
	CommandPlayer me{0, "Host", {10, 64, 10}, true}, ann{2, "Ann", {0, 70, 0}, false};
	f.players = {me, ann};
	Commands c(f.world());

	CHECK(says(c.run(ann, "/give Ann core:torch 3"), "Only operators"));
	CHECK(f.given.empty());
	CHECK(says(c.run(me, "/give Ann torch 3"), "Gave 3 core:torch to Ann")); // ("torch": core's)
	CHECK(f.given[2]["core:torch"] == 3);
	CHECK(says(c.run(me, "give ann core:torch"), "Gave 1")); // (no "/", names in any case, count 1)
	CHECK(says(c.run(me, "/give Ann core:nothing"), "No item core:nothing"));
	CHECK(says(c.run(me, "/give Bob core:torch"), "No player called Bob"));
	CHECK(says(c.run(me, "/give Ann core:torch many"), "Usage"));

	// /help: what each may run.
	CHECK(says(c.run(me, "/help"), "/give"));
	CHECK_FALSE(says(c.run(ann, "/help"), "/give"));
	CHECK(says(c.run(ann, "/help"), "/msg"));
	CHECK(says(c.run(ann, "/help tp"), "moves you"));
	CHECK(says(c.run(ann, "/nonsense"), "Unknown command /nonsense"));
	// Talking: /msg to one player, /list, /seed.
	CHECK(says(c.run(ann, "/msg Host hi there"), "[you -> Host] hi there"));
	REQUIRE(f.told.size() == 1);
	CHECK(f.told[0] == std::pair<int, std::string>{0, "[Ann -> you] hi there"});
	CHECK(says(c.run(ann, "/list"), "2 playing: Host (op), Ann"));
	CHECK(says(c.run(ann, "/seed"), "777"));
	// /op and /deop.
	c.run(me, "/op Ann");
	c.run(me, "/deop Ann");
	CHECK(f.ops_changed == std::vector<std::string>{"Ann+", "Ann-"});
	// Completion: what `who` may run.
	CHECK(c.complete(me, "/ti") == std::vector<std::string>{"time"});
	CHECK(c.complete(ann, "g").empty()); // (/give is for operators)
	CHECK(c.complete(me, "he") == std::vector<std::string>{"heal", "help"});
}

TEST_CASE("commands: places, relative to the player; blocks; the limits") {
	Fake f;
	CommandPlayer me{0, "Host", {10, 64, 10}, true}, ann{2, "Ann", {0, 70, 0}, false};
	f.players = {me, ann};
	Commands c(f.world());

	c.run(me, "/tp ~ ~5 ~-2.5");
	CHECK(f.moved[0] == std::array<float, 3>{10, 69, 7.5f});
	c.run(me, "/tp Ann");                 // to Ann
	CHECK(f.moved[0] == std::array<float, 3>{0, 70, 0});
	c.run(me, "/tp Ann Host");            // Ann to us
	CHECK(f.moved[2] == std::array<float, 3>{10, 64, 10});
	c.run(me, "/tp Ann 1 2 3");
	CHECK(f.moved[2] == std::array<float, 3>{1, 2, 3});
	CHECK(says(c.run(me, "/tp 1 2"), "No player called 1")); // (two words: players)
	CHECK(says(c.run(me, "/tp 1 2 3 4 5"), "Usage"));

	CHECK(says(c.run(me, "/setblock ~ ~-1 ~ stone"), "Put core:stone"));
	CHECK(f.blocks[{10, 63, 10}] == "core:stone");
	CHECK(says(c.run(me, "/setblock 0 0 0 core:lava"), "No block core:lava"));
	CHECK(says(c.run(me, "/fill 0 0 0 1 1 1 air"), "Filled 8 blocks"));
	CHECK(says(c.run(me, "/fill 0 0 0 100 100 100 stone"), "at most"));
	CHECK(f.blocks.size() == 9); // (nothing of the refused fill)

	CHECK(says(c.run(me, "/time set noon"), "0.500"));
	CHECK(f.time == check::Approx(0.5));
	CHECK(says(c.run(me, "/time set 2"), "Usage"));
	CHECK(says(c.run(me, "/time"), "0.500"));
	CHECK(c.run(me, "/say hello").empty());
	CHECK(f.said == std::vector<std::string>{"[Host] hello"});
	CHECK(says(c.run(me, "/summon dragon"), "No creature core:dragon"));

	CHECK(says(c.run(ann, "/gamemode creative"), "Only operators"));
	CHECK(says(c.run(me, "/gamemode"), "in survival"));
	c.run(me, "/gamemode c");
	CHECK(f.creative);
	CHECK(f.said.back() == "The game is now in creative (Host).");
	CHECK(says(c.run(me, "/gamemode creative"), "already in creative"));
	CHECK(says(c.run(me, "/gamemode adventure"), "Usage"));
	c.run(me, "/gamemode survival");
	CHECK_FALSE(f.creative);
}

TEST_CASE("commands: mods' commands and command files") {
	Fake f;
	CommandPlayer me{0, "Host", {0, 0, 0}, true}, ann{2, "Ann", {0, 0, 0}, false};
	f.players = {me, ann};
	f.functions["kit:tower"] = {"# a tower", "", "/setblock 0 0 0 stone", "setblock 0 1 0 stone", "/say built"};
	f.functions["kit:loop"] = {"/run kit:loop"};
	Commands c(f.world());

	// A mod's command: its own op setting; what it answers.
	CHECK(says(c.run(ann, "/heal Ann"), "Only operators"));
	CHECK(c.run(ann, "/wave") == std::vector<std::string>{"o/"});
	CHECK(c.run(me, "/heal Ann").empty()); // ("" says nothing)
	CHECK(f.mod_calls == std::vector<std::string>{"wave by Ann 0", "heal by Host 1"});
	CHECK(says(c.run(ann, "/help heal"), "heals"));

	// A command file: a command a line (comments and blank lines skipped), as whoever runs it.
	CHECK(says(c.run(me, "/run kit:tower"), "Put core:stone"));
	CHECK(f.blocks.size() == 2);
	CHECK(f.said == std::vector<std::string>{"[Host] built"});
	CHECK(says(c.run(ann, "/run kit:tower"), "Only operators"));
	CHECK(says(c.run(me, "/run kit:nothing"), "No command file"));
	// One that runs itself stops.
	CHECK(says(c.run(me, "/run kit:loop"), "too deep"));
}
