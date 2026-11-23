// GoCube: a voxel sandbox. The title menu lists the worlds (play, create,
// delete); a world is a folder with world.json (a data table); the game's
// content is data (content/content.json, its textures and sounds). Esc
// pauses. Started with --world, --world-dir or a testing option, it goes
// straight into that world (see app/options.c3 for the options).
//
// Controls: click to look around with the mouse (Esc lets go). W A S D walk,
// Space jumps (swims up, flies up), Shift sneaks (flies down), Ctrl sprints,
// F flies. Left click breaks, right click places (or opens a door; sneak to
// place against it), the wheel picks the block to place, middle click picks
// the block aimed at. Your skin, as others see you: %APPDATA%\GoCube\skin.png
// (the usual 64x64 or 64x32 skin layout), else the base game's.
module gocube;
import gocube::app, gocube::window, gocube::renderer, gocube::game::settings, gocube::log, gocube::clock, gocube::win;

fn int main(String[] args)
{
	clock::fine_sleep(true); // (the frame rate limit sleeps to the millisecond)
	defer clock::fine_sleep(false);
	win::set_process_dpi_awareness_context(win::DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	app::Options options = app::parse(args);
	app::settings = settings::new();
	if (!options.hidden) // (tests play with the defaults)
	{
		DString why;
		defer why.free();
		why.init(mem);
		@pool()
		{
			if (!app::settings.load(app::settings_file(), &why)) log::log("Can't read the settings (%s); using the defaults", why.str_view());
		};
	}
	Window window;
	defer window.free();
	// Three quarters of the screen (hidden, for tests and screenshots: always 1280 x 720).
	if (!window.create("GoCube", options.hidden ? 1280 : 0, options.hidden ? 720 : 0, !options.hidden))
	{
		log::log("Couldn't create the window");
		return 1;
	}
	Renderer renderer;
	defer renderer.free();
	RendererOptions renderer_options = options.renderer;
	renderer_options.vsync = !options.no_vsync && app::settings.vsync;
	if (!renderer.init(window.hwnd, window.width, window.height, renderer_options))
	{
		log::log("Couldn't start Direct3D 11");
		return 1;
	}
	renderer.set_render_scale(app::render_scale(&options));
	app::Ui ui; // (before the renderer goes: it draws with its device)
	defer ui.free();
	if (!ui.init(&window, &renderer))
	{
		log::log("Couldn't set up the UI's drawing");
		return 1;
	}
	app::the_ui = &ui;
	bool ok = true;
	DString problem;
	defer problem.free();
	problem.init(mem);
	// Started for a world (or for testing): straight into it.
	bool direct = !options.menu && (options.world_name.len || options.world_dir.len || options.hidden || options.frames > 0 || options.has_pos ||
		options.host_port > 0 || options.join.len);
	if (direct)
	{
		String dir = options.world_dir;
		@pool()
		{
			if (!dir.len && options.world_name.len) dir = string::tformat("%s/worlds/%s", app::user_dir(), options.world_name);
			app::WorldChoice choice = app::choice(dir, options.seed, options.mode, options.name, options.host_port, options.join, options.join_port);
			defer choice.free();
			ok = app::play(&options, &choice, false, &window, &renderer, &problem) != FAILED;
		};
		if (problem.len()) log::log("%s", problem.str_view());
	}
	else
	{
		app::TitleMenu menu;
		menu.init();
		defer menu.free();
		while (true)
		{
			app::WorldChoice choice;
			defer choice.free();
			if (!app::title_menu(&options, &window, &renderer, &menu, &problem, &choice)) break; // the window closed
			problem.clear();
			app::End end = app::play(&options, &choice, true, &window, &renderer, &problem);
			if (end == QUIT) break;
			if (end == FAILED && !problem.len()) problem.append("That world couldn't be opened (see the log).");
			menu.refresh = true;
		}
	}
	app::the_ui = null;
	return ok ? 0 : 1;
}
