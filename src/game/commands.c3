// Chat commands ("/give Ann core:planks 5"). Whoever plays the world runs
// them (the host runs a client's as that player). Each command says who may
// run it (ops change the world; everyone can /help, /msg, /list, /seed),
// reads its arguments (numbers, coordinates with ~ for "from where I am",
// block ids, player names) and answers in the chat. What they act on
// goes through a CommandWorld (the session fills it in from the game; the
// tests with fakes).
module gocube::game::commands;
import gocube::world, gocube::util::vec, std::math, std::sort, std::io;

// A player running or named by a command.
struct CommandPlayer
{
	int peer;      // 0: the local player (single player, the host)
	String name;   // (a view: the caller's)
	float[3] feet;
	bool op;       // may change the world
}

interface CommandWorld
{
	fn void players(Vec{CommandPlayer}* out); // everyone playing (the local one first)
	fn bool is_block(String id);
	fn bool is_item(String id);                    // a block one can hold
	fn bool teleport(int peer, float[3] feet);
	fn int give(int peer, String block, int count); // how many went in
	fn bool kill(int peer);
	fn void set_world_spawn(float[3] feet);
	fn double time();                              // 0..1 (0.5 noon)
	fn void set_time(double t);
	fn bool creative();                            // the world's game mode
	fn void set_creative(bool creative);           // (for everyone playing)
	fn bool set_block(BlockPos p, String id);
	fn bool kick(int peer, String why);
	fn void set_op(String name, bool op);
	fn void tell(int peer, String line);           // a line to one player
	fn void say_all(String line);                  // a line to everyone
	fn uint seed();
}

const int MAX_FILL = 32768; // blocks one /fill may change
const int MAX_GIVE = 64 * 36;

struct Builtin @private
{
	String name, usage, help;
	bool op;
}

// (Sorted by name.)
const Builtin[*] BUILTINS @private = {
	{ "deop", "<player>", "takes that away", true },
	{ "fill", "<x1 y1 z1> <x2 y2 z2> <block>", "fills a box with a block (32768 at most)", true },
	{ "gamemode", "[survival|creative]", "the world's game mode, or changes it for everyone", true },
	{ "give", "<player> <block> [count]", "puts blocks in a player's inventory", true },
	{ "help", "[command]", "lists the commands, or tells what one does", false },
	{ "kick", "<player> [reason]", "sends a player away", true },
	{ "kill", "[player]", "ends a player's life (survival)", true },
	{ "list", "", "who is playing", false },
	{ "msg", "<player> <text>", "a message only that player sees", false },
	{ "op", "<player>", "lets a player change the world", true },
	{ "say", "<text>", "a message to everyone, from you", true },
	{ "seed", "", "the world's seed", false },
	{ "setblock", "<x y z> <block>", "puts a block there (air removes one)", true },
	{ "spawnpoint", "", "makes where you stand the world's spawn", true },
	{ "time", "[set <day|noon|night|midnight|0..1>]", "the time of day, or sets it", true },
	{ "tp", "<x y z> | <player> | <player> <x y z> | <player> <player>", "moves you (or a player) to a place or to another player", true },
};

// The lines a command answers (heap strings: free() the list).
struct Lines
{
	Vec{String} lines;
}

fn void Lines.add(&self, String fmt, args...) @format(1) => self.lines.push(string::format(mem, fmt, ...args));

fn void Lines.free(&self)
{
	foreach (l : self.lines) free(l.ptr);
	self.lines.free();
}

fn String lower(String s) @private => s.to_lower_tcopy();

fn bool parse_number(String s, double* out) @private
{
	if (s.len == 0) return false;
	double? d = s.to_double();
	if (catch d) return false;
	if (math::is_nan(d) || math::is_inf(d)) return false;
	*out = d;
	return true;
}

fn bool parse_int(String s, int* out) @private
{
	double d;
	if (!parse_number(s, &d) || d != math::floor(d) || math::abs(d) > 1e9) return false;
	*out = (int)d;
	return true;
}

// A coordinate: a number, or ~ (where `from` is) plus an optional number.
fn bool parse_coord(String s, float from, float* out) @private
{
	if (s.len > 0 && s[0] == '~')
	{
		double d = 0;
		if (s.len > 1 && !parse_number(s[1..], &d)) return false;
		*out = from + (float)d;
		return true;
	}
	double d;
	if (!parse_number(s, &d) || math::abs(d) > 1e7) return false;
	*out = (float)d;
	return true;
}

// Three coordinates from args[at..at+2].
fn bool parse_xyz(String[] args, sz at, float[3] from, float[3]* out) @private
{
	if (args.len < at + 3) return false;
	for (int k = 0; k < 3; k++)
	{
		if (!parse_coord(args[at + k], from[k], &(*out)[k])) return false;
	}
	return true;
}

fn BlockPos block_of(float[3] p) @private => { (int)math::floor(p[0]), (int)math::floor(p[1]), (int)math::floor(p[2]) };

fn String join(String[] words, sz from) @private
{
	DString out;
	out.tinit();
	for (sz i = from; i < words.len; i++)
	{
		if (i > from) out.append_char(' ');
		out.append(words[i]);
	}
	return out.str_view();
}

// A line's words (temp); "quoted words" stay one.
fn String[] split(String line)
{
	Vec{String} out;
	defer out.free();
	DString word;
	word.tinit();
	bool quoted = false;
	bool has_word = false;
	foreach (c : line)
	{
		if (c == '"')
		{
			quoted = !quoted;
			has_word = true;
		}
		else if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r'))
		{
			if (has_word) out.push(word.tcopy_str());
			word.clear();
			has_word = false;
		}
		else
		{
			word.append_char(c);
			has_word = true;
		}
	}
	if (has_word) out.push(word.tcopy_str());
	String[] result = mem::talloc_array(String, out.len());
	result[..] = out.array_view()[..];
	return result;
}

struct Commands
{
	CommandWorld w;
}

// An id with "core:" if it has no ":" (temp).
fn String full_id(String given) @private => given.contains_char(':') ? given.tcopy() : string::tformat("core:%s", given);

fn bool Commands.find_player(&self, CommandPlayer* who, String name, CommandPlayer* out)
{
	if (name == "@s" || name.len == 0)
	{
		*out = *who;
		return true;
	}
	String want = lower(name);
	Vec{CommandPlayer} all;
	defer all.free();
	self.w.players(&all);
	foreach (p : all)
	{
		if (lower(p.name) == want)
		{
			*out = p;
			return true;
		}
	}
	return false;
}

// Runs a line ("/" optional) as `who`: what it answers (for `who` only) goes
// into `out`.
fn void Commands.run(&self, CommandPlayer who, String line, Lines* out) => @pool()
{
	String text = line.trim();
	if (text.len && text[0] == '/') text = text[1..];
	String[] words = split(text);
	if (words.len == 0) return;
	String name = lower(words[0]);
	String[] args = words[1..];
	foreach (b : BUILTINS)
	{
		if (b.name != name) continue;
		if (b.op && !who.op)
		{
			out.add("Only operators may use /%s.", name);
			return;
		}
		self.builtin(&who, b, args, out);
		return;
	}
	out.add("Unknown command /%s: /help lists them.", name);
}

// The commands `who` may run whose names start with `typed` (Tab), sorted (constants).
fn void Commands.complete(&self, CommandPlayer* who, String typed, Vec{String}* out) => @pool()
{
	String start = lower(typed);
	if (start.len && start[0] == '/') start = start[1..];
	foreach (b : BUILTINS)
	{
		if ((!b.op || who.op) && b.name.starts_with(start)) out.push(b.name);
	}
}

fn void usage(Builtin b, Lines* out) @private => out.add("Usage: /%s %s", b.name, b.usage);

fn void Commands.builtin(&self, CommandPlayer* who, Builtin b, String[] args, Lines* out)
{
	switch (b.name)
	{
		case "help":
			if (args.len)
			{
				String name = lower(args[0][0] == '/' ? args[0][1..] : args[0]);
				foreach (c : BUILTINS)
				{
					if (c.name == name)
					{
						out.add("/%s %s: %s", name, c.usage, c.help);
						return;
					}
				}
				out.add("No command /%s.", name);
				return;
			}
			Vec{String} names;
			defer names.free();
			self.complete(who, "", &names);
			DString list;
			list.tinit();
			foreach (i, n : names)
			{
				list.append(i == 0 ? "/" : ", /");
				list.append(n);
			}
			out.add("Commands: %s. /help NAME tells more.", list.str_view());
		case "msg":
			if (args.len < 2)
			{
				out.add("Usage: /msg <player> <text>");
				return;
			}
			CommandPlayer to;
			if (!self.find_player(who, args[0], &to))
			{
				out.add("No player called %s.", args[0]);
				return;
			}
			self.w.tell(to.peer, string::tformat("[%s -> you] %s", who.name, join(args, 1)));
			out.add("[you -> %s] %s", to.name, join(args, 1));
		case "list":
			Vec{CommandPlayer} all;
			defer all.free();
			self.w.players(&all);
			DString names;
			names.tinit();
			foreach (i, p : all)
			{
				if (i > 0) names.append(", ");
				names.append(p.name);
				if (p.op) names.append(" (op)");
			}
			out.add("%d playing: %s", all.len(), names.str_view());
		case "seed":
			out.add("Seed: %d", self.w.seed());
		case "say":
			if (args.len == 0)
			{
				out.add("Usage: /say <text>");
				return;
			}
			self.w.say_all(string::tformat("[%s] %s", who.name, join(args, 0)));
		case "tp":
			CommandPlayer target = *who;
			float[3] to;
			if (args.len == 3 && parse_xyz(args, 0, who.feet, &to))
			{
			}
			else if (args.len == 1 || args.len == 2)
			{
				CommandPlayer first;
				if (!self.find_player(who, args[0], &first))
				{
					out.add("No player called %s.", args[0]);
					return;
				}
				CommandPlayer dest = first;
				if (args.len == 2 && !self.find_player(who, args[1], &dest))
				{
					out.add("No player called %s.", args[1]);
					return;
				}
				if (args.len == 2) target = first;
				to = dest.feet;
			}
			else if (args.len == 4)
			{
				if (!self.find_player(who, args[0], &target))
				{
					out.add("No player called %s.", args[0]);
					return;
				}
				if (!parse_xyz(args, 1, target.feet, &to))
				{
					usage(b, out);
					return;
				}
			}
			else
			{
				usage(b, out);
				return;
			}
			if (!self.w.teleport(target.peer, to))
			{
				out.add("Couldn't move %s there.", target.name);
				return;
			}
			out.add("Moved %s to %.1f, %.1f, %.1f.", target.name, to[0], to[1], to[2]);
		case "give":
			if (args.len < 2 || args.len > 3)
			{
				usage(b, out);
				return;
			}
			CommandPlayer to;
			if (!self.find_player(who, args[0], &to))
			{
				out.add("No player called %s.", args[0]);
				return;
			}
			String item = full_id(args[1]);
			if (!self.w.is_item(item))
			{
				out.add("No block %s.", item);
				return;
			}
			int count = 1;
			if (args.len == 3 && (!parse_int(args[2], &count) || count < 1))
			{
				usage(b, out);
				return;
			}
			count = math::min(count, MAX_GIVE);
			int given = self.w.give(to.peer, item, count);
			if (given <= 0)
			{
				out.add("%s has no room for %s.", to.name, item);
				return;
			}
			out.add("Gave %d %s to %s.", given, item, to.name);
		case "time":
			if (args.len == 0)
			{
				out.add("Time: %.3f (0.5 is noon)", self.w.time());
				return;
			}
			if (args.len != 2 || lower(args[0]) != "set")
			{
				usage(b, out);
				return;
			}
			double t;
			switch (lower(args[1]))
			{
				case "day": t = 0.3;
				case "morning": t = 0.26;
				case "noon": t = 0.5;
				case "evening": t = 0.72;
				case "night": t = 0.85;
				case "midnight": t = 0.0;
				default:
					if (!parse_number(args[1], &t) || t < 0 || t > 1)
					{
						usage(b, out);
						return;
					}
			}
			self.w.set_time(t);
			out.add("Time set to %.3f.", t);
		case "gamemode":
			bool now = self.w.creative();
			if (args.len == 0)
			{
				out.add("The game is in %s.", now ? "creative" : "survival");
				return;
			}
			bool creative;
			switch (lower(args[0]))
			{
				case "creative":
				case "c":
				case "1":
					creative = true;
				case "survival":
				case "s":
				case "0":
					creative = false;
				default:
					usage(b, out);
					return;
			}
			if (args.len != 1)
			{
				usage(b, out);
				return;
			}
			if (creative == now)
			{
				out.add("The game is already in %s.", now ? "creative" : "survival");
				return;
			}
			self.w.set_creative(creative);
			self.w.say_all(string::tformat("The game is now in %s (%s).", creative ? "creative" : "survival", who.name));
		case "kill":
			CommandPlayer target;
			if (!self.find_player(who, args.len == 0 ? "@s" : args[0], &target))
			{
				out.add("No player called %s.", args[0]);
				return;
			}
			if (!self.w.kill(target.peer))
			{
				out.add("%s can't die (creative).", target.name);
				return;
			}
			out.add("%s died.", target.name);
		case "spawnpoint":
			self.w.set_world_spawn(who.feet);
			out.add("The world's spawn is here now.");
		case "setblock":
			float[3] at;
			if (args.len != 4 || !parse_xyz(args, 0, who.feet, &at))
			{
				usage(b, out);
				return;
			}
			String block = full_id(args[3]);
			if (!self.w.is_block(block))
			{
				out.add("No block %s.", block);
				return;
			}
			if (!self.w.set_block(block_of(at), block))
			{
				out.add("Couldn't put it there (not loaded?).");
				return;
			}
			out.add("Put %s there.", block);
		case "fill":
			float[3] a;
			float[3] c;
			if (args.len != 7 || !parse_xyz(args, 0, who.feet, &a) || !parse_xyz(args, 3, who.feet, &c))
			{
				usage(b, out);
				return;
			}
			String block = full_id(args[6]);
			if (!self.w.is_block(block))
			{
				out.add("No block %s.", block);
				return;
			}
			BlockPos p = block_of(a);
			BlockPos q = block_of(c);
			BlockPos lo = { math::min(p.x, q.x), math::min(p.y, q.y), math::min(p.z, q.z) };
			BlockPos hi = { math::max(p.x, q.x), math::max(p.y, q.y), math::max(p.z, q.z) };
			long volume = (long)(hi.x - lo.x + 1) * (long)(hi.y - lo.y + 1) * (long)(hi.z - lo.z + 1);
			if (volume > MAX_FILL)
			{
				out.add("That's %d blocks: %d at most.", volume, MAX_FILL);
				return;
			}
			int changed = 0;
			for (int y = lo.y; y <= hi.y; y++)
			{
				for (int z = lo.z; z <= hi.z; z++)
				{
					for (int x = lo.x; x <= hi.x; x++) changed += self.w.set_block({ x, y, z }, block) ? 1 : 0;
				}
			}
			out.add("Filled %d blocks with %s.", changed, block);
		case "kick":
			if (args.len == 0)
			{
				usage(b, out);
				return;
			}
			CommandPlayer target;
			if (!self.find_player(who, args[0], &target))
			{
				out.add("No player called %s.", args[0]);
				return;
			}
			String why = args.len > 1 ? join(args, 1) : string::tformat("Kicked by %s.", who.name);
			if (!self.w.kick(target.peer, why))
			{
				out.add("Couldn't kick %s (the host can't be).", target.name);
				return;
			}
			out.add("Kicked %s.", target.name);
		case "op":
		case "deop":
			bool on = b.name == "op";
			if (args.len != 1)
			{
				out.add("Usage: /%s <player>", b.name);
				return;
			}
			self.w.set_op(args[0], on);
			out.add("%s%s", args[0], on ? " is an operator now." : " isn't an operator any more.");
		default:
			out.add("Unknown command /%s: /help lists them.", b.name);
	}
}
