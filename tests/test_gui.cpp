// The game's own UI (ui/gui.h), with a fixed-width "font" (8 x 16 pixels a
// character): clicks, text fields, sliders, drop-downs, scrolling, layout.

#include "ui/gui.h"

#include "check.h"

#include <cmath>
#include <functional>
#include <string>

using namespace gc::ui;

namespace {

size_t chars(std::string_view s) {
	size_t n = 0;
	for (unsigned char c : s) {
		n += (c & 0xc0) != 0x80;
	}
	return n;
}

Vec2 measure(std::string_view text, float size, float wrap) {
	const float w = float(chars(text)) * size / 15.0f * 8.0f, h = size / 15.0f * 16.0f;
	if (wrap > 0 && w > wrap) {
		return {wrap, h * std::ceil(w / wrap)};
	}
	return {w, h};
}

struct Ui {
	Gui gui{measure};
	Input in;
	std::vector<DrawCmd> cmds;
	Ui() {
		// (The layout these tests measure: fixed here, whatever the game's theme.)
		gui.theme.font = 15;
		gui.theme.padding = {10, 10};
		gui.theme.spacing = {8, 5};
		gui.theme.frame = {7, 4};
		in.width = 800;
		in.height = 600;
		in.mouse = {-100, -100};
	}
	// One frame; presses, typing and keys last one frame.
	void frame(const std::function<void(Gui &)> &f) {
		gui.begin_frame(in);
		f(gui);
		cmds = gui.end_frame();
		in.clicks[0] = in.clicks[1] = in.clicks[2] = 0;
		in.text.clear();
		in.keys.clear();
		in.wheel = 0;
	}
	void press() {
		in.down[0] = true;
		in.clicks[0] = 1;
	}
	void release() { in.down[0] = false; }
	bool drew_text(std::string_view t) const {
		for (const DrawCmd &c : cmds) {
			if (c.type == DrawCmd::TEXT && c.text == t) {
				return true;
			}
		}
		return false;
	}
};

WindowOptions at(float x, float y, float w = 400, float h = 300) {
	WindowOptions o;
	o.pos = {x, y};
	o.size = {w, h};
	return o;
}

} // namespace

TEST_CASE("gui: a button clicks on release over it, not when let go elsewhere, nor when disabled") {
	Ui ui;
	Rect button;
	bool clicked = false, other = false;
	auto draw = [&](Gui &g) {
		g.begin_window("w", at(100, 100));
		clicked = g.button("Play", {100, 30});
		button = g.item_rect();
		g.begin_disabled(true);
		other = g.button("Nope", {100, 30});
		g.end_disabled();
		g.end_window();
	};
	ui.frame(draw);
	CHECK(button.x0 == 110); // inside the padding
	CHECK(button.y0 == 110);
	ui.in.mouse = {button.x0 + 5, button.y0 + 5};
	ui.frame(draw); // (hovering: windows take the mouse from the frame before)
	CHECK(ui.gui.wants_mouse());
	ui.press();
	ui.frame(draw);
	CHECK_FALSE(clicked); // down
	ui.release();
	ui.frame(draw);
	CHECK(clicked); // up, over it
	// Pressed, dragged off, let go: nothing.
	ui.press();
	ui.frame(draw);
	ui.in.mouse = {700, 500};
	ui.release();
	ui.frame(draw);
	CHECK_FALSE(clicked);
	// Press and release within one frame (a quick click) counts.
	ui.in.mouse = {button.x0 + 5, button.y0 + 5};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(clicked);
	// Disabled: never.
	ui.in.mouse = {button.x0 + 5, button.y1 + 10};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK_FALSE(other);
}

TEST_CASE("gui: the same label in different id scopes are different buttons; the top window takes the mouse") {
	Ui ui;
	bool a = false, b = false, under = false;
	Rect ra, rb;
	auto draw = [&](Gui &g) {
		g.begin_window("back", at(0, 0, 500, 500));
		under = g.button("Under", {400, 400});
		g.end_window();
		g.begin_window("front", at(50, 50, 300, 200));
		g.push_id(1);
		a = g.button("Same", {80, 30});
		ra = g.item_rect();
		g.pop_id();
		g.same_line();
		g.push_id(2);
		b = g.button("Same", {80, 30});
		rb = g.item_rect();
		g.pop_id();
		g.end_window();
	};
	ui.frame(draw);
	CHECK(rb.x0 == ra.x1 + 8); // same line, the spacing between
	CHECK(rb.y0 == ra.y0);
	ui.in.mouse = {rb.x0 + 3, rb.y0 + 3};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(b);
	CHECK_FALSE(a);
	CHECK_FALSE(under); // (the window behind doesn't get it)
}

TEST_CASE("gui: checkbox, slider, drop-down") {
	Ui ui;
	bool on = false;
	int value = 5, mode = 0;
	bool changed = false;
	Rect check, slider, combo;
	auto draw = [&](Gui &g) {
		g.begin_window("w", at(0, 0, 600, 400));
		g.checkbox("Fly", on);
		check = g.item_rect();
		g.next_width(200);
		g.slider("##s", value, 0, 10);
		slider = g.item_rect();
		g.next_width(150);
		changed = g.combo("Mode", mode, {"Survival", "Creative", "Spectator"});
		combo = g.item_rect();
		g.end_window();
	};
	ui.frame(draw);
	ui.in.mouse = {check.x0 + 4, check.y0 + 4};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(on);
	// Dragging the slider: to the far left, then the far right.
	ui.in.mouse = {slider.x0 + 100, slider.y0 + 5};
	ui.frame(draw);
	ui.press();
	ui.frame(draw);
	ui.in.mouse = {slider.x0 - 50, slider.y0 + 5}; // (past its end, still held)
	ui.frame(draw);
	CHECK(value == 0);
	ui.in.mouse = {slider.x1 + 50, slider.y0 + 5};
	ui.frame(draw);
	CHECK(value == 10);
	ui.release();
	ui.frame(draw);
	// The drop-down: open it, pick the second.
	ui.in.mouse = {combo.x0 + 10, combo.y0 + 5};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(ui.drew_text("Spectator")); // open
	const float row = 16 + 6;
	ui.in.mouse = {combo.x0 + 10, combo.y1 + 2 + 3 + row * 1.5f};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(changed);
	CHECK(mode == 1);
	ui.frame(draw);
	CHECK_FALSE(ui.drew_text("Spectator")); // closed
	// Opened, then a click elsewhere: closed, unchanged.
	ui.in.mouse = {combo.x0 + 10, combo.y0 + 5};
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(ui.drew_text("Spectator"));
	ui.in.mouse = {550, 350};
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	ui.frame(draw);
	CHECK_FALSE(ui.drew_text("Spectator"));
	CHECK(mode == 1);
}

TEST_CASE("gui: a text field: typing, the caret, limits, digits, paste, UTF-8") {
	Ui ui;
	std::string name = "ab", seed;
	Rect field, seed_field;
	auto draw = [&](Gui &g) {
		g.begin_window("w", at(0, 0, 600, 400));
		g.next_width(200);
		g.input_text("Name", name, 6);
		field = g.item_rect();
		g.next_width(200);
		g.input_text("Seed", seed, 10, "random", true);
		seed_field = g.item_rect();
		g.end_window();
	};
	ui.gui.clipboard = [] { return std::string("xyz\n"); };
	ui.frame(draw);
	CHECK(ui.drew_text("random")); // the hint, while empty
	ui.in.mouse = {field.x0 + 150, field.y0 + 5}; // (past the end: the caret goes last)
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(ui.gui.typing());
	ui.in.text = "c";
	ui.frame(draw);
	CHECK(name == "abc");
	ui.in.keys = {KEY_LEFT, KEY_LEFT};
	ui.frame(draw);
	ui.in.text = "X";
	ui.frame(draw);
	CHECK(name == "aXbc");
	ui.in.keys = {KEY_BACK};
	ui.frame(draw);
	CHECK(name == "abc");
	ui.in.keys = {KEY_DELETE};
	ui.frame(draw);
	CHECK(name == "ac");
	ui.in.keys = {KEY_HOME};
	ui.frame(draw);
	ui.in.text = "\xc3\xa9"; // é: two bytes
	ui.frame(draw);
	CHECK(name == "\xc3\xa9" "ac");
	ui.in.keys = {KEY_END};
	ui.in.ctrl = true;
	ui.frame(draw);
	ui.in.keys = {KEY_V}; // paste "xyz\n": only what fits (6 bytes), no new line
	ui.frame(draw);
	ui.in.ctrl = false;
	CHECK(name == "\xc3\xa9" "acxy");
	ui.in.keys = {KEY_BACK};
	ui.frame(draw);
	ui.in.text = "\xc3\xa9"; // (doesn't fit: 2 bytes, 1 left)
	ui.frame(draw);
	CHECK(name == "\xc3\xa9" "acx");
	ui.in.keys = {KEY_HOME, KEY_RIGHT, KEY_BACK}; // a whole character goes
	ui.frame(draw);
	CHECK(name == "acx");
	ui.in.keys = {KEY_ENTER};
	ui.frame(draw);
	CHECK_FALSE(ui.gui.typing());
	ui.in.text = "zz";
	ui.frame(draw);
	CHECK(name == "acx"); // (not typing any more)
	// Digits only.
	ui.in.mouse = {seed_field.x0 + 5, seed_field.y0 + 5};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	ui.in.text = "4a2-7";
	ui.frame(draw);
	CHECK(seed == "427");
	// A click elsewhere: done typing.
	ui.in.mouse = {500, 350};
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK_FALSE(ui.gui.typing());
}

TEST_CASE("gui: a selectable's double click; a scrolling region") {
	Ui ui;
	int clicks = 0, doubles = 0;
	Rect first, region;
	bool last_hovered = false;
	auto draw = [&](Gui &g) {
		g.begin_window("w", at(0, 0, 400, 400));
		g.begin_child("list", {0, 100}, true);
		for (int i = 0; i < 20; i++) {
			g.push_id(i);
			if (g.selectable("World " + std::to_string(i), false)) {
				clicks++;
				doubles += g.double_clicked();
			}
			if (i == 0) {
				first = g.item_rect();
			}
			if (i == 19) {
				last_hovered = g.item_hovered();
			}
			g.pop_id();
		}
		g.end_child();
		region = g.item_rect();
		g.end_window();
	};
	ui.frame(draw);
	CHECK(region.height() == 100);
	ui.in.mouse = {first.x0 + 5, first.y0 + 5};
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.in.dt = 0.1f;
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(clicks == 2);
	CHECK(doubles == 1);
	ui.in.dt = 1.0f; // (a slow second click isn't a double one)
	ui.frame(draw);
	ui.in.clicks[0] = 1;
	ui.frame(draw);
	CHECK(doubles == 1);
	// Scrolled down by the wheel: the first row moves up and out.
	const float before = first.y0;
	ui.in.wheel = -10;
	ui.frame(draw);
	ui.frame(draw);
	CHECK(first.y0 < before - 100);
	// The last row is in view and hoverable; nothing outside the region is.
	ui.in.mouse = {region.x0 + 10, region.y1 - 8};
	ui.frame(draw);
	ui.frame(draw);
	CHECK(last_hovered);
}

TEST_CASE("gui: a window that fits its content shows from its second frame; groups and wrapped text") {
	Ui ui;
	Rect group, after;
	auto draw = [&](Gui &g) {
		WindowOptions o;
		o.pos = {400, 300};
		o.pivot = {0.5f, 0.5f};
		g.begin_window("fit", o);
		g.begin_group();
		g.text("one");
		g.text("three");
		g.end_group();
		group = g.item_rect();
		g.same_line();
		g.text("beside");
		after = g.item_rect();
		g.end_window();
	};
	ui.frame(draw);
	CHECK_FALSE(ui.drew_text("one")); // (its size isn't known yet)
	ui.frame(draw);
	CHECK(ui.drew_text("one"));
	CHECK(group.width() == 5 * 8);
	CHECK(group.height() == 16 + 5 + 16);
	CHECK(after.x0 == group.x1 + 8);
	CHECK(after.y0 == group.y0);
	// Centred: the window's width is the content's and the padding.
	const float width = 5 * 8 + 8 + 6 * 8 + 2 * 10;
	CHECK(group.x0 == std::floor(400 - width / 2) + 10);
	// Wrapped text takes the lines it needs.
	ui.frame([&](Gui &g) {
		g.begin_window("w", at(0, 0, 120, 300));
		g.text_wrapped("twenty characters!!!", rgba(255, 255, 255));
		group = g.item_rect();
		g.end_window();
	});
	CHECK(group.height() == 2 * 16);
}

TEST_CASE("gui: a tooltip is drawn last, by the mouse, inside the screen") {
	Ui ui;
	ui.in.mouse = {790, 590};
	ui.frame([](Gui &g) {
		g.foreground().fill({0, 0, 10, 10}, rgba(1, 2, 3));
		g.tooltip("Stone");
	});
	REQUIRE(!ui.cmds.empty());
	CHECK(ui.cmds.back().color.r == 1); // (the foreground list is over even the tooltip)
	bool found = false;
	for (const DrawCmd &c : ui.cmds) {
		if (c.type == DrawCmd::TEXT && c.text == "Stone") {
			found = true;
			CHECK(c.rect.x0 + 5 * 8 <= 800);
			CHECK(c.rect.y0 + 16 <= 600);
		}
	}
	CHECK(found);
}
