// The player's settings: view distance, mouse sensitivity, frame rate limit,
// vsync, smooth lighting, sun shadows, render scale, the clock, volumes, key
// bindings, and the title menu's name and last address. Saved in
// %APPDATA%\GoCube\settings.json (JSON); what isn't there, or isn't valid, is
// the default. The settings screen changes them live; the game reads them
// every frame.
module gocube::game::settings;
import gocube::data, gocube::data::json, std::io, std::math;

const int[*] VIEW_DISTANCES = { 64, 96, 128, 160, 192, 256 };
const int[*] FPS_LIMITS = { 30, 60, 120, 144, 0 }; // 0: unlimited
// The world is drawn at this part of the window's resolution and stretched
// over it (menus and the HUD stay sharp): less uses less GPU.
const float[*] RENDER_SCALES = { 1.0f, 0.85f, 0.75f, 0.5f };
// Sun shadows: "blocks" (each block face in sun or shadow, worked out as the
// sun moves) or "off".
const String[*] SHADOW_MODES = { "blocks", "off" };

// A rebindable action: its id (saved), what the screen calls it, its default
// key (a Windows virtual-key code).
struct Action
{
	String id, label;
	int key;
}

const Action[*] ACTIONS = {
	{ "move_forward", "Move forward", 'W' },
	{ "move_back", "Move back", 'S' },
	{ "move_left", "Move left", 'A' },
	{ "move_right", "Move right", 'D' },
	{ "jump", "Jump / swim up / fly up", 0x20 },  // space
	{ "sneak", "Sneak / fly down", 0x10 },        // shift
	{ "sprint", "Sprint", 0x11 },                 // ctrl
	{ "toggle_fly", "Toggle flying", 'F' },
	{ "inventory", "Inventory", 'E' },
	{ "throw", "Throw (Ctrl: the stack)", 'Q' },
};

const int ACTION_COUNT = ACTIONS.len;

struct Settings
{
	int view_distance;
	float mouse_sensitivity; // radians per mouse count
	int max_fps;
	bool vsync;
	bool smooth_lighting;    // corner shading (ambient occlusion) and soft light
	bool retro_filter;       // the screen filter: vignette, film grain, vivid colours
	String shadow_mode;      // (one of SHADOW_MODES: a constant)
	float render_scale;
	bool clock_24h;          // 07:19 instead of 7:19 AM
	int worker_threads;      // chunk threads (0: as many as cores, less one); file only
	float master_volume, effects_volume, ambience_volume; // 0..1
	int[ACTION_COUNT] keys;  // by ACTIONS' order
	char[64] name_buf;
	sz name_len;
	char[128] address_buf;
	sz address_len;
}

// The defaults.
fn Settings new()
{
	Settings s = { .view_distance = 128, .mouse_sensitivity = 0.003f, .vsync = true, .smooth_lighting = false, .retro_filter = true,
		.shadow_mode = "blocks", .render_scale = 1.0f, .master_volume = 1.0f, .effects_volume = 1.0f, .ambience_volume = 0.7f };
	s.reset_keys();
	s.set_player_name("Player");
	s.set_last_address("127.0.0.1");
	return s;
}

fn String Settings.player_name(&self) => (String)self.name_buf[:self.name_len];
fn String Settings.last_address(&self) => (String)self.address_buf[:self.address_len];

fn void Settings.set_player_name(&self, String s)
{
	self.name_len = math::min(s.len, (sz)self.name_buf.len);
	self.name_buf[:self.name_len] = s[:self.name_len];
}

fn void Settings.set_last_address(&self, String s)
{
	self.address_len = math::min(s.len, (sz)self.address_buf.len);
	self.address_buf[:self.address_len] = s[:self.address_len];
}

fn int action_index(String id)
{
	foreach (i, a : ACTIONS)
	{
		if (a.id == id) return (int)i;
	}
	return -1;
}

// An action's key (0: none).
fn int Settings.key(&self, String action)
{
	int i = action_index(action);
	return i < 0 ? 0 : self.keys[i];
}

fn void Settings.reset_keys(&self)
{
	foreach (i, a : ACTIONS) self.keys[i] = a.key;
}

// How many actions use this key (more than one: a clash).
fn int Settings.actions_using(&self, int k)
{
	int n = 0;
	foreach (bound : self.keys) n += bound == k ? 1 : 0;
	return n;
}

fn void number(Data* d, String name, double lo, double hi, double* out) @private
{
	Data* v = d.get(name);
	if (v && v.is_number() && !math::is_nan(v.as_double()) && !math::is_inf(v.as_double())) *out = math::clamp(v.as_double(), lo, hi);
}

// False (and the defaults kept) if the file is there but can't be read.
fn bool Settings.load(&self, String path, DString* error = null) => @pool()
{
	if (!file::exists(path)) return true; // (the defaults)
	Data d;
	defer d.free();
	DString why;
	why.tinit();
	if (!json::read_file(path, &d, &why) || !d.is_object())
	{
		if (error)
		{
			if (!*error) error.init(mem);
			error.append(why.len() ? why.str_view() : "it isn't an object of settings");
		}
		return false;
	}
	double v = self.view_distance;
	number(&d, "view_distance", 16, 512, &v);
	self.view_distance = (int)v;
	v = self.mouse_sensitivity;
	number(&d, "mouse_sensitivity", 0.0005, 0.01, &v);
	self.mouse_sensitivity = (float)v;
	v = self.max_fps;
	number(&d, "max_fps", 0, 1000, &v);
	self.max_fps = (int)v;
	self.vsync = d.flag("vsync", self.vsync);
	self.smooth_lighting = d.flag("smooth_lighting", self.smooth_lighting);
	self.retro_filter = d.flag("retro_filter", self.retro_filter);
	String mode = d.str("shadow_mode", self.shadow_mode);
	foreach (m : SHADOW_MODES)
	{
		if (m == mode) self.shadow_mode = m;
	}
	v = self.render_scale;
	number(&d, "render_scale", 0.25, 1.0, &v);
	self.render_scale = (float)v;
	self.clock_24h = d.flag("clock_24h", self.clock_24h);
	v = self.worker_threads;
	number(&d, "worker_threads", 0, 64, &v);
	self.worker_threads = (int)v;
	v = self.master_volume;
	number(&d, "master_volume", 0, 1, &v);
	self.master_volume = (float)v;
	v = self.effects_volume;
	number(&d, "effects_volume", 0, 1, &v);
	self.effects_volume = (float)v;
	v = self.ambience_volume;
	number(&d, "ambience_volume", 0, 1, &v);
	self.ambience_volume = (float)v;
	if (Data* keys = d.get("keys"))
	{
		foreach (i, a : ACTIONS)
		{
			Data* k = keys.get(a.id);
			if (k && k.is_integer() && k.i > 0 && k.i < 256) self.keys[i] = (int)k.i;
		}
	}
	String name = d.str("player_name", self.player_name());
	if (name.len <= 64) self.set_player_name(name.tcopy());
	String address = d.str("last_address", self.last_address());
	if (address.len <= 128) self.set_last_address(address.tcopy());
	return true;
}

fn bool Settings.save(&self, String path) => @pool()
{
	Data keys = data::object();
	foreach (i, a : ACTIONS) keys.set_long(a.id, self.keys[i]);
	Data d = data::object();
	defer d.free();
	d.set_long("view_distance", self.view_distance);
	d.set_double("mouse_sensitivity", self.mouse_sensitivity);
	d.set_long("max_fps", self.max_fps);
	d.set_bool("vsync", self.vsync);
	d.set_bool("smooth_lighting", self.smooth_lighting);
	d.set_bool("retro_filter", self.retro_filter);
	d.set_string("shadow_mode", self.shadow_mode);
	d.set_double("render_scale", self.render_scale);
	d.set_bool("clock_24h", self.clock_24h);
	d.set_long("worker_threads", self.worker_threads);
	d.set_double("master_volume", self.master_volume);
	d.set_double("effects_volume", self.effects_volume);
	d.set_double("ambience_volume", self.ambience_volume);
	d.set("keys", keys);
	d.set_string("player_name", self.player_name());
	d.set_string("last_address", self.last_address());
	if (try slash = path.rindex_of_char('/'))
	{
		(void)path::mkdir(path::tnew(path[:slash])!!, true);
	}
	else if (try slash2 = path.rindex_of_char('\\'))
	{
		(void)path::mkdir(path::tnew(path[:slash2])!!, true);
	}
	return json::save_file(path, &d);
}
