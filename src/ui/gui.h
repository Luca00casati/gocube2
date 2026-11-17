#pragma once
// The game's own immediate-mode UI (what Dear ImGui did): windows, layout,
// text, buttons, checkboxes, sliders, drop-down lists, text fields, scrolling
// regions and tooltips. Each frame the screens call it, it works out what is
// hovered and clicked from the mouse and keys, and it records what to draw
// (rectangles, gradients, text, item icons) for a backend to draw
// (ui/gui_d2d.h: Direct2D and DirectWrite). Text is measured through a
// callback, so tests run it with a fixed-width "font".
//
// As in Dear ImGui: a widget's label is also its id, unique within its
// window and push_id scope; "##" starts the part that is only id ("Address##join"
// shows "Address"; "##view" shows nothing). Windows that fit their content
// take their size from the frame before (they aren't drawn the first frame).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gc::ui {

struct Vec2 {
	float x = 0, y = 0;
};

struct Rect {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool contains(Vec2 p) const { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }
	float width() const { return x1 - x0; }
	float height() const { return y1 - y0; }
};

struct Color {
	uint8_t r = 255, g = 255, b = 255, a = 255;
};
constexpr Color rgba(int r, int g, int b, int a = 255) {
	return {uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a)};
}

// What to draw, in order.
struct DrawCmd {
	enum Type { FILL, STROKE, GRADIENT, TEXT, IMAGE, CLIP, UNCLIP } type = FILL;
	Rect rect;
	Color color, color2;   // GRADIENT: from color to color2
	bool vertical = true;  // GRADIENT: top to bottom (else left to right)
	float rounding = 0, thickness = 1;
	float size = 0;        // TEXT: font size in pixels
	float wrap = 0;        // TEXT: wrap width (0: one line)
	std::string text;
	int image = -1;        // IMAGE: an item icon (an atlas tile)
};

class DrawList {
public:
	void fill(Rect r, Color c, float rounding = 0);
	void stroke(Rect r, Color c, float rounding = 0, float thickness = 1);
	void gradient(Rect r, Color from, Color to, bool vertical);
	void text(Vec2 at, Color c, std::string_view text, float size, float wrap = 0);
	void image(Rect r, int tile);
	void clip(Rect r);
	void unclip();
	// Text with a black outline (the HUD's).
	void outlined_text(Vec2 at, Color c, std::string_view text, float size);

	std::vector<DrawCmd> cmds;
	float alpha = 1; // multiplies every colour added (disabled widgets)
	float font_size = 15;
};

// One frame's input.
struct Input {
	float width = 0, height = 0; // the screen
	float dt = 1.0f / 60.0f;
	Vec2 mouse{-1e6f, -1e6f};    // (far away: not over the window, or captured)
	bool down[3] = {};           // left, right, middle
	int clicks[3] = {};          // presses since the last frame
	float wheel = 0;             // notches (positive: away from you)
	std::string text;            // typed (UTF-8)
	std::vector<int> keys;       // keys pressed, as virtual-key codes (repeats too)
	bool ctrl = false;
};

// Virtual-key codes the text field knows (as Windows').
enum Key {
	KEY_BACK = 0x08, KEY_TAB = 0x09, KEY_ENTER = 0x0d, KEY_ESCAPE = 0x1b, KEY_END = 0x23, KEY_HOME = 0x24,
	KEY_LEFT = 0x25, KEY_RIGHT = 0x27, KEY_DELETE = 0x2e, KEY_V = 0x56,
};

// "Minimal glass": dark see-through panels over the world, thin light
// outlines, white text; what is hovered or chosen brightens (no colour).
struct Theme {
	float font = 16;
	Vec2 padding{20, 18};       // inside windows
	Vec2 spacing{8, 6};         // between items
	Vec2 frame{10, 6};          // inside buttons and fields
	float rounding = 6;
	Color text = rgba(245, 245, 245);
	Color text_dim = rgba(255, 255, 255, 130);
	Color window = rgba(6, 8, 12, 165);
	Color border = rgba(255, 255, 255, 34);
	Color frame_bg = rgba(255, 255, 255, 12);   // fields, drop-downs, checkboxes
	Color frame_hover = rgba(255, 255, 255, 26);
	Color line = rgba(255, 255, 255, 60);       // under fields; slider tracks
	Color line_hover = rgba(255, 255, 255, 200);
	Color button = rgba(255, 255, 255, 14);
	Color button_hover = rgba(255, 255, 255, 38);
	Color button_active = rgba(255, 255, 255, 64);
	Color accent = rgba(255, 255, 255, 235);    // check marks, slider grabs, carets, hover marks
	Color selected = rgba(255, 255, 255, 24);
	Color popup = rgba(10, 12, 16, 235);
	float disabled_alpha = 0.3f;
};

struct WindowOptions {
	Vec2 pos;              // where `pivot` of the window goes
	Vec2 pivot{0, 0};      // (0.5, 0.5): centred on `pos`
	Vec2 size{0, 0};       // 0: fit the content (that axis)
	bool background = true;
	float alpha = 1;       // of the background
	bool inputs = true;    // false: the mouse goes through it
};

class Gui {
public:
	// Width and height of `text` at `size` pixels, wrapped at `wrap` (0: one line).
	using Measure = std::function<Vec2(std::string_view text, float size, float wrap)>;
	explicit Gui(Measure measure);

	Theme theme;
	// What the clipboard holds (for Ctrl+V in text fields).
	std::function<std::string()> clipboard;

	void begin_frame(const Input &in);
	// Every command, in drawing order: the background list, windows, open
	// drop-downs, the tooltip, the foreground list.
	std::vector<DrawCmd> end_frame();

	// Under / over every window, in screen coordinates.
	DrawList &background() { return background_; }
	DrawList &foreground() { return foreground_; }
	const Input &input() const { return in_; }
	float framerate() const { return framerate_; }
	// The mouse is over a window (or busy with a widget): clicks are the UI's.
	bool wants_mouse() const { return wants_mouse_; }
	bool typing() const { return focus_ != 0; }
	Vec2 text_size(std::string_view text, float scale = 1, float wrap = 0) const;
	float frame_height() const { return line_height() + 2 * theme.frame.y; }
	float line_height() const { return line_height_; }

	// --- Windows and layout ---
	void begin_window(std::string_view id, const WindowOptions &o);
	void end_window();
	DrawList &draw_list(); // the current window's
	Vec2 cursor() const;   // where the next item goes (screen)
	void set_cursor_x(float x); // from the window's content left
	float window_width() const;
	float available_width() const; // from the cursor to the content's right
	void same_line(float x = -1, float spacing = -1); // x: from the content left
	void dummy(Vec2 size);
	void space(float height) { dummy({0, height}); }
	void separator();
	void begin_group();
	void end_group();
	// A scrolling region; size.x 0: the rest of the width, size.y below 0:
	// the rest of the height less that.
	void begin_child(std::string_view id, Vec2 size, bool border);
	void end_child();
	void next_width(float w); // the next field's width; below 0: to the right edge less that
	void push_id(std::string_view id);
	void push_id(int id) { push_id(std::to_string(id)); }
	void pop_id();
	void push_spacing(Vec2 spacing);
	void pop_spacing();
	void begin_disabled(bool disabled);
	void end_disabled();
	// The next text on this line lines up with the fields beside it.
	void align_text_to_frame() { align_next_text_ = true; }

	// --- Widgets ---
	void text(std::string_view text, float scale = 1);
	void text(std::string_view text, Color color, float scale = 1);
	void text_dim(std::string_view text) { this->text(text, theme.text_dim); }
	void text_wrapped(std::string_view text, Color color);
	void bullet(std::string_view text);
	bool button(std::string_view label, Vec2 size = {});
	bool invisible_button(std::string_view id, Vec2 size);
	bool checkbox(std::string_view label, bool &value);
	bool slider(std::string_view id, float &value, float lo, float hi);
	bool slider(std::string_view id, int &value, int lo, int hi);
	bool combo(std::string_view label, int &index, const std::vector<std::string> &options);
	bool selectable(std::string_view label, bool selected);
	bool input_text(std::string_view label, std::string &value, size_t max_bytes, std::string_view hint = {},
			bool digits_only = false);
	// About the last item.
	bool item_hovered() const { return last_hovered_; }
	bool item_hovered_even_disabled() const { return last_hovered_any_; }
	Rect item_rect() const { return last_rect_; }
	bool double_clicked() const { return double_click_; }
	void tooltip(std::string_view text);

private:
	struct Region { // a window or a child region in it
		uint32_t id = 0;
		Rect rect, clip;
		Vec2 content_origin;     // content's top left (screen, scrolled)
		Vec2 cursor;
		float indent = 0;        // where new lines start (screen x)
		float line_y = 0, line_h = 0, prev_x = 0;
		bool same_line = false;
		Vec2 max;                // furthest content reached (screen)
		float scroll = 0;        // child regions
		bool hovered = false;
		bool fit_w = false, fit_h = false;
		DrawList *list = nullptr;
	};
	struct Group {
		Vec2 start;
		float indent;
		Vec2 max;
		float line_y, line_h;
		bool same_line;
		size_t depth; // the region it is in (regions_.size())
	};
	struct Remembered { // per window / child, from the frame before
		Vec2 size;
		Rect rect;
		float content_h = 0;
		float scroll = 0;
		int seen = -1; // the frame it was last shown
	};

	uint32_t id_of(std::string_view label) const;
	static std::string_view shown(std::string_view label);
	Region &region() { return regions_.back(); }
	const Region &region() const { return regions_.back(); }
	Rect place(Vec2 size); // lays out an item of that size
	bool hit(uint32_t id, const Rect &r, bool &held); // hover and click (on release) for an item
	float width_for(float fallback);
	void caret_from_mouse(const std::string &value, float x0);

	Measure measure_;
	Input in_;
	DrawList background_, foreground_, popup_, tooltip_;
	std::vector<std::unique_ptr<DrawList>> lists_; // the windows', in order
	std::vector<Region> regions_;
	std::vector<Group> groups_;
	std::vector<uint32_t> ids_;
	std::vector<Vec2> spacings_;
	std::vector<bool> disabled_;
	std::map<uint32_t, Remembered> remembered_;
	int frame_ = 0;
	float framerate_ = 60;
	float line_height_ = 18;
	float next_width_ = 0;
	bool has_next_width_ = false;
	bool align_next_text_ = false;
	bool wants_mouse_ = false;
	uint32_t hovered_window_ = 0; // from the window rects of the frame before
	uint32_t active_ = 0;         // pressed on, button still down
	uint32_t focus_ = 0;          // the text field typed in
	size_t caret_ = 0;
	float caret_time_ = 0;
	uint32_t open_popup_ = 0;     // the drop-down open
	Rect popup_rect_, prev_popup_rect_;
	bool popup_drawn_ = false;
	bool last_hovered_ = false, last_hovered_any_ = false;
	Rect last_rect_;
	float click_time_ = 1e9f;     // since the last left click
	Vec2 click_pos_;
	bool double_click_ = false;
	std::string tooltip_text_;
	std::vector<bool> hidden_;    // per window list: not drawn (fits its content, first frame)
	std::vector<std::pair<uint32_t, Rect>> rects_, prev_rects_; // windows taking the mouse
	bool focus_claimed_ = false;  // a text field was clicked this frame
	bool popup_claimed_ = false;  // its drop-down was clicked this frame
	bool wheel_used_ = false;
};

} // namespace gc::ui
