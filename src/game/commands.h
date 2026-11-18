#pragma once
// Chat commands ("/give Ann core:torch 5"): the game's own and mods' (Lua's
// commands.register, and mods' command files: functions/*.txt, run with
// /run mod:name). Whoever plays the world runs them (the host runs a
// client's as that player). Each command says who may run it (ops change
// the world; everyone can /help, /msg, /list, /seed), reads its arguments
// (numbers, coordinates with ~ for "from where I am", block and item ids,
// player names) and answers in the chat. What they act on goes through
// CommandWorld (net/session.cpp fills it in from the game; the tests with
// fakes).

#include "world/world.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gc {

// A player running or named by a command.
struct CommandPlayer {
	int peer = 0;             // 0: the local player (single player, the host)
	std::string name;
	float feet[3] = {0, 0, 0};
	bool op = false;          // may change the world
};

// A command a mod added (commands.register).
struct ModCommand {
	std::string name, usage, help;
	bool op = true;
};

struct CommandWorld {
	std::function<std::vector<CommandPlayer>()> players;           // everyone playing (the local one first)
	std::function<bool(const std::string &)> is_block, is_item, is_creature;
	std::function<bool(int peer, const float feet[3])> teleport;
	std::function<int(int peer, const std::string &item, int count)> give; // how many went in
	std::function<bool(int peer)> kill;
	std::function<void(const float feet[3])> set_world_spawn;
	std::function<double()> time;                                   // 0..1 (0.5 noon)
	std::function<void(double)> set_time;
	std::function<bool()> creative;                                 // the world's game mode
	std::function<void(bool creative)> set_creative;                // (for everyone playing)
	std::function<bool(const BlockPos &, const std::string &)> set_block;
	std::function<bool(const std::string &kind, const float at[3])> summon;
	std::function<bool(int peer, const std::string &why)> kick;
	std::function<void(const std::string &name, bool op)> set_op;
	std::function<void(int peer, const std::string &line)> tell;    // a line to one player
	std::function<void(const std::string &line)> say_all;           // a line to everyone
	uint32_t seed = 0;
	// Mods: their commands, running one (what it answers; nullopt if it
	// failed), and their command files (null: none of that name).
	std::function<std::vector<ModCommand>()> mod_commands;
	std::function<std::optional<std::string>(const std::string &name, const CommandPlayer &who,
			const std::vector<std::string> &args)> run_mod_command;
	std::function<const std::vector<std::string> *(const std::string &name)> function;
};

class Commands {
public:
	static constexpr int MAX_FILL = 32768; // blocks one /fill may change
	static constexpr int MAX_DEPTH = 8;    // /run inside /run inside...
	static constexpr int MAX_GIVE = 64 * 36;

	explicit Commands(CommandWorld world);
	// Runs a line ("/" optional) as `who`: what it answers (for `who` only).
	std::vector<std::string> run(const CommandPlayer &who, const std::string &line);
	// The commands `who` may run whose names start with `typed` (Tab), sorted.
	std::vector<std::string> complete(const CommandPlayer &who, const std::string &typed) const;
	// A line's words; "quoted words" stay one.
	static std::vector<std::string> split(const std::string &line);

private:
	struct Builtin {
		std::string usage, help;
		bool op = true;
		std::function<void(const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out)> run;
	};
	CommandWorld w_;
	std::map<std::string, Builtin> builtins_;
	int depth_ = 0;

	void add_builtins();
	void run_into(const CommandPlayer &who, const std::string &line, std::vector<std::string> &out);
	std::optional<CommandPlayer> find_player(const CommandPlayer &who, const std::string &name) const;
	std::string full_id(const std::string &id) const;
};

} // namespace gc
