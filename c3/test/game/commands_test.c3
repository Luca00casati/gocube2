// Chat commands on a fake world: who may run what, the arguments (relative
// coordinates, ids, players), the limits. (tests/test_commands.cpp)
module gocube::test::commands_test;
import gocube::game::commands, gocube::world, gocube::util::vec, gocube::util::map, gocube::util::intern;
import std::io, std::math;

struct Moved
{
	int peer;
	float[3] to;
}

struct Fake (CommandWorld)
{
	Vec{CommandPlayer} people;
	Map{long, String} blocks;
	Vec{Moved} moved;
	Vec{String} given; // "peer item count" (interned)
	Vec{String} told;  // "peer: line"
	Vec{String} said;
	Vec{String} ops_changed;
	double t;
	bool creative_mode;
}

fn void Fake.free(&self)
{
	self.people.free();
	self.blocks.free();
	self.moved.free();
	self.given.free();
	self.told.free();
	self.said.free();
	self.ops_changed.free();
}

fn void Fake.players(&self, Vec{CommandPlayer}* out) @dynamic => out.push_all(self.people.array_view());
fn bool Fake.is_block(&self, String id) @dynamic => id == "core:stone" || id == "core:air";
fn bool Fake.is_item(&self, String id) @dynamic => id == "core:torch";
fn bool Fake.is_creature(&self, String id) @dynamic => id == "core:block_eater";
fn bool Fake.teleport(&self, int peer, float[3] feet) @dynamic
{
	self.moved.push({ peer, feet });
	return true;
}
fn int Fake.give(&self, int peer, String item, int count) @dynamic
{
	self.given.push(intern::intern(string::tformat("%d %s %d", peer, item, count)));
	return count;
}
fn bool Fake.kill(&self, int peer) @dynamic => true;
fn void Fake.set_world_spawn(&self, float[3] feet) @dynamic {}
fn double Fake.time(&self) @dynamic => self.t;
fn void Fake.set_time(&self, double t) @dynamic => self.t = t;
fn bool Fake.creative(&self) @dynamic => self.creative_mode;
fn void Fake.set_creative(&self, bool on) @dynamic => self.creative_mode = on;
fn bool Fake.set_block(&self, BlockPos p, String id) @dynamic
{
	self.blocks[world::key(p.x, p.y, p.z)] = intern::intern(id);
	return true;
}
fn bool Fake.summon(&self, String kind, float[3] at) @dynamic => true;
fn bool Fake.kick(&self, int peer, String why) @dynamic => peer != 0;
fn void Fake.set_op(&self, String name, bool on) @dynamic => self.ops_changed.push(intern::intern(string::tformat("%s%s", name, on ? "+" : "-")));
fn void Fake.tell(&self, int peer, String line) @dynamic => self.told.push(intern::intern(string::tformat("%d: %s", peer, line)));
fn void Fake.say_all(&self, String line) @dynamic => self.said.push(intern::intern(line));
fn uint Fake.seed(&self) @dynamic => 777;

// Runs `line` as `who`: whether an answer has `part` in it.
fn bool says(Commands* c, CommandPlayer who, String line, String part)
{
	Lines out;
	defer out.free();
	c.run(who, line, &out);
	foreach (l : out.lines)
	{
		if (l.contains(part)) return true;
	}
	return false;
}

fn sz answers(Commands* c, CommandPlayer who, String line)
{
	Lines out;
	defer out.free();
	c.run(who, line, &out);
	return out.lines.len();
}

fn void words_quotes() @test => @pool()
{
	String[] w = commands::split("give  Ann \"core:torch\" 5");
	assert(w.len == 4 && w[0] == "give" && w[1] == "Ann" && w[2] == "core:torch" && w[3] == "5");
	w = commands::split("msg Ann \"hello there\"");
	assert(w.len == 3 && w[2] == "hello there");
	assert(commands::split("   ").len == 0);
}

fn void operators_change_others_talk() @test
{
	Fake f = { .t = 0.3 };
	defer f.free();
	CommandPlayer me = { 0, "Host", { 10, 64, 10 }, true };
	CommandPlayer ann = { 2, "Ann", { 0, 70, 0 }, false };
	f.people.push(me);
	f.people.push(ann);
	Commands c = { &f };
	assert(says(&c, ann, "/give Ann core:torch 3", "Only operators"));
	assert(f.given.is_empty());
	assert(says(&c, me, "/give Ann torch 3", "Gave 3 core:torch to Ann")); // ("torch": core's)
	assert(f.given[0] == "2 core:torch 3");
	assert(says(&c, me, "give ann core:torch", "Gave 1")); // (no "/", names in any case, count 1)
	assert(says(&c, me, "/give Ann core:nothing", "No item core:nothing"));
	assert(says(&c, me, "/give Bob core:torch", "No player called Bob"));
	assert(says(&c, me, "/give Ann core:torch many", "Usage"));
	// /help: what each may run.
	assert(says(&c, me, "/help", "/give"));
	assert(!says(&c, ann, "/help", "/give"));
	assert(says(&c, ann, "/help", "/msg"));
	assert(says(&c, ann, "/help tp", "moves you"));
	assert(says(&c, ann, "/nonsense", "Unknown command /nonsense"));
	// Talking: /msg to one player, /list, /seed.
	assert(says(&c, ann, "/msg Host hi there", "[you -> Host] hi there"));
	assert(f.told.len() == 1 && f.told[0] == "0: [Ann -> you] hi there");
	assert(says(&c, ann, "/list", "2 playing: Host (op), Ann"));
	assert(says(&c, ann, "/seed", "777"));
	// /op and /deop.
	assert(says(&c, me, "/op Ann", "operator"));
	assert(says(&c, me, "/deop Ann", "operator"));
	assert(f.ops_changed.len() == 2 && f.ops_changed[0] == "Ann+" && f.ops_changed[1] == "Ann-");
	// Completion: what `who` may run.
	Vec{String} names;
	defer names.free();
	c.complete(&me, "/ti", &names);
	assert(names.len() == 1 && names[0] == "time");
	names.clear();
	c.complete(&ann, "g", &names);
	assert(names.is_empty()); // (/give is for operators)
}

fn bool moved_to(Fake* f, int peer, float x, float y, float z)
{
	for (sz i = f.moved.len(); i-- > 0;)
	{
		Moved m = f.moved[i];
		if (m.peer == peer) return math::abs(m.to[0] - x) < 1e-4 && math::abs(m.to[1] - y) < 1e-4 && math::abs(m.to[2] - z) < 1e-4;
	}
	return false;
}

fn void places_blocks_limits() @test
{
	Fake f = { .t = 0.3 };
	defer f.free();
	CommandPlayer me = { 0, "Host", { 10, 64, 10 }, true };
	CommandPlayer ann = { 2, "Ann", { 0, 70, 0 }, false };
	f.people.push(me);
	f.people.push(ann);
	Commands c = { &f };
	answers(&c, me, "/tp ~ ~5 ~-2.5");
	assert(moved_to(&f, 0, 10, 69, 7.5f));
	answers(&c, me, "/tp Ann"); // to Ann
	assert(moved_to(&f, 0, 0, 70, 0));
	answers(&c, me, "/tp Ann Host"); // Ann to us
	assert(moved_to(&f, 2, 10, 64, 10));
	answers(&c, me, "/tp Ann 1 2 3");
	assert(moved_to(&f, 2, 1, 2, 3));
	assert(says(&c, me, "/tp 1 2", "No player called 1")); // (two words: players)
	assert(says(&c, me, "/tp 1 2 3 4 5", "Usage"));
	assert(says(&c, me, "/setblock ~ ~-1 ~ stone", "Put core:stone"));
	assert(f.blocks.get_or(world::key(10, 63, 10), "") == "core:stone");
	assert(says(&c, me, "/setblock 0 0 0 core:lava", "No block core:lava"));
	assert(says(&c, me, "/fill 0 0 0 1 1 1 air", "Filled 8 blocks"));
	assert(says(&c, me, "/fill 0 0 0 100 100 100 stone", "at most"));
	assert(f.blocks.len() == 9); // (nothing of the refused fill)
	assert(says(&c, me, "/time set noon", "0.500"));
	assert(math::abs(f.t - 0.5) < 1e-9);
	assert(says(&c, me, "/time set 2", "Usage"));
	assert(says(&c, me, "/time", "0.500"));
	assert(answers(&c, me, "/say hello") == 0);
	assert(f.said.len() == 1 && f.said[0] == "[Host] hello");
	assert(says(&c, me, "/summon dragon", "No creature core:dragon"));
	assert(says(&c, ann, "/gamemode creative", "Only operators"));
	assert(says(&c, me, "/gamemode", "in survival"));
	answers(&c, me, "/gamemode c");
	assert(f.creative_mode);
	assert(f.said[f.said.len() - 1] == "The game is now in creative (Host).");
	assert(says(&c, me, "/gamemode creative", "already in creative"));
	assert(says(&c, me, "/gamemode adventure", "Usage"));
	answers(&c, me, "/gamemode survival");
	assert(!f.creative_mode);
}
