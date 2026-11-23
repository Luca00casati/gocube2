// The game's options, folders and settings.
//
// Options:
//   --world=NAME     play the saved world NAME (in %APPDATA%\GoCube\worlds;
//                    a new one is made if it doesn't exist)
//   --world-dir=DIR  play the world in this folder
//                    Without either, the world isn't saved.
//   --seed=N         seed for a new world (default 777)
//   --mode=survival|creative  for a new world
//   --view=N         view distance in blocks (default: the settings')
//   --pos=X,Y,Z      start flying at this position (feet)
//   --look=YAW,PITCH start view direction in degrees (0, 0: towards -z)
//   --drop=BLOCK,... blocks dropped in front of the player
//   --time=T         the time of day to start at (0..1)
//   --mannequin      your own figure, in your skin, in front of you
//   --details        start with the details screen (F3) open
//   --profile        write the frame times to the log when the world ends
//   --warp           render on WARP, Windows' software adapter (no GPU needed)
//   --hidden         don't show the window (tests)
//   --frames=N       quit after N frames (with --screenshot: counted from
//                    when the world around has finished loading)
//   --screenshot=F   save the last frame as a PNG
//   --render-scale=F draw the world at this part of the window's resolution
//   --screen=inventory|settings|chat  a screen open at the start
//   --say=LINE       type LINE into the chat once the world has loaded
//   --menu           the title menu even with testing options
//   --host[=PORT]    open the world to other players (default port 24680)
//   --join=ADDRESS[:PORT]  play in someone else's world
//   --name=NAME      the name others see (default "Player")
//   --no-sound / --sound   no sound / with --hidden: sound at volume 0
//   --no-vsync       present as fast as possible
//   --debug / --no-debug   the Direct3D 11 debug layer
module gocube::app;
import gocube::renderer, gocube::net::session, gocube::game::settings, gocube::log, gocube::win, gocube::util::vec;
import std::io, std::os::env;

const String VERSION = "0.1.0";

struct Options
{
	RendererOptions renderer;
	bool hidden, no_sound, sound;
	long frames;          // 0: until the window is closed
	String screenshot;
	String world_name;
	String world_dir;
	uint seed;
	int view;             // --view (0: the settings')
	float render_scale;   // --render-scale (0: the settings')
	bool no_vsync;
	bool has_pos;
	float[3] pos;
	bool has_look;
	float[2] look;
	String mode;          // for a new world
	Vec{String} drop;     // blocks dropped in front of the player at the start
	String screen;        // a screen open at the start
	bool mannequin;
	bool details;
	Vec{String} say;      // lines typed into the chat at the start (commands too)
	bool profile;
	double time;          // time of day to start at (0..1; -1: the world's)
	bool menu;
	int host_port;        // open to others on this port (0: not)
	String join;          // the host's address
	int join_port;
	String name;
}

// The player's settings (%APPDATA%\GoCube\settings.json; tests run with the
// defaults: --hidden doesn't read them).
Settings settings;

fn void split_list(String list, Vec{String}* out) @private
{
	foreach (part : list.tsplit(",")) if (part.len) out.push(part.copy(mem));
}

fn float[3] floats3(String s, bool* ok) @private
{
	String[] parts = s.tsplit(",");
	float[3] out;
	*ok = parts.len == 3;
	for (int i = 0; i < 3 && i < parts.len; i++)
	{
		if (try f = parts[i].to_float())
		{
			out[i] = f;
		}
		else
		{
			*ok = false;
		}
	}
	return out;
}

fn Options parse(String[] args) => @pool()
{
	Options o = { .renderer = { .vsync = true }, .seed = 777, .mode = "survival", .time = -1, .join_port = session::DEFAULT_PORT, .name = "Player" };
	foreach (i, a : args)
	{
		if (i == 0) continue;
		String v;
		switch
		{
			case a == "--warp": o.renderer.warp = true;
			case a == "--hidden": o.hidden = true;
			case a.starts_with("--frames="): o.frames = a[9..].to_long() ?? 0;
			case a.starts_with("--screenshot="): o.screenshot = a[13..];
			case a.starts_with("--world="): o.world_name = a[8..];
			case a.starts_with("--world-dir="): o.world_dir = a[12..];
			case a.starts_with("--seed="): o.seed = (uint)(a[7..].to_long() ?? 777);
			case a.starts_with("--drop="): split_list(a[7..], &o.drop);
			case a == "--details": o.details = true;
			case a == "--profile": o.profile = true;
			case a == "--mannequin": o.mannequin = true;
			case a == "--menu": o.menu = true;
			case a.starts_with("--time="): o.time = a[7..].to_double() ?? -1;
			case a.starts_with("--screen="): o.screen = a[9..];
			case a.starts_with("--say="): o.say.push(a[6..].copy(mem));
			case a.starts_with("--mode="): o.mode = a[7..];
			case a.starts_with("--view="): o.view = (int)(a[7..].to_long() ?? 0);
			case a.starts_with("--render-scale="): o.render_scale = a[15..].to_float() ?? 0;
			case a.starts_with("--pos="): o.pos = floats3(a[6..], &o.has_pos);
			case a.starts_with("--look="):
				String[] parts = a[7..].tsplit(",");
				o.has_look = parts.len == 2;
				if (o.has_look) o.look = { parts[0].to_float() ?? 0, parts[1].to_float() ?? 0 };
			case a == "--host": o.host_port = session::DEFAULT_PORT;
			case a.starts_with("--host="): o.host_port = (int)(a[7..].to_long() ?? session::DEFAULT_PORT);
			case a.starts_with("--join="):
				o.join = a[7..];
				if (try colon = o.join.rindex_of_char(':'))
				{
					o.join_port = (int)(o.join[colon + 1..].to_long() ?? session::DEFAULT_PORT);
					o.join = o.join[:colon];
				}
			case a.starts_with("--name="): o.name = a[7..];
			case a == "--no-sound": o.no_sound = true;
			case a == "--sound": o.sound = true;
			case a == "--no-vsync":
				o.renderer.vsync = false;
				o.no_vsync = true;
			case a == "--debug": o.renderer.debug = true;
			case a == "--no-debug": o.renderer.debug = false;
			default: log::log("Unknown option %s (ignored)", a);
		}
	}
	return o;
}

extern fn uint get_module_file_name(Handle exe_module, WChar* out, uint size) @cname("GetModuleFileNameW");

// The base game's content folder (content/, with content.json): next to the
// game, or in a parent of its folder (a build tree). "" if none (heap).
fn String find_content()
{
	WChar[1024] exe;
	uint n = get_module_file_name(null, &exe, 1024);
	if (n == 0) return "";
	String dir = win::utf8_of(mem, &exe, (int)n).replace(mem, "\\", "/");
	while (true)
	{
		sz slash = dir.rindex_of_char('/') ?? -1;
		if (slash < 0) break;
		dir = dir[:slash];
		bool found;
		@pool()
		{
			found = file::exists(string::tformat("%s/content/content.json", dir));
		};
		if (found) return string::format(mem, "%s/content", dir);
	}
	return "";
}

// %APPDATA%\GoCube (temp).
fn String user_dir() => string::tformat("%s/GoCube", env::tget_var("APPDATA") ?? ".");
fn String settings_file() => string::tformat("%s/settings.json", user_dir());

// What the game draws and plays with: the settings, unless an option says otherwise.
fn int view_distance(Options* o) => o.view > 0 ? o.view : settings.view_distance;
fn float render_scale(Options* o) => o.render_scale > 0 ? o.render_scale : settings.render_scale;
