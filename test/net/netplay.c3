// A player in a multiplayer game without a window: its world and game (Play),
// its Session, and frames of play as the game's loop runs them (in real time:
// the host times mining, movement and request rates by the clock). Players
// are driven like a person would: they stand where they're put (no physics:
// moving is the test's), aim, and click (GameInput).
module gocube::test::netplay;
import gocube::test::play, gocube::net::session, gocube::game, gocube::game::day_cycle, gocube::game::inventory;
import gocube::game::player, gocube::content, gocube::data, gocube::world, gocube::clock, gocube::util::vec;
import std::math, std::io;

const float FLOOR_Y = 100; // the host's stone floor, where everyone stands

struct NetPlay
{
	Play* play;
	Session session;
	DayCycle day;
	GameInput input; // held for every frame (clicks: one frame)
	double tick_time;
	double last;
}

fn Game* NetPlay.game(&self) @inline => &self.play.game;
fn World* NetPlay.world(&self) @inline => &self.play.world;
fn Player* NetPlay.player(&self) @inline => &self.play.player;

fn void NetPlay.free(&self)
{
	self.session.free();
	if (self.play) self.play.free();
	free(self);
}

// The host: a world (seed 4242) with a stone floor at FLOOR_Y around the
// origin, open on `port`; its player stands on it at (0.5, 0.5).
fn NetPlay* host(int port, bool creative = false, String name = "Host")
{
	NetPlay* n = mem::new(NetPlay);
	n.session.init();
	n.day = day_cycle::new();
	n.last = clock::now();
	n.play = play::new(play::setup(creative));
	Play* p = n.play;
	for (int x = -14; x <= 14; x++)
	{
		for (int z = -14; z <= 14; z++)
		{
			p.world.set_block({ x, (int)FLOOR_Y - 1, z }, p.id("core:stone"));
			for (int y = (int)FLOOR_Y; y < (int)FLOOR_Y + 4; y++) p.world.set_block({ x, y, z }, 0);
		}
	}
	p.player.position = { 0.5f, FLOOR_Y, 0.5f };
	p.game.world_spawn = { 0.5f, FLOOR_Y, 0.5f };
	p.game.has_world_spawn = true;
	n.day.time = 0.4; // day
	@pool()
	{
		assert(n.session.host(port, name) == "");
	};
	Data info = data::object();
	info.set_long("seed", 4242);
	Data ids = data::array();
	foreach (id : p.content.block_ids()) ids.push_string(id);
	info.set("block_ids", ids);
	n.session.set_world_info(info);
	n.attach();
	return n;
}

alias OthersFn = fn void(void* context);

// A client: connects, waits for the host's welcome (while `others` run their
// frames), then builds the world from it. Null if it was turned away (`why`
// says why).
fn NetPlay* join(int port, String name, OthersFn others, void* context, DString* why = null, bool udp = true)
{
	NetPlay* n = mem::new(NetPlay);
	n.session.init();
	n.day = day_cycle::new();
	n.last = clock::now();
	n.session.udp_disabled = !udp;
	@pool()
	{
		assert(n.session.join("127.0.0.1", port, name) == "");
	};
	double deadline = clock::now() + 20;
	while (!n.session.welcomed && n.session.ended().len == 0 && clock::now() < deadline)
	{
		n.session.poll();
		others(context);
		clock::sleep_ms(2);
	}
	if (!n.session.welcomed)
	{
		if (why) why.append(n.session.ended());
		n.free();
		return null;
	}
	Data* w = n.session.welcome();
	Vec{String} ids;
	defer ids.free();
	foreach (&id : w.get("block_ids").items()) ids.push(id.s);
	Data* spawn = w.get("spawn");
	Setup s = play::setup(w.str("mode", "survival") == "creative");
	s.seed = (uint)w.num("seed", 0);
	s.block_ids = ids.array_view();
	s.x = spawn.at(0).as_float();
	s.z = spawn.at(2).as_float();
	n.play = play::new(s); // (the host waits meanwhile: it's quick)
	n.play.player.position = { spawn.at(0).as_float(), spawn.at(1).as_float() - player::EYE_HEIGHT, spawn.at(2).as_float() };
	n.play.game.set_world_spawn(w.get("world_spawn"));
	n.day.time = w.num("time", 0.4);
	n.attach();
	return n;
}

fn void NetPlay.attach(&self)
{
	Play* p = self.play;
	self.session.attach({ &p.game, &p.world, &p.player, &p.content, &p.rules, &self.day });
}

// One frame: the network, the world, the game, what others see of us.
fn void NetPlay.frame(&self)
{
	double now = clock::now();
	float dt = math::min(0.05f, (float)(now - self.last));
	self.last = now;
	Play* p = self.play;
	self.session.poll();
	float[3] eye;
	if (self.session.take_correction(&eye)) p.player.position = { eye[0], eye[1] - player::EYE_HEIGHT, eye[2] };
	p.world.update(p.player.position[0], p.player.position[1], p.player.position[2]);
	p.drain_meshes();
	p.game.physics_step(dt);
	self.input.captured = true;
	p.game.update(dt, self.input);
	self.input.break_pressed = self.input.use_pressed = self.input.pick_pressed = false; // (clicks last one frame)
	self.input.throw_pressed = self.input.throw_stack = false;
	self.input.number_key = 0;
	p.game.tick(dt);
	self.day.update(dt);
	p.game.day = self.day.day_amount();
	LocalPose pose = { .eye = p.player.eye(), .yaw = p.player.yaw, .pitch = p.player.pitch, .item = p.game.selected_item(),
		.selected = p.game.selected, .swings = p.game.swings };
	self.session.update(dt, pose);
}

// Looks at a point (from the eyes).
fn void NetPlay.aim(&self, float x, float y, float z)
{
	float[3] eye = self.player().eye();
	float dx = x - eye[0];
	float dy = y - eye[1];
	float dz = z - eye[2];
	self.player().yaw = math::atan2(-dx, -dz);
	self.player().pitch = math::atan2(dy, math::hypot(dx, dz));
}

// The top face of the floor's stone under cell (x, z): where to aim to place on it.
fn void NetPlay.aim_at_floor(&self, int x, int z) => self.aim(x + 0.5f, FLOOR_Y - 0.001f, z + 0.5f);

// Creative: takes `item` from the palette into hotbar slot `slot` (as a
// player does in the inventory screen: the host keeps the inventory too).
fn void NetPlay.take_from_palette(&self, String item, int slot)
{
	Data pick = game::action("pick");
	pick.push_string(item);
	self.game().inventory_action(pick);
	self.game().inventory_action(game::action("click", { slot }));
}

// Puts this item's hotbar slot in hand (the first). False if none.
fn bool NetPlay.hold(&self, String item)
{
	for (int i = 0; i < inventory::HOTBAR; i++)
	{
		if (self.game().inventory.slots[i].id == item)
		{
			self.input.number_key = i + 1;
			return true;
		}
	}
	return false;
}

alias Condition = fn bool(void* context);

// Runs frames of every player until `cond` holds (true) or `seconds` pass.
fn bool run_until(NetPlay*[] players, Condition cond, void* context, float seconds)
{
	double deadline = clock::now() + seconds;
	while (!cond(context))
	{
		if (clock::now() > deadline) return false;
		foreach (p : players) p.frame();
		clock::sleep_ms(4);
	}
	return true;
}

fn void run_for(NetPlay*[] players, float seconds) => run_until(players, fn bool(void* c) => false, null, seconds);
