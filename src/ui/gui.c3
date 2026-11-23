// The game's own immediate-mode UI: windows, layout, text, buttons,
// checkboxes, sliders, drop-down lists, text fields, scrolling regions and
// tooltips. Each frame the screens call it, it works out what is hovered and
// clicked from the mouse and keys, and it records what to draw (rectangles,
// gradients, text, item icons) for a backend to draw (ui/gui_d3d: Direct3D
// 11, text rasterized by GDI). Text is measured through a callback, so tests
// run it with a fixed-width "font".
//
// A widget's label is also its id, unique within its window and push_id
// scope; "##" starts the part that is only id ("Address##join" shows
// "Address"; "##view" shows nothing). Windows that fit their content take
// their size from the frame before (they aren't drawn the first frame).
module gocube::gui;
import gocube::util::vec, gocube::util::map;
import std::math;

struct Pt
{
	float x, y;
}

struct Area
{
	float x0, y0, x1, y1;
}

fn bool Area.contains(&self, Pt p) => p.x >= self.x0 && p.x < self.x1 && p.y >= self.y0 && p.y < self.y1;
fn float Area.width(&self) => self.x1 - self.x0;
fn float Area.height(&self) => self.y1 - self.y0;

struct Color
{
	char r, g, b, a;
}

fn Color rgba(int r, int g, int b, int a = 255) => { (char)r, (char)g, (char)b, (char)a };

enum CmdType : char
{
	FILL,
	STROKE,
	GRADIENT,
	TEXT,
	IMAGE,
	CLIP,
	UNCLIP,
}

// What to draw, in order.
struct DrawCmd
{
	CmdType type;
	Area rect;
	Color color, color2; // GRADIENT: from color to color2
	bool vertical;       // GRADIENT: top to bottom (else left to right)
	float rounding, thickness;
	float size;          // TEXT: font size in pixels
	float wrap;          // TEXT: wrap width (0: one line)
	String text;         // (heap)
	int image;           // IMAGE: an item icon (an atlas tile)
	uint dye;            // IMAGE: a dyed block's (0xFFRRGGBB, 0 none)
}

alias DrawCmds = Vec{DrawCmd};

fn void free_cmds(DrawCmds* cmds)
{
	foreach (&c : *cmds) if (c.text.len) free(c.text.ptr);
	cmds.clear();
}

struct DrawList
{
	DrawCmds cmds;
	float alpha;     // multiplies every colour added (disabled widgets)
	float font_size;
}

fn Color with_alpha(Color c, float alpha) @private
{
	c.a = (char)math::round((float)c.a * math::clamp(alpha, 0.0f, 1.0f));
	return c;
}

fn void DrawList.fill(&self, Area r, Color c, float rounding = 0) => self.cmds.push({ .type = FILL, .rect = r, .color = with_alpha(c, self.alpha), .rounding = rounding, .thickness = 1, .image = -1 });
fn void DrawList.stroke(&self, Area r, Color c, float rounding = 0, float thickness = 1)
	=> self.cmds.push({ .type = STROKE, .rect = r, .color = with_alpha(c, self.alpha), .rounding = rounding, .thickness = thickness, .image = -1 });
fn void DrawList.gradient(&self, Area r, Color from, Color to, bool vertical)
	=> self.cmds.push({ .type = GRADIENT, .rect = r, .color = with_alpha(from, self.alpha), .color2 = with_alpha(to, self.alpha), .vertical = vertical, .thickness = 1, .image = -1 });

fn void DrawList.text(&self, Pt at, Color c, String text, float size, float wrap = 0)
{
	if (!text.len) return;
	self.cmds.push({ .type = TEXT, .rect = { at.x, at.y, at.x, at.y }, .color = with_alpha(c, self.alpha), .size = size > 0 ? size : self.font_size,
		.wrap = wrap, .text = text.copy(mem), .thickness = 1, .image = -1 });
}

fn void DrawList.image(&self, Area r, int tile, uint dye = 0)
	=> self.cmds.push({ .type = IMAGE, .rect = r, .color = with_alpha(rgba(255, 255, 255), self.alpha), .image = tile, .dye = dye, .thickness = 1 });
fn void DrawList.clip(&self, Area r) => self.cmds.push({ .type = CLIP, .rect = r, .image = -1 });
fn void DrawList.unclip(&self) => self.cmds.push({ .type = UNCLIP, .image = -1 });

// Text with a black outline (the HUD's).
fn void DrawList.outlined_text(&self, Pt at, Color c, String t, float size)
{
	Pt[6] offsets = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, 1 } };
	foreach (o : offsets) self.text({ at.x + o.x, at.y + o.y }, rgba(0, 0, 0, c.a), t, size);
	self.text(at, c, t, size);
}

fn void DrawList.reset(&self, float font)
{
	free_cmds(&self.cmds);
	self.alpha = 1;
	self.font_size = font;
}

fn void DrawList.free(&self)
{
	free_cmds(&self.cmds);
	self.cmds.free();
}

// One frame's input.
struct Input
{
	float width, height; // the screen
	float dt;
	Pt mouse;            // (far away: not over the window, or captured)
	bool[3] down;        // left, right, middle
	int[3] clicks;       // presses since the last frame
	float wheel;         // notches (positive: away from you)
	String text;         // typed (UTF-8; the caller's, for the frame)
	int[] keys;          // keys pressed, as virtual-key codes (repeats too)
	bool ctrl;
}

fn Input input() => { .dt = 1.0f / 60.0f, .mouse = { -1e6f, -1e6f } };

// Virtual-key codes the text field knows (as Windows').
const int KEY_BACK = 0x08;
const int KEY_TAB = 0x09;
const int KEY_ENTER = 0x0d;
const int KEY_ESCAPE = 0x1b;
const int KEY_END = 0x23;
const int KEY_HOME = 0x24;
const int KEY_LEFT = 0x25;
const int KEY_RIGHT = 0x27;
const int KEY_DELETE = 0x2e;
const int KEY_V = 0x56;

// "Minimal glass": dark see-through panels over the world, thin light
// outlines, white text; what is hovered or chosen brightens (no colour).
struct Theme
{
	float font;
	Pt padding;  // inside windows
	Pt spacing;  // between items
	Pt frame;    // inside buttons and fields
	float rounding;
	Color text, text_dim, window, border, frame_bg, frame_hover, line, line_hover, button, button_hover, button_active, accent, selected, popup;
	float disabled_alpha;
}

fn Theme theme() => {
	.font = 16, .padding = { 20, 18 }, .spacing = { 8, 6 }, .frame = { 10, 6 }, .rounding = 6,
	.text = rgba(245, 245, 245), .text_dim = rgba(255, 255, 255, 130), .window = rgba(6, 8, 12, 165), .border = rgba(255, 255, 255, 34),
	.frame_bg = rgba(255, 255, 255, 12), .frame_hover = rgba(255, 255, 255, 26), .line = rgba(255, 255, 255, 60), .line_hover = rgba(255, 255, 255, 200),
	.button = rgba(255, 255, 255, 14), .button_hover = rgba(255, 255, 255, 38), .button_active = rgba(255, 255, 255, 64), .accent = rgba(255, 255, 255, 235),
	.selected = rgba(255, 255, 255, 24), .popup = rgba(10, 12, 16, 235), .disabled_alpha = 0.3f };

struct WindowOptions
{
	Pt pos;          // where `pivot` of the window goes
	Pt pivot;        // (0.5, 0.5): centred on `pos`
	Pt size;         // 0: fit the content (that axis)
	bool no_background;
	float alpha;     // of the background (0: 1)
	bool no_inputs;  // the mouse goes through it
}

// Width and height of `text` at `size` pixels, wrapped at `wrap` (0: one line).
alias MeasureFn = fn Pt(void* context, String text, float size, float wrap);
// What the clipboard holds (for Ctrl+V; temp).
alias ClipboardFn = fn String(void* context);

struct Region @private // a window or a child region in it
{
	uint id;
	Area rect, clip;
	Pt content_origin; // content's top left (screen, scrolled)
	Pt cursor;
	float indent;      // where new lines start (screen x)
	float line_y, line_h, prev_x;
	bool same_line;
	Pt max;            // furthest content reached (screen)
	float scroll;      // child regions
	bool hovered;
	bool fit_w, fit_h;
	DrawList* list;
}

struct Group @private
{
	Pt start;
	float indent;
	Pt max;
	float line_y, line_h;
	bool same_line;
	sz depth; // the region it is in (regions.len())
}

struct Remembered @private // per window / child, from the frame before
{
	Pt size;
	float content_h;
	float scroll;
	int seen; // the frame it was last shown
}

struct WindowRect @private
{
	uint id;
	Area rect;
}

const uint POPUP_WINDOW @private = 1; // hovered_window when the mouse is over an open drop-down
const float DOUBLE_CLICK_SECONDS @private = 0.45f;

struct Gui
{
	Theme theme;
	MeasureFn measure;
	void* measure_context;
	ClipboardFn clipboard;
	void* clipboard_context;
	Input in;
	DrawList background, foreground, popup, tooltip_list;
	Vec{DrawList*} lists; // the windows', in order (reused: `list_count` used this frame)
	sz list_count;
	Vec{bool} hidden;     // per window list: not drawn (fits its content, first frame)
	Vec{Region} regions;
	Vec{Group} groups;
	Vec{uint} ids;
	Vec{Pt} spacings;
	Vec{bool} disabled;
	Map{uint, Remembered} remembered;
	int frame;
	float framerate;
	float line_height;
	float next_width;
	bool has_next_width;
	bool align_next_text;
	bool wants_mouse;
	uint hovered_window; // from the window rects of the frame before
	uint active;         // pressed on, button still down
	uint focus;          // the text field typed in
	sz caret;
	float caret_time;
	uint open_popup;     // the drop-down open
	Area popup_rect, prev_popup_rect;
	bool popup_drawn;
	bool last_hovered, last_hovered_any;
	Area last_rect;
	float click_time;    // since the last left click
	Pt click_pos;
	bool double_click;
	DString tooltip_text;
	Vec{WindowRect} rects, prev_rects; // windows taking the mouse
	bool focus_claimed;  // a text field was clicked this frame
	bool popup_claimed;  // its drop-down was clicked this frame
	bool wheel_used;
	bool focus_next;
}

fn void Gui.init(&self, MeasureFn measure, void* context)
{
	*self = { .theme = theme(), .measure = measure, .measure_context = context, .framerate = 60, .line_height = 18, .click_time = 1e9f };
	self.tooltip_text.init(mem);
}

fn void Gui.free(&self)
{
	self.background.free();
	self.foreground.free();
	self.popup.free();
	self.tooltip_list.free();
	foreach (l : self.lists)
	{
		l.free();
		free(l);
	}
	self.lists.free();
	self.hidden.free();
	self.regions.free();
	self.groups.free();
	self.ids.free();
	self.spacings.free();
	self.disabled.free();
	self.remembered.free();
	self.tooltip_text.free();
	self.rects.free();
	self.prev_rects.free();
}

fn uint fnv(String s, uint seed) @private
{
	uint h = 2166136261u ^ (seed * 16777619u);
	foreach (c : s) h = (h ^ c) * 16777619u;
	return h < 2 ? h + 2 : h; // (0: no id; 1: the drop-down)
}

fn Area intersect(Area a, Area b) @private => { math::max(a.x0, b.x0), math::max(a.y0, b.y0), math::min(a.x1, b.x1), math::min(a.y1, b.y1) };

// UTF-8: the start of the character before / after byte `i`.
fn sz prev_char(String s, sz i) @private
{
	if (i == 0) return 0;
	i--;
	while (i > 0 && (s[i] & 0xc0) == 0x80) i--;
	return i;
}

fn sz next_char(String s, sz i) @private
{
	if (i >= s.len) return s.len;
	i++;
	while (i < s.len && (s[i] & 0xc0) == 0x80) i++;
	return i;
}

// The whole characters of `text` that fit in `room` bytes.
fn String fit_bytes(String text, sz room) @private
{
	if (text.len <= room) return text;
	sz n = room;
	while (n > 0 && (text[n] & 0xc0) == 0x80) n--;
	return text[:n];
}

fn Pt Gui.text_size(&self, String text, float scale = 1, float wrap = 0)
{
	if (!text.len) return { 0, self.line_height * scale };
	return self.measure(self.measure_context, text, self.theme.font * scale, wrap);
}

fn float Gui.frame_height(&self) => self.line_height + 2 * self.theme.frame.y;
// The mouse is over a window (or busy with a widget): clicks are the UI's.
fn bool Gui.typing(&self) => self.focus != 0;

fn void Gui.begin_frame(&self, Input in)
{
	self.in = in;
	self.frame++;
	if (in.dt > 0) self.framerate = self.framerate * 0.95f + (1.0f / in.dt) * 0.05f;
	self.line_height = math::max(1.0f, self.measure(self.measure_context, "Ag", self.theme.font, 0).y);
	self.caret_time += in.dt;
	// A double click: a second press soon after the first, near it.
	self.click_time += in.dt;
	self.double_click = false;
	if (in.clicks[0] > 0)
	{
		float dx = in.mouse.x - self.click_pos.x;
		float dy = in.mouse.y - self.click_pos.y;
		self.double_click = in.clicks[0] > 1 || (self.click_time < DOUBLE_CLICK_SECONDS && dx * dx + dy * dy < 36);
		self.click_time = self.double_click ? 1e9f : 0; // (a third click starts again)
		self.click_pos = in.mouse;
	}
	// The window under the mouse: the topmost one of the frame before (or the open drop-down).
	self.hovered_window = 0;
	foreach (r : self.prev_rects)
	{
		if (r.rect.contains(in.mouse)) self.hovered_window = r.id;
	}
	if (self.open_popup && self.prev_popup_rect.contains(in.mouse)) self.hovered_window = POPUP_WINDOW;
	self.wants_mouse = self.hovered_window != 0 || self.active != 0;
	self.background.reset(self.theme.font);
	self.foreground.reset(self.theme.font);
	self.popup.reset(self.theme.font);
	self.tooltip_list.reset(self.theme.font);
	for (sz i = 0; i < self.list_count; i++) self.lists[i].reset(self.theme.font);
	self.list_count = 0;
	self.hidden.clear();
	self.rects.clear();
	self.regions.clear();
	self.groups.clear();
	self.ids.clear();
	self.disabled.clear();
	self.spacings.clear();
	self.tooltip_text.clear();
	self.popup_drawn = false;
	self.focus_claimed = false;
	self.popup_claimed = false;
	self.wheel_used = false;
	self.has_next_width = false;
	self.align_next_text = false;
	self.last_hovered = self.last_hovered_any = false;
}

fn void move_cmds(DrawCmds* to, DrawList* from) @private
{
	to.push_all(from.cmds.array_view());
	from.cmds.clear(); // (the texts are `to`'s now)
}

// Every command, in drawing order (appended to `out`, which owns them now:
// free_cmds() it): the background list, windows, open drop-downs, the
// tooltip, the foreground list.
fn void Gui.end_frame(&self, DrawCmds* out)
{
	while (!self.regions.is_empty()) self.end_window(); // (a window left open: closed here)
	if (self.tooltip_text.len())
	{
		float wrap = 420;
		Pt size = self.text_size(self.tooltip_text.str_view(), 1, wrap);
		float x = math::max(0.0f, math::min(self.in.mouse.x + 14, self.in.width - size.x - 16));
		float y = math::max(0.0f, math::min(self.in.mouse.y + 18, self.in.height - size.y - 12));
		Area r = { x, y, x + size.x + 16, y + size.y + 12 };
		self.tooltip_list.fill(r, self.theme.popup, self.theme.rounding);
		self.tooltip_list.stroke(r, self.theme.border, self.theme.rounding);
		self.tooltip_list.text({ x + 8, y + 6 }, self.theme.text, self.tooltip_text.str_view(), self.theme.font, wrap);
	}
	// A press elsewhere ends typing and closes the drop-down; so does it not being drawn.
	if (self.in.clicks[0] > 0 && !self.focus_claimed) self.focus = 0;
	if (self.open_popup && (!self.popup_drawn || (self.in.clicks[0] > 0 && !self.popup_claimed && !self.popup_rect.contains(self.in.mouse)))) self.open_popup = 0;
	if (!self.in.down[0] && self.in.clicks[0] == 0) self.active = 0;
	self.prev_rects.clear();
	self.prev_rects.push_all(self.rects.array_view());
	self.prev_popup_rect = self.popup_drawn ? self.popup_rect : {};
	move_cmds(out, &self.background);
	for (sz i = 0; i < self.list_count; i++)
	{
		if (self.hidden[i])
		{
			free_cmds(&self.lists[i].cmds);
		}
		else
		{
			move_cmds(out, self.lists[i]);
		}
	}
	move_cmds(out, &self.popup);
	move_cmds(out, &self.tooltip_list);
	move_cmds(out, &self.foreground);
}

// --- Windows and layout ---------------------------------------------------------------

fn uint Gui.id_of(&self, String label) => fnv(label, self.ids.is_empty() ? 0 : *self.ids.last());

fn String shown(String label)
{
	sz hash = label.index_of("##") ?? label.len;
	return label[:hash];
}

fn Region* Gui.region(&self) => self.regions.last();

fn Remembered* Gui.remember(&self, uint id)
{
	bool made;
	Remembered* r = self.remembered.slot(id, &made);
	if (made) r.seen = -1;
	return r;
}

fn void Gui.begin_window(&self, String name, WindowOptions o)
{
	uint id = fnv(name, 0);
	Remembered* rem = self.remember(id);
	bool fit_w = o.size.x <= 0;
	bool fit_h = o.size.y <= 0;
	bool hidden = (fit_w || fit_h) && rem.seen != self.frame - 1; // (its size isn't known yet)
	Pt size = { fit_w ? rem.size.x : o.size.x, fit_h ? rem.size.y : o.size.y };
	float x = math::floor(o.pos.x - o.pivot.x * size.x);
	float y = math::floor(o.pos.y - o.pivot.y * size.y);
	if (self.list_count == self.lists.len()) self.lists.push(mem::new(DrawList));
	DrawList* list = self.lists[self.list_count++];
	list.reset(self.theme.font);
	self.hidden.push(hidden);
	Region r = { .id = id, .rect = { x, y, x + size.x, y + size.y }, .fit_w = fit_w, .fit_h = fit_h,
		.content_origin = { x + self.theme.padding.x, y + self.theme.padding.y }, .list = list };
	r.cursor = r.content_origin;
	r.indent = r.cursor.x;
	r.line_y = r.cursor.y;
	r.max = r.content_origin;
	r.clip = { x, y, fit_w ? 1e9f : x + size.x, fit_h ? 1e9f : y + size.y };
	r.hovered = !hidden && !o.no_inputs && self.hovered_window == id;
	if (!o.no_background)
	{
		list.fill(r.rect, with_alpha(self.theme.window, o.alpha > 0 ? o.alpha : 1), self.theme.rounding * 1.5f);
		list.stroke(r.rect, self.theme.border, self.theme.rounding * 1.5f);
	}
	list.clip(r.clip);
	if (!hidden && !o.no_inputs) self.rects.push({ id, r.rect });
	self.regions.push(r);
	self.ids.push(id);
}

fn void Gui.end_window(&self)
{
	if (self.regions.is_empty()) return;
	Region* r = self.region();
	Remembered* rem = self.remember(r.id);
	rem.size = { r.fit_w ? r.max.x - r.rect.x0 + self.theme.padding.x : r.rect.width(), r.fit_h ? r.max.y - r.rect.y0 + self.theme.padding.y : r.rect.height() };
	rem.size.y = math::max(rem.size.y, r.fit_h ? 2 * self.theme.padding.y : 0.0f);
	rem.seen = self.frame;
	r.list.unclip();
	self.regions.pop();
	self.ids.pop();
}

// The current window's list (outside one: the foreground).
fn DrawList* Gui.draw_list(&self) => self.regions.is_empty() ? &self.foreground : self.region().list;
// Where the next item goes (screen).
fn Pt Gui.cursor(&self) => self.regions.is_empty() ? {} : self.region().cursor;

// From the window's content left.
fn void Gui.set_cursor_x(&self, float x)
{
	if (!self.regions.is_empty()) self.region().cursor.x = self.region().content_origin.x + x;
}

fn float Gui.window_width(&self)
{
	if (self.regions.is_empty()) return self.in.width;
	Region* r = self.region();
	if (!r.fit_w) return r.rect.width();
	Remembered* rem = self.remembered.get_ref(r.id);
	return rem ? rem.size.x : 0;
}

// From the cursor to the content's right.
fn float Gui.available_width(&self)
{
	if (self.regions.is_empty()) return 0;
	Region* r = self.region();
	float right = r.rect.x0 + self.window_width() - (self.regions.len() > 1 && r.id != self.regions[0].id ? 14 : self.theme.padding.x);
	return math::max(0.0f, right - r.cursor.x);
}

// Lays out an item of that size.
fn Area Gui.place(&self, Pt size)
{
	if (self.regions.is_empty()) return {};
	Region* g = self.region();
	if (!g.same_line)
	{
		g.line_y = g.cursor.y;
		g.line_h = 0;
	}
	Area rect = { g.cursor.x, g.line_y, g.cursor.x + size.x, g.line_y + size.y };
	g.line_h = math::max(g.line_h, size.y);
	g.prev_x = rect.x1;
	g.max.x = math::max(g.max.x, rect.x1);
	g.max.y = math::max(g.max.y, rect.y1);
	g.cursor = { g.indent, g.line_y + g.line_h + self.theme.spacing.y };
	g.same_line = false;
	foreach (&grp : self.groups)
	{
		if (grp.depth == self.regions.len())
		{
			grp.max.x = math::max(grp.max.x, rect.x1);
			grp.max.y = math::max(grp.max.y, rect.y1);
		}
	}
	self.last_rect = rect;
	self.last_hovered = self.last_hovered_any = false;
	return rect;
}

// x: from the content left.
fn void Gui.same_line(&self, float x = -1, float spacing = -1)
{
	if (self.regions.is_empty()) return;
	Region* g = self.region();
	g.same_line = true;
	g.cursor.y = g.line_y;
	g.cursor.x = x >= 0 ? g.content_origin.x + x : g.prev_x + (spacing >= 0 ? spacing : self.theme.spacing.x);
}

fn void Gui.dummy(&self, Pt size) => (void)self.place(size);
fn void Gui.space(&self, float height) => self.dummy({ 0, height });

fn void Gui.separator(&self)
{
	float w = math::max(self.available_width(), 1.0f);
	Area r = self.place({ w, 1 + self.theme.spacing.y });
	self.draw_list().fill({ r.x0, r.y0 + self.theme.spacing.y * 0.5f, r.x1, r.y0 + self.theme.spacing.y * 0.5f + 1 }, self.theme.border);
}

fn void Gui.begin_group(&self)
{
	if (self.regions.is_empty()) return;
	Region* g = self.region();
	if (!g.same_line)
	{
		g.line_y = g.cursor.y;
		g.line_h = 0;
	}
	Group grp = { .start = { g.cursor.x, g.line_y }, .indent = g.indent, .line_y = g.line_y, .line_h = g.line_h, .same_line = g.same_line, .depth = self.regions.len() };
	grp.max = grp.start;
	self.groups.push(grp);
	g.indent = grp.start.x;
	g.cursor = grp.start;
	g.same_line = false;
}

fn void Gui.end_group(&self)
{
	if (self.groups.is_empty() || self.regions.is_empty()) return;
	Group grp = self.groups.pop();
	Region* g = self.region();
	g.indent = grp.indent;
	g.line_y = grp.line_y;
	g.line_h = grp.line_h;
	g.same_line = true; // (the group sits where it started)
	g.cursor = grp.start;
	self.place({ grp.max.x - grp.start.x, grp.max.y - grp.start.y });
}

// A scrolling region; size.x 0: the rest of the width, size.y below 0: the
// rest of the height less that.
fn void Gui.begin_child(&self, String name, Pt size, bool border)
{
	if (self.regions.is_empty()) return;
	uint id = self.id_of(name);
	Region* parent = self.region();
	float w = size.x > 0 ? size.x : self.available_width();
	float h = size.y;
	if (h <= 0) h = math::max(20.0f, parent.rect.y1 - self.theme.padding.y - parent.cursor.y + h);
	Area rect = self.place({ w, h });
	parent = self.region();
	Remembered* rem = self.remember(id);
	Region r = { .id = id, .rect = rect, .scroll = rem.scroll, .list = parent.list };
	r.clip = intersect({ rect.x0 + 1, rect.y0 + 1, rect.x1 - 1, rect.y1 - 1 }, parent.clip);
	r.content_origin = { rect.x0 + 6, rect.y0 + 6 - r.scroll };
	r.cursor = r.content_origin;
	r.indent = r.cursor.x;
	r.line_y = r.cursor.y;
	r.max = r.content_origin;
	r.hovered = parent.hovered && r.clip.contains(self.in.mouse);
	if (border)
	{
		r.list.fill(rect, rgba(0, 0, 0, 60), self.theme.rounding);
		r.list.stroke(rect, self.theme.border, self.theme.rounding);
	}
	r.list.clip(r.clip);
	self.regions.push(r);
	self.ids.push(id);
}

fn void Gui.end_child(&self)
{
	if (self.regions.len() < 2) return;
	Region r = *self.region();
	Remembered* rem = self.remember(r.id);
	float content_h = r.max.y - (r.rect.y0 + 6 - r.scroll) + 6;
	float visible = r.rect.height();
	float max_scroll = math::max(0.0f, content_h - visible);
	float scroll = r.scroll;
	if (r.hovered && self.in.wheel != 0 && !self.wheel_used)
	{
		scroll -= self.in.wheel * 3 * self.line_height;
		self.wheel_used = true;
	}
	if (max_scroll > 0) // a scroll bar, which can be dragged
	{
		Area track = { r.rect.x1 - 8, r.rect.y0 + 4, r.rect.x1 - 4, r.rect.y1 - 4 };
		float thumb_h = math::max(20.0f, track.height() * visible / content_h);
		uint bar = r.id ^ 0x5c5c5c5cu;
		bool over = r.hovered && track.contains(self.in.mouse);
		if (over && self.in.clicks[0] > 0) self.active = bar;
		if (self.active == bar && self.in.down[0]) scroll = (self.in.mouse.y - track.y0 - thumb_h / 2) / math::max(1.0f, track.height() - thumb_h) * max_scroll;
		scroll = math::clamp(scroll, 0.0f, max_scroll);
		float ty = track.y0 + (track.height() - thumb_h) * (scroll / max_scroll);
		r.list.fill(track, rgba(255, 255, 255, 12), 2);
		r.list.fill({ track.x0, ty, track.x1, ty + thumb_h }, self.active == bar || over ? self.theme.line_hover : self.theme.line, 2);
	}
	rem.scroll = math::clamp(scroll, 0.0f, max_scroll);
	rem.content_h = content_h;
	rem.seen = self.frame;
	r.list.unclip();
	self.regions.pop();
	self.ids.pop();
	self.last_rect = r.rect; // (the region is the last item now)
	self.last_hovered = self.last_hovered_any = r.hovered;
}

// The next field's width; below 0: to the right edge less that.
fn void Gui.set_next_width(&self, float w)
{
	self.next_width = w;
	self.has_next_width = true;
}

fn float Gui.width_for(&self, float fallback)
{
	if (!self.has_next_width) return fallback;
	self.has_next_width = false;
	return self.next_width > 0 ? self.next_width : math::max(10.0f, self.available_width() + self.next_width);
}

fn void Gui.push_id(&self, String id) => self.ids.push(self.id_of(id));
fn void Gui.push_int_id(&self, int id) => @pool() { self.push_id(string::tformat("%d", id)); }
fn void Gui.pop_id(&self) { if (!self.ids.is_empty()) self.ids.pop(); }

fn void Gui.push_spacing(&self, Pt spacing)
{
	self.spacings.push(self.theme.spacing);
	self.theme.spacing = spacing;
}

fn void Gui.pop_spacing(&self)
{
	if (!self.spacings.is_empty()) self.theme.spacing = self.spacings.pop();
}

fn bool Gui.is_disabled(&self) => !self.disabled.is_empty() && *self.disabled.last();

fn void Gui.begin_disabled(&self, bool disabled)
{
	self.disabled.push(disabled || self.is_disabled());
	if (!self.regions.is_empty()) self.region().list.alpha = self.is_disabled() ? self.theme.disabled_alpha : 1.0f;
}

fn void Gui.end_disabled(&self)
{
	if (!self.disabled.is_empty()) self.disabled.pop();
	if (!self.regions.is_empty()) self.region().list.alpha = self.is_disabled() ? self.theme.disabled_alpha : 1.0f;
}

// The next text on this line lines up with the fields beside it.
fn void Gui.align_text_to_frame(&self) => self.align_next_text = true;

// --- Widgets -------------------------------------------------------------------------

// Hover and click (on release) for an item.
fn bool Gui.hit(&self, uint id, Area r, bool* held)
{
	*held = false;
	if (self.regions.is_empty()) return false;
	Region* g = self.region();
	bool over = g.hovered && r.contains(self.in.mouse) && g.clip.contains(self.in.mouse);
	bool disabled = self.is_disabled();
	self.last_hovered_any = over;
	self.last_hovered = over && !disabled && (self.active == 0 || self.active == id);
	if (disabled) return false;
	if (self.last_hovered && self.in.clicks[0] > 0) self.active = id;
	*held = self.active == id && self.in.down[0];
	if (self.active == id && !self.in.down[0])
	{
		self.active = 0;
		return over;
	}
	return false;
}

fn void Gui.text(&self, String t, float scale = 1) => self.text_colored(t, self.theme.text, scale);
fn void Gui.text_dim(&self, String t) => self.text_colored(t, self.theme.text_dim);

fn void Gui.text_colored(&self, String t, Color color, float scale = 1)
{
	Pt size = self.text_size(t, scale);
	float lift = self.align_next_text ? self.theme.frame.y : 0;
	self.align_next_text = false;
	Area r = self.place({ size.x, size.y + 2 * lift });
	self.draw_list().text({ r.x0, r.y0 + lift }, color, t, self.theme.font * scale);
}

fn void Gui.text_wrapped(&self, String t, Color color)
{
	float wrap = math::max(40.0f, self.available_width());
	Pt size = self.text_size(t, 1, wrap);
	Area r = self.place(size);
	self.draw_list().text({ r.x0, r.y0 }, color, t, self.theme.font, wrap);
}

fn void Gui.bullet(&self, String t) => @pool() { self.text(string::tformat("• %s", t)); }

fn bool Gui.button(&self, String label, Pt size = {})
{
	String s = shown(label);
	Pt ts = self.text_size(s);
	float w = size.x > 0 ? size.x : size.x < 0 ? math::max(10.0f, self.available_width() + size.x) : ts.x + 2 * self.theme.frame.x;
	float h = size.y > 0 ? size.y : self.frame_height();
	Area r = self.place({ w, h });
	bool held;
	bool clicked = self.hit(self.id_of(label), r, &held);
	DrawList* dl = self.draw_list();
	dl.fill(r, held ? self.theme.button_active : self.last_hovered ? self.theme.button_hover : self.theme.button, self.theme.rounding);
	dl.stroke(r, self.last_hovered || held ? self.theme.line_hover : self.theme.border, self.theme.rounding);
	dl.text({ math::floor(r.x0 + (w - ts.x) / 2), math::floor(r.y0 + (h - ts.y) / 2) }, self.theme.text, s, self.theme.font);
	return clicked;
}

fn bool Gui.invisible_button(&self, String id, Pt size)
{
	Area r = self.place(size);
	bool held;
	return self.hit(self.id_of(id), r, &held);
}

fn bool Gui.checkbox(&self, String label, bool* value)
{
	String s = shown(label);
	float box = self.frame_height();
	Pt ts = self.text_size(s);
	Area r = self.place({ box + (s.len ? self.theme.frame.x + ts.x : 0), box });
	bool held;
	bool clicked = self.hit(self.id_of(label), r, &held);
	if (clicked) *value = !*value;
	DrawList* dl = self.draw_list();
	Area b = { r.x0, r.y0, r.x0 + box, r.y0 + box };
	dl.fill(b, self.last_hovered ? self.theme.frame_hover : self.theme.frame_bg, self.theme.rounding);
	dl.stroke(b, self.last_hovered ? self.theme.line_hover : self.theme.line, self.theme.rounding);
	if (*value)
	{
		float in = math::floor(box * 0.28f);
		dl.fill({ b.x0 + in, b.y0 + in, b.x1 - in, b.y1 - in }, self.theme.accent, 3);
	}
	dl.text({ b.x1 + self.theme.frame.x, r.y0 + self.theme.frame.y }, self.theme.text, s, self.theme.font);
	return clicked;
}

fn bool Gui.slider(&self, String id, float* value, float lo, float hi)
{
	float w = self.width_for(200);
	Area r = self.place({ w, self.frame_height() });
	bool held;
	self.hit(self.id_of(id), r, &held);
	float grab = 12;
	bool changed = false;
	if (held && hi > lo)
	{
		float t = math::clamp((self.in.mouse.x - r.x0 - grab / 2) / math::max(1.0f, w - grab), 0.0f, 1.0f);
		float v = lo + t * (hi - lo);
		changed = v != *value;
		*value = v;
	}
	float t = hi > lo ? math::clamp((*value - lo) / (hi - lo), 0.0f, 1.0f) : 0;
	DrawList* dl = self.draw_list();
	bool lit = self.last_hovered || held;
	float cy = math::floor((r.y0 + r.y1) / 2);
	float gx = math::floor(r.x0 + t * (w - grab)); // the grab's left
	dl.fill({ r.x0, cy - 1, r.x1, cy + 1 }, self.theme.line, 1);
	dl.fill({ r.x0, cy - 1, gx + grab / 2, cy + 1 }, lit ? self.theme.accent : self.theme.line_hover, 1);
	float d = lit ? grab + 2 : grab; // (a little bigger under the mouse)
	float gcx = gx + grab / 2;
	dl.fill({ gcx - d / 2, cy - d / 2, gcx + d / 2, cy + d / 2 }, self.theme.accent, d / 2);
	return changed;
}

fn bool Gui.slider_int(&self, String id, int* value, int lo, int hi)
{
	float f = (float)*value;
	self.slider(id, &f, (float)lo, (float)hi);
	int v = math::clamp((int)math::round(f), lo, hi);
	bool changed = v != *value;
	*value = v;
	return changed;
}

fn bool Gui.combo(&self, String label, int* index, String[] options)
{
	uint id = self.id_of(label);
	String s = shown(label);
	float w = self.width_for(200);
	Pt ts = self.text_size(s);
	Area r = self.place({ w + (s.len ? self.theme.frame.x + ts.x : 0), self.frame_height() });
	Area box = { r.x0, r.y0, r.x0 + w, r.y1 };
	bool held;
	if (self.hit(id, box, &held))
	{
		self.open_popup = self.open_popup == id ? 0 : id;
		self.popup_claimed = true;
	}
	DrawList* dl = self.draw_list();
	bool lit = self.last_hovered || self.open_popup == id;
	dl.fill(box, lit ? self.theme.frame_hover : self.theme.frame_bg, self.theme.rounding);
	dl.fill({ box.x0 + 2, box.y1 - 1, box.x1 - 2, box.y1 }, lit ? self.theme.line_hover : self.theme.line);
	String current = *index >= 0 && *index < options.len ? options[*index] : "";
	dl.clip({ box.x0, box.y0, box.x1 - 22, box.y1 });
	dl.text({ box.x0 + self.theme.frame.x, box.y0 + self.theme.frame.y }, self.theme.text, current, self.theme.font);
	dl.unclip();
	dl.text({ box.x1 - 18, box.y0 + self.theme.frame.y }, lit ? self.theme.text : self.theme.text_dim, "▾", self.theme.font);
	dl.text({ box.x1 + self.theme.frame.x, r.y0 + self.theme.frame.y }, self.theme.text, s, self.theme.font);
	bool changed = false;
	if (self.open_popup != id) return false;
	float row = self.line_height + 6;
	float pw = w;
	foreach (o : options) pw = math::max(pw, self.text_size(o).x + 2 * self.theme.frame.x + 8);
	float ph = row * (float)options.len + 6;
	float py = box.y1 + 2;
	if (py + ph > self.in.height && box.y0 - ph - 2 >= 0) py = box.y0 - ph - 2; // (above, if it doesn't fit below)
	self.popup_rect = { box.x0, py, box.x0 + pw, py + ph };
	self.popup_drawn = true;
	self.popup.fill(self.popup_rect, self.theme.popup, self.theme.rounding);
	self.popup.stroke(self.popup_rect, self.theme.border, self.theme.rounding);
	bool popup_hovered = self.hovered_window == POPUP_WINDOW || self.popup_rect.contains(self.in.mouse);
	foreach (i, o : options)
	{
		Area or = { self.popup_rect.x0 + 3, py + 3 + row * (float)i, self.popup_rect.x1 - 3, py + 3 + row * (float)(i + 1) };
		bool over = popup_hovered && or.contains(self.in.mouse);
		if ((int)i == *index) self.popup.fill(or, self.theme.selected, self.theme.rounding - 1);
		if (over)
		{
			self.popup.fill(or, self.theme.frame_hover, self.theme.rounding - 1);
			self.popup.fill({ or.x0 + 1, or.y0 + 5, or.x0 + 3, or.y1 - 5 }, self.theme.accent, 1);
			if (self.in.clicks[0] > 0)
			{
				changed = *index != (int)i;
				*index = (int)i;
				self.open_popup = 0;
				self.popup_claimed = true;
			}
		}
		self.popup.text({ or.x0 + self.theme.frame.x, or.y0 + 3 }, self.theme.text, o, self.theme.font);
	}
	return changed;
}

fn bool Gui.selectable(&self, String label, bool selected)
{
	String s = shown(label);
	float w = math::max(self.available_width(), self.text_size(s).x);
	Area r = self.place({ w, self.line_height + 4 });
	bool held;
	self.hit(self.id_of(label), r, &held);
	bool pressed = self.last_hovered && self.in.clicks[0] > 0; // (on the press: a double click shows at once)
	DrawList* dl = self.draw_list();
	if (selected || self.last_hovered) dl.fill(r, self.last_hovered ? self.theme.frame_hover : self.theme.selected, self.theme.rounding - 1);
	if (selected) dl.fill({ r.x0 + 1, r.y0 + 4, r.x0 + 3, r.y1 - 4 }, self.theme.accent, 1);
	dl.text({ r.x0 + 8, r.y0 + 2 }, selected || self.last_hovered ? self.theme.text : rgba(225, 225, 225), s, self.theme.font);
	return pressed;
}

fn void Gui.caret_from_mouse(&self, String value, float x0)
{
	self.caret = value.len;
	for (sz i = 0; i < value.len; i = next_char(value, i))
	{
		sz j = next_char(value, i);
		float mid = (self.text_size(value[:i]).x + self.text_size(value[:j]).x) / 2;
		if (self.in.mouse.x < x0 + mid)
		{
			self.caret = i;
			return;
		}
	}
}

fn bool Gui.insert_text(&self, DString* value, String add, sz max_bytes, bool digits_only)
{
	DString clean;
	clean.tinit();
	foreach (c : add)
	{
		if (c < 32 || c == 127 || (digits_only && !(c >= '0' && c <= '9'))) continue;
		clean.append_char(c);
	}
	String fits = fit_bytes(clean.str_view(), max_bytes > value.len() ? max_bytes - value.len() : 0);
	if (!fits.len) return false;
	value.insert_at(self.caret, fits);
	self.caret += fits.len;
	self.caret_time = 0;
	return true;
}

// A text field over `value` (init(mem)ed: it grows).
fn bool Gui.input_text(&self, String label, DString* value, sz max_bytes, String hint = "", bool digits_only = false) => @pool()
{
	uint id = self.id_of(label);
	String s = shown(label);
	float w = self.width_for(200);
	Pt ts = self.text_size(s);
	Area r = self.place({ w + (s.len ? self.theme.frame.x + ts.x : 0), self.frame_height() });
	Area box = { r.x0, r.y0, r.x0 + w, r.y1 };
	bool held;
	self.hit(id, box, &held);
	float text_x = box.x0 + self.theme.frame.x;
	if (self.focus_next)
	{
		self.focus_next = false;
		if (self.focus != id) self.caret_time = 0;
		self.focus = id;
		self.focus_claimed = true;
		self.caret = value.len();
	}
	if (self.last_hovered && self.in.clicks[0] > 0)
	{
		if (self.focus != id) self.caret_time = 0;
		self.focus = id;
		self.focus_claimed = true;
		self.caret_from_mouse(value.str_view(), text_x);
	}
	if (self.focus == id && self.is_disabled()) self.focus = 0;
	bool changed = false;
	if (self.focus == id)
	{
		self.caret = math::min(self.caret, value.len());
		changed = self.insert_text(value, self.in.text, max_bytes, digits_only) || changed;
		foreach (k : self.in.keys)
		{
			String v = value.str_view();
			switch (k)
			{
				case KEY_BACK:
					if (self.caret > 0)
					{
						sz p = prev_char(v, self.caret);
						value.delete(p, self.caret - p);
						self.caret = p;
						changed = true;
					}
				case KEY_DELETE:
					if (self.caret < v.len)
					{
						value.delete(self.caret, next_char(v, self.caret) - self.caret);
						changed = true;
					}
				case KEY_LEFT: self.caret = prev_char(v, self.caret);
				case KEY_RIGHT: self.caret = next_char(v, self.caret);
				case KEY_HOME: self.caret = 0;
				case KEY_END: self.caret = v.len;
				case KEY_ENTER:
				case KEY_ESCAPE:
				case KEY_TAB: self.focus = 0;
				case KEY_V:
					if (self.in.ctrl && self.clipboard) changed = self.insert_text(value, self.clipboard(self.clipboard_context), max_bytes, digits_only) || changed;
				default:
			}
			self.caret_time = 0;
		}
	}
	DrawList* dl = self.draw_list();
	bool focused = self.focus == id;
	bool lit = focused || self.last_hovered;
	dl.fill(box, lit ? self.theme.frame_hover : self.theme.frame_bg, self.theme.rounding);
	dl.fill({ box.x0 + 2, box.y1 - (focused ? 2 : 1), box.x1 - 2, box.y1 }, lit ? self.theme.line_hover : self.theme.line);
	// The text, moved left when the caret would be past the box's end.
	String v = value.str_view();
	float room = w - 2 * self.theme.frame.x;
	float caret_x = focused ? self.text_size(v[:math::min(self.caret, v.len)]).x : 0;
	float shift = math::max(0.0f, caret_x - room + 2);
	dl.clip({ box.x0 + 2, box.y0, box.x1 - 2, box.y1 });
	if (!v.len && !focused && hint.len)
	{
		dl.text({ text_x, box.y0 + self.theme.frame.y }, self.theme.text_dim, hint, self.theme.font);
	}
	else
	{
		dl.text({ text_x - shift, box.y0 + self.theme.frame.y }, self.theme.text, v, self.theme.font);
	}
	if (focused && self.caret_time % 1.0f < 0.6f)
	{
		float x = math::floor(text_x - shift + caret_x);
		dl.fill({ x, box.y0 + self.theme.frame.y, x + 1, box.y1 - self.theme.frame.y }, self.theme.text);
	}
	dl.unclip();
	dl.text({ box.x1 + self.theme.frame.x, r.y0 + self.theme.frame.y }, self.theme.text, s, self.theme.font);
	return changed;
}

// The next text field drawn gets the keyboard (the caret at its end).
fn void Gui.focus_next_field(&self) => self.focus_next = true;
// About the last item.
fn bool Gui.item_hovered(&self) => self.last_hovered;
fn bool Gui.item_hovered_even_disabled(&self) => self.last_hovered_any;
fn Area Gui.item_rect(&self) => self.last_rect;

fn void Gui.tooltip(&self, String text)
{
	self.tooltip_text.clear();
	self.tooltip_text.append(text);
}
