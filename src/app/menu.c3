// The title menu: the worlds (play, create, delete), playing with others,
// the settings.
module gocube::app @link("shell32");
import gocube::gui, gocube::window, gocube::renderer, gocube::settings_screen, gocube::data, gocube::data::json, gocube::net::session;
import gocube::win, gocube::util::vec, gocube::util::rand;
import std::io, std::io::path, std::io::file, std::sort, std::math, gocube::clock;

// A world in the title menu's list. (Strings: heap.)
struct WorldEntry
{
	String dir, name, mode, saved, seed;
}

fn void WorldEntry.free(&self)
{
	foreach (s : (String[*]){ self.dir, self.name, self.mode, self.saved, self.seed }) if (s.len) free(s.ptr);
}

struct TitleMenu
{
	bool refresh;
	Vec{WorldEntry} worlds;
	int selected;
	bool confirm_delete;
	DString new_name, new_seed, player_name, join_address;
	int new_mode; // 0 survival, 1 creative
	bool open_to_others;
	bool loaded_names;
	bool in_settings; // the settings screen, over the menu
	SettingsScreen settings_screen;
	bool esc_was_down;
}

fn void TitleMenu.init(&self)
{
	*self = { .refresh = true, .selected = -1 };
	self.new_name.init(mem);
	self.new_name.append("New World");
	self.new_seed.init(mem);
	self.player_name.init(mem);
	self.join_address.init(mem);
}

fn void TitleMenu.free_worlds(&self)
{
	foreach (&w : self.worlds) w.free();
	self.worlds.clear();
}

fn void TitleMenu.free(&self)
{
	self.free_worlds();
	self.worlds.free();
	self.new_name.free();
	self.new_seed.free();
	self.player_name.free();
	self.join_address.free();
}

fn void TitleMenu.find_worlds(&self) => @pool()
{
	self.free_worlds();
	String root = string::tformat("%s/worlds", user_dir());
	PathList? list = path::ls(tmem, (Path)root, false, true, "", false, true);
	if (catch list) return;
	foreach (p : list)
	{
		String name = (String)p;
		String dir = string::tformat("%s/%s", root, name);
		String meta_file = string::tformat("%s/world.json", dir);
		if (!file::is_dir(dir) || !file::exists(meta_file)) continue;
		WorldEntry w = { .dir = dir.copy(mem), .name = name.copy(mem) };
		Data meta;
		DString why;
		why.tinit();
		if (json::read_file(meta_file, &meta, &why) && meta.is_object())
		{
			w.mode = meta.str("mode", "creative").copy(mem);
			w.saved = meta.str("saved", meta.str("created", "")).copy(mem);
			w.seed = meta.get("seed") && meta.get("seed").is_number() ? string::format(mem, "%d", (long)meta.num("seed", 0)) : "".copy(mem);
		}
		else
		{
			w.mode = "(damaged)".copy(mem);
			w.saved = "".copy(mem);
			w.seed = "".copy(mem);
		}
		meta.free();
		self.worlds.push(w);
	}
	// The latest saved first.
	sort::quicksort(self.worlds.array_view(), fn int(WorldEntry a, WorldEntry b) => -a.saved.compare_to(b.saved));
}

struct ShFileOp
{
	Hwnd hwnd;
	uint func;
	WChar* from;
	WChar* to;
	ushort flags;
	Bool any_aborted;
	void* name_mappings;
	WChar* progress_title;
}

extern fn int sh_file_operation(ShFileOp* op) @cname("SHFileOperationW");

// Moves a folder to the Recycle Bin.
fn bool to_recycle_bin(String dir) => @pool()
{
	WString w = dir.replace(tmem, "/", "\\").to_temp_wstring()!!;
	sz n = 0;
	while (w[n]) n++;
	WChar[] from = mem::new_array(WChar, n + 2); // (a double-null-terminated list)
	defer free(from.ptr);
	for (sz i = 0; i < n; i++) from[i] = w[i];
	ShFileOp op = { .func = 3, .from = from.ptr, .flags = 0x40 | 0x10 | 0x4 | 0x400 }; // FO_DELETE; ALLOWUNDO, NOCONFIRMATION, SILENT, NOERRORUI
	return sh_file_operation(&op) == 0 && !op.any_aborted;
}

// Why that can't be a new world's folder name ("" if it can; temp).
fn String name_problem(String name)
{
	if (!name.trim(" ").len) return "Give the world a name.";
	foreach (c : name) if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') return "A world's name can't have < > : \" / \\ | ? * or end with a dot or space.";
	if (name[^1] == '.' || name[^1] == ' ') return "A world's name can't have < > : \" / \\ | ? * or end with a dot or space.";
	if (file::exists(string::tformat("%s/worlds/%s", user_dir(), name))) return "There's already a world with that name.";
	return "";
}

fn void present_menu(Renderer* renderer, String screenshot = "") @private
{
	sky_backdrop();
	end_ui();
	renderer.begin_frame({ 0.18f, 0.32f, 0.62f, 1.0f });
	renderer.end_frame(&ui_overlay, renderer, screenshot);
}

// One frame of the title menu at a time, until a world is chosen (true, in
// `choice`) or the window closes (false).
fn bool title_menu(Options* options, Window* window, Renderer* renderer, TitleMenu* menu, DString* problem, WorldChoice* choice)
{
	long frame = 0;
	bool chosen = false;
	// (In C3 `break` and `continue` in a macro's body, @pool's here, act on
	// this loop: `continue` ends the frame, `break` leaves the menu.)
	while (window.pump())
	{
		@pool()
		{
			if (window.take_resized()) renderer.resize(window.width, window.height);
			if (window.minimized())
			{
				clock::sleep_ms(16);
				continue;
			}
			if (menu.refresh)
			{
				menu.find_worlds();
				menu.selected = menu.worlds.is_empty() ? -1 : 0;
				menu.refresh = false;
			}
			if (!menu.loaded_names) // (the last ones used)
			{
				menu.loaded_names = true;
				menu.player_name.clear();
				menu.player_name.append(settings.player_name());
				menu.join_address.clear();
				menu.join_address.append(settings.last_address().len ? settings.last_address() : "127.0.0.1");
			}
			begin_ui(window);
			bool esc_down = window.key_down(win::VK_ESCAPE);
			bool esc = esc_down && !menu.esc_was_down;
			menu.esc_was_down = esc_down;
			if (menu.in_settings)
			{
				menu.in_settings = menu.settings_screen.draw(g(), &settings, window, esc, settings_file());
				renderer.options.vsync = !options.no_vsync && settings.vsync;
				renderer.set_render_scale(render_scale(options));
				present_menu(renderer);
				continue;
			}
			Gui* gu = g();
			Color warn = gui::rgba(255, 153, 102);
			gu.begin_window("GoCube", { .pos = { gu.in.width * 0.5f, gu.in.height * 0.5f }, .pivot = { 0.5f, 0.5f },
				.size = { math::min(760.0f, gu.in.width - 40), math::min(680.0f, gu.in.height - 40) } });
			gu.text("GoCube", 2.0f);
			gu.text_dim(title_line());
			gu.same_line(gu.window_width() - 2 * gu.theme.padding.x - 110);
			if (gu.button("Settings", { 110, 0 }))
			{
				menu.settings_screen.open();
				menu.in_settings = true;
			}
			gu.separator();
			gu.begin_child("menu", { 0, 0 }, false); // (scrolls, on a small window)
			gu.text("Worlds");
			gu.begin_child("worlds", { 0, 140 }, true);
			foreach (i, &w : menu.worlds)
			{
				gu.push_int_id((int)i);
				if (gu.selectable(string::tformat("%s##world", w.name), menu.selected == (int)i))
				{
					menu.selected = (int)i;
					menu.confirm_delete = false;
					if (gu.double_click) *choice = app::choice(w.dir, 0, w.mode, menu.player_name.str_view());
					chosen = chosen || gu.double_click;
				}
				gu.same_line(300);
				gu.text_colored(string::tformat("%s  -  seed %s  -  %s", w.mode, w.seed, w.saved), gu.theme.text_dim);
				gu.pop_id();
			}
			if (menu.worlds.is_empty()) gu.text_dim("No worlds yet: create one below.");
			gu.end_child();
			bool has = menu.selected >= 0 && menu.selected < menu.worlds.len();
			gu.begin_disabled(!has);
			if (gu.button("Play", { 120, 32 }) && has && !chosen)
			{
				WorldEntry* w = menu.worlds.get_ref(menu.selected);
				*choice = app::choice(w.dir, 0, w.mode, menu.player_name.str_view());
				chosen = true;
			}
			gu.same_line();
			if (!menu.confirm_delete)
			{
				if (gu.button("Delete...", { 120, 32 })) menu.confirm_delete = true;
			}
			else if (has)
			{
				gu.align_text_to_frame();
				gu.text_colored(string::tformat("Move \"%s\" to the Recycle Bin?", menu.worlds[menu.selected].name), warn);
				gu.same_line();
				if (gu.button("Delete"))
				{
					if (!to_recycle_bin(menu.worlds[menu.selected].dir))
					{
						problem.clear();
						problem.append("The world couldn't be moved to the Recycle Bin.");
					}
					menu.confirm_delete = false;
					menu.refresh = true;
				}
				gu.same_line();
				if (gu.button("Keep it")) menu.confirm_delete = false;
			}
			gu.end_disabled();
			gu.space(8);
			gu.separator();
			gu.text("New world");
			gu.set_next_width(260);
			gu.input_text("Name", &menu.new_name, 63);
			gu.set_next_width(260);
			gu.input_text("Seed", &menu.new_seed, 20, "random", true);
			gu.set_next_width(260);
			gu.combo("Mode", &menu.new_mode, { "Survival", "Creative" });
			String why = name_problem(menu.new_name.str_view());
			gu.begin_disabled(why.len > 0);
			if (gu.button("Create and play", { 200, 32 }) && !why.len && !chosen)
			{
				Rng rng = rand::seed_random();
				uint seed = menu.new_seed.len() ? (uint)(menu.new_seed.str_view().to_long() ?? 0) : (uint)rng.next();
				*choice = app::choice(string::tformat("%s/worlds/%s", user_dir(), menu.new_name.str_view()), seed,
					menu.new_mode == 1 ? "creative" : "survival", menu.player_name.str_view());
				chosen = true;
			}
			gu.end_disabled();
			if (why.len)
			{
				gu.same_line();
				gu.align_text_to_frame();
				gu.text_dim(why);
			}
			gu.space(8);
			gu.separator();
			gu.text("Playing with others");
			gu.set_next_width(260);
			gu.input_text("Your name", &menu.player_name, 31);
			gu.checkbox("Open the world I play to others (port 24680)", &menu.open_to_others);
			gu.set_next_width(260);
			gu.input_text("Address##join", &menu.join_address, 63);
			gu.same_line();
			gu.begin_disabled(!menu.join_address.len());
			if (gu.button("Join their world", { 160, 0 }) && menu.join_address.len() && !chosen)
			{
				String address = menu.join_address.str_view();
				int port = session::DEFAULT_PORT;
				if (try colon = address.rindex_of_char(':'))
				{
					port = (int)(address[colon + 1..].to_long() ?? session::DEFAULT_PORT);
					address = address[:colon];
				}
				*choice = app::choice("", 0, "survival", menu.player_name.str_view(), 0, address, port);
				chosen = true;
			}
			gu.end_disabled();
			if (problem.len()) gu.text_wrapped(problem.str_view(), warn);
			gu.end_child();
			gu.end_window();
			bool last_frame = options.frames > 0 && ++frame >= options.frames; // (testing)
			present_menu(renderer, last_frame ? options.screenshot : "");
			if (chosen)
			{
				settings.set_player_name(menu.player_name.str_view()); // (kept for next time)
				if (choice.join.len) settings.set_last_address(menu.join_address.str_view());
				if (!options.hidden) settings.save(settings_file());
				if (!choice.join.len && menu.open_to_others) choice.host_port = session::DEFAULT_PORT;
				break;
			}
			if (last_frame) break;
		};
	}
	return chosen; // (false: the window closed, or the test's last frame)
}

// Under the title: the version, or (one launch in TITLE_ODDS) something else.
const int TITLE_ODDS @private = 12;
String title_word @private;

fn String title_line() @private
{
	if (!title_word.len)
	{
		Rng r = rand::seed_random();
		String[*] odd = { "you are alone", "0.0.0", "it is quiet here", "still here", "don't dig down", "the light is not yours" };
		title_word = r.unit() * TITLE_ODDS < 1.0 ? odd[(sz)(r.unit() * odd.len) % odd.len] : VERSION;
	}
	return title_word;
}
