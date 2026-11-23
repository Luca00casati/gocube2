// The game's own UI (ui/gui), with a fixed-width "font" (8 x 16 pixels a
// character at 15): clicks, controls, text fields.
module gocube::test::gui_test;
import gocube::gui, gocube::util::vec;
import std::math, std::io;

fn Pt measure(void* context, String text, float size, float wrap)
{
	sz n = 0;
	foreach (c : text) n += (c & 0xc0) != 0x80 ? 1 : 0;
	float w = (float)n * size / 15.0f * 8.0f;
	float h = size / 15.0f * 16.0f;
	if (wrap > 0 && w > wrap) return { wrap, h * math::ceil(w / wrap) };
	return { w, h };
}

alias DrawFn = fn void(Gui* g, void* state);

struct Ui
{
	Gui gui;
	Input in;
	DrawCmds cmds;
	DString text; // what's typed this frame
	Vec{int} keys;
}

fn Ui* new_ui()
{
	Ui* ui = mem::new(Ui);
	ui.gui.init(&measure, null);
	// (The layout these tests measure: fixed here, whatever the game's theme.)
	ui.gui.theme.font = 15;
	ui.gui.theme.padding = { 10, 10 };
	ui.gui.theme.spacing = { 8, 5 };
	ui.gui.theme.frame = { 7, 4 };
	ui.in = gui::input();
	ui.in.width = 800;
	ui.in.height = 600;
	ui.in.mouse = { -100, -100 };
	ui.text.init(mem);
	return ui;
}

fn void Ui.free(&self)
{
	gui::free_cmds(&self.cmds);
	self.cmds.free();
	self.gui.free();
	self.text.free();
	self.keys.free();
	free(self);
}

// One frame; presses, typing and keys last one frame.
fn void Ui.frame(&self, DrawFn draw, void* state)
{
	self.in.text = self.text.str_view();
	self.in.keys = self.keys.array_view();
	self.gui.begin_frame(self.in);
	draw(&self.gui, state);
	gui::free_cmds(&self.cmds);
	self.gui.end_frame(&self.cmds);
	self.in.clicks = {};
	self.text.clear();
	self.keys.clear();
	self.in.wheel = 0;
}

fn void Ui.press(&self)
{
	self.in.down[0] = true;
	self.in.clicks[0] = 1;
}

fn void Ui.release(&self) => self.in.down[0] = false;

fn bool Ui.drew_text(&self, String t)
{
	foreach (&c : self.cmds) if (c.type == TEXT && c.text == t) return true;
	return false;
}

fn WindowOptions at(float x, float y, float w = 400, float h = 300) => { .pos = { x, y }, .size = { w, h } };

struct Buttons
{
	Area button;
	bool clicked, other;
}

fn void a_button_clicks_on_release() @test
{
	Ui* ui = new_ui();
	defer ui.free();
	Buttons s;
	DrawFn draw = fn void(Gui* g, void* x)
	{
		Buttons* s = x;
		g.begin_window("w", at(100, 100));
		s.clicked = g.button("Play", { 100, 30 });
		s.button = g.item_rect();
		g.begin_disabled(true);
		s.other = g.button("Nope", { 100, 30 });
		g.end_disabled();
		g.end_window();
	};
	ui.frame(draw, &s);
	assert(s.button.x0 == 110 && s.button.y0 == 110); // inside the padding
	ui.in.mouse = { s.button.x0 + 5, s.button.y0 + 5 };
	ui.frame(draw, &s); // (hovering: windows take the mouse from the frame before)
	assert(ui.gui.wants_mouse);
	ui.press();
	ui.frame(draw, &s);
	assert(!s.clicked); // down
	ui.release();
	ui.frame(draw, &s);
	assert(s.clicked); // up, over it
	// Pressed, dragged off, let go: nothing.
	ui.press();
	ui.frame(draw, &s);
	ui.in.mouse = { 700, 500 };
	ui.release();
	ui.frame(draw, &s);
	assert(!s.clicked);
	// Press and release within one frame (a quick click) counts.
	ui.in.mouse = { s.button.x0 + 5, s.button.y0 + 5 };
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(s.clicked);
	// Disabled: never.
	ui.in.mouse = { s.button.x0 + 5, s.button.y1 + 10 };
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(!s.other);
}

struct Controls
{
	bool on;
	int value, mode;
	bool changed;
	Area check, slider, combo;
}

fn void checkbox_slider_drop_down() @test
{
	Ui* ui = new_ui();
	defer ui.free();
	Controls s = { .value = 5 };
	DrawFn draw = fn void(Gui* g, void* x)
	{
		Controls* s = x;
		g.begin_window("w", at(0, 0, 600, 400));
		g.checkbox("Fly", &s.on);
		s.check = g.item_rect();
		g.set_next_width(200);
		g.slider_int("##s", &s.value, 0, 10);
		s.slider = g.item_rect();
		g.set_next_width(150);
		s.changed = g.combo("Mode", &s.mode, { "Survival", "Creative", "Spectator" });
		s.combo = g.item_rect();
		g.end_window();
	};
	ui.frame(draw, &s);
	ui.in.mouse = { s.check.x0 + 4, s.check.y0 + 4 };
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(s.on);
	// Dragging the slider: to the far left, then the far right.
	ui.in.mouse = { s.slider.x0 + 100, s.slider.y0 + 5 };
	ui.frame(draw, &s);
	ui.press();
	ui.frame(draw, &s);
	ui.in.mouse = { s.slider.x0 - 50, s.slider.y0 + 5 }; // (past its end, still held)
	ui.frame(draw, &s);
	assert(s.value == 0);
	ui.in.mouse = { s.slider.x1 + 50, s.slider.y0 + 5 };
	ui.frame(draw, &s);
	assert(s.value == 10);
	ui.release();
	ui.frame(draw, &s);
	// The drop-down: open it, pick the second.
	ui.in.mouse = { s.combo.x0 + 10, s.combo.y0 + 5 };
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(ui.drew_text("Spectator")); // open
	float row = 16 + 6;
	ui.in.mouse = { s.combo.x0 + 10, s.combo.y1 + 2 + 3 + row * 1.5f };
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(s.changed && s.mode == 1);
	ui.frame(draw, &s);
	assert(!ui.drew_text("Spectator")); // closed
	// Opened, then a click elsewhere: closed, unchanged.
	ui.in.mouse = { s.combo.x0 + 10, s.combo.y0 + 5 };
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(ui.drew_text("Spectator"));
	ui.in.mouse = { 550, 350 };
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	ui.frame(draw, &s);
	assert(!ui.drew_text("Spectator") && s.mode == 1);
}

struct Fields
{
	DString name, seed;
	Area field, seed_field;
}

fn void a_text_field() @test
{
	Ui* ui = new_ui();
	defer ui.free();
	Fields s;
	s.name.init(mem);
	s.seed.init(mem);
	defer
	{
		s.name.free();
		s.seed.free();
	}
	s.name.append("ab");
	DrawFn draw = fn void(Gui* g, void* x)
	{
		Fields* s = x;
		g.begin_window("w", at(0, 0, 600, 400));
		g.set_next_width(200);
		g.input_text("Name", &s.name, 6);
		s.field = g.item_rect();
		g.set_next_width(200);
		g.input_text("Seed", &s.seed, 10, "random", true);
		s.seed_field = g.item_rect();
		g.end_window();
	};
	ui.gui.clipboard = fn String(void* c) => "xyz\n";
	ui.frame(draw, &s);
	assert(ui.drew_text("random")); // the hint, while empty
	ui.in.mouse = { s.field.x0 + 150, s.field.y0 + 5 }; // (past the end: the caret goes last)
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(ui.gui.typing());
	ui.text.append("c");
	ui.frame(draw, &s);
	assert(s.name.str_view() == "abc");
	ui.keys.push(gui::KEY_LEFT);
	ui.keys.push(gui::KEY_LEFT);
	ui.frame(draw, &s);
	ui.text.append("X");
	ui.frame(draw, &s);
	assert(s.name.str_view() == "aXbc");
	ui.keys.push(gui::KEY_BACK);
	ui.frame(draw, &s);
	assert(s.name.str_view() == "abc");
	ui.keys.push(gui::KEY_DELETE);
	ui.frame(draw, &s);
	assert(s.name.str_view() == "ac");
	ui.keys.push(gui::KEY_HOME);
	ui.frame(draw, &s);
	ui.text.append("\xc3\xa9"); // é: two bytes
	ui.frame(draw, &s);
	assert(s.name.str_view() == "\xc3\xa9" "ac");
	ui.keys.push(gui::KEY_END);
	ui.in.ctrl = true;
	ui.frame(draw, &s);
	ui.keys.push(gui::KEY_V); // paste "xyz\n": only what fits (6 bytes), no new line
	ui.frame(draw, &s);
	ui.in.ctrl = false;
	assert(s.name.str_view() == "\xc3\xa9" "acxy", "%s", s.name.str_view());
	ui.keys.push(gui::KEY_BACK);
	ui.frame(draw, &s);
	ui.text.append("\xc3\xa9"); // (doesn't fit: 2 bytes, 1 left)
	ui.frame(draw, &s);
	assert(s.name.str_view() == "\xc3\xa9" "acx");
	ui.keys.push(gui::KEY_HOME);
	ui.keys.push(gui::KEY_RIGHT);
	ui.keys.push(gui::KEY_BACK); // a whole character goes
	ui.frame(draw, &s);
	assert(s.name.str_view() == "acx");
	ui.keys.push(gui::KEY_ENTER);
	ui.frame(draw, &s);
	assert(!ui.gui.typing());
	ui.text.append("zz");
	ui.frame(draw, &s);
	assert(s.name.str_view() == "acx"); // (not typing any more)
	// Digits only.
	ui.in.mouse = { s.seed_field.x0 + 5, s.seed_field.y0 + 5 };
	ui.frame(draw, &s);
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	ui.text.append("4a2-7");
	ui.frame(draw, &s);
	assert(s.seed.str_view() == "427");
	// A click elsewhere: done typing.
	ui.in.mouse = { 500, 350 };
	ui.in.clicks[0] = 1;
	ui.frame(draw, &s);
	assert(!ui.gui.typing());
}
