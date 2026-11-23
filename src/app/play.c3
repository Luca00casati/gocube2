// Playing a world: loading it (a folder, a new one, or someone else's over
// the network), then a frame at a time until the player quits: input,
// physics, the game, the network, sound, the world's chunks, the UI, drawing.
module gocube::app;
import gocube::renderer, gocube::chunk_renderer, gocube::window, gocube::gui, gocube::screens, gocube::settings_screen, gocube::audio;
import gocube::game, gocube::game::player, gocube::game::blocks, gocube::game::hand, gocube::game::day_cycle, gocube::game::creature_models;
import gocube::game::figure, gocube::game::inventory, gocube::game::drops, gocube::game::block_entities;
import gocube::content, gocube::content::item_models, gocube::content::skin, gocube::content::image, gocube::data, gocube::voxel;
import gocube::world, gocube::world::save, gocube::net::session, gocube::profiler, gocube::mat, gocube::maths, gocube::log, gocube::win;
import gocube::clock, gocube::d3d11, gocube::util::vec, gocube::util::rand, gocube::util::intern;
import std::math, std::io::file;

enum End : char
{
	QUIT,
	MENU,
	FAILED,
}

// What to play: a world folder ("": not saved), and for a new one its seed
// and mode; or someone else's world (join). (Strings: heap.)
struct WorldChoice
{
	String dir;
	uint seed;
	String mode;
	int host_port; // open it to others on this port (0: not)
	String join;   // play in the world of the host at this address
	int join_port;
	String name;
}

fn void WorldChoice.free(&self)
{
	foreach (s : (String[*]){ self.dir, self.mode, self.join, self.name }) if (s.len) free(s.ptr);
	*self = {};
}

fn WorldChoice choice(String dir, uint seed, String mode, String name, int host_port = 0, String join = "", int join_port = session::DEFAULT_PORT)
	=> { dir.copy(mem), seed, mode.copy(mem), host_port, join.copy(mem), join_port, name.copy(mem) };

enum Screen : char
{
	NONE,
	INVENTORY,
	CONTAINER,
	PAUSE,
	SETTINGS,
	CHAT,
}

struct Controls
{
	bool fly_key_was_down, f3_was_down;
	bool details; // F3: the details screen
	bool[10] number_was_down;
	bool[256] key_was_down;
}

// A key that went down since the last frame (while the window has focus).
fn bool Controls.pressed(&self, Window* window, int vk)
{
	bool down = window.key_down(vk);
	bool was = self.key_was_down[vk & 255];
	self.key_was_down[vk & 255] = down;
	return down && !was;
}

// Number keys 1..9 pressed this frame (0: none).
fn int Controls.number_pressed(&self, Window* window)
{
	int pressed = 0;
	for (int k = 1; k <= 9; k++)
	{
		bool down = window.captured && window.key_down('0' + k);
		if (down && !self.number_was_down[k]) pressed = k;
		self.number_was_down[k] = down;
	}
	return pressed;
}

struct Puff
{
	float[3] pos, vel;
	float size, age;
}

const float NIGHT_FIGURE_LIGHT @private = 0.02f;
const int OWN_SKIN @private = chunk_renderer::SKIN_SLOTS - 1;

// Our skin (RGBA, SKIN_BYTES): %APPDATA%\GoCube\skin.png if it's one (not
// with --hidden: tests play with the defaults), else the base game's.
fn void load_skin(bool hidden, String content_dir, Vec{char}* out) => @pool()
{
	Image img;
	Image sk;
	defer
	{
		img.free();
		sk.free();
	}
	String mine = string::tformat("%s/skin.png", user_dir());
	if (!hidden && file::exists(mine))
	{
		if (image::read_png(mine, &img) && skin::to_skin(&img, &sk))
		{
			out.push_all(sk.rgba);
			return;
		}
		log::log("%%APPDATA%%\\GoCube\\skin.png isn't a skin (a 64x64 or 64x32 PNG): the default one instead");
		img.free();
	}
	if (image::read_png(string::tformat("%s/textures/player.png", content_dir), &img) && skin::to_skin(&img, &sk))
	{
		out.push_all(sk.rgba);
		return;
	}
	log::log("The base game's skin (textures/player.png) is missing");
	for (sz i = 0; i < skin::SKIN_BYTES; i++) out.push(255); // (white)
}

// sRGB to linear (the shaders' curve: shaders/color.hlsli).
fn float[3] linear(Rgb c) @private
{
	float[3] out;
	for (int k = 0; k < 3; k++) out[k] = c[k] * (c[k] * (c[k] * 0.305306011f + 0.682171111f) + 0.012522878f);
	return out;
}

// The sky and the terrain's light for this time of day.
fn void apply_day(DayCycle* day, float view, SkyParams* sky) @private
{
	sky.fog_start = day.fog_begin(view);
	sky.fog_end = day.fog_end(view);
	sky.daylight = day.daylight();
	sky.sun_amount = day.light_amount(); // (the moon's at night)
	Rgb tint = day.sky_tint();
	sky.sky_tint = { tint[0], tint[1], tint[2] }; // (raw: see chunk.hlsl)
	sky.sun_dir = day.sun_dir();
	sky.light_dir = day.light_dir();
	sky.fog_color = linear(day.haze());
	sky.sky_top = linear(day.sky_top());
	sky.sky_horizon = linear(day.sky_horizon());
	sky.sky_ground = sky.fog_color; // below the horizon: the haze, so faded hills meet the sky without a seam
	sky.day = day.day_amount();
	sky.dusk = day.dusk();
	sky.sky_angle = day.angle();
	sky.cloud_offset = day.cloud_offset;
	sky.moon_phase = (float)day.moon_phase() / (float)day_cycle::MOON_PHASES;
}

fn Data player_state(Player* p) @private
{
	Data s = data::object();
	Data pos = data::array();
	foreach (v : p.position) pos.push_double(v);
	s.set("position", pos);
	Data rot = data::array();
	rot.push_double(p.pitch);
	rot.push_double(p.yaw);
	s.set("rotation", rot);
	s.set_bool("flying", p.flying);
	return s;
}

fn void restore_player(Player* p, Data* s) @private
{
	if (!s) return;
	Data* pos = s.get("position");
	if (pos && pos.is_array() && pos.len() == 3) for (int i = 0; i < 3; i++) p.position[i] = pos.at(i).as_float();
	Data* rot = s.get("rotation");
	if (rot && rot.is_array() && rot.len() == 2)
	{
		p.pitch = rot.at(0).as_float();
		p.yaw = rot.at(1).as_float();
	}
	p.flying = s.flag("flying");
}

// Everything a world being played needs (on the heap: the game, the world
// and the network point at each other).
struct Playing
{
	Options* options;
	WorldChoice* choice;
	bool from_menu;
	Window* window;
	Renderer* renderer;
	DString* problem;
	String content_dir;
	Content content;
	BlockTable table;
	BlockRules rules;
	Generator generator;
	World world;
	Player player;
	Game game;
	Session session;
	ChunkRenderer chunks;
	ItemModels item_models;
	Audio audio;
	Hand hand;
	DayCycle day;
	Mood mood;
	SkyParams sky;
	Camera camera;
	SettingsScreen settings_screen;
	ChatState chat;
	Controls controls;
	Screen screen;
	BlockPos container;
	WorldSave save;
	bool has_save;
	bool joining, online;
	uint seed;
	float[3] previous_position;
	float accumulator;
	double start, last, last_save;
	long frame;
	End end;
	bool quit_requested;
	float sleeping;  // seconds asleep (-1: awake)
	double frame_time_total;
	int seen_swings;
	Vec{Puff} puffs;
	Rng random;
	Vec{String} say; // (typed once the world around has loaded; the options')
	Vec{char} my_skin, default_skin;
	int[chunk_renderer::SKIN_SLOTS] skin_slots; // the player id each slot shows (0: ours)
	int skins_seen;
	DString net_problem;
	bool warned;
	bool loaded_parts; // what free() has to undo
}

fn void Playing.free(&self)
{
	if (self.loaded_parts)
	{
		self.session.leave();
		self.audio.free();
		self.item_models.free();
		self.chunks.free();
		self.game.free();
		self.world.free();
		self.generator.free();
		self.rules.free();
		self.table.free();
	}
	self.session.free();
	self.content.free();
	self.chat.free();
	self.puffs.free();
	self.say.free();
	self.my_skin.free();
	self.default_skin.free();
	self.net_problem.free();
	if (self.has_save) self.save.free();
	free(self.content_dir.ptr);
	free(self);
}

fn bool Playing.ui_frame_joining(&self, bool* cancelled) => @pool()
{
	if (!self.window.pump()) return false;
	if (self.window.take_resized()) self.renderer.resize(self.window.width, self.window.height);
	self.session.poll();
	begin_ui(self.window);
	Gui* gu = g();
	gu.begin_window("Joining", { .pos = { gu.in.width * 0.5f, gu.in.height * 0.5f }, .pivot = { 0.5f, 0.5f } });
	gu.text(string::tformat("Connecting to %s...", self.choice.join));
	*cancelled = gu.button("Cancel", { 160, 32 });
	gu.end_window();
	sky_backdrop();
	end_ui();
	self.renderer.begin_frame({ 0.18f, 0.32f, 0.62f, 1.0f });
	self.renderer.end_frame(&ui_overlay, self.renderer);
	return true;
}

// Plays a world until the player quits (to the title menu, or the game).
fn End play(Options* options, WorldChoice* choice, bool from_menu, Window* window, Renderer* renderer, DString* problem)
{
	String content_dir = find_content();
	if (!content_dir.len)
	{
		log::log("Couldn't find the game's content (content/content.json)");
		return FAILED;
	}
	Playing* p = mem::new(Playing);
	defer p.free();
	*p = { .options = options, .choice = choice, .from_menu = from_menu, .window = window, .renderer = renderer, .problem = problem,
		.content_dir = content_dir, .sleeping = -1, .random = rand::seed_random(), .skins_seen = -1, .hand = hand::new(), .day = day_cycle::new(),
		.sky = chunk_renderer::sky_params(), .camera = chunk_renderer::camera() };
	p.chat.init();
	p.net_problem.init(mem);
	p.session.init();
	p.skin_slots[..] = -1;
	foreach (s : options.say) p.say.push(s);
	DString why;
	defer why.free();
	why.init(mem);
	if (!p.content.load(content_dir, &why))
	{
		log::log("Couldn't load the base game: %s", why.str_view());
		return FAILED;
	}
	// Multiplayer: someone else's world comes from its host (after a welcome).
	load_skin(options.hidden, content_dir, &p.my_skin);
	load_skin(true, content_dir, &p.default_skin); // (those who sent none)
	p.session.set_skin(p.my_skin.array_view());
	p.joining = choice.join.len > 0;
	if (p.joining)
	{
		@pool()
		{
			String e = p.session.join(choice.join, choice.join_port, choice.name);
			if (e.len) problem.append(e);
		};
		if (problem.len()) return FAILED;
		bool cancelled;
		while (!p.session.welcomed && p.session.ended().len == 0 && !cancelled)
		{
			if (!p.ui_frame_joining(&cancelled)) return QUIT;
		}
		if (!p.session.welcomed)
		{
			if (!cancelled) problem.append(p.session.ended());
			return cancelled ? MENU : FAILED;
		}
	}
	return p.run();
}

fn End Playing.run(&self)
{
	Options* options = self.options;
	WorldChoice* choice = self.choice;
	Data* welcome = self.session.welcome();
	// The world: its folder decides the seed and block ids.
	self.seed = choice.seed;
	Vec{String} ids;
	defer ids.free();
	if (self.joining)
	{
		self.seed = (uint)welcome.num("seed", 0);
		Data* list = welcome.get("block_ids");
		if (list) foreach (&id : list.items()) ids.push(id.as_string());
		self.content.freeze(ids.array_view()); // (the host's block ids)
		String[] mine = self.content.block_ids();
		bool same = mine.len >= ids.len();
		for (sz i = 0; same && i < ids.len(); i++) same = mine[i] == ids[i];
		if (!same)
		{
			self.problem.append("The server's blocks differ from ours.");
			return FAILED;
		}
		log::log("Joined %s: seed %d", choice.join, self.seed);
	}
	else if (choice.dir.len)
	{
		self.has_save = true;
		String e;
		@pool()
		{
			e = self.save.open(choice.dir, choice.seed, choice.mode).copy(mem);
		};
		defer free(e.ptr);
		if (e.len)
		{
			log::log("Can't open the world in %s: %s", choice.dir, e);
			self.problem.append(e);
			return FAILED;
		}
		self.seed = self.save.seed();
		self.save.block_ids(&ids);
		self.content.freeze(ids.array_view());
		self.save.set_block_ids(self.content.block_ids());
		log::log("World %s: seed %d, %s", choice.dir, self.seed, self.save.mode());
	}
	else
	{
		self.content.freeze();
	}
	self.table = self.content.build_block_table();
	self.rules.init(&self.content);
	self.content.configure_generator(&self.generator, self.seed);
	log::log("%d blocks, %d atlas tiles", self.table.blocks.len(), self.table.atlas.tiles.len());
	the_ui.painter.set_icons(&self.table.atlas); // (item icons)
	self.loaded_parts = true;
	if (!self.chunks.init(self.renderer.device, self.renderer.context, &self.table.atlas))
	{
		log::log("Couldn't set up the terrain renderer");
		return FAILED;
	}
	@pool()
	{
		self.world.init(self.table.blocks.array_view(), &self.table.atlas.layout, &self.generator,
			self.has_save ? self.save.chunks_dir(tmem) : "", settings.worker_threads);
	};
	self.world.set_view_distance(view_distance(options));
	Player* player = &self.player;
	player.position = { 0.5f, (float)self.generator.height_at(0, 0) + 1.0f, 0.5f }; // on the generated ground
	if (self.has_save) restore_player(player, self.save.player());
	if (self.joining) // where the host put us
	{
		Data* spawn = welcome.get("spawn");
		if (spawn && spawn.is_array() && spawn.len() == 3 && spawn.at(0).is_number())
		{
			player.position = { spawn.at(0).as_float(), spawn.at(1).as_float() - player::EYE_HEIGHT, spawn.at(2).as_float() };
		}
	}
	if (options.has_pos)
	{
		player.position = options.pos;
		player.flying = true; // stays where it was put
	}
	if (options.has_look)
	{
		player.yaw = options.look[0] * maths::PI / 180.0f;
		player.pitch = options.look[1] * maths::PI / 180.0f;
	}
	self.previous_position = player.position;
	self.controls.details = options.details;
	self.screen = options.screen == "inventory" ? INVENTORY : options.screen == "chat" ? CHAT : NONE;
	String mode = self.joining ? welcome.str("mode", "survival") : self.has_save ? self.save.mode() : choice.mode;
	Game* game = &self.game;
	game.init(&self.content, &self.table, &self.rules, &self.world, player, mode == "creative");
	game.generator = &self.generator;
	if (self.has_save)
	{
		Data* saved = self.save.player();
		if (saved)
		{
			game.load_player(saved);
		}
		else
		{
			game.start_new_player();
		}
		if (Data* be = self.save.field("block_entities")) game.entities.deserialize(be);
		game.set_world_spawn(self.save.field("spawn"));
		if (Data* plants = self.save.field("plants")) game.load_plants(plants);
	}
	else
	{
		game.start_new_player();
	}
	if (self.joining) game.set_world_spawn(welcome.get("world_spawn"));
	if (!options.has_pos && !self.joining && !(self.has_save && self.save.player() && self.save.player().has("position")))
	{
		game.spawn_on_ground(); // a new player: on dry ground near the origin, once it's loaded
	}
	foreach (i, item : options.drop) // (for trying things out)
	{
		float[3] look = player.look();
		float side = ((float)i - (float)(options.drop.len() - 1) * 0.5f) * 0.6f;
		game.drop_item(item, 1, { player.position[0] + look[0] * 4.0f - look[2] * side, player.position[1] + 1.0f, player.position[2] + look[2] * 4.0f + look[0] * side });
	}
	self.camera.far_z = (float)view_distance(options) * 2.0f;
	if (self.screen == NONE && options.screen == "settings") // (screenshots)
	{
		self.settings_screen.open();
		self.screen = SETTINGS;
		self.window.set_mouse_captured(false);
	}
	self.item_models = { .content = &self.content, .table = &self.table };
	if (!options.no_sound && (!options.hidden || options.sound)) self.audio.init(&self.content, options.hidden ? 0.0f : 1.0f);
	self.seen_swings = game.swings;
	self.day.time = day_cycle::new().time; // a new world starts in the morning
	if (self.has_save)
	{
		if (Data* t = self.save.field("time")) if (t.is_number()) self.day.set_clock(t.as_double()); // (days + time)
		if (Data* l = self.save.field("day_length")) if (l.is_number()) self.day.day_length = math::max(10.0, l.as_double());
	}
	if (options.time >= 0) self.day.time = options.time % 1.0; // (the same day)
	if (self.joining)
	{
		self.day.set_clock(welcome.num("time", self.day.clock()));
		self.day.day_length = math::max(10.0, welcome.num("day_length", self.day.day_length));
	}
	// Multiplayer: as a client (the host does what we do to the world), or
	// the host (open to others).
	Attach attach = { game, &self.world, player, &self.content, &self.rules, &self.day };
	self.session.set_local_name(choice.name);
	if (self.has_save)
	{
		if (Data* ops = self.save.field("ops"))
		{
			Vec{String} names;
			defer names.free();
			foreach (&n : ops.items()) if (n.is_string()) names.push(n.as_string());
			self.session.set_ops(names.array_view());
		}
	}
	if (!self.joining)
	{
		Data info = data::object();
		info.set_long("seed", self.seed);
		Data list = data::array();
		foreach (id : self.content.block_ids()) list.push_string(id);
		info.set("block_ids", list);
		self.session.set_world_info(info);
	}
	if (self.joining)
	{
		self.session.attach(attach);
	}
	else if (choice.host_port > 0)
	{
		@pool()
		{
			String e = self.session.host(choice.host_port, choice.name);
			if (e.len)
			{
				self.net_problem.append(e);
				log::log("%s", e);
			}
		};
		if (!self.net_problem.len())
		{
			if (self.has_save) self.session.set_saved_players(self.save.field("players"));
			self.session.attach(attach);
		}
	}
	if (self.session.mode == OFFLINE) self.session.attach(attach); // (alone: chat and commands still go through it)
	self.online = self.session.mode != OFFLINE;
	// Block shadows, from where the sun (or the moon) is.
	self.world.set_sun_angle(self.day.light_angle());
	self.world.set_block_shadows(true);
	self.start = self.last = self.last_save = clock::now();
	self.end = QUIT;
	while (true)
	{
		bool go_on;
		@pool()
		{
			profiler::profiler.begin_frame(); // (the F3 screen shows the times)
			profiler::@profile("frame")
			{
				go_on = self.step();
			};
		};
		if (!go_on) break;
	}
	if (options.profile)
	{
		WorldStats ws = self.world.stats();
		log::log("Frame times (ms; the GPU's a few frames late):");
		@pool()
		{
			foreach (line : profiler::profiler.report().tsplit("\n")) if (line.len) log::log("%s", line);
		};
		log::log("World threads (%d): %d chunks generated, %.2f ms each; %d meshed, light %.2f ms + mesh %.2f ms each", ws.workers, ws.generated,
			ws.generated ? ws.generate_ms / (double)ws.generated : 0.0, ws.meshed_jobs, ws.meshed_jobs ? ws.light_ms / (double)ws.meshed_jobs : 0.0,
			ws.meshed_jobs ? ws.mesh_ms / (double)ws.meshed_jobs : 0.0);
	}
	self.save_all();
	self.session.leave();
	self.window.set_mouse_captured(false);
	return self.end;
}

fn void Playing.save_all(&self) => @pool()
{
	if (!self.has_save) return;
	int chunks_saved = self.world.save_modified();
	Data state = player_state(&self.player);
	defer state.free();
	Data gp = self.game.save_player();
	foreach (&f : gp.fields()) state.set(f.key, f.value.copy());
	gp.free();
	self.save.set_field("block_entities", self.game.entities.serialize());
	self.save.set_field("time", data::number(self.day.clock())); // (days + time)
	self.save.set_field("plants", self.game.save_plants());
	self.save.set_field("mode", data::string(self.game.creative ? "creative" : "survival"));
	Data ops = data::array();
	foreach (name : self.session.ops) ops.push_string(name);
	self.save.set_field("ops", ops);
	if (self.session.is_host()) self.save.set_field("players", self.session.saved_players()); // joined players' inventories
	if (self.game.has_world_spawn)
	{
		Data spawn = data::array();
		foreach (v : self.game.world_spawn) spawn.push_double(v);
		self.save.set_field("spawn", spawn);
	}
	self.save.save(&state);
	log::log("Saved the world (%d chunks)", chunks_saved);
}

fn void Playing.close_screen(&self)
{
	self.game.inventory_action(game::action("close")); // (what the mouse held goes back in, or is dropped)
	if (self.screen == CONTAINER && self.joining) self.session.close_container(self.container);
	self.screen = NONE;
	self.window.set_mouse_captured(true);
}

fn float[3] light_at(void* p, float[3] at) @private
{
	Playing* self = p;
	Rgb tint = self.day.sky_tint();
	float daylight = self.day.daylight();
	return hand::light_for(&self.world, { tint[0] * daylight, tint[1] * daylight, tint[2] * daylight }, at, self.camera.position, NIGHT_FIGURE_LIGHT);
}

fn void sky_between(void* p) @private
{
	Playing* self = p;
	self.renderer.gpu_mark("sky");
	self.chunks.draw_sky(&self.camera, self.aspect(), &self.sky);
	self.renderer.gpu_mark("water, lava");
}

fn float Playing.aspect(&self) => (float)self.renderer.width / (float)math::max(1, self.renderer.height);

// One frame; false when the world ends.
fn bool Playing.step(&self)
{
	Window* window = self.window;
	Options* options = self.options;
	Player* player = &self.player;
	Game* game = &self.game;
	Session* session = &self.session;
	if (!window.pump())
	{
		self.end = QUIT;
		return false;
	}
	session.poll();
	if (self.joining && session.ended().len)
	{
		self.problem.append(session.ended()); // (the host left, or we lost it)
		self.end = FAILED;
		return false;
	}
	double now = clock::now();
	float dt = math::min(0.25f, (float)(now - self.last));
	self.last = now;
	if (window.take_resized()) self.renderer.resize(window.width, window.height);
	bool left = window.take_pressed(LEFT);
	bool right = window.take_pressed(RIGHT);
	bool middle = window.take_pressed(MIDDLE);
	float wheel = window.take_wheel();
	// Screens: E opens the inventory; E or Esc closes it. While one is open the mouse is free.
	bool key_e = self.controls.pressed(window, settings.key("inventory"));
	bool key_esc = self.controls.pressed(window, win::VK_ESCAPE);
	bool key_chat = self.controls.pressed(window, 'T');
	bool key_command = self.controls.pressed(window, 0xBF) || self.controls.pressed(window, 0x6F); // "/"
	switch (self.screen)
	{
		case NONE:
			if (key_e && window.captured)
			{
				self.screen = INVENTORY;
				window.set_mouse_captured(false);
			}
			else if ((key_chat || key_command) && window.captured)
			{
				self.screen = CHAT; // (T, or "/" to start a command)
				self.chat.typing.clear();
				if (key_command) self.chat.typing.append("/");
				self.chat.frames_open = 0;
				self.chat.sent_at = -1;
				window.set_mouse_captured(false);
			}
			else if (key_esc && !game.dead)
			{
				self.screen = PAUSE;
				window.set_mouse_captured(false);
			}
		case PAUSE:
		case CHAT:
			if (key_esc)
			{
				self.screen = NONE;
				window.set_mouse_captured(true);
			}
		case SETTINGS: // (Esc closes it, back to the pause menu: the screen sees it)
		default:
			if (key_e || key_esc) self.close_screen();
	}
	bool paused = (self.screen == PAUSE || self.screen == SETTINGS) && !self.online; // (single player: the world waits)
	bool captured = window.captured && self.screen == NONE;
	if (left && !window.captured && self.screen == NONE && !g().wants_mouse) window.set_mouse_captured(true); // (that click only captures)
	if (window.minimized())
	{
		clock::sleep_ms(16); // nothing to present to
		return true;
	}
	// Looking around, then fixed physics steps; the camera is drawn between
	// the last two steps, so movement is smooth at any frame rate.
	float mx;
	float my;
	window.take_mouse_delta(&mx, &my);
	float sensitivity = settings.mouse_sensitivity; // radians per mouse count
	player.yaw -= mx * sensitivity;
	player.pitch = math::clamp(player.pitch - my * sensitivity, -1.55f, 1.55f);
	bool fly_key = captured && window.key_down(settings.key("toggle_fly"));
	if (fly_key && !self.controls.fly_key_was_down)
	{
		player.flying = !player.flying;
		player.velocity[1] = 0;
	}
	self.controls.fly_key_was_down = fly_key;
	bool f3 = window.key_down(win::VK_F3);
	if (f3 && !self.controls.f3_was_down) self.controls.details = !self.controls.details;
	self.controls.f3_was_down = f3;
	PlayerInput input;
	if (window.captured)
	{
		input.forward = (float)window.key_down(settings.key("move_forward")) - (float)window.key_down(settings.key("move_back"));
		input.right = (float)window.key_down(settings.key("move_right")) - (float)window.key_down(settings.key("move_left"));
		input.jump = window.key_down(settings.key("jump"));
		input.sneak = window.key_down(settings.key("sneak"));
		input.sprint = window.key_down(settings.key("sprint"));
	}
	float[3] corrected;
	if (session.take_correction(&corrected)) // the host put us back
	{
		player.position = { corrected[0], corrected[1] - player::EYE_HEIGHT, corrected[2] };
		player.velocity = {};
		self.previous_position = player.position;
	}
	profiler::@profile("physics (player, drops)")
	{
		self.accumulator += paused ? 0.0f : dt;
		while (self.accumulator >= player::STEP)
		{
			self.previous_position = player.position;
			if (!game.frozen())
			{
				player.step(input, &self.world, &self.rules);
				game.after_move(player::STEP);
			}
			game.physics_step(player::STEP);
			self.accumulator -= player::STEP;
		}
	};
	float alpha = self.accumulator / player::STEP;
	// Hotbar, mining, breaking, placing and picking.
	GameInput gin = { .captured = captured, .break_held = captured && window.key_down(win::VK_LBUTTON), .break_pressed = captured && left,
		.use_pressed = captured && right, .pick_pressed = captured && middle, .sneak = window.key_down(win::VK_SHIFT), .wheel = -(int)wheel,
		.number_key = self.controls.number_pressed(window) };
	gin.throw_pressed = self.controls.pressed(window, settings.key("throw")) && captured;
	gin.throw_stack = gin.throw_pressed && window.key_down(win::VK_CONTROL);
	profiler::@profile("game (mining, using, time)")
	{
		if (!paused)
		{
			game.update(dt, gin);
			game.tick(dt);
			self.day.update(dt);
			game.day = self.day.day_amount();
		}
	};
	// A bed used at night: sleep (the screen fades out), then it's morning.
	if (game.take_bed_used())
	{
		if (self.day.day_amount() > 0.3f)
		{
			game.show_notice("You can only sleep at night (respawn point set)");
		}
		else
		{
			self.sleeping = 0;
			session.set_sleeping(true); // (a client: the host skips the night once everyone sleeps)
		}
	}
	if (self.sleeping >= 0)
	{
		self.sleeping += dt;
		// The night is skipped when everyone is in bed (the host decides; a
		// client gets up when it says so).
		if (self.sleeping >= 2.5f && !self.joining && session.peers_sleeping())
		{
			self.day.set_time(day_cycle::MORNING);
			self.sleeping = -1;
			session.night_skipped();
		}
		if (self.online && self.sleeping >= 2.5f && self.sleeping < 2.5f + dt) game.show_notice("Waiting for everyone to sleep...");
	}
	if (session.take_woken()) self.sleeping = -1;
	if (self.online && self.sleeping >= 0 && (window.key_down('W') || window.key_down('S') || window.key_down(win::VK_SPACE)))
	{
		self.sleeping = -1; // got up
		session.set_sleeping(false);
	}
	BlockPos opened;
	if (game.take_opened(&opened))
	{
		self.container = opened;
		self.screen = CONTAINER;
		window.set_mouse_captured(false);
		if (self.joining) session.open_container(self.container); // (its contents come from the host)
	}
	float[3] cam = { self.previous_position[0] + (player.position[0] - self.previous_position[0]) * alpha,
		self.previous_position[1] + (player.position[1] - self.previous_position[1]) * alpha + player::EYE_HEIGHT,
		self.previous_position[2] + (player.position[2] - self.previous_position[2]) * alpha };
	self.camera.position = cam;
	self.camera.yaw = player.yaw;
	self.camera.pitch = player.pitch;
	// The arm swings with every hit, place, use and meal, and keeps swinging
	// while mining; it bobs while walking.
	if (game.swings != self.seen_swings)
	{
		self.seen_swings = game.swings;
		self.hand.swing();
	}
	bool walking = player.on_floor && math::hypot(player.velocity[0], player.velocity[2]) > 0.5f;
	self.hand.update(paused ? 0.0f : dt, game.mining_progress > 0.0f, walking);
	// How others see us.
	LocalPose pose = { .eye = player.eye(), .yaw = player.yaw, .pitch = player.pitch, .item = game.selected_item(), .selected = game.selected,
		.flags = (window.key_down(win::VK_SHIFT) && !player.flying ? session::FLAG_SNEAK : 0) | (self.sleeping >= 0 ? session::FLAG_SLEEP : 0) |
			(player.flying ? session::FLAG_FLY : 0),
		.swings = game.swings + (game.mining_progress > 0 ? (int)(self.frame_time_total * 3.0) : 0) };
	self.frame_time_total += dt;
	profiler::@profile("network") { session.update(dt, pose); };
	// Sounds: heard from the camera; muffled with the head in a fluid, which
	// also tints the view (blue in water, orange in lava).
	Data* eye_block = self.rules.def_of(self.world.block_at((int)math::floor(cam[0]), (int)math::floor(cam[1]), (int)math::floor(cam[2])));
	bool in_fluid = eye_block && eye_block.has("fluid");
	bool in_lava = in_fluid && eye_block.str("fluid") == "lava";
	profiler::@profile("sound")
	{
		self.audio.set_listener(cam, self.camera.forward());
		Vec{SoundEvent} sounds = game.take_sounds();
		foreach (&e : sounds) self.audio.play(e.set, e.kind, e.at, e.volume_db);
		sounds.free();
		self.audio.set_volumes(options.hidden ? 0.0f : settings.master_volume, settings.effects_volume, settings.ambience_volume, settings.music_volume);
		self.mood.update(&self.audio, &self.world, cam, paused ? 0.0f : dt);
		self.audio.set_underwater(in_fluid);
		self.audio.update();
	};
	// The settings, as they are now (the settings screen changes them live).
	self.world.set_view_distance(view_distance(options));
	self.camera.far_z = (float)view_distance(options) * 2.0f;
	self.world.set_smooth_lighting(settings.smooth_lighting);
	self.world.set_block_shadows(settings.shadow_mode == "blocks");
	self.renderer.set_render_scale(render_scale(options));
	self.renderer.set_retro(settings.retro_filter);
	self.renderer.options.vsync = !options.no_vsync && settings.vsync;
	self.world.set_sun_angle(self.day.light_angle()); // (shadows follow the sun or moon in steps)
	profiler::@profile("world (loading, meshes in)")
	{
		self.world.update(player.position[0], player.position[1], player.position[2]);
		Vec{ChunkPos} gone = self.world.take_unloaded();
		foreach (cp : gone) self.chunks.remove(cp);
		gone.free();
		Vec{ChunkMesh} meshes = self.world.take_meshes();
		foreach (&m : meshes)
		{
			self.chunks.upload(m);
			m.free();
		}
		meshes.free();
	};
	String net_status = "";
	if (session.is_host())
	{
		net_status = string::tformat(" · open to others (port %d) · %d playing", self.choice.host_port, session.player_count());
	}
	else if (self.joining)
	{
		net_status = string::tformat(" · in %s's world · %d playing", self.choice.join, session.player_count());
	}
	else if (self.net_problem.len())
	{
		net_status = string::tformat(" · %s", self.net_problem.str_view());
	}
	if (self.has_save && now - self.last_save >= 60.0)
	{
		self.save_all(); // autosave
		self.last_save = now;
	}
	if (!self.draw_screens(captured, key_esc, net_status, in_fluid, in_lava)) return false;
	self.draw_world(dt, alpha, paused);
	// With a screenshot, count frames once the world around has loaded.
	WorldStats ws = self.world.stats();
	if (!self.say.is_empty() && (ws.settled || self.sky.time > 60.0f))
	{
		foreach (line : self.say) session.chat(line);
		self.say.clear();
	}
	bool counting = !options.screenshot.len || ws.settled;
	if (!counting && self.sky.time > 60.0f)
	{
		if (!self.warned) log::log("The world hasn't finished loading after 60 s (%d jobs left); taking the screenshot anyway", ws.queued);
		self.warned = true;
		counting = true;
	}
	bool last_frame = options.frames > 0 && counting && ++self.frame >= options.frames;
	bool presented;
	profiler::@profile("present (UI, GPU, vsync)")
	{
		presented = self.renderer.end_frame(&ui_overlay, self.renderer, last_frame ? options.screenshot : "");
	};
	if (!presented)
	{
		self.end = FAILED;
		return false;
	}
	foreach (t : self.renderer.gpu_times) // (a few frames late)
	{
		profiler::profiler.add(intern::intern(string::tformat("GPU: %s", t.name)), t.ms, t.name == "frame" ? 0 : 1);
	}
	if (last_frame || self.quit_requested) return false;
	profiler::@profile("frame rate limit (waiting)")
	{
		if (settings.max_fps > 0) // the frame rate limit
		{
			double until = now + 1.0 / settings.max_fps;
			while (clock::now() < until - 0.0015) clock::sleep_ms(1);
			while (clock::now() < until) {}
		}
	};
	return true;
}

// The screens and the HUD (UI); false when the player quit.
fn bool Playing.draw_screens(&self, bool captured, bool key_esc, String net_status, bool in_fluid, bool in_lava)
{
	Window* window = self.window;
	Game* game = &self.game;
	Session* session = &self.session;
	profiler::@profile("screens, HUD (UI)")
	{
		begin_ui(window);
		if (self.screen == NONE)
		{
			draw_info(self.renderer, &self.player, &self.world, &self.chunks, self.has_save ? self.choice.dir : "", self.seed, game, window.captured,
				self.day.time, self.controls.details, net_status);
		}
		UiIcons icons = { &self.table, &self.content };
		if (game.dead)
		{
			if (window.captured) window.set_mouse_captured(false);
			if (death_screen())
			{
				game.respawn();
				window.set_mouse_captured(true);
			}
		}
		else if (self.screen == INVENTORY && !screens::inventory_screen(g(), game, &icons))
		{
			self.close_screen();
		}
		else if (self.screen == CONTAINER && !screens::container_screen(g(), game, self.container, &icons))
		{
			self.close_screen();
		}
		else if (self.screen == PAUSE)
		{
			switch (pause_menu(self.has_save, self.from_menu))
			{
				case RESUME:
					self.screen = NONE;
					window.set_mouse_captured(true);
				case SETTINGS:
					self.settings_screen.open();
					self.screen = SETTINGS;
				case QUIT:
					self.end = self.from_menu ? MENU : QUIT;
					self.quit_requested = true;
				default:
			}
		}
		else if (self.screen == SETTINGS && !self.settings_screen.draw(g(), &settings, window, key_esc, settings_file()))
		{
			self.screen = PAUSE; // (saved; back to the pause menu)
		}
		if (in_fluid) g().background.fill(screen_rect(), in_lava ? gui::rgba(230, 89, 13, 191) : gui::rgba(26, 77, 140, 115)); // (under the HUD)
		// Name tags over other players' heads.
		Matrix vp = self.camera.view() * self.camera.projection(self.aspect());
		foreach (&a : session.avatars)
		{
			float[4] clip;
			float[3] at = { a.shown[0], a.shown[1] + 0.55f, a.shown[2] };
			for (int j = 0; j < 4; j++) clip[j] = at[0] * vp.m[j] + at[1] * vp.m[4 + j] + at[2] * vp.m[8 + j] + vp.m[12 + j];
			float dx = a.shown[0] - self.camera.position[0];
			float dy = a.shown[1] - self.camera.position[1];
			float dz = a.shown[2] - self.camera.position[2];
			if (clip[3] <= 0.1f || dx * dx + dy * dy + dz * dz > 64.0f * 64.0f) continue; // behind us, or far
			float sx = (clip[0] / clip[3] * 0.5f + 0.5f) * g().in.width;
			float sy = (0.5f - clip[1] / clip[3] * 0.5f) * g().in.height;
			Pt size = g().text_size(a.name);
			outlined_text(&g().background, { math::floor(sx - size.x * 0.5f), math::floor(sy - size.y) }, gui::rgba(255, 255, 255), a.name);
		}
		if (self.screen != SETTINGS) draw_hud(game, &self.table, captured); // (it covers the screen)
		if (self.sleeping >= 0) g().foreground.fill(screen_rect(), gui::rgba(0, 0, 0, (int)(230 * math::min(1.0f, self.sleeping / 1.5f))));
		Vec{String} lines = session.take_chat();
		foreach (line : lines) self.chat.add(line, self.frame_time_total);
		lines.free();
		if (!draw_chat(&self.chat, session, self.screen == CHAT, self.frame_time_total) && self.screen == CHAT)
		{
			self.screen = NONE; // (sent)
			window.set_mouse_captured(true);
		}
		screens::draw_held(g(), game.inventory.cursor, &icons);
		end_ui();
	};
	return true;
}

// The world, the things in it and the hand.
fn void Playing.draw_world(&self, float dt, float alpha, bool paused)
{
	Options* options = self.options;
	Renderer* r = self.renderer;
	Game* game = &self.game;
	Session* session = &self.session;
	self.sky.time = (float)(clock::now() - self.start);
	if (options.screenshot.len)
	{
		// The clouds (and their shadows) as 3 seconds in, however long
		// loading took: the same in every screenshot.
		self.day.cloud_offset = { 0.35f * 3.0f, 0.12f * 3.0f };
	}
	apply_day(&self.day, (float)view_distance(options), &self.sky);
	self.sky.player_light = hand::PLAYER_LIGHT;
	r.begin_frame({ self.sky.fog_color[0], self.sky.fog_color[1], self.sky.fog_color[2], 1.0f }, false); // (the sky covers what the world doesn't)
	float aspect = self.aspect();
	profiler::@profile("draw: terrain, sky")
	{
		r.gpu_mark("terrain");
		self.chunks.draw(&self.camera, aspect, &self.sky, &sky_between, self);
	};
	r.gpu_mark("items, players, hand");
	Matrix vp = self.camera.view() * self.camera.projection(aspect);
	profiler::@profile("draw: items, players, hand")
	{
		// Dropped items: lit like the terrain around them (never quite black).
		Vec{WorldVertex} drop_mesh;
		defer drop_mesh.free();
		game.drops.build_mesh(&self.item_models, &light_at, self, &drop_mesh);
		self.chunks.draw_items(drop_mesh.array_view(), vp, self.camera.position, &self.sky, true);
		CreatureVertices creature_mesh;
		defer creature_mesh.free();
		// Other players: their figures in their skins (a slot each, the last
		// one ours), what they hold, their names.
		if (self.skins_seen != session.skins_changed)
		{
			self.skins_seen = session.skins_changed;
			self.skin_slots[..] = -1; // (all again)
		}
		if (self.skin_slots[OWN_SKIN] != 0)
		{
			self.chunks.set_skin(OWN_SKIN, self.my_skin.array_view());
			self.skin_slots[OWN_SKIN] = 0;
		}
		foreach (i, &a : session.avatars)
		{
			if (i >= OWN_SKIN) break;
			if (self.skin_slots[i] == a.id) continue;
			char[] theirs = session.skin_of(a.id);
			self.chunks.set_skin((int)i, theirs.len ? theirs : self.default_skin.array_view());
			self.skin_slots[i] = a.id;
		}
		if (options.mannequin) // (ours, three blocks ahead, facing us)
		{
			Figure still = figure::new();
			still.update(0.0f);
			float[3] from = self.player.eye();
			float[3] look = self.player.look();
			float len = math::max(0.001f, math::hypot(look[0], look[2]));
			float[3] at = { from[0] + look[0] / len * 3.0f, from[1], from[2] + look[2] / len * 3.0f };
			creature_models::build_player(&still, 0, at, self.player.yaw + maths::PI, light_at(self, { at[0], at[1] - 0.6f, at[2] }), &creature_mesh, OWN_SKIN);
		}
		Vec{WorldVertex} held_by_others;
		defer held_by_others.free();
		foreach (i, &a : session.avatars)
		{
			float[3] lit = light_at(self, { a.shown[0], a.shown[1] - 0.6f, a.shown[2] });
			Matrix hand_m = creature_models::build_player(&a.figure, a.id, a.shown, a.shown_yaw, lit, &creature_mesh, i < OWN_SKIN ? (int)i : -1);
			if (!a.item.len) continue;
			// A block: a small cube in the fist; a flat one (a ladder) held by its
			// lower corner, tip forward and up.
			Data* def = self.rules.def(a.item);
			bool block = self.rules.is_placeable(a.item) && !(def && def.str("shape") == "cross");
			Matrix local = block ? mat::scaling(0.9f, 0.9f, 0.9f) * mat::rotation_y(0.6f) * mat::translation(0, -0.08f, -0.06f)
				: mat::scaling(0.85f, 0.85f, 0.85f) * mat::rotation_y(1.5708f) * mat::translation(0, 0.1f, -0.12f);
			Matrix m = local * hand_m;
			foreach (mv : self.item_models.model(a.item))
			{
				WorldVertex w = { .pos = m.point(mv.pos), .normal = mv.normal, .uv = mv.uv, .color = { lit[0], lit[1], lit[2], 1 } };
				held_by_others.push(w);
			}
		}
		self.chunks.draw_creatures(creature_mesh.array_view(), vp, self.camera.position, &self.sky);
		self.chunks.draw_items(held_by_others.array_view(), vp, self.camera.position, &self.sky, true);
		// Puffs of smoke: a burst of grey cubes rising for 0.9 s.
		Vec{Effect} effects = game.take_effects();
		foreach (&e : effects)
		{
			if (e.name != "smoke") continue;
			for (int i = 0; i < 24; i++)
			{
				float spread = 70.0f * maths::PI / 180.0f * math::sqrt((float)self.random.unit());
				float turn = (float)self.random.unit() * maths::TAU;
				float speed = 1.0f + 1.5f * (float)self.random.unit();
				self.puffs.push({ .pos = e.at, .vel = { math::sin(spread) * math::cos(turn) * speed, math::cos(spread) * speed, math::sin(spread) * math::sin(turn) * speed },
					.size = 0.2f + 0.25f * (float)self.random.unit(), .age = -0.045f * (float)self.random.unit() }); // (nearly all at once)
			}
		}
		effects.free();
		CreatureVertices puff_mesh;
		defer puff_mesh.free();
		for (sz i = 0; i < self.puffs.len();)
		{
			Puff* p = self.puffs.get_ref(i);
			p.age += paused ? 0.0f : dt;
			if (p.age >= 0.9f)
			{
				self.puffs.remove_at(i);
				continue;
			}
			if (p.age > 0 && !paused)
			{
				p.vel[1] += 0.8f * dt; // it rises
				for (int k = 0; k < 3; k++) p.pos[k] += p.vel[k] * dt;
			}
			if (p.age > 0) creature_models::build_cube(p.pos, p.size, { 0.82f, 0.82f, 0.86f }, &puff_mesh);
			i++;
		}
		self.chunks.draw_creatures(puff_mesh.array_view(), vp, self.camera.position, &self.sky);
		RayHit hit = self.player.target(&self.world);
		if (hit.hit)
		{
			BoxList boxes;
			defer boxes.free();
			self.world.block_boxes(hit.position.x, hit.position.y, hit.position.z, hit.block, false, &boxes);
			self.chunks.draw_outline(boxes.array_view());
		}
		if (!game.dead && self.sleeping < 0)
		{
			// Over everything (it never pokes into walls), the item over the arm.
			float[3] lit = light_at(self, self.camera.position);
			Vec{WorldVertex} arm;
			Vec{WorldVertex} held;
			CreatureVertices skinned_arm; // (our right arm, in our skin)
			defer
			{
				arm.free();
				held.free();
				skinned_arm.free();
			}
			self.hand.build(&self.item_models, game.selected_item(), lit, &arm, &held, &skinned_arm, OWN_SKIN);
			Matrix projection = self.camera.projection(aspect);
			r.context.clear_depth(r.depth, 1.0f);
			self.chunks.draw_creatures(skinned_arm.array_view(), projection, { 0, 0, 0 }, &self.sky);
			r.context.clear_depth(r.depth, 1.0f);
			self.chunks.draw_items(held.array_view(), projection, { 0, 0, 0 }, &self.sky, true, true);
		}
	};
}
