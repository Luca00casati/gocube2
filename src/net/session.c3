// Multiplayer over TCP and UDP (net/transport). Everyone plays the same game
// (there are no mods), so the hello checks the protocol only.
//
// The host is an authoritative server that also plays. World generation is
// deterministic, so clients generate terrain themselves from the seed and only
// edits travel over the network:
//   - handshake: the protocol, then the seed, the block table and every edited
//     chunk (the chunk file format)
//   - block changes: a client asks (breaking, a right click), the host checks
//     it and does it as that player (Game.break_block, Game.use), and every
//     change goes to everyone; a client that hasn't loaded that chunk asks for
//     the whole chunk instead
//   - player positions (clients -> host -> everyone, 20 a second) and how each
//     looks (the held item, sneaking, asleep, flying, arm swings)
//   - survival: the host keeps every joined player's inventory (saved in its
//     world by name) and health (falls, lava and drowning from where it says
//     it is, healing: Vitals), and sends them. A client does
//     each inventory click at once and tells the host, which does the same to
//     its copy. A break comes no sooner than the block's mining time after
//     the client said it started mining it
//   - the host's dropped blocks and chests, time of day, sounds and
//     sights; beds and skipping the night
//   - the host checks what clients say: movement (speed, walls), reach,
//     request rates, what they hold
module gocube::net::session;
import gocube::net::transport, gocube::net::wire, gocube::game, gocube::game::player, gocube::game::inventory, gocube::game::vitals;
import gocube::game::figure, gocube::game::commands, gocube::game::blocks, gocube::game::drops, gocube::game::day_cycle;
import gocube::game::rules, gocube::game::block_entities;
import gocube::content, gocube::content::skin, gocube::data, gocube::data::json, gocube::world, gocube::voxel, gocube::maths, gocube::clock, gocube::log;
import gocube::util::vec, gocube::util::map, gocube::util::intern;
import std::math;

alias Message = Vec{char};
alias Messages = Vec{Message};

const int PROTOCOL = 8; // (any change to the messages: one more)
const int DEFAULT_PORT = 24680;
const int MAX_PLAYERS = 8;
const int HOST_ID = 1;
const int FLAG_SNEAK = 1;
const int FLAG_SLEEP = 2;
const int FLAG_FLY = 4;
const float STATE_INTERVAL = 0.05f; // positions: 20 a second
const float WORLD_INTERVAL = 0.1f;  // drops, containers: 10 a second
const float TIME_SYNC_SECONDS = 5.0f;
const float EDIT_REACH = 16.0f;     // from a player's eyes to a block it changes
const float USE_EYE_SLACK = 2.5f;   // a click's eye from where we last had it
const float CONTAINER_REACH = 8.0f;
const double CONNECT_TIMEOUT = 8.0;
const sz NAME_LENGTH = 16;
// Host-side movement check: the fastest a player can go (horizontal, up,
// down), in blocks a second; survival covers sprinting and falling, creative
// sprint-flying.
const float[3] MOVE_SURVIVAL = { 14, 13, 62 };
const float[3] MOVE_CREATIVE = { 28, 28, 62 };
const float MOVE_BURST = 0.75f; // seconds of movement that may arrive at once (lag)
const float MOVE_SLACK = 1.0f;  // blocks always allowed
const float RESPAWN_RADIUS = 2.0f;
const float CORRECT_EVERY = 0.5f;
const float WALL_STEP = 0.1f;
const float WALL_GRACE = 0.15f;
const float ESCAPE_DISTANCE = 1.5f;
// A survival break comes no sooner than this share of the mining time after
// mining started, less MINE_LAG.
const float MINE_SLACK = 0.8f;
const float MINE_LAG = 0.1f;
const sz MAX_CHAT = 256; // bytes a line
const sz MAX_CHUNK_BYTES @private = 256 * 1024;

// Messages. Clients send C_*, the host H_*.
const char C_HELLO @private = 1;     // protocol u16, name
const char C_STATE @private = 2;     // eye, yaw, pitch, item, flags u8, swings u32, selected u8 (unreliable)
const char C_MINING @private = 3;    // block
const char C_BREAK @private = 4;     // block, slot u8
const char C_USE @private = 5;       // eye, dir, slot u8, item, sneak
const char C_INVENTORY @private = 8; // seq u32, action (data)
const char C_OPEN @private = 9;      // block (a container screen)
const char C_CLOSE @private = 10;    // block
const char C_CHUNK @private = 11;    // chunk position
const char C_BED @private = 12;      // block (the bed's head)
const char C_SLEEP @private = 13;    // bool
const char C_RESPAWNED @private = 14;
const char C_SKIN @private = 15;     // bytes (SKIN_BYTES of RGBA)
const char C_CHAT @private = 16;     // text (a line, or a "/" command)
const char C_THROW @private = 17;    // eye, dir, slot u8, item, whole bool (Q)

const char H_WELCOME @private = 64;     // info (data), chunks (u32 count, position + bytes)
const char H_REJECTED @private = 65;    // reason
const char H_PLAYERS @private = 66;     // u16 count, {id u32, name, eye, yaw, pitch, item, flags u8, swings u32} (unreliable)
const char H_BLOCK @private = 67;       // block, id u16
const char H_CHUNK @private = 68;       // chunk position, bytes
const char H_TIME @private = 69;        // clock f64 (days + time), day_length f64
const char H_CORRECT @private = 70;     // eye
const char H_INVENTORY @private = 71;   // done u32, {inventory, cursor} (data)
const char H_HEALTH @private = 72;      // i32
const char H_DROPS @private = 74;       // u16 count, {item, count u16, pos, age f32, turn f32} (unreliable)
const char H_CONTAINER @private = 76;   // block, entity (data; false: none)
const char H_PICKED @private = 77;      // item, count u16
const char H_SOUND @private = 78;       // set, kind, at, dB f32
const char H_EFFECT @private = 79;      // name, at
const char H_WAKE @private = 80;
const char H_SKIN @private = 81;        // id u32, bytes (SKIN_BYTES of RGBA)
const char H_CHAT @private = 82;        // a line to show
const char H_WORLD_SPAWN @private = 83; // feet (a command moved it)
const char H_MODE @private = 84;        // creative u8 (a command changed it)

// What a peer may ask how often: requests of each kind a second, and in a burst.
enum RateKind : char
{
	EDIT,      // breaking and right clicks (creative mines fast)
	THROW,     // Q
	CHUNK,
	CONTAINER,
	OTHER,     // bed, sleeping
	INV,       // clicks in screens
	MINE,
	SKIN,      // a player's skin (sent once, maybe again)
	CHAT,      // lines and commands typed
}

struct Rate
{
	float per_second, burst;
}

const Rate[*] RATES = { { 20, 40 }, { 8, 16 }, { 30, 120 }, { 20, 40 }, { 4, 10 }, { 30, 60 }, { 20, 40 }, { 0.2f, 2 }, { 2, 6 } };

// How the local player looks to others (set every frame).
struct LocalPose
{
	float[3] eye;
	float yaw, pitch;
	String item;   // in hand
	int selected;  // hotbar slot
	int flags;     // FLAG_*
	int swings;    // Game.swings
}

// Another player as we see them: where the host last said, and smoothed
// between its updates for drawing.
struct Avatar
{
	int id;
	String name;   // (interned)
	float[3] eye;  // as sent
	float yaw, pitch;
	String item;   // (interned)
	int flags, swings;
	// Drawn: eye position and facing, speed (from how it moves), the figure.
	float[3] shown;
	float shown_yaw;
	float[3] velocity;
	Figure figure;
	int seen_swings;
}

enum Mode : char
{
	OFFLINE,
	HOST,
	CLIENT,
}

struct PendingBreak
{
	BlockPos pos;
	int slot;
	double at;
}

struct Peer
{
	Connection* conn;
	int id;
	bool joined;
	String name; // (interned)
	// As it says it is (accepted by the movement check).
	float[3] eye;
	float yaw, pitch;
	String item; // (interned)
	int flags, swings, selected;
	// The movement check.
	float[3] budget;
	double time;
	bool stuck;
	float[3] stuck_at;
	double corrected;
	// Its inventory as the host keeps it (both modes), and its health (survival).
	Inventory* inv;
	uint done;   // the last inventory action of its we did
	bool inv_changed;
	Vitals vitals;
	bool has_bed;
	BlockPos bed;
	bool sleeping;
	// Mining: what it started on, when; a break waiting for its mining time.
	bool has_mining;
	BlockPos mining;
	double mining_since, mined_at;
	bool has_waiting;
	PendingBreak waiting;
	double[2][RateKind::values.len] rates; // tokens, last update
	bool[RateKind::values.len] rate_used;
	double flood_logged;
	Map{long, String} viewing; // open containers -> what was last sent (heap)
}

fn float[3] Peer.feet(&self) => { self.eye[0], self.eye[1] - player::EYE_HEIGHT, self.eye[2] };

fn void Peer.free(&self)
{
	if (self.inv) free(self.inv);
	self.viewing.@each(; long k, String s) { free(s.ptr); };
	self.viewing.free();
}

// The game this session plays in (the host: its own world; a client: the one
// built from the welcome, which this makes remote).
struct Attach
{
	Game* game;
	World* world;
	Player* player;
	Content* content;
	BlockRules* rules;
	DayCycle* day;
}

struct Session (Remote, GameEvents, CommandWorld)
{
	Mode mode;
	Transport* net;
	Connection* server; // a client's connection to the host
	bool udp_disabled;
	String name; // (interned)
	int my_id;
	bool connected, welcomed;
	Data welcome_;
	DString ended_;
	double started;
	Attach a;
	Vec{Peer*} peers;
	int next_id;
	Vec{Avatar} avatars;
	Commands commands; // (the host's, or alone: who runs them)
	bool has_commands;
	Vec{String} chat_in; // (heap)
	Vec{String} ops;     // (heap)
	String local_name;   // (interned)
	Vec{char} my_skin;
	bool skin_sent;
	Map{int, char[]} skins; // by player id (the host's own too; heap)
	int skins_changed;
	int player_count_;
	float state_timer, world_timer, time_timer;
	LocalPose last_pose;
	Data saved_players_;
	bool woken;
	bool has_correction;
	float[3] correction;
	uint inventory_seq; // a client: inventory actions sent
	Data world_info;
	Vec{EditedChunk} welcome_chunks;
	Messages held; // a client: what came before the world was built
	BlockChangedFn previous_hook;
	void* previous_hook_context;
}

fn void Session.init(&self)
{
	*self = { .next_id = HOST_ID + 1, .player_count_ = 1, .local_name = "Player", .saved_players_ = data::object(), .world_info = data::object() };
	self.ended_.init(mem);
}

fn void Session.free(&self)
{
	self.leave();
	if (self.net)
	{
		self.net.free();
		free(self.net);
	}
	self.welcome_.free();
	self.ended_.free();
	self.peers.free();
	self.avatars.free();
	foreach (s : self.chat_in) free(s.ptr);
	self.chat_in.free();
	foreach (s : self.ops) free(s.ptr);
	self.ops.free();
	self.my_skin.free();
	self.skins.free();
	self.saved_players_.free();
	self.world_info.free();
	self.welcome_chunks.free();
	self.held.free();
}

fn bool Session.is_host(&self) => self.mode == HOST;
fn bool Session.is_client(&self) => self.mode == CLIENT;
// A client: the host took us in; the world to build (block_ids, seed, mode,
// time, day_length, spawn, world_spawn, id).
fn Data* Session.welcome(&self) => &self.welcome_;
// Why it ended (a client: refused, lost, timed out); "" while it goes on.
fn String Session.ended(&self) => self.ended_.str_view();
// The host: what a client needs to build the world: seed, block_ids (taken over).
fn void Session.set_world_info(&self, Data info)
{
	self.world_info.free();
	self.world_info = info;
}
// Our name when playing alone (in multiplayer: the one host/join got).
fn void Session.set_local_name(&self, String name) => self.local_name = intern::intern(name);

// Operators (they may change the world with commands), by name. The host, or
// whoever plays alone, always is one. Saved with the world.
fn void Session.set_ops(&self, String[] names)
{
	foreach (s : self.ops) free(s.ptr);
	self.ops.clear();
	foreach (n : names) self.ops.push(n.copy(mem));
}

fn bool Session.is_op(&self, String name)
{
	foreach (o : self.ops)
	{
		if (o == name) return true;
	}
	return false;
}

// A player name as others see it (temp): printable, trimmed, NAME_LENGTH at most.
fn String clean_name(String requested)
{
	// Printable UTF-8 only: no control characters (C0, DEL, C1) and no
	// invisible or direction-changing ones.
	DString out;
	out.tinit();
	sz chars = 0;
	for (sz i = 0; i < requested.len && chars < NAME_LENGTH * 4;)
	{
		char c = requested[i];
		int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (len == 0 || i + len > requested.len)
		{
			i++;
			continue; // not UTF-8
		}
		uint u = len == 1 ? c : c & (0xFF >> (len + 1));
		bool valid = true;
		for (int k = 1; k < len; k++)
		{
			char d = requested[i + k];
			valid = valid && (d >> 6) == 2;
			u = (u << 6) | (d & 0x3F);
		}
		bool printable = u >= 32 && u != 127 && !(u >= 0x80 && u < 0xA0) && !(u >= 0x200B && u <= 0x200F) &&
			!(u >= 0x202A && u <= 0x202E) && !(u >= 0x2066 && u <= 0x2069) && !(u >= 0xD800 && u <= 0xDFFF);
		if (valid && printable)
		{
			out.append(requested[i : len]);
			chars++;
		}
		i += valid ? len : 1;
	}
	// Trim, then at most NAME_LENGTH characters, trimmed again.
	String s = out.str_view().trim(" ");
	sz n = 0;
	sz cut = 0;
	for (sz i = 0; i < s.len; n++)
	{
		char c = s[i];
		i += c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
		if (n + 1 == NAME_LENGTH) cut = i;
	}
	if (n > NAME_LENGTH) s = s[:cut];
	return s.trim(" ").tcopy();
}

// --- Setup and teardown -------------------------------------------------------------

fn void Session.make_net(&self)
{
	if (!self.net)
	{
		self.net = mem::new(Transport);
		*self.net = transport::new();
	}
	self.net.set_udp_enabled(!self.udp_disabled);
}

// The host: listens on `port` ("" or what went wrong: temp).
fn String Session.host(&self, int port, String name)
{
	self.leave();
	self.make_net();
	String e = self.net.listen(port, MAX_PLAYERS - 1);
	if (e.len) return e;
	self.mode = HOST;
	String clean = clean_name(name);
	self.name = intern::intern(clean.len ? clean : "Host");
	self.my_id = HOST_ID;
	if (!self.my_skin.is_empty()) self.set_skin_of(HOST_ID, self.my_skin.array_view());
	log::log("Hosting on port %d", port);
	return "";
}

// A client: connects to the host (then poll() until welcomed or ended()).
fn String Session.join(&self, String address, int port, String name)
{
	self.leave();
	self.make_net();
	String e = self.net.connect(address, port);
	if (e.len) return e;
	self.mode = CLIENT;
	String clean = clean_name(name);
	self.name = intern::intern(clean.len ? clean : "Player");
	self.started = clock::now();
	self.ended_.clear();
	return "";
}

fn void Session.free_skins(&self)
{
	self.skins.@each(; int id, char[] s) { free(s.ptr); };
	self.skins.clear();
}

fn void Session.leave(&self)
{
	self.detach();
	if (self.net)
	{
		if (self.server) self.net.disconnect_now(self.server);
		foreach (p : self.peers) self.net.disconnect_now(p.conn);
		self.net.close();
	}
	self.server = null;
	foreach (p : self.peers)
	{
		p.free();
		free(p);
	}
	self.peers.clear();
	self.avatars.clear();
	self.free_skins();
	self.skin_sent = false;
	self.skins_changed++;
	self.mode = OFFLINE;
	self.connected = self.welcomed = false;
	foreach (&c : self.welcome_chunks) c.bytes.free();
	self.welcome_chunks.clear();
	foreach (&h : self.held) h.free();
	self.held.clear();
}

fn void Session.end(&self, String why)
{
	if (self.ended_.len() == 0)
	{
		self.ended_.append(why);
		log::log("Multiplayer: %s", why);
	}
}

fn void on_block_hook(void* context, BlockPos p, BlockId id) @private
{
	Session* self = context;
	if (self.previous_hook) self.previous_hook(self.previous_hook_context, p, id);
	Writer w = wire::new(H_BLOCK);
	defer w.free();
	block_pos(&w, p).u16(id);
	self.to_players(w.view());
}

fn void Session.attach(&self, Attach a)
{
	self.detach();
	self.a = a;
	Game* game = a.game;
	if (!self.is_client())
	{
		self.commands = { self };
		self.has_commands = true;
	}
	if (self.is_client())
	{
		game.remote = self;
		foreach (&c : self.welcome_chunks)
		{
			a.world.set_chunk(c.pos, c.bytes.array_view());
			c.bytes.free();
		}
		self.welcome_chunks.clear();
		Messages held = self.held;
		self.held = {};
		foreach (&m : held)
		{
			self.on_client_message(m.array_view());
			m.free();
		}
		held.free();
		return;
	}
	if (!self.is_host()) return;
	// Every block change goes to every joined player.
	self.previous_hook = a.world.on_block_changed;
	self.previous_hook_context = a.world.on_block_changed_context;
	a.world.on_block_changed = &on_block_hook;
	a.world.on_block_changed_context = self;
	game.events = self;
}

fn void Session.detach(&self)
{
	if (!self.a.game) return;
	self.has_commands = false;
	if (self.is_host())
	{
		self.a.world.on_block_changed = self.previous_hook;
		self.a.world.on_block_changed_context = self.previous_hook_context;
		self.a.world.extra_viewers.clear();
		self.a.game.events = null;
		self.a.game.set_others({});
		Data saved = self.saved_players();
		saved.free();
	}
	self.a.game.remote = null;
	self.previous_hook = null;
	self.previous_hook_context = null;
	self.a = {};
}

// --- Sending ----------------------------------------------------------------------

fn Writer* block_pos(Writer* w, BlockPos p) @private => w.i32(p.x).i32(p.y).i32(p.z);

fn BlockPos read_block(Reader* r) @private
{
	int x = r.i32();
	int y = r.i32();
	int z = r.i32();
	return { x, y, z };
}

fn float distance(float[3] a, float[3] b) @private
	=> math::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));

fn void Session.send(&self, Connection* to, char[] bytes, bool reliable = true)
{
	if (to && self.net) self.net.send(to, bytes, reliable);
}

// Whether positions go by UDP now (the tests).
fn bool Session.udp_ready(&self)
{
	if (!self.net) return false;
	if (self.is_client()) return self.net.udp_ready(self.server);
	foreach (p : self.peers)
	{
		if (p.joined && !self.net.udp_ready(p.conn)) return false;
	}
	return !self.peers.is_empty();
}

fn void Session.send_host(&self, char[] bytes, bool reliable = true)
{
	if (self.is_client() && self.connected) self.send(self.server, bytes, reliable);
}

fn void Session.send_host_message(&self, Writer* w, bool reliable = true)
{
	self.send_host(w.view(), reliable);
	w.free();
}

fn void Session.to_players(&self, char[] bytes, bool reliable = true)
{
	foreach (p : self.peers)
	{
		if (p.joined) self.send(p.conn, bytes, reliable);
	}
}

fn Peer* Session.peer_by_id(&self, int id)
{
	foreach (p : self.peers)
	{
		if (p.id == id && p.joined) return p;
	}
	return null;
}

// --- Receiving --------------------------------------------------------------------

// Network traffic in and out: every frame (a client also before it plays).
fn void Session.poll(&self)
{
	if (!self.net) return;
	Event ev;
	while (self.net && self.net.poll(&ev))
	{
		// (The host: the player on that connection.)
		Peer* from = null;
		foreach (p : self.peers)
		{
			if (p.conn == ev.connection) from = p;
		}
		switch (ev.type)
		{
			case CONNECT:
				ev.data.free();
				if (self.is_client())
				{
					self.connected = true;
					self.server = ev.connection;
					Writer w = wire::new(C_HELLO);
					w.u16((ushort)PROTOCOL).str(self.name);
					self.send_host_message(&w);
				}
				else
				{
					Peer* p = mem::new(Peer);
					*p = { .conn = ev.connection, .id = self.next_id++, .corrected = -1e9, .flood_logged = -1e9 };
					self.peers.push(p);
				}
			case RECEIVE:
				if (self.is_client() && self.welcomed && !self.a.game)
				{
					self.held.push(ev.data); // welcomed, the world still being built: kept for when it's there
					continue;
				}
				if (self.is_client())
				{
					self.on_client_message(ev.data.array_view());
				}
				else if (from)
				{
					self.on_host_message(from, ev.data.array_view());
				}
				ev.data.free();
			case DISCONNECT:
				ev.data.free();
				if (self.is_client())
				{
					self.end(self.connected ? "The server closed the connection." : "Couldn't connect to the server.");
					self.connected = false;
					self.server = null;
				}
				else if (from)
				{
					if (from.joined)
					{
						log::log("%s left", from.name);
						Data saved = self.saved_players(); // (keeps its inventory and health)
						saved.free();
						if (char[] s = self.skins.get_or(from.id, {}))
						{
							free(s.ptr);
							self.skins.remove(from.id);
							self.skins_changed++;
						}
					}
					foreach (i, p : self.peers)
					{
						if (p == from)
						{
							self.peers.remove_at(i);
							break;
						}
					}
					from.free();
					free(from);
					if (self.a.game && self.a.world) self.tell_game_others(); // (the game held its inventory: not a moment longer)
				}
		}
	}
	if (self.is_client() && !self.welcomed && self.ended_.len() == 0 && clock::now() - self.started > CONNECT_TIMEOUT)
	{
		self.end("Connection timed out.");
	}
}

fn void Session.reject(&self, Peer* p, String reason)
{
	log::log("Turned a player away: %s", reason);
	Writer w = wire::new(H_REJECTED);
	defer w.free();
	w.str(reason);
	self.send(p.conn, w.view());
	self.net.disconnect_later(p.conn); // (after the message)
}

fn void Session.hello(&self, Peer* p, char[] data) => @pool()
{
	Reader r = wire::reader(data);
	r.u8();
	int protocol = r.u16();
	String requested = r.str(256);
	if (!r.ok())
	{
		self.reject(p, "Bad hello.");
		return;
	}
	if (protocol != PROTOCOL)
	{
		self.reject(p, string::tformat("Game version mismatch (server protocol %d, yours %d).", PROTOCOL, protocol));
		return;
	}
	String name = clean_name(requested);
	if (!name.len) name = string::tformat("Player %d", p.id);
	bool taken = name == self.name;
	foreach (o : self.peers) taken = taken || (o.joined && o.name == name);
	if (taken)
	{
		// Survival inventories are kept by name: one player per name.
		self.reject(p, string::tformat("Someone called %s is already playing here: pick another name.", name));
		return;
	}
	if (!self.a.game)
	{
		self.reject(p, "The server isn't ready yet.");
		return;
	}
	p.name = intern::intern(name);
	p.joined = true;
	// It appears next to us.
	float[3] eye = self.a.player.eye();
	p.eye = { eye[0] + 2, eye[1], eye[2] + 2 };
	p.time = clock::now();
	float[3] speeds = self.a.game.creative ? MOVE_CREATIVE : MOVE_SURVIVAL;
	for (int k = 0; k < 3; k++) p.budget[k] = speeds[k] * MOVE_BURST + MOVE_SLACK;
	self.load_player(p);
	Data info = self.world_info.copy();
	defer info.free();
	info.set_long("id", p.id);
	info.set_string("mode", self.a.game.creative ? "creative" : "survival");
	info.set_double("time", self.a.day ? self.a.day.clock() : 0.3); // (days + time)
	info.set_double("day_length", self.a.day ? self.a.day.day_length : day_cycle::DEFAULT_DAY_LENGTH);
	info.set("spawn", float3_data(p.eye));
	if (self.a.game.has_world_spawn) info.set("world_spawn", float3_data(self.a.game.world_spawn));
	Writer w = wire::new(H_WELCOME);
	defer w.free();
	w.data(&info);
	Vec{EditedChunk} chunks;
	defer
	{
		foreach (&c : chunks) c.bytes.free();
		chunks.free();
	}
	self.a.world.edited_chunks(&chunks);
	w.u32((uint)chunks.len());
	foreach (&c : chunks) w.i32(c.pos.x).i32(c.pos.y).i32(c.pos.z).blob(c.bytes.array_view());
	self.send(p.conn, w.view());
	self.skins.@each(; int id, char[] skin) // (everyone's skin, ours too)
	{
		Writer s = wire::new(H_SKIN);
		s.u32((uint)id).blob(skin);
		self.send(p.conn, s.view());
		s.free();
	};
	log::log("%s joined (player %d)", name, p.id);
}

fn Data float3_data(float[3] v) @private
{
	Data d = data::array();
	foreach (x : v) d.push_double(x);
	return d;
}

fn bool Session.allow(&self, Peer* p, RateKind kind)
{
	Rate rate = RATES[kind.ordinal];
	double t = clock::now();
	double[2]* r = &p.rates[kind.ordinal];
	if (!p.rate_used[kind.ordinal])
	{
		p.rate_used[kind.ordinal] = true;
		*r = { rate.burst, t };
	}
	double tokens = math::min((double)rate.burst, (*r)[0] + (t - (*r)[1]) * rate.per_second);
	if (tokens < 1.0)
	{
		*r = { tokens, t };
		if (t - p.flood_logged > 5.0)
		{
			p.flood_logged = t;
			log::log("%s sends too many %s requests; ignoring some", p.name, kind);
		}
		return false;
	}
	*r = { tokens - 1.0, t };
	return true;
}

fn String Session.held_item(&self, Peer* p) => p.inv ? p.inv.slots[math::clamp(p.selected, 0, inventory::HOTBAR - 1)].id : p.item;

fn bool Session.move_allowed(&self, Peer* p, float[3] eye, int flags)
{
	double t = clock::now();
	float[3] speeds = self.a.game.creative ? MOVE_CREATIVE : MOVE_SURVIVAL;
	float[3] budget;
	float[3] cap;
	for (int k = 0; k < 3; k++)
	{
		cap[k] = speeds[k] * MOVE_BURST + MOVE_SLACK;
		budget[k] = math::min(cap[k], p.budget[k] + speeds[k] * (float)(t - p.time));
	}
	p.time = t;
	float[3] d = { eye[0] - p.eye[0], eye[1] - p.eye[1], eye[2] - p.eye[2] };
	float[3] cost = { math::hypot(d[0], d[2]), math::max(d[1], 0.0f), math::max(-d[1], 0.0f) };
	if (cost[0] <= budget[0] && cost[1] <= budget[1] && cost[2] <= budget[2] &&
		(((p.flags | flags) & FLAG_SLEEP) != 0 || self.way_clear(p.eye, eye, p))) // (into or out of bed: eyes low)
	{
		for (int k = 0; k < 3; k++) p.budget[k] = budget[k] - cost[k];
		return true;
	}
	// Respawning: a jump to the world spawn or its bed is fine.
	bool near_spawn = self.a.game.has_world_spawn && math::hypot(eye[0] - self.a.game.world_spawn[0], eye[2] - self.a.game.world_spawn[2]) <= RESPAWN_RADIUS;
	bool near_bed = p.has_bed && math::hypot(eye[0] - (p.bed.x + 0.5f), eye[2] - (p.bed.z + 0.5f)) <= RESPAWN_RADIUS;
	if (near_spawn || near_bed)
	{
		p.budget = cap;
		p.stuck = false;
		return true;
	}
	p.budget = budget;
	return false;
}

fn bool Session.way_clear(&self, float[3] from, float[3] to, Peer* p)
{
	// A body WALL_GRACE thinner on every side, followed in WALL_STEP steps: a
	// corner cut between two updates passes, going through a block doesn't.
	float lift = player::EYE_HEIGHT - WALL_GRACE;
	float[3] body = { player::RADIUS * 2 - WALL_GRACE * 2, player::HEIGHT - WALL_GRACE * 2, player::RADIUS * 2 - WALL_GRACE * 2 };
	float[3] a = { from[0], from[1] - lift, from[2] };
	float[3] b = { to[0], to[1] - lift, to[2] };
	// Where we haven't loaded the chunks we can't tell: no.
	int[3] c0 = { maths::floor_div((int)math::floor(math::min(a[0], b[0]) - 1), voxel::CHUNK),
		maths::floor_div((int)math::floor(math::min(a[1], b[1]) - 1), voxel::CHUNK),
		maths::floor_div((int)math::floor(math::min(a[2], b[2]) - 1), voxel::CHUNK) };
	int[3] c1 = { maths::floor_div((int)math::floor(math::max(a[0], b[0]) + 1), voxel::CHUNK),
		maths::floor_div((int)math::floor(math::max(a[1], b[1]) + player::HEIGHT + 1), voxel::CHUNK),
		maths::floor_div((int)math::floor(math::max(a[2], b[2]) + 1), voxel::CHUNK) };
	if (c1[0] - c0[0] > 4 || c1[2] - c0[2] > 4) return false;
	for (int cx = c0[0]; cx <= c1[0]; cx++)
	{
		for (int cy = math::max(c0[1], world::CHUNK_Y_MIN); cy <= math::min(c1[1], world::CHUNK_Y_MAX); cy++)
		{
			for (int cz = c0[2]; cz <= c1[2]; cz++)
			{
				if (!self.a.world.is_loaded(cx * voxel::CHUNK, cy * voxel::CHUNK, cz * voxel::CHUNK)) return false;
			}
		}
	}
	// A body in a block (one appeared there) may get out, within
	// ESCAPE_DISTANCE of where it got stuck; once out the rest must be clear.
	bool stuck = p.stuck;
	float[3] stuck_at = p.stuck_at;
	if (self.a.world.is_box_free(a, body))
	{
		stuck = false;
	}
	else if (!stuck)
	{
		stuck = true;
		stuck_at = from;
	}
	bool escaping = stuck;
	float length = math::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
	int steps = math::max(1, (int)math::ceil(length / WALL_STEP));
	for (int i = 1; i <= steps; i++)
	{
		float f = (float)i / (float)steps;
		float[3] at = { a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f };
		bool clear = self.a.world.is_box_free(at, body);
		if (escaping && clear)
		{
			escaping = false; // out: from here on the way must be clear
		}
		else if (escaping)
		{
			if (distance({ at[0], at[1] + lift, at[2] }, stuck_at) > ESCAPE_DISTANCE) return false;
		}
		else if (!clear)
		{
			return false;
		}
	}
	p.stuck = escaping;
	p.stuck_at = stuck_at;
	return true;
}

fn bool Session.survival(&self) => self.a.game && !self.a.game.creative;

fn void Session.do_break(&self, Peer* p, BlockPos pos, int slot)
{
	if (p.vitals.dead && self.survival()) return;
	if (distance(p.eye, { pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f }) > EDIT_REACH) return;
	int old = self.a.world.block_id_at(pos.x, pos.y, pos.z);
	if (old <= 0 || self.a.rules.is_fluid(old)) return; // nothing there (or not loaded here)
	self.a.game.break_block(pos);
	p.inv_changed = p.inv != null;
}

fn void Session.on_host_message(&self, Peer* p, char[] data) => @pool()
{
	if (data.len == 0 || !self.a.game) return;
	char type = data[0];
	if (type == C_HELLO)
	{
		if (!p.joined) self.hello(p, data);
		return;
	}
	if (!p.joined) return;
	Reader r = wire::reader(data[1..]);
	Game* game = self.a.game;
	bool dead = self.survival() && p.vitals.dead;
	switch (type)
	{
		case C_STATE:
			float[3] eye = r.vec3();
			float yaw = r.f32();
			float pitch = r.f32();
			String item = r.str(128);
			int flags = r.u8() & 7;
			int swings = (int)r.u32();
			int selected = r.u8();
			if (!r.ok()) return;
			if (!self.move_allowed(p, eye, flags))
			{
				if (clock::now() - p.corrected >= CORRECT_EVERY)
				{
					p.corrected = clock::now();
					Writer w = wire::new(H_CORRECT);
					w.vec3(p.eye);
					self.send(p.conn, w.view());
					w.free();
				}
				return;
			}
			p.eye = eye;
			p.yaw = yaw - maths::TAU * math::round(yaw / maths::TAU);
			p.pitch = math::clamp(pitch, -1.5708f, 1.5708f);
			p.selected = math::clamp(selected, 0, inventory::HOTBAR - 1);
			p.flags = flags;
			p.swings = swings;
			// Only a real block (survival: the one in its hand in our copy).
			bool known = self.a.rules.is_placeable(item);
			p.item = known ? intern::intern(item) : "";
			p.item = self.held_item(p);
			if (self.survival()) p.vitals.moved(self.a.world, self.a.rules, p.feet(), (p.flags & (FLAG_FLY | FLAG_SLEEP)) != 0);
		case C_MINING:
			BlockPos pos = read_block(&r);
			if (r.ok() && self.allow(p, MINE))
			{
				// (not before a break still waiting is done: one block at a time)
				p.has_mining = true;
				p.mining = pos;
				p.mining_since = math::max(clock::now(), p.mined_at);
			}
		case C_BREAK:
			BlockPos pos = read_block(&r);
			int slot = r.u8();
			if (!r.ok() || !self.allow(p, EDIT) || dead) return;
			if (self.survival()) // no sooner than its mining time after it started
			{
				bool had = p.has_mining;
				BlockPos m = p.mining;
				p.has_mining = false;
				int old = self.a.world.block_id_at(pos.x, pos.y, pos.z);
				if (!had || !(m == pos) || old <= 0) return;
				double at = p.mining_since + (double)(game.mine_seconds_of(self.a.rules.id_of(old)) * MINE_SLACK - MINE_LAG);
				p.mined_at = math::max(at, clock::now());
				if (at > clock::now())
				{
					p.has_waiting = true;
					p.waiting = { pos, slot, at }; // (done when the time comes)
					return;
				}
			}
			self.do_break(p, pos, slot);
		case C_THROW:
			float[3] eye = r.vec3();
			float[3] dir = r.vec3();
			int slot = r.u8();
			String item = r.str(128);
			bool whole = r.boolean();
			float len = math::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (!r.ok() || !self.allow(p, THROW) || dead || slot >= inventory::HOTBAR || len < 0.9f || len > 1.1f || distance(eye, p.eye) > USE_EYE_SLACK)
			{
				p.inv_changed = p.inv != null; // (its copy as it is)
				return;
			}
			Actor who = { .peer = p.id, .inventory = p.inv, .selected = slot, .item = intern::intern(item) }; // (from our copy)
			game.throw_item(&who, eye, dir, whole);
			p.inv_changed = p.inv != null;
		case C_USE:
			float[3] eye = r.vec3();
			float[3] dir = r.vec3();
			int slot = r.u8();
			String item = r.str(128);
			bool sneak = r.boolean();
			float len = math::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (!r.ok() || !self.allow(p, EDIT) || dead || slot >= inventory::HOTBAR || len < 0.9f || len > 1.1f || distance(eye, p.eye) > USE_EYE_SLACK)
			{
				p.inv_changed = p.inv != null; // (its copy as it is)
				return;
			}
			if (p.inv)
			{
				item = p.inv.slots[slot].id;
			}
			else if (!self.a.rules.is_placeable(item))
			{
				item = "";
			}
			Player body; // where it is, looking where it clicked
			body.position = { eye[0], eye[1] - player::EYE_HEIGHT, eye[2] };
			body.yaw = math::atan2(-dir[0] / len, -dir[2] / len);
			body.pitch = math::asin(math::clamp(dir[1] / len, -1.0f, 1.0f));
			Actor who = { .peer = p.id, .body = &body, .inventory = p.inv, .selected = slot, .item = intern::intern(item) };
			game.use(&who, sneak);
			p.inv_changed = p.inv != null;
		case C_INVENTORY:
			uint seq = r.u32();
			Data action = r.data(512);
			defer action.free();
			if (!r.ok() || !p.inv) return;
			// Every numbered request counts as done, refused or not, so the client
			// knows when our copy has caught up with it.
			p.done = seq;
			p.inv_changed = true;
			if (!self.allow(p, INV) || dead) return;
			if (action.is_array() && action.len() == 5 && action.at(0).as_string() == "container")
			{
				// Only a container it opened.
				long k = world::key(action.at(1).as_int(), action.at(2).as_int(), action.at(3).as_int());
				if (!p.viewing.has(k) || !self.allow(p, CONTAINER)) return;
			}
			game.apply_inventory_action(p.inv, &action, p.feet());
		case C_OPEN:
			BlockPos pos = read_block(&r);
			if (r.ok() && self.allow(p, CONTAINER) && game.entities.at(pos) && distance({ pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f }, p.eye) <= CONTAINER_REACH)
			{
				long k = world::key(pos.x, pos.y, pos.z);
				if (!p.viewing.has(k)) p.viewing[k] = "".copy(mem); // (sent with the next update)
			}
		case C_CLOSE:
			BlockPos pos = read_block(&r);
			long k = world::key(pos.x, pos.y, pos.z);
			if (String* s = p.viewing.get_ref(k))
			{
				free(s.ptr);
				p.viewing.remove(k);
			}
		case C_CHUNK:
			int cx = r.i32();
			int cy = r.i32();
			int cz = r.i32();
			if (r.ok() && self.allow(p, CHUNK))
			{
				Bytes bytes;
				defer bytes.free();
				if (self.a.world.chunk_bytes({ cx, cy, cz }, &bytes))
				{
					Writer w = wire::new(H_CHUNK);
					w.i32(cx).i32(cy).i32(cz).blob(bytes.array_view());
					self.send(p.conn, w.view());
					w.free();
				}
			}
		case C_BED:
			BlockPos pos = read_block(&r);
			if (r.ok() && self.allow(p, OTHER) && distance({ pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f }, p.eye) <= CONTAINER_REACH &&
				self.a.rules.def_of(self.a.world.block_at(pos.x, pos.y, pos.z)).flag("bed"))
			{
				p.has_bed = true; // respawning there is a jump we allow
				p.bed = pos;
			}
		case C_SLEEP:
			bool on = r.boolean();
			if (r.ok() && self.allow(p, OTHER)) p.sleeping = on;
		case C_RESPAWNED:
			if (self.survival() && p.vitals.dead) p.vitals.respawn();
		case C_CHAT:
			String text = r.str(MAX_CHAT);
			if (r.ok() && text.len && self.allow(p, CHAT)) self.handle_chat(p.id, text);
		case C_SKIN:
			char[] skin = r.blob(skin::SKIN_BYTES);
			if (!r.ok() || skin.len != skin::SKIN_BYTES || !self.allow(p, SKIN)) break;
			Writer w = wire::new(H_SKIN);
			defer w.free();
			w.u32((uint)p.id).blob(skin);
			foreach (other : self.peers) // (to the others; it has its own)
			{
				if (other.joined && other != p) self.send(other.conn, w.view());
			}
			self.set_skin_of(p.id, skin);
			self.skins_changed++;
		default:
	}
}

fn void Session.set_skin_of(&self, int id, char[] skin)
{
	if (char[]* old = self.skins.get_ref(id)) free(old.ptr);
	char[] copy = mem::alloc_array(char, skin.len);
	copy[..] = skin[..];
	self.skins[id] = copy;
}

fn void Session.on_client_message(&self, char[] data) => @pool()
{
	if (data.len == 0) return;
	Reader r = wire::reader(data[1..]);
	char type = data[0];
	switch (type)
	{
		case H_REJECTED:
			String reason = r.str();
			self.end(reason.len ? reason : "The server turned us away.");
			return;
		case H_CHAT:
			String line = r.str(MAX_CHAT * 2);
			if (r.ok() && self.chat_in.len() < 200) self.chat_in.push(line.copy(mem));
			return;
		case H_SKIN: // (may come before we play)
			int id = (int)r.u32();
			char[] skin = r.blob(skin::SKIN_BYTES);
			if (r.ok() && skin.len == skin::SKIN_BYTES && id != self.my_id)
			{
				self.set_skin_of(id, skin);
				self.skins_changed++;
			}
			return;
		case H_WELCOME:
			if (self.welcomed) return;
			Data info = r.data(4 << 20);
			uint count = r.u32();
			Vec{EditedChunk} chunks;
			for (uint i = 0; i < count && r.ok(); i++)
			{
				int cx = r.i32();
				int cy = r.i32();
				int cz = r.i32();
				EditedChunk c = { .pos = { cx, cy, cz } };
				c.bytes.push_all(r.blob(MAX_CHUNK_BYTES));
				chunks.push(c);
			}
			if (!r.ok() || !info.is_object() || !info.has("block_ids") || !info.has("seed"))
			{
				info.free();
				foreach (&c : chunks) c.bytes.free();
				chunks.free();
				self.end("The server sent a bad welcome.");
				return;
			}
			self.welcome_.free();
			self.welcome_ = info;
			self.welcome_chunks = chunks;
			self.my_id = (int)self.welcome_.num("id", 0);
			self.welcomed = true;
			return;
		default:
	}
	if (!self.a.game) return; // (not playing yet)
	Game* game = self.a.game;
	switch (type)
	{
		case H_PLAYERS:
			int count = r.u16();
			Vec{Avatar} states;
			defer states.free();
			for (int i = 0; i < count && i < 64 && r.ok(); i++)
			{
				Avatar a = { .id = (int)r.u32() };
				a.name = intern::intern(r.str(64));
				a.eye = r.vec3();
				a.yaw = r.f32();
				a.pitch = r.f32();
				a.item = intern::intern(r.str(128));
				a.flags = r.u8();
				a.swings = (int)r.u32();
				states.push(a);
			}
			if (!r.ok()) return;
			self.player_count_ = (int)states.len();
			for (sz i = 0; i < states.len();)
			{
				if (states[i].id == self.my_id)
				{
					states.remove_at(i);
				}
				else
				{
					i++;
				}
			}
			self.merge_avatars(states.array_view());
		case H_BLOCK:
			BlockPos pos = read_block(&r);
			BlockId id = r.u16();
			if (!r.ok()) return;
			int old = self.a.world.block_id_at(pos.x, pos.y, pos.z);
			if (old < 0 || !self.a.world.set_block(pos, id))
			{
				// Not loaded here: get the whole chunk (else it would come back as
				// the generator makes it).
				Writer w = wire::new(C_CHUNK);
				w.i32(maths::floor_div(pos.x, voxel::CHUNK)).i32(maths::floor_div(pos.y, voxel::CHUNK)).i32(maths::floor_div(pos.z, voxel::CHUNK));
				self.send_host_message(&w);
				return;
			}
			if (old != (int)id) // what it sounds like: breaking, or placing
			{
				float[3] center = { pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f };
				if (id == 0)
				{
					game.play_block_sound(self.a.rules.id_of(old), "break", center);
				}
				else if (!self.a.rules.is_fluid(id))
				{
					game.play_block_sound(self.a.rules.id_of(id), "place", center);
				}
			}
		case H_CHUNK:
			int cx = r.i32();
			int cy = r.i32();
			int cz = r.i32();
			char[] bytes = r.blob(MAX_CHUNK_BYTES);
			if (r.ok()) self.a.world.set_chunk({ cx, cy, cz }, bytes);
		case H_TIME:
			double t = r.f64();
			double length = r.f64();
			if (r.ok() && self.a.day)
			{
				self.a.day.day_length = math::max(10.0, length);
				double diff = t - self.a.day.clock(); // (t: days + time)
				if (math::abs(diff) * self.a.day.day_length > 0.5) self.a.day.set_clock(t); // (seconds)
			}
		case H_CORRECT:
			float[3] eye = r.vec3();
			if (r.ok())
			{
				self.has_correction = true;
				self.correction = eye;
			}
		case H_INVENTORY:
			uint done = r.u32();
			Data got = r.data();
			defer got.free();
			// Shown only when no action of ours is still on its way (it would be
			// undone for a moment).
			if (r.ok() && got.is_object() && done >= self.inventory_seq)
			{
				Inventory* inv = &game.inventory;
				Data none = data::array();
				defer none.free();
				Data* list = got.get("inventory");
				inv.deserialize(list ? list : &none);
				Data* cursor = got.get("cursor");
				if (cursor && cursor.is_array() && cursor.len() == 2 && cursor.at(0).is_string() && cursor.at(1).is_number())
				{
					inv.cursor = inventory::stack(cursor.at(0).s, cursor.at(1).as_int());
				}
			}
		case H_HEALTH:
			int h = r.i32();
			if (r.ok()) game.set_health_from_host(h);
		case H_DROPS:
			int count = r.u16();
			Vec{Drop} drops;
			defer drops.free();
			for (int i = 0; i < count && r.ok(); i++)
			{
				Drop d = { .item = intern::intern(r.str(128)), .count = r.u16() };
				d.pos = r.vec3();
				d.age = r.f32();
				d.turn = r.f32();
				drops.push(d);
			}
			if (r.ok()) game.drops.set_all(drops.array_view());
		case H_CONTAINER:
			BlockPos pos = read_block(&r);
			Data got = r.data();
			defer got.free();
			if (r.ok()) game.entities.set_entity_data(pos, &got);
		case H_PICKED:
			r.str(128); // (the host sends our inventory as it now is)
			r.u16();
			game.play_sound("core:effects", "pickup", self.a.player.eye(), -8.0f);
		case H_SOUND:
			String set = r.str(128);
			String kind = r.str(64);
			float[3] at = r.vec3();
			float db = r.f32();
			if (r.ok()) game.play_sound(set, kind, at, math::clamp(db, -60.0f, 12.0f));
		case H_EFFECT:
			String name = r.str(64);
			float[3] at = r.vec3();
			if (r.ok()) game.add_effect({ intern::intern(name), at });
		case H_WAKE:
			self.woken = true;
		case H_WORLD_SPAWN:
			float[3] at = r.vec3();
			if (r.ok())
			{
				game.world_spawn = at;
				game.has_world_spawn = true;
			}
		case H_MODE:
			char on = r.u8();
			if (r.ok()) game.set_creative(on != 0); // (survival: the inventory follows from the host)
		default:
	}
}

// --- Every frame ------------------------------------------------------------------

// Each frame, after the game's update: states out, the host's checks, avatars smoothed.
fn void Session.update(&self, float dt, LocalPose pose)
{
	self.last_pose = pose;
	if (self.is_host() && self.a.game)
	{
		self.host_update(dt);
	}
	else if (self.is_client() && self.a.game)
	{
		self.client_update(dt, pose);
	}
	self.update_avatars(dt);
	if (self.net) self.net.flush();
}

fn void Session.players_message(&self, LocalPose* pose, Writer* w)
{
	int count = 1;
	foreach (p : self.peers) count += p.joined ? 1 : 0;
	w.u16((ushort)count);
	w.u32(HOST_ID).str(self.name).vec3(pose.eye).f32(pose.yaw).f32(pose.pitch).str(pose.item).u8((char)(pose.flags & 7))
		.u32((uint)pose.swings);
	foreach (p : self.peers)
	{
		if (!p.joined) continue;
		w.u32((uint)p.id).str(p.name).vec3(p.eye).f32(p.yaw).f32(p.pitch).str(p.item).u8((char)p.flags).u32((uint)p.swings);
	}
}

// Other players: chunks stay loaded around them; the game knows where they
// are (drops, placing) and their inventories (picking things up).
fn void Session.tell_game_others(&self)
{
	Vec{OtherPlayer} others;
	defer others.free();
	self.a.world.extra_viewers.clear();
	foreach (p : self.peers)
	{
		if (!p.joined) continue;
		OtherPlayer o = { .peer = p.id, .feet = p.feet(), .alive = !(self.survival() && p.vitals.dead), .inventory = p.inv };
		others.push(o);
		self.a.world.extra_viewers.push(o.feet);
	}
	self.a.game.set_others(others.array_view());
}

fn void Session.host_update(&self, float dt) => @pool()
{
	Game* game = self.a.game;
	self.tell_game_others();
	double t = clock::now();
	foreach (p : self.peers)
	{
		if (!p.joined) continue;
		if (p.has_waiting && t >= p.waiting.at) // a break that waited for its mining time
		{
			p.has_waiting = false;
			self.do_break(p, p.waiting.pos, p.waiting.slot);
		}
		if (!p.inv) continue;
		if (self.survival()) p.vitals.tick(self.a.world, self.a.rules, p.feet(), (p.flags & FLAG_SLEEP) != 0, dt);
		if (p.vitals.changed)
		{
			p.vitals.changed = false;
			Writer w = wire::new(H_HEALTH);
			w.i32(p.vitals.health);
			self.send(p.conn, w.view());
			w.free();
		}
		if (p.inv_changed)
		{
			p.inv_changed = false;
			Data d = data::object();
			defer d.free();
			d.set("inventory", p.inv.serialize());
			if (p.inv.cursor.is_empty())
			{
				d.set_bool("cursor", false);
			}
			else
			{
				Data c = data::array();
				c.push_string(p.inv.cursor.id);
				c.push_long(p.inv.cursor.count);
				d.set("cursor", c);
			}
			Writer w = wire::new(H_INVENTORY);
			w.u32(p.done).data(&d);
			self.send(p.conn, w.view());
			w.free();
		}
	}
	self.time_timer += dt;
	if (self.time_timer >= TIME_SYNC_SECONDS && self.a.day)
	{
		self.time_timer = 0;
		Writer w = wire::new(H_TIME);
		w.f64(self.a.day.clock()).f64(self.a.day.day_length);
		self.to_players(w.view());
		w.free();
	}
	self.world_timer += dt;
	if (self.world_timer >= WORLD_INTERVAL)
	{
		self.world_timer = 0;
		Writer d = wire::new(H_DROPS);
		Drop[] drops = game.drops.drops.array_view();
		sz n = math::min(drops.len, (sz)512);
		d.u16((ushort)n);
		for (sz i = 0; i < n; i++)
		{
			d.str(drops[i].item).u16((ushort)math::clamp(drops[i].count, 0, 65535)).vec3(drops[i].pos).f32(drops[i].age).f32(drops[i].turn);
		}
		self.to_players(d.view(), false);
		d.free();
		// Containers players look at: when they change.
		foreach (p : self.peers)
		{
			p.viewing.@each_ref(; long k, String* sent)
			{
				ChunkPos cp = world::pos_of(k);
				BlockPos pos = { cp.x, cp.y, cp.z };
				Data e = game.entities.entity_data(pos);
				defer e.free();
				String text = e.is_null() ? "" : json::to_string(tmem, &e, true);
				if (text != *sent)
				{
					free(sent.ptr);
					*sent = text.copy(mem);
					Writer w = wire::new(H_CONTAINER);
					if (e.is_null())
					{
						Data no = data::boolean(false);
						block_pos(&w, pos).data(&no);
					}
					else
					{
						block_pos(&w, pos).data(&e);
					}
					self.send(p.conn, w.view());
					w.free();
				}
			};
		}
	}
	self.state_timer += dt;
	if (self.state_timer >= STATE_INTERVAL)
	{
		self.state_timer = 0;
		Vec{Avatar} states;
		defer states.free();
		foreach (p : self.peers)
		{
			if (!p.joined) continue;
			states.push({ .id = p.id, .name = p.name, .eye = p.eye, .yaw = p.yaw, .pitch = p.pitch, .item = p.item, .flags = p.flags,
				.swings = p.swings });
		}
		self.merge_avatars(states.array_view());
		Writer w = wire::new(H_PLAYERS);
		self.players_message(&self.last_pose, &w);
		self.to_players(w.view(), false);
		w.free();
	}
}

// --- Chat and commands -------------------------------------------------------------

// A typed line, cleaned (temp): no control characters, MAX_CHAT bytes at most.
fn String clean_chat(String text) @private
{
	DString out;
	out.tinit();
	foreach (c : text)
	{
		if (c >= 32 && c != 127) out.append_char(c);
	}
	String s = out.str_view().trim_left(" ");
	if (s.len > MAX_CHAT)
	{
		sz n = MAX_CHAT;
		while (n > 0 && (s[n] & 0xc0) == 0x80) n--; // (whole UTF-8 characters)
		s = s[:n];
	}
	return s;
}

// Chat. What the local player typed: a client sends it to the host, the host
// (or a world played alone) handles it: a line starting with "/" is a command
// run as that player, its answer to that player only; anything else goes to
// everyone as "<name> text". The lines to show come out of take_chat().
fn void Session.chat(&self, String typed) => @pool()
{
	String text = clean_chat(typed);
	if (!text.len) return;
	if (self.is_client())
	{
		Writer w = wire::new(C_CHAT);
		w.str(text);
		self.send_host_message(&w);
	}
	else
	{
		self.handle_chat(0, text);
	}
}

// The lines to show since the last call (the caller frees them and the list).
fn Vec{String} Session.take_chat(&self)
{
	Vec{String} out = self.chat_in;
	self.chat_in = {};
	return out;
}

// Command names starting with `typed` (Tab).
fn void Session.complete_command(&self, String typed, Vec{String}* out)
{
	CommandPlayer me = { .op = true };
	Commands c = { self };
	c.complete(&me, typed, out);
}

fn void Session.handle_chat(&self, int peer, String typed) => @pool()
{
	String text = clean_chat(typed);
	if (!text.len || !self.has_commands) return;
	Vec{CommandPlayer} all;
	defer all.free();
	self.players(&all);
	CommandPlayer who;
	foreach (p : all)
	{
		if (p.peer == peer) who = p;
	}
	if (!who.name.len) return; // (gone)
	if (text[0] == '/')
	{
		log::log("%s: %s", who.name, text);
		Lines out;
		defer out.free();
		self.commands.run(who, text, &out);
		foreach (line : out.lines) self.chat_to(peer, line);
	}
	else
	{
		self.chat_all(string::tformat("<%s> %s", who.name, text));
	}
}

fn void Session.chat_to(&self, int peer, String line)
{
	if (peer == 0)
	{
		self.chat_in.push(line.copy(mem));
		return;
	}
	if (Peer* p = self.peer_by_id(peer))
	{
		Writer w = wire::new(H_CHAT);
		w.str(line);
		self.send(p.conn, w.view());
		w.free();
	}
}

fn void Session.chat_all(&self, String line)
{
	self.chat_in.push(line.copy(mem));
	if (self.is_host())
	{
		Writer w = wire::new(H_CHAT);
		w.str(line);
		self.to_players(w.view());
		w.free();
	}
}

// --- CommandWorld: what commands act on (our game and world, and, hosting, the joined players) ---

fn void Session.players(&self, Vec{CommandPlayer}* out) @dynamic
{
	CommandPlayer me = { .name = self.is_host() ? self.name : self.local_name, .op = true };
	if (self.a.player) me.feet = self.a.player.position;
	out.push(me);
	foreach (p : self.peers)
	{
		if (p.joined) out.push({ .peer = p.id, .name = p.name, .feet = p.feet(), .op = self.is_op(p.name) });
	}
}

fn bool Session.is_block(&self, String id) @dynamic => self.a.content && self.a.content.block_index(id) >= 0;
fn bool Session.is_item(&self, String id) @dynamic => self.a.rules && self.a.rules.is_placeable(id);

fn bool Session.teleport(&self, int peer, float[3] feet) @dynamic
{
	if (peer == 0)
	{
		if (!self.a.player) return false;
		self.a.player.position = feet;
		self.a.player.velocity = { 0, 0, 0 };
		return true;
	}
	Peer* p = self.peer_by_id(peer);
	if (!p) return false;
	p.eye = { feet[0], feet[1] + player::EYE_HEIGHT, feet[2] };
	p.stuck = false;
	Writer w = wire::new(H_CORRECT); // (the client goes where we say)
	w.vec3(p.eye);
	self.send(p.conn, w.view());
	w.free();
	return true;
}

fn int Session.give(&self, int peer, String item, int count) @dynamic
{
	Inventory* inv = null;
	if (peer == 0)
	{
		inv = self.a.game ? &self.a.game.inventory : null;
	}
	else if (Peer* p = self.peer_by_id(peer))
	{
		inv = p.inv;
		p.inv_changed = inv != null;
	}
	return inv ? count - inv.add(item, count) : 0;
}

fn bool Session.kill(&self, int peer) @dynamic
{
	if (peer == 0)
	{
		if (!self.a.game || self.a.game.creative || self.a.game.dead) return false;
		self.a.game.damage(1 << 20, "command");
		return true;
	}
	Peer* p = self.peer_by_id(peer);
	if (!p || !self.survival()) return false;
	p.vitals.hurt(1 << 20);
	return true;
}

fn void Session.set_world_spawn(&self, float[3] feet) @dynamic
{
	if (self.a.game)
	{
		self.a.game.world_spawn = feet;
		self.a.game.has_world_spawn = true;
	}
	if (self.is_host())
	{
		Writer w = wire::new(H_WORLD_SPAWN);
		w.vec3(feet);
		self.to_players(w.view());
		w.free();
	}
}

fn bool Session.creative(&self) @dynamic => self.a.game && self.a.game.creative;

fn void Session.set_creative(&self, bool on) @dynamic
{
	if (!self.a.game || self.a.game.creative == on) return;
	self.a.game.set_creative(on); // (inventories stay as they are)
	if (!self.is_host()) return;
	foreach (p : self.peers)
	{
		if (!p.joined) continue;
		p.has_waiting = false;
		p.vitals = vitals::new(); // (full health)
	}
	Writer w = wire::new(H_MODE);
	w.u8(on ? 1 : 0);
	self.to_players(w.view());
	w.free();
}

fn double Session.time(&self) @dynamic => self.a.day ? self.a.day.time : 0.0;

fn void Session.set_time(&self, double t) @dynamic
{
	if (!self.a.day) return;
	self.a.day.set_time(t);
	self.time_timer = TIME_SYNC_SECONDS; // (everyone gets it at once)
}

fn bool Session.set_block(&self, BlockPos pos, String id) @dynamic
{
	int index = self.a.content ? self.a.content.block_index(id) : -1;
	return index >= 0 && self.a.world && self.a.world.set_block(pos, (BlockId)index);
}


fn bool Session.kick(&self, int peer, String why) @dynamic
{
	Peer* p = self.peer_by_id(peer);
	if (!p) return false;
	log::log("Kicked %s: %s", p.name, why);
	self.reject(p, why);
	return true;
}

fn void Session.set_op(&self, String name, bool on) @dynamic
{
	foreach (i, o : self.ops)
	{
		if (o == name)
		{
			free(o.ptr);
			self.ops.remove_at(i);
			break;
		}
	}
	if (on) self.ops.push(name.copy(mem));
}

fn void Session.tell(&self, int peer, String line) @dynamic => self.chat_to(peer, line);
fn void Session.say_all(&self, String line) @dynamic => self.chat_all(line);
fn uint Session.seed(&self) @dynamic => (uint)self.world_info.num("seed", 0);

// --- Skins ---------------------------------------------------------------------------

// Ours (before or while playing; any other size: none): a client sends it to
// the host once welcomed, the host gives it to everyone who joins.
fn void Session.set_skin(&self, char[] rgba)
{
	self.my_skin.clear();
	if (rgba.len == skin::SKIN_BYTES) self.my_skin.push_all(rgba);
	self.skin_sent = false;
	if (self.is_host() && !self.my_skin.is_empty())
	{
		self.set_skin_of(HOST_ID, self.my_skin.array_view());
		Writer w = wire::new(H_SKIN);
		w.u32((uint)HOST_ID).blob(self.my_skin.array_view());
		self.to_players(w.view());
		w.free();
	}
}

// A player's skin, or an empty one.
fn char[] Session.skin_of(&self, int id) => self.skins.get_or(id, {});

fn void Session.client_update(&self, float dt, LocalPose pose)
{
	if (self.welcomed && !self.skin_sent && !self.my_skin.is_empty())
	{
		self.skin_sent = true;
		Writer w = wire::new(C_SKIN);
		w.blob(self.my_skin.array_view());
		self.send_host_message(&w);
	}
	self.state_timer += dt;
	if (self.state_timer >= STATE_INTERVAL && self.welcomed)
	{
		self.state_timer = 0;
		Writer w = wire::new(C_STATE);
		w.vec3(pose.eye).f32(pose.yaw).f32(pose.pitch).str(pose.item).u8((char)(pose.flags & 7)).u32((uint)pose.swings)
			.u8((char)math::clamp(pose.selected, 0, inventory::HOTBAR - 1));
		self.send_host_message(&w, false);
	}
}

fn void Session.merge_avatars(&self, Avatar[] states)
{
	for (sz i = 0; i < self.avatars.len();)
	{
		bool found = false;
		foreach (s : states) found = found || s.id == self.avatars[i].id;
		if (found)
		{
			i++;
		}
		else
		{
			self.avatars.remove_at(i);
		}
	}
	foreach (s : states)
	{
		Avatar* a = null;
		foreach (&x : self.avatars)
		{
			if (x.id == s.id) a = x;
		}
		if (!a)
		{
			Avatar n = s;
			n.shown = s.eye;
			n.shown_yaw = s.yaw;
			n.seen_swings = s.swings;
			n.figure = figure::new();
			self.avatars.push(n);
			continue;
		}
		a.name = s.name;
		a.eye = s.eye;
		a.yaw = s.yaw;
		a.pitch = s.pitch;
		a.item = s.item;
		a.flags = s.flags;
		a.swings = s.swings;
	}
}

fn void Session.update_avatars(&self, float dt)
{
	if (dt <= 0) return;
	foreach (&a : self.avatars)
	{
		// Smooth between the 20 Hz updates; the speed comes from how far the
		// figure moves.
		float t = 1.0f - math::exp(-15.0f * dt);
		float v = 1.0f - math::exp(-10.0f * dt);
		for (int k = 0; k < 3; k++)
		{
			float before = a.shown[k];
			a.shown[k] += (a.eye[k] - a.shown[k]) * t;
			a.velocity[k] += ((a.shown[k] - before) / dt - a.velocity[k]) * v;
		}
		a.shown_yaw = maths::lerp_angle(a.shown_yaw, a.yaw, t);
		a.figure.move_speed = math::hypot(a.velocity[0], a.velocity[2]);
		a.figure.on_floor = (a.flags & FLAG_FLY) == 0 && math::abs(a.velocity[1]) < 1.5f;
		a.figure.sneaking = (a.flags & FLAG_SNEAK) != 0;
		a.figure.lying = (a.flags & FLAG_SLEEP) != 0;
		a.figure.pitch = a.pitch;
		if (a.swings != a.seen_swings)
		{
			a.seen_swings = a.swings;
			a.figure.swing();
		}
		a.figure.update(dt);
	}
}

fn int Session.player_count(&self)
{
	if (self.is_host())
	{
		int n = 1;
		foreach (p : self.peers) n += p.joined ? 1 : 0;
		return n;
	}
	return self.is_client() ? self.player_count_ : 1;
}

// --- Beds and sleeping --------------------------------------------------------------

// A client: we lay down or got up.
fn void Session.set_sleeping(&self, bool on)
{
	Writer w = wire::new(C_SLEEP);
	w.boolean(on);
	self.send_host_message(&w);
}

// The host: every joined player is (true alone).
fn bool Session.peers_sleeping(&self)
{
	foreach (p : self.peers)
	{
		if (p.joined && !p.sleeping) return false;
	}
	return true;
}

// The host: the new time, and everyone up.
fn void Session.night_skipped(&self)
{
	if (!self.is_host()) return;
	foreach (p : self.peers) p.sleeping = false;
	if (self.a.day)
	{
		Writer w = wire::new(H_TIME);
		w.f64(self.a.day.clock()).f64(self.a.day.day_length);
		self.to_players(w.view());
		w.free();
	}
	Writer wake = wire::new(H_WAKE);
	self.to_players(wake.view());
	wake.free();
}

// A client: get up.
fn bool Session.take_woken(&self)
{
	bool w = self.woken;
	self.woken = false;
	return w;
}

// A client: the host put us back (a move it refused); the eye position.
fn bool Session.take_correction(&self, float[3]* eye)
{
	if (!self.has_correction) return false;
	self.has_correction = false;
	*eye = self.correction;
	return true;
}

// We keep its inventory and health (back from last time); a new player gets
// the starting items (survival) or nothing (creative).
fn void Session.load_player(&self, Peer* p)
{
	if (p.inv) free(p.inv);
	p.inv = mem::new(Inventory);
	*p.inv = inventory::new(self.a.content);
	p.vitals = vitals::new();
	Data* saved = self.saved_players_.get(p.name);
	if (saved && saved.is_object() && saved.has("inventory"))
	{
		p.inv.deserialize(saved.get("inventory"));
	}
	if (saved && saved.get("health") && saved.get("health").is_number()) p.vitals.health = math::clamp(saved.int_of("health"), 1, vitals::MAX_HEALTH);
	p.vitals.changed = true;
	p.inv_changed = true;
}

// --- Saved players ------------------------------------------------------------------

// The host: joined players' inventories and health, kept by name in the
// world (world.json's "players"): {name: {inventory, health}}.
fn void Session.set_saved_players(&self, Data* players)
{
	self.saved_players_.free();
	self.saved_players_ = players && players.is_object() ? players.copy() : data::object();
}

// (A copy: free() it.)
fn Data Session.saved_players(&self)
{
	foreach (p : self.peers)
	{
		if (!p.joined || !p.inv) continue;
		// What the mouse holds goes back in first.
		Inventory copy = *p.inv;
		if (!copy.cursor.is_empty())
		{
			copy.add(copy.cursor.id, copy.cursor.count);
			copy.cursor = {};
		}
		Data entry = data::object();
		entry.set("inventory", copy.serialize());
		if (!p.vitals.dead) entry.set_long("health", p.vitals.health);
		self.saved_players_.set(p.name, entry);
	}
	return self.saved_players_.copy();
}

// --- Remote: a client's game asks the host ----------------------------------------------

fn void Session.start_mining(&self, BlockPos p) @dynamic
{
	Writer w = wire::new(C_MINING);
	block_pos(&w, p);
	self.send_host_message(&w);
}

fn void Session.request_break(&self, BlockPos p, int slot) @dynamic
{
	Writer w = wire::new(C_BREAK);
	block_pos(&w, p).u8((char)math::clamp(slot, 0, 255));
	self.send_host_message(&w);
}

fn void Session.request_use(&self, float[3] eye, float[3] dir, int slot, String item, bool sneak) @dynamic
{
	Writer w = wire::new(C_USE);
	w.vec3(eye).vec3(dir).u8((char)math::clamp(slot, 0, 255)).str(item).boolean(sneak);
	self.send_host_message(&w);
}

fn void Session.request_throw(&self, float[3] eye, float[3] dir, int slot, String item, bool whole) @dynamic
{
	Writer w = wire::new(C_THROW);
	w.vec3(eye).vec3(dir).u8((char)math::clamp(slot, 0, 255)).str(item).boolean(whole);
	self.send_host_message(&w);
}

fn void Session.inventory_action(&self, Data action) @dynamic
{
	Writer w = wire::new(C_INVENTORY);
	w.u32(++self.inventory_seq).data(&action);
	action.free();
	self.send_host_message(&w);
}

fn void Session.set_bed(&self, BlockPos head) @dynamic
{
	Writer w = wire::new(C_BED);
	block_pos(&w, head);
	self.send_host_message(&w);
}

fn void Session.respawned(&self) @dynamic
{
	Writer w = wire::new(C_RESPAWNED);
	self.send_host_message(&w);
}

// A client: a container screen opened or closed (its contents come from the host).
fn void Session.open_container(&self, BlockPos p)
{
	Writer w = wire::new(C_OPEN);
	block_pos(&w, p);
	self.send_host_message(&w);
}

fn void Session.close_container(&self, BlockPos p)
{
	Writer w = wire::new(C_CLOSE);
	block_pos(&w, p);
	self.send_host_message(&w);
}

// --- GameEvents: what happens to other players here, and what everyone sees -------------

fn void Session.picked_up(&self, int peer, Stack s) @dynamic
{
	Peer* p = self.peer_by_id(peer);
	if (!p) return;
	p.inv_changed = true;
	Writer w = wire::new(H_PICKED);
	w.str(s.id).u16((ushort)math::clamp(s.count, 0, 65535));
	self.send(p.conn, w.view());
	w.free();
}

fn void Session.shared_sound(&self, SoundEvent* e) @dynamic
{
	Writer w = wire::new(H_SOUND);
	w.str(e.set).str(e.kind).vec3(e.at).f32(e.volume_db);
	self.to_players(w.view());
	w.free();
}

fn void Session.effect(&self, Effect* e) @dynamic
{
	Writer w = wire::new(H_EFFECT);
	w.str(e.name).vec3(e.at);
	self.to_players(w.view());
	w.free();
}
