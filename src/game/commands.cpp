#include "game/commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gc {

namespace {

std::string lower(std::string s) {
	for (char &c : s) {
		c = char(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

bool parse_number(const std::string &s, double &out) {
	if (s.empty()) {
		return false;
	}
	char *end = nullptr;
	out = std::strtod(s.c_str(), &end);
	return end && *end == '\0' && std::isfinite(out);
}

bool parse_int(const std::string &s, int &out) {
	double d;
	if (!parse_number(s, d) || d != std::floor(d) || std::abs(d) > 1e9) {
		return false;
	}
	out = int(d);
	return true;
}

// A coordinate: a number, or ~ (where `from` is) plus an optional number.
bool parse_coord(const std::string &s, float from, float &out) {
	if (!s.empty() && s[0] == '~') {
		double d = 0;
		if (s.size() > 1 && !parse_number(s.substr(1), d)) {
			return false;
		}
		out = from + float(d);
		return true;
	}
	double d;
	if (!parse_number(s, d) || std::abs(d) > 1e7) {
		return false;
	}
	out = float(d);
	return true;
}

// Three coordinates from args[at..at+2].
bool parse_xyz(const std::vector<std::string> &args, size_t at, const float from[3], float out[3]) {
	if (args.size() < at + 3) {
		return false;
	}
	for (int k = 0; k < 3; k++) {
		if (!parse_coord(args[at + size_t(k)], from[k], out[k])) {
			return false;
		}
	}
	return true;
}

BlockPos block_at(const float p[3]) {
	return {int(std::floor(p[0])), int(std::floor(p[1])), int(std::floor(p[2]))};
}

std::string join(const std::vector<std::string> &words, size_t from) {
	std::string out;
	for (size_t i = from; i < words.size(); i++) {
		out += (i > from ? " " : "") + words[i];
	}
	return out;
}

} // namespace

std::vector<std::string> Commands::split(const std::string &line) {
	std::vector<std::string> out;
	std::string word;
	bool quoted = false, any = false;
	for (char c : line) {
		if (c == '"') {
			quoted = !quoted;
			any = true;
		} else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
			if (any) {
				out.push_back(word);
			}
			word.clear();
			any = false;
		} else {
			word += c;
			any = true;
		}
	}
	if (any) {
		out.push_back(word);
	}
	return out;
}

Commands::Commands(CommandWorld world) : w_(std::move(world)) {
	add_builtins();
}

std::string Commands::full_id(const std::string &given) const {
	std::string id = given.find(':') == std::string::npos ? "core:" + given : given;
	if (const size_t hash = id.find('#'); hash != std::string::npos) { // a dye: "stone#FF8800" is "core:stone#ff8800"
		for (size_t i = hash; i < id.size(); i++) {
			id[i] = char(std::tolower((unsigned char)id[i]));
		}
	}
	return id;
}

std::optional<CommandPlayer> Commands::find_player(const CommandPlayer &who, const std::string &name) const {
	if (name == "@s" || name.empty()) {
		return who;
	}
	const std::string want = lower(name);
	for (const CommandPlayer &p : w_.players ? w_.players() : std::vector<CommandPlayer>{}) {
		if (lower(p.name) == want) {
			return p;
		}
	}
	return std::nullopt;
}

std::vector<std::string> Commands::run(const CommandPlayer &who, const std::string &line) {
	std::vector<std::string> out;
	run_into(who, line, out);
	return out;
}

void Commands::run_into(const CommandPlayer &who, const std::string &line, std::vector<std::string> &out) {
	std::string text = line;
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
		text.erase(text.begin());
	}
	if (!text.empty() && text[0] == '/') {
		text.erase(text.begin());
	}
	std::vector<std::string> words = split(text);
	if (words.empty()) {
		return;
	}
	const std::string name = lower(words[0]);
	words.erase(words.begin());
	if (auto it = builtins_.find(name); it != builtins_.end()) {
		if (it->second.op && !who.op) {
			out.push_back("Only operators may use /" + name + ".");
			return;
		}
		it->second.run(who, words, out);
		return;
	}
	for (const ModCommand &m : w_.mod_commands ? w_.mod_commands() : std::vector<ModCommand>{}) {
		if (m.name != name) {
			continue;
		}
		if (m.op && !who.op) {
			out.push_back("Only operators may use /" + name + ".");
			return;
		}
		const std::optional<std::string> reply = w_.run_mod_command ? w_.run_mod_command(name, who, words) : std::nullopt;
		if (!reply) {
			out.push_back("/" + name + " failed (see the log).");
		} else if (!reply->empty()) {
			out.push_back(*reply);
		}
		return;
	}
	out.push_back("Unknown command /" + name + ": /help lists them.");
}

std::vector<std::string> Commands::complete(const CommandPlayer &who, const std::string &typed) const {
	std::string start = lower(typed);
	if (!start.empty() && start[0] == '/') {
		start.erase(start.begin());
	}
	std::vector<std::string> out;
	for (const auto &[name, b] : builtins_) {
		if ((!b.op || who.op) && name.compare(0, start.size(), start) == 0) {
			out.push_back(name);
		}
	}
	for (const ModCommand &m : w_.mod_commands ? w_.mod_commands() : std::vector<ModCommand>{}) {
		if ((!m.op || who.op) && m.name.compare(0, start.size(), start) == 0) {
			out.push_back(m.name);
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

void Commands::add_builtins() {
	auto usage = [this](const std::string &name, std::vector<std::string> &out) {
		out.push_back("Usage: /" + name + " " + builtins_[name].usage);
	};

	builtins_["help"] = {"[command]", "lists the commands, or tells what one does", false,
			[this](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (!args.empty()) {
					const std::string name = lower(args[0][0] == '/' ? args[0].substr(1) : args[0]);
					if (auto it = builtins_.find(name); it != builtins_.end()) {
						out.push_back("/" + name + " " + it->second.usage + ": " + it->second.help);
						return;
					}
					for (const ModCommand &m : w_.mod_commands ? w_.mod_commands() : std::vector<ModCommand>{}) {
						if (m.name == name) {
							out.push_back("/" + name + " " + m.usage + ": " + m.help);
							return;
						}
					}
					out.push_back("No command /" + name + ".");
					return;
				}
				std::string list;
				for (const std::string &name : complete(who, "")) {
					list += (list.empty() ? "/" : ", /") + name;
				}
				out.push_back("Commands: " + list + ". /help NAME tells more.");
			}};

	builtins_["msg"] = {"<player> <text>", "a message only that player sees", false,
			[this](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.size() < 2) {
					return out.push_back("Usage: /msg <player> <text>");
				}
				const auto to = find_player(who, args[0]);
				if (!to) {
					return out.push_back("No player called " + args[0] + ".");
				}
				if (w_.tell) {
					w_.tell(to->peer, "[" + who.name + " -> you] " + join(args, 1));
				}
				out.push_back("[you -> " + to->name + "] " + join(args, 1));
			}};

	builtins_["list"] = {"", "who is playing", false,
			[this](const CommandPlayer &, const std::vector<std::string> &, std::vector<std::string> &out) {
				std::string names;
				const auto all = w_.players ? w_.players() : std::vector<CommandPlayer>{};
				for (const CommandPlayer &p : all) {
					names += (names.empty() ? "" : ", ") + p.name + (p.op ? " (op)" : "");
				}
				out.push_back(std::to_string(all.size()) + " playing: " + names);
			}};

	builtins_["seed"] = {"", "the world's seed", false,
			[this](const CommandPlayer &, const std::vector<std::string> &, std::vector<std::string> &out) {
				out.push_back("Seed: " + std::to_string(w_.seed));
			}};

	builtins_["say"] = {"<text>", "a message to everyone, from you", true,
			[this](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.empty()) {
					return out.push_back("Usage: /say <text>");
				}
				if (w_.say_all) {
					w_.say_all("[" + who.name + "] " + join(args, 0));
				}
			}};

	builtins_["tp"] = {"<x y z> | <player> | <player> <x y z> | <player> <player>",
			"moves you (or a player) to a place or to another player", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				CommandPlayer target = who;
				float to[3];
				if (args.size() == 3 && parse_xyz(args, 0, who.feet, to)) {
				} else if (args.size() == 1 || args.size() == 2) {
					const auto first = find_player(who, args[0]);
					if (!first) {
						return out.push_back("No player called " + args[0] + ".");
					}
					const auto dest = args.size() == 1 ? first : find_player(who, args[1]);
					if (!dest) {
						return out.push_back("No player called " + args[1] + ".");
					}
					if (args.size() == 2) {
						target = *first;
					}
					std::copy(dest->feet, dest->feet + 3, to);
				} else if (args.size() == 4) {
					const auto first = find_player(who, args[0]);
					if (!first) {
						return out.push_back("No player called " + args[0] + ".");
					}
					target = *first;
					if (!parse_xyz(args, 1, target.feet, to)) {
						return usage("tp", out);
					}
				} else {
					return usage("tp", out);
				}
				if (!w_.teleport || !w_.teleport(target.peer, to)) {
					return out.push_back("Couldn't move " + target.name + " there.");
				}
				char where[96];
				std::snprintf(where, sizeof(where), "%.1f, %.1f, %.1f", to[0], to[1], to[2]);
				out.push_back("Moved " + target.name + " to " + where + ".");
			}};

	builtins_["give"] = {"<player> <item> [count]", "puts items in a player's inventory (survival)", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.size() < 2 || args.size() > 3) {
					return usage("give", out);
				}
				const auto to = find_player(who, args[0]);
				if (!to) {
					return out.push_back("No player called " + args[0] + ".");
				}
				const std::string item = full_id(args[1]);
				if (!(w_.is_item && w_.is_item(item)) && !(w_.is_block && w_.is_block(item))) {
					return out.push_back("No item " + item + ".");
				}
				int count = 1;
				if (args.size() == 3 && (!parse_int(args[2], count) || count < 1)) {
					return usage("give", out);
				}
				count = std::min(count, MAX_GIVE);
				const int given = w_.give ? w_.give(to->peer, item, count) : 0;
				if (given <= 0) {
					return out.push_back(to->name + " has no room for " + item + " (or plays in creative).");
				}
				out.push_back("Gave " + std::to_string(given) + " " + item + " to " + to->name + ".");
			}};

	builtins_["time"] = {"[set <day|noon|night|midnight|0..1>]", "the time of day, or sets it", true,
			[this, usage](const CommandPlayer &, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.empty()) {
					char t[48];
					std::snprintf(t, sizeof(t), "Time: %.3f (0.5 is noon)", w_.time ? w_.time() : 0.0);
					return out.push_back(t);
				}
				if (args.size() != 2 || lower(args[0]) != "set") {
					return usage("time", out);
				}
				static const std::map<std::string, double> NAMED = {
						{"day", 0.3}, {"morning", 0.26}, {"noon", 0.5}, {"evening", 0.72}, {"night", 0.85}, {"midnight", 0.0}};
				double t;
				if (auto it = NAMED.find(lower(args[1])); it != NAMED.end()) {
					t = it->second;
				} else if (!parse_number(args[1], t) || t < 0 || t > 1) {
					return usage("time", out);
				}
				if (w_.set_time) {
					w_.set_time(t);
				}
				char s[48];
				std::snprintf(s, sizeof(s), "Time set to %.3f.", t);
				out.push_back(s);
			}};

	builtins_["gamemode"] = {"[survival|creative]", "the world's game mode, or changes it for everyone", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				const bool now = w_.creative && w_.creative();
				if (args.empty()) {
					return out.push_back(std::string("The game is in ") + (now ? "creative." : "survival."));
				}
				const std::string m = lower(args[0]);
				bool creative;
				if (m == "creative" || m == "c" || m == "1") {
					creative = true;
				} else if (m == "survival" || m == "s" || m == "0") {
					creative = false;
				} else {
					return usage("gamemode", out);
				}
				if (args.size() != 1) {
					return usage("gamemode", out);
				}
				if (creative == now) {
					return out.push_back(std::string("The game is already in ") + (now ? "creative." : "survival."));
				}
				if (w_.set_creative) {
					w_.set_creative(creative);
				}
				if (w_.say_all) {
					w_.say_all(std::string("The game is now in ") + (creative ? "creative" : "survival") + " (" + who.name + ").");
				}
			}};

	builtins_["kill"] = {"[player]", "ends a player's life (survival)", true,
			[this](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				const auto target = find_player(who, args.empty() ? "@s" : args[0]);
				if (!target) {
					return out.push_back("No player called " + args[0] + ".");
				}
				if (!w_.kill || !w_.kill(target->peer)) {
					return out.push_back(target->name + " can't die (creative).");
				}
				out.push_back(target->name + " died.");
			}};

	builtins_["spawnpoint"] = {"", "makes where you stand the world's spawn", true,
			[this](const CommandPlayer &who, const std::vector<std::string> &, std::vector<std::string> &out) {
				if (w_.set_world_spawn) {
					w_.set_world_spawn(who.feet);
				}
				out.push_back("The world's spawn is here now.");
			}};

	builtins_["setblock"] = {"<x y z> <block>", "puts a block there (air removes one)", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				float at[3];
				if (args.size() != 4 || !parse_xyz(args, 0, who.feet, at)) {
					return usage("setblock", out);
				}
				const std::string block = full_id(args[3]);
				if (!w_.is_block || !w_.is_block(block)) {
					return out.push_back("No block " + block + ".");
				}
				if (!w_.set_block || !w_.set_block(block_at(at), block)) {
					return out.push_back("Couldn't put it there (not loaded?).");
				}
				out.push_back("Put " + block + " there.");
			}};

	builtins_["fill"] = {"<x1 y1 z1> <x2 y2 z2> <block>", "fills a box with a block (32768 at most)", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				float a[3], b[3];
				if (args.size() != 7 || !parse_xyz(args, 0, who.feet, a) || !parse_xyz(args, 3, who.feet, b)) {
					return usage("fill", out);
				}
				const std::string block = full_id(args[6]);
				if (!w_.is_block || !w_.is_block(block)) {
					return out.push_back("No block " + block + ".");
				}
				const BlockPos p = block_at(a), q = block_at(b);
				const BlockPos lo{std::min(p.x, q.x), std::min(p.y, q.y), std::min(p.z, q.z)};
				const BlockPos hi{std::max(p.x, q.x), std::max(p.y, q.y), std::max(p.z, q.z)};
				const long long volume = (long long)(hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
				if (volume > MAX_FILL) {
					return out.push_back("That's " + std::to_string(volume) + " blocks: " + std::to_string(MAX_FILL) +
										 " at most.");
				}
				int changed = 0;
				for (int y = lo.y; y <= hi.y; y++) {
					for (int z = lo.z; z <= hi.z; z++) {
						for (int x = lo.x; x <= hi.x; x++) {
							changed += w_.set_block && w_.set_block({x, y, z}, block) ? 1 : 0;
						}
					}
				}
				out.push_back("Filled " + std::to_string(changed) + " blocks with " + block + ".");
			}};

	builtins_["summon"] = {"<creature> [x y z]", "a creature, in front of you or there", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.empty() || (args.size() != 1 && args.size() != 4)) {
					return usage("summon", out);
				}
				const std::string kind = full_id(args[0]);
				if (!w_.is_creature || !w_.is_creature(kind)) {
					return out.push_back("No creature " + kind + ".");
				}
				float at[3] = {who.feet[0], who.feet[1], who.feet[2] - 2.0f};
				if (args.size() == 4 && !parse_xyz(args, 1, who.feet, at)) {
					return usage("summon", out);
				}
				if (!w_.summon || !w_.summon(kind, at)) {
					return out.push_back("Couldn't summon it there.");
				}
				out.push_back("Summoned " + kind + ".");
			}};

	builtins_["kick"] = {"<player> [reason]", "sends a player away", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.empty()) {
					return usage("kick", out);
				}
				const auto target = find_player(who, args[0]);
				if (!target) {
					return out.push_back("No player called " + args[0] + ".");
				}
				const std::string why = args.size() > 1 ? join(args, 1) : "Kicked by " + who.name + ".";
				if (!w_.kick || !w_.kick(target->peer, why)) {
					return out.push_back("Couldn't kick " + target->name + " (the host can't be).");
				}
				out.push_back("Kicked " + target->name + ".");
			}};

	for (const bool on : {true, false}) {
		builtins_[on ? "op" : "deop"] = {"<player>", on ? "lets a player change the world" : "takes that away", true,
				[this, on](const CommandPlayer &, const std::vector<std::string> &args, std::vector<std::string> &out) {
					if (args.size() != 1) {
						return out.push_back(std::string("Usage: /") + (on ? "op" : "deop") + " <player>");
					}
					if (w_.set_op) {
						w_.set_op(args[0], on);
					}
					out.push_back(args[0] + (on ? " is an operator now." : " isn't an operator any more."));
				}};
	}

	builtins_["run"] = {"<mod:name>", "runs a mod's command file (functions/name.txt), a command a line", true,
			[this, usage](const CommandPlayer &who, const std::vector<std::string> &args, std::vector<std::string> &out) {
				if (args.size() != 1) {
					return usage("run", out);
				}
				const std::vector<std::string> *lines = w_.function ? w_.function(args[0]) : nullptr;
				if (!lines) {
					return out.push_back("No command file " + args[0] + ".");
				}
				if (depth_ >= MAX_DEPTH) {
					return out.push_back("Command files run inside each other too deep.");
				}
				depth_++;
				for (const std::string &line : *lines) {
					const size_t start = line.find_first_not_of(" \t");
					if (start == std::string::npos || line[start] == '#') {
						continue; // (blank lines and comments)
					}
					run_into(who, line, out);
				}
				depth_--;
			}};
}

} // namespace gc
