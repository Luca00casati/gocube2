// The game's UI around the world: a frame's input, the menus' sky, the death
// screen, the HUD, the details screen (F3), the pause menu and the chat.
module gocube::app;
import gocube::gui, gocube::gui_d3d, gocube::window, gocube::renderer, gocube::chunk_renderer, gocube::profiler, gocube::win;
import gocube::game, gocube::game::player, gocube::game::inventory, gocube::game::day_cycle, gocube::game::blocks;
import gocube::content, gocube::world, gocube::net::session, gocube::clock, gocube::util::vec;
import std::math;

// The UI: the game's own (ui/gui), drawn with Direct3D 11 (ui/gui_d3d).
struct Ui
{
	GuiD3D painter;
	Gui gui;
	DrawCmds cmds;
	UiEvents events; // this frame's (the input's text and keys point into it)
	double last;
	Window* window;
}

Ui* the_ui;

fn Gui* g() @inline => &the_ui.gui;

fn String clipboard(void* window) @private => ((Window*)window).clipboard_text(tmem);

fn bool Ui.init(&self, Window* window, Renderer* r)
{
	self.window = window;
	self.last = clock::now();
	if (!self.painter.init(r.device, r.context)) return false;
	self.gui.init(&gui_d3d::measure, &self.painter);
	self.gui.clipboard = &clipboard;
	self.gui.clipboard_context = window;
	return true;
}

fn void Ui.free(&self)
{
	gui::free_cmds(&self.cmds);
	self.cmds.free();
	self.events.free();
	self.gui.free();
	self.painter.free();
}

// Starts a UI frame with what the window got (nothing for the UI while the
// mouse is captured for looking around).
fn void begin_ui(Window* window)
{
	double now = clock::now();
	Input in = gui::input();
	in.dt = math::clamp((float)(now - the_ui.last), 0.0001f, 0.25f);
	the_ui.last = now;
	in.width = (float)window.width;
	in.height = (float)window.height;
	float x;
	float y;
	if (window.mouse_position(&x, &y)) in.mouse = { x, y };
	the_ui.events.free();
	the_ui.events = window.take_ui_events();
	UiEvents* e = &the_ui.events;
	for (int b = 0; b < 3; b++)
	{
		in.down[b] = !window.captured && window.down[b];
		in.clicks[b] = e.clicks[b];
	}
	in.wheel = window.captured ? 0 : e.wheel;
	in.text = (void*)e.text ? e.text.str_view() : "";
	in.keys = e.keys.array_view();
	in.ctrl = window.key_down(win::VK_CONTROL);
	g().begin_frame(in);
}

fn void end_ui()
{
	gui::free_cmds(&the_ui.cmds);
	g().end_frame(&the_ui.cmds);
}

// For Renderer.end_frame: the UI, over the frame.
fn void ui_overlay(void* r)
{
	Renderer* renderer = r;
	the_ui.painter.draw(renderer.width, renderer.height, the_ui.cmds.array_view());
}

fn Area screen_rect() => { 0, 0, g().in.width, g().in.height };

// Behind the menus with no world: an evening sky, for the glass panels to sit on.
fn void sky_backdrop()
{
	Area r = screen_rect();
	float a = math::floor(r.y1 * 0.5f);
	float b = math::floor(r.y1 * 0.68f);
	Color top = gui::rgba(12, 20, 42);
	Color sky = gui::rgba(66, 92, 132);
	Color glow = gui::rgba(170, 136, 124);
	Color ground = gui::rgba(20, 22, 30);
	DrawList* bg = &g().background;
	bg.gradient({ r.x0, r.y0, r.x1, a }, top, sky, true);
	bg.gradient({ r.x0, a, r.x1, b }, sky, glow, true);
	bg.gradient({ r.x0, b, r.x1, r.y1 }, glow, ground, true);
}

// The death screen; true when Respawn is pressed.
fn bool death_screen()
{
	Gui* gu = g();
	gu.background.fill(screen_rect(), gui::rgba(90, 0, 0, 140));
	gu.begin_window("Dead", { .pos = { gu.in.width * 0.5f, gu.in.height * 0.45f }, .pivot = { 0.5f, 0.5f }, .no_background = true });
	gu.text_colored("You died", gui::rgba(255, 255, 255), 2.5f);
	gu.space(10);
	bool respawn = gu.button("Respawn", { math::max(160.0f, gu.text_size("You died", 2.5f).x), 36 });
	gu.end_window();
	return respawn;
}

fn void outlined_text(DrawList* dl, Pt at, Color col, String text) => dl.outlined_text(at, col, text, g().theme.font);

// A bar with its text on it.
fn void status_bar(DrawList* dl, Pt a, Pt size, float fraction, Color fill, Color highlight, String text) @private
{
	Area r = { a.x, a.y, a.x + size.x, a.y + size.y };
	dl.fill(r, gui::rgba(0, 0, 0, 140));
	float fx = a.x + size.x * math::clamp(fraction, 0.0f, 1.0f);
	dl.fill({ a.x, a.y, fx, r.y1 }, fill);
	dl.fill({ a.x, a.y, fx, a.y + 3 }, highlight);
	dl.stroke(r, gui::rgba(255, 255, 255, 128));
	Pt ts = g().text_size(text);
	outlined_text(dl, { math::floor(a.x + (size.x - ts.x) / 2), math::floor(a.y + (size.y - ts.y) / 2) }, gui::rgba(255, 255, 255), text);
}

float shown_health_value @private = game::MAX_HEALTH;

// Health as the HUD shows it: down at once when hurt, up no faster than the
// game heals (a point every 2.5 s), so the red fades evenly rather than in
// steps; a jump of more than a point (respawning, food) comes at once.
fn float shown_health(int health, float dt) @private
{
	float target = (float)health;
	if (target <= shown_health_value)
	{
		shown_health_value = target;
	}
	else
	{
		shown_health_value = math::min(target, math::max(shown_health_value, target - 1.0f) + dt / 2.5f);
	}
	return shown_health_value;
}

// Hurt: the screen reddens the lower health is, mostly at the edges.
fn void draw_wounds(DrawList* dl, Pt size, float health) @private
{
	float missing = math::clamp(1.0f - health / (float)game::MAX_HEALTH, 0.0f, 1.0f);
	if (missing <= 0.0f) return;
	int tint = (int)(110.0f * missing * missing);
	int edge = (int)(220.0f * missing);
	dl.fill({ 0, 0, size.x, size.y }, gui::rgba(150, 0, 0, tint));
	Color red = gui::rgba(170, 0, 0, edge);
	Color clear = gui::rgba(170, 0, 0, 0);
	float band = 0.35f * math::min(size.x, size.y) * (0.5f + missing);
	dl.gradient({ 0, 0, size.x, band }, red, clear, true);
	dl.gradient({ 0, size.y - band, size.x, size.y }, clear, red, true);
	dl.gradient({ 0, 0, band, size.y }, red, clear, false);
	dl.gradient({ size.x - band, 0, size.x, size.y }, clear, red, false);
}

// The HUD: the crosshair, the mining progress under it, the 9 hotbar slots,
// breath above them in survival, the held item's name, notices. Health has
// no bar: the screen reddens instead.
fn void draw_hud(Game* game, BlockTable* table, bool captured) => @pool()
{
	Gui* gu = g();
	Pt display = { gu.in.width, gu.in.height };
	DrawList* dl = &gu.foreground;
	Pt c = { math::floor(display.x * 0.5f), math::floor(display.y * 0.5f) };
	Color white = gui::rgba(255, 255, 255);
	// Hurt: red, the more the lower health is (under the rest of the HUD, which stays readable).
	draw_wounds(dl, display, shown_health(game.health, gu.in.dt));
	if (captured) // two bars, 16 x 2
	{
		Color col = gui::rgba(255, 255, 255, 191);
		dl.fill({ c.x - 8, c.y - 1, c.x + 8, c.y + 1 }, col);
		dl.fill({ c.x - 1, c.y - 8, c.x + 1, c.y - 1 }, col);
		dl.fill({ c.x - 1, c.y + 1, c.x + 1, c.y + 8 }, col);
	}
	if (game.mining_progress > 0) // 120 x 8, under the crosshair
	{
		Area r = { c.x - 60, c.y + 22, c.x + 60, c.y + 30 };
		dl.fill(r, gui::rgba(26, 26, 26, 153), 3);
		dl.fill({ r.x0, r.y0, r.x0 + 120 * game.mining_progress, r.y1 }, gui::rgba(178, 196, 222), 3);
	}
	float slot = 52;
	float gap = 4;
	float total = inventory::HOTBAR * slot + (inventory::HOTBAR - 1) * gap;
	float x0 = math::floor((display.x - total) / 2);
	float y0 = display.y - slot - 12;
	for (int i = 0; i < inventory::HOTBAR; i++)
	{
		Area r = { x0 + i * (slot + gap), y0, x0 + i * (slot + gap) + slot, y0 + slot };
		dl.fill(r, gui::rgba(0, 0, 0, 115));
		Stack s = game.inventory.slots[i];
		if (!s.is_empty())
		{
			dl.image({ r.x0 + 8, r.y0 + 8, r.x1 - 8, r.y1 - 8 }, table.icon(content::undyed(s.id)), content::dye_of(s.id));
			if (!game.creative && s.count > 1)
			{
				String n = string::tformat("%d", s.count);
				Pt size = gu.text_size(n);
				outlined_text(dl, { r.x1 - 6 - size.x, r.y1 - 6 - size.y }, white, n);
			}
		}
		if (i == game.selected)
		{
			dl.stroke(r, gui::rgba(255, 255, 255, 242), 0, 3);
		}
		else
		{
			dl.stroke(r, gui::rgba(255, 255, 255, 64));
		}
	}
	// Survival: breath on the right above the hotbar (while it's being used).
	bool breath_bar = !game.creative && game.breath < game::MAX_BREATH;
	if (breath_bar)
	{
		float width = (total - 12.0f) / 2.0f;
		status_bar(dl, { x0 + total - width, y0 - 20 }, { width, 14 }, game.breath / game::MAX_BREATH, gui::rgba(89, 166, 242), gui::rgba(150, 199, 246), "Air");
	}
	// Hurt: a flash at the blow. A notice at the top.
	if (game.since_hurt < 0.4f) dl.fill({ 0, 0, display.x, display.y }, gui::rgba(200, 0, 0, (int)(90 * (1.0f - game.since_hurt / 0.4f))));
	if (game.notice_time > 0)
	{
		Pt size = gu.text_size(game.notice());
		outlined_text(dl, { math::floor(c.x - size.x / 2), 60 }, gui::rgba(255, 255, 255, (int)(255 * math::min(1.0f, game.notice_time))), game.notice());
	}
	// The held item's name above the hotbar (above the bar in survival).
	Stack held = game.inventory.slots[game.selected];
	if (!held.is_empty())
	{
		String name = game.content.display_name(tmem, held.id);
		Pt size = gu.text_size(name);
		float bottom = y0 - 10 - (breath_bar ? 18.0f : 0.0f);
		outlined_text(dl, { math::floor(c.x - size.x / 2), bottom - size.y }, white, name);
	}
}

WorldStats info_last @private;
double info_last_time @private;
double info_busy @private;

// Top left: the status line (frame rate, the time, mode, flying), and with
// F3 the details (the debug screen and the profiler).
fn void draw_info(Renderer* r, Player* player, World* world, ChunkRenderer* chunks, String world_name, uint seed, Game* game, bool captured,
	double day_time, bool details, String net_status) => @pool()
{
	Gui* gu = g();
	DrawList* fg = &gu.foreground;
	String clock = day_cycle::clock_text(day_time, settings.clock_24h);
	outlined_text(fg, { 8, 8 }, gui::rgba(255, 255, 255), string::tformat("%.0f FPS · %s · %s · %s · E inventory%s", gu.framerate, clock,
		game.creative ? "creative" : "survival", player.flying ? "flying (F)" : "walking (F to fly)", net_status));
	float y = 8 + gu.line_height + 4;
	if (!captured)
	{
		outlined_text(fg, { 8, y }, gui::rgba(220, 220, 220),
			"Click to play. W A S D, Space, Shift, Ctrl, F to fly. Left click breaks, right click places, 1-9 or the wheel choose a slot. F3: details");
		y += gu.line_height + 2;
	}
	if (!details) return;
	gu.begin_window("Info", { .pos = { 8, y + 4 }, .alpha = 0.6f, .no_inputs = true });
	WorldStats ws = world.stats();
	gu.text(string::tformat("GoCube %s (C3)  -  %s", VERSION, r.adapter_name));
	gu.text(string::tformat("%.0f fps (%.2f ms)  -  %s", gu.framerate, 1000.0f / math::max(gu.framerate, 0.001f), clock));
	gu.text(string::tformat("Position %.1f, %.1f, %.1f%s  -  seed %d", player.position[0], player.position[1], player.position[2],
		player.flying ? "  (flying)" : "", seed));
	gu.text(string::tformat("Chunks: %d loaded, %d meshed, %d drawn, %d jobs waiting", ws.loaded, ws.meshed, chunks.stats.drawn, ws.queued));
	if (world_name.len)
	{
		gu.text(string::tformat("World: %s  (%d edited chunks not saved yet)", world_name, ws.dirty));
	}
	else
	{
		gu.text_dim("World not saved (use --world=NAME), or someone else's");
	}
	gu.text(string::tformat("%s  -  %d items lying around", game.creative ? "Creative" : "Survival", game.drops.drops.len()));
	// The profiler's timers (what's slow): ms a frame, on average and at worst lately.
	gu.separator();
	gu.text_dim("Frame times");
	gu.same_line(330);
	gu.text_dim("average");
	gu.same_line(410);
	gu.text_dim("worst (2 s)");
	foreach (&e : profiler::profiler.entries)
	{
		DString name;
		name.tinit();
		for (int i = 0; i < math::max(0, e.depth - 1) * 4; i++) name.append_char(' ');
		name.append(e.name);
		gu.text(name.str_view());
		gu.same_line(330);
		gu.text(string::tformat("%6.2f ms", e.average));
		gu.same_line(410);
		gu.text_colored(string::tformat("%6.2f ms", e.worst), e.worst > 1000.0 / 30 ? gui::rgba(255, 150, 110) : gu.theme.text);
	}
	double t = clock::now();
	if (t - info_last_time >= 1.0)
	{
		double span = (t - info_last_time) * 1000.0;
		double work = (ws.generate_ms + ws.light_ms + ws.mesh_ms) - (info_last.generate_ms + info_last.light_ms + info_last.mesh_ms);
		info_busy = ws.workers > 0 ? work / (span * ws.workers) : 0;
		info_last = ws;
		info_last_time = t;
	}
	gu.text(string::tformat("World threads (%d, %.0f%% busy): generating %.2f ms a chunk, light %.2f + meshing %.2f ms a chunk", ws.workers, info_busy * 100.0,
		ws.generated ? ws.generate_ms / (double)ws.generated : 0.0, ws.meshed_jobs ? ws.light_ms / (double)ws.meshed_jobs : 0.0,
		ws.meshed_jobs ? ws.mesh_ms / (double)ws.meshed_jobs : 0.0));
	gu.text_dim("(GPU times are a few frames late; \"present\" is the CPU waiting for the GPU and vsync)");
	gu.end_window();
}

enum PauseChoice : char
{
	NONE,
	RESUME,
	SETTINGS,
	QUIT,
}

// The pause menu (single player: the world waits behind it).
fn PauseChoice pause_menu(bool saved, bool from_menu)
{
	Gui* gu = g();
	gu.background.fill(screen_rect(), gui::rgba(0, 0, 0, 120));
	gu.begin_window("Paused", { .pos = { gu.in.width * 0.5f, gu.in.height * 0.5f }, .pivot = { 0.5f, 0.5f } });
	gu.text("Paused", 1.4f);
	gu.separator();
	gu.space(2);
	PauseChoice choice = NONE;
	if (gu.button("Resume", { 260, 36 })) choice = RESUME;
	if (gu.button("Settings", { 260, 36 })) choice = SETTINGS;
	String quit = from_menu ? (saved ? "Save and quit to title" : "Quit to title") : (saved ? "Save and quit" : "Quit");
	if (gu.button(quit, { 260, 36 })) choice = QUIT;
	gu.text_dim("Esc to resume");
	gu.end_window();
	return choice;
}

// The chat: the lines that came (each fades CHAT_FADE seconds after), what's
// being typed, and what was sent (up and down go through it).
const double CHAT_FADE = 10.0;
const int CHAT_SHOWN = 10;

struct ChatLine
{
	String text; // (heap)
	double at;
}

struct ChatState
{
	Vec{ChatLine} lines; // (the latest 100)
	DString typing;
	Vec{String} sent;    // (the latest 50; heap)
	int sent_at;         // -1: not going through them
	int frames_open;
	bool refocus;        // the field gets the keyboard back next frame
}

fn void ChatState.init(&self)
{
	*self = { .sent_at = -1 };
	self.typing.init(mem);
}

fn void ChatState.free(&self)
{
	foreach (&l : self.lines) free(l.text.ptr);
	self.lines.free();
	foreach (s : self.sent) free(s.ptr);
	self.sent.free();
	self.typing.free();
}

fn void ChatState.add(&self, String line, double at)
{
	self.lines.push({ line, at });
	while (self.lines.len() > 100)
	{
		free(self.lines[0].text.ptr);
		self.lines.remove_at(0);
	}
}

fn bool pressed_key(int vk) @private
{
	foreach (k : g().in.keys) if (k == vk) return true;
	return false;
}

// The chat, bottom left above the hotbar: the lines that came lately (they
// fade), and while it's open the last ones and the line being typed. Enter
// sends it, up and down go through what was sent, Tab completes a command's
// name. False when it was sent.
fn bool draw_chat(ChatState* chat, Session* session, bool open, double now) => @pool()
{
	Gui* gu = g();
	float bottom = gu.in.height - 96.0f; // (above the hotbar)
	float width = math::min(560.0f, gu.in.width - 16.0f);
	float y = bottom - (open ? gu.frame_height() + 24.0f : 0.0f);
	int shown = 0;
	for (sz i = chat.lines.len(); i > 0 && shown < CHAT_SHOWN; i--, shown++)
	{
		ChatLine* l = chat.lines.get_ref(i - 1);
		double age = now - l.at;
		if (!open && age > CHAT_FADE) break;
		float alpha = open ? 1.0f : (float)math::clamp(CHAT_FADE - age, 0.0, 1.0);
		y -= gu.line_height + 2;
		if (open)
		{
			float w = math::max(width, gu.text_size(l.text).x + 8.0f); // (a long line: as long as it)
			gu.background.fill({ 4, y - 1, 4 + w, y + gu.line_height + 1 }, gui::rgba(0, 0, 0, 110));
		}
		outlined_text(&gu.foreground, { 8, y }, gui::rgba(255, 255, 255, (int)(255 * alpha)), l.text);
	}
	if (!open) return true;
	if (++chat.frames_open == 1) return true; // (the key that opened it isn't typed into it)
	if (pressed_key(win::VK_UP) && !chat.sent.is_empty())
	{
		chat.sent_at = math::min(chat.sent_at + 1, (int)chat.sent.len() - 1);
		chat.typing.clear();
		chat.typing.append(chat.sent[chat.sent.len() - 1 - chat.sent_at]);
		chat.refocus = true;
	}
	else if (pressed_key(win::VK_DOWN) && chat.sent_at >= 0)
	{
		chat.sent_at--;
		chat.typing.clear();
		if (chat.sent_at >= 0) chat.typing.append(chat.sent[chat.sent.len() - 1 - chat.sent_at]);
		chat.refocus = true;
	}
	if (pressed_key(win::VK_TAB))
	{
		chat.refocus = true; // (Tab takes the keyboard from the field: give it back)
		String t = chat.typing.str_view();
		if (t.len > 1 && t[0] == '/' && !t.contains_char(' '))
		{
			Vec{String} names;
			defer names.free();
			session.complete_command(t[1..], &names);
			if (names.len() == 1)
			{
				String one = string::tformat("/%s ", names[0]);
				chat.typing.clear();
				chat.typing.append(one);
			}
			else if (!names.is_empty())
			{
				DString list;
				list.tinit();
				foreach (i, n : names) list.appendf("%s/%s", i ? ", " : "", n);
				chat.add(list.str_view().copy(mem), now);
			}
		}
	}
	gu.begin_window("chat", { .pos = { 4, bottom - gu.frame_height() - 20.0f }, .size = { width, gu.frame_height() + 2 * gu.theme.padding.y }, .alpha = 0.8f });
	if (chat.frames_open == 2 || chat.refocus)
	{
		chat.refocus = false;
		gu.focus_next_field();
	}
	gu.set_next_width(-1);
	gu.input_text("##chat", &chat.typing, session::MAX_CHAT, "Say something, or /help");
	gu.end_window();
	if (pressed_key(win::VK_RETURN))
	{
		if (chat.typing.len())
		{
			session.chat(chat.typing.str_view());
			chat.sent.push(chat.typing.str_view().copy(mem));
			if (chat.sent.len() > 50)
			{
				free(chat.sent[0].ptr);
				chat.sent.remove_at(0);
			}
		}
		chat.typing.clear();
		return false;
	}
	return true;
}
