#include "ui/gui.h"

#include <algorithm>
#include <cmath>

namespace gc::ui {

namespace {

constexpr uint32_t POPUP_WINDOW = 1; // hovered_window_ when the mouse is over an open drop-down
constexpr float DOUBLE_CLICK_SECONDS = 0.45f;

uint32_t fnv(std::string_view s, uint32_t seed) {
	uint32_t h = 2166136261u ^ (seed * 16777619u);
	for (unsigned char c : s) {
		h = (h ^ c) * 16777619u;
	}
	return h < 2 ? h + 2 : h; // (0: no id; 1: the drop-down)
}

Rect intersect(const Rect &a, const Rect &b) {
	return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

// UTF-8: the start of the character before / after byte `i`.
size_t prev_char(const std::string &s, size_t i) {
	if (i == 0) {
		return 0;
	}
	i--;
	while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) {
		i--;
	}
	return i;
}

size_t next_char(const std::string &s, size_t i) {
	if (i >= s.size()) {
		return s.size();
	}
	i++;
	while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) {
		i++;
	}
	return i;
}

// The whole characters of `text` that fit in `room` bytes.
std::string_view fit_bytes(std::string_view text, size_t room) {
	if (text.size() <= room) {
		return text;
	}
	size_t n = room;
	while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) {
		n--;
	}
	return text.substr(0, n);
}

} // namespace

// --- DrawList ----------------------------------------------------------------------

namespace {
Color with_alpha(Color c, float alpha) {
	c.a = uint8_t(std::lround(float(c.a) * std::clamp(alpha, 0.0f, 1.0f)));
	return c;
}
} // namespace

void DrawList::fill(Rect r, Color c, float rounding) {
	DrawCmd d;
	d.type = DrawCmd::FILL;
	d.rect = r;
	d.color = with_alpha(c, alpha);
	d.rounding = rounding;
	cmds.push_back(std::move(d));
}

void DrawList::stroke(Rect r, Color c, float rounding, float thickness) {
	DrawCmd d;
	d.type = DrawCmd::STROKE;
	d.rect = r;
	d.color = with_alpha(c, alpha);
	d.rounding = rounding;
	d.thickness = thickness;
	cmds.push_back(std::move(d));
}

void DrawList::gradient(Rect r, Color from, Color to, bool vertical) {
	DrawCmd d;
	d.type = DrawCmd::GRADIENT;
	d.rect = r;
	d.color = with_alpha(from, alpha);
	d.color2 = with_alpha(to, alpha);
	d.vertical = vertical;
	cmds.push_back(std::move(d));
}

void DrawList::text(Vec2 at, Color c, std::string_view text, float size, float wrap) {
	if (text.empty()) {
		return;
	}
	DrawCmd d;
	d.type = DrawCmd::TEXT;
	d.rect = {at.x, at.y, at.x, at.y};
	d.color = with_alpha(c, alpha);
	d.size = size > 0 ? size : font_size;
	d.wrap = wrap;
	d.text = std::string(text);
	cmds.push_back(std::move(d));
}

void DrawList::image(Rect r, int tile) {
	DrawCmd d;
	d.type = DrawCmd::IMAGE;
	d.rect = r;
	d.color = with_alpha(rgba(255, 255, 255), alpha);
	d.image = tile;
	cmds.push_back(std::move(d));
}

void DrawList::clip(Rect r) {
	DrawCmd d;
	d.type = DrawCmd::CLIP;
	d.rect = r;
	cmds.push_back(std::move(d));
}

void DrawList::unclip() {
	DrawCmd d;
	d.type = DrawCmd::UNCLIP;
	cmds.push_back(std::move(d));
}

void DrawList::outlined_text(Vec2 at, Color c, std::string_view t, float size) {
	for (const Vec2 o : {Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}, Vec2{-1, -1}, Vec2{1, 1}}) {
		text({at.x + o.x, at.y + o.y}, rgba(0, 0, 0, c.a), t, size);
	}
	text(at, c, t, size);
}

// --- Frames ------------------------------------------------------------------------

Gui::Gui(Measure measure) : measure_(std::move(measure)) {}

Vec2 Gui::text_size(std::string_view text, float scale, float wrap) const {
	if (text.empty()) {
		return {0, line_height_ * scale};
	}
	return measure_(text, theme.font * scale, wrap);
}

void Gui::begin_frame(const Input &in) {
	in_ = in;
	frame_++;
	if (in.dt > 0) {
		framerate_ = framerate_ * 0.95f + (1.0f / in.dt) * 0.05f;
	}
	line_height_ = std::max(1.0f, measure_("Ag", theme.font, 0).y);
	caret_time_ += in.dt;
	// A double click: a second press soon after the first, near it.
	click_time_ += in.dt;
	double_click_ = false;
	if (in.clicks[0] > 0) {
		const float dx = in.mouse.x - click_pos_.x, dy = in.mouse.y - click_pos_.y;
		double_click_ = in.clicks[0] > 1 || (click_time_ < DOUBLE_CLICK_SECONDS && dx * dx + dy * dy < 36);
		click_time_ = double_click_ ? 1e9f : 0; // (a third click starts again)
		click_pos_ = in.mouse;
	}
	// The window under the mouse: the topmost one of the frame before (or the open drop-down).
	hovered_window_ = 0;
	for (const auto &[id, r] : prev_rects_) {
		if (r.contains(in.mouse)) {
			hovered_window_ = id;
		}
	}
	if (open_popup_ && prev_popup_rect_.contains(in.mouse)) {
		hovered_window_ = POPUP_WINDOW;
	}
	wants_mouse_ = hovered_window_ != 0 || active_ != 0;
	for (DrawList *l : {&background_, &foreground_, &popup_, &tooltip_}) {
		l->cmds.clear();
		l->alpha = 1;
		l->font_size = theme.font;
	}
	lists_.clear();
	hidden_.clear();
	rects_.clear();
	regions_.clear();
	groups_.clear();
	ids_.clear();
	disabled_.clear();
	spacings_.clear();
	tooltip_text_.clear();
	popup_drawn_ = false;
	focus_claimed_ = false;
	popup_claimed_ = false;
	wheel_used_ = false;
	has_next_width_ = false;
	align_next_text_ = false;
	last_hovered_ = last_hovered_any_ = false;
}

std::vector<DrawCmd> Gui::end_frame() {
	while (!regions_.empty()) { // (a window left open: closed here)
		end_window();
	}
	if (!tooltip_text_.empty()) {
		const float wrap = 420;
		const Vec2 size = text_size(tooltip_text_, 1, wrap);
		float x = in_.mouse.x + 14, y = in_.mouse.y + 18;
		x = std::max(0.0f, std::min(x, in_.width - size.x - 16));
		y = std::max(0.0f, std::min(y, in_.height - size.y - 12));
		const Rect r{x, y, x + size.x + 16, y + size.y + 12};
		tooltip_.fill(r, theme.popup, theme.rounding);
		tooltip_.stroke(r, theme.border, theme.rounding);
		tooltip_.text({x + 8, y + 6}, theme.text, tooltip_text_, theme.font, wrap);
	}
	// A press elsewhere ends typing and closes the drop-down; so does it not being drawn.
	if (in_.clicks[0] > 0 && !focus_claimed_) {
		focus_ = 0;
	}
	if (open_popup_ && (!popup_drawn_ || (in_.clicks[0] > 0 && !popup_claimed_ && !popup_rect_.contains(in_.mouse)))) {
		open_popup_ = 0;
	}
	if (!in_.down[0] && in_.clicks[0] == 0) {
		active_ = 0;
	}
	prev_rects_ = rects_;
	prev_popup_rect_ = popup_drawn_ ? popup_rect_ : Rect{};

	std::vector<DrawCmd> out = std::move(background_.cmds);
	for (size_t i = 0; i < lists_.size(); i++) {
		if (!hidden_[i]) {
			out.insert(out.end(), lists_[i]->cmds.begin(), lists_[i]->cmds.end());
		}
	}
	for (DrawList *l : {&popup_, &tooltip_, &foreground_}) {
		out.insert(out.end(), l->cmds.begin(), l->cmds.end());
	}
	background_.cmds.clear();
	return out;
}

// --- Windows and layout ------------------------------------------------------------

uint32_t Gui::id_of(std::string_view label) const {
	return fnv(label, ids_.empty() ? 0 : ids_.back());
}

std::string_view Gui::shown(std::string_view label) {
	const size_t hash = label.find("##");
	return hash == std::string_view::npos ? label : label.substr(0, hash);
}

void Gui::begin_window(std::string_view name, const WindowOptions &o) {
	const uint32_t id = fnv(name, 0);
	Remembered &rem = remembered_[id];
	const bool fit_w = o.size.x <= 0, fit_h = o.size.y <= 0;
	const bool hidden = (fit_w || fit_h) && rem.seen != frame_ - 1; // (its size isn't known yet)
	const Vec2 size{fit_w ? rem.size.x : o.size.x, fit_h ? rem.size.y : o.size.y};
	const float x = std::floor(o.pos.x - o.pivot.x * size.x), y = std::floor(o.pos.y - o.pivot.y * size.y);
	lists_.push_back(std::make_unique<DrawList>());
	hidden_.push_back(hidden);
	DrawList *list = lists_.back().get();
	list->font_size = theme.font;
	Region r;
	r.id = id;
	r.rect = {x, y, x + size.x, y + size.y};
	r.fit_w = fit_w;
	r.fit_h = fit_h;
	r.content_origin = {x + theme.padding.x, y + theme.padding.y};
	r.cursor = r.content_origin;
	r.indent = r.cursor.x;
	r.line_y = r.cursor.y;
	r.max = r.content_origin;
	r.clip = {x, y, fit_w ? 1e9f : x + size.x, fit_h ? 1e9f : y + size.y};
	r.hovered = !hidden && o.inputs && hovered_window_ == id;
	r.list = list;
	if (o.background) {
		list->fill(r.rect, with_alpha(theme.window, o.alpha), theme.rounding * 1.5f);
		list->stroke(r.rect, theme.border, theme.rounding * 1.5f);
	}
	list->clip(r.clip);
	if (!hidden && o.inputs) {
		rects_.push_back({id, r.rect});
	}
	regions_.push_back(r);
	ids_.push_back(id);
}

void Gui::end_window() {
	if (regions_.empty()) {
		return;
	}
	Region &r = region();
	Remembered &rem = remembered_[r.id];
	rem.size = {r.fit_w ? r.max.x - r.rect.x0 + theme.padding.x : r.rect.width(),
			r.fit_h ? r.max.y - r.rect.y0 + theme.padding.y : r.rect.height()};
	rem.size.y = std::max(rem.size.y, r.fit_h ? 2 * theme.padding.y : 0.0f);
	rem.seen = frame_;
	r.list->unclip();
	regions_.pop_back();
	ids_.pop_back();
}

DrawList &Gui::draw_list() {
	return regions_.empty() ? foreground_ : *region().list;
}

Vec2 Gui::cursor() const {
	return regions_.empty() ? Vec2{} : region().cursor;
}

void Gui::set_cursor_x(float x) {
	if (!regions_.empty()) {
		region().cursor.x = region().content_origin.x + x;
	}
}

float Gui::window_width() const {
	if (regions_.empty()) {
		return in_.width;
	}
	const Region &r = region();
	return r.fit_w ? remembered_.count(r.id) ? remembered_.at(r.id).size.x : 0 : r.rect.width();
}

float Gui::available_width() const {
	if (regions_.empty()) {
		return 0;
	}
	const Region &r = region();
	const float right = r.rect.x0 + window_width() - (regions_.size() > 1 && r.id != regions_.front().id ? 14 : theme.padding.x);
	return std::max(0.0f, right - r.cursor.x);
}

Rect Gui::place(Vec2 size) {
	if (regions_.empty()) {
		return {};
	}
	Region &g = region();
	if (!g.same_line) {
		g.line_y = g.cursor.y;
		g.line_h = 0;
	}
	const Rect rect{g.cursor.x, g.line_y, g.cursor.x + size.x, g.line_y + size.y};
	g.line_h = std::max(g.line_h, size.y);
	g.prev_x = rect.x1;
	g.max.x = std::max(g.max.x, rect.x1);
	g.max.y = std::max(g.max.y, rect.y1);
	g.cursor = {g.indent, g.line_y + g.line_h + theme.spacing.y};
	g.same_line = false;
	for (Group &grp : groups_) {
		if (grp.depth == regions_.size()) {
			grp.max.x = std::max(grp.max.x, rect.x1);
			grp.max.y = std::max(grp.max.y, rect.y1);
		}
	}
	last_rect_ = rect;
	last_hovered_ = last_hovered_any_ = false;
	return rect;
}

void Gui::same_line(float x, float spacing) {
	if (regions_.empty()) {
		return;
	}
	Region &g = region();
	g.same_line = true;
	g.cursor.y = g.line_y;
	g.cursor.x = x >= 0 ? g.content_origin.x + x : g.prev_x + (spacing >= 0 ? spacing : theme.spacing.x);
}

void Gui::dummy(Vec2 size) {
	place(size);
}

void Gui::separator() {
	const float w = std::max(available_width(), 1.0f);
	const Rect r = place({w, 1 + theme.spacing.y});
	draw_list().fill({r.x0, r.y0 + theme.spacing.y * 0.5f, r.x1, r.y0 + theme.spacing.y * 0.5f + 1}, theme.border);
}

void Gui::begin_group() {
	if (regions_.empty()) {
		return;
	}
	Region &g = region();
	Group grp;
	if (!g.same_line) {
		g.line_y = g.cursor.y;
		g.line_h = 0;
	}
	grp.start = {g.cursor.x, g.line_y};
	grp.indent = g.indent;
	grp.max = grp.start;
	grp.line_y = g.line_y;
	grp.line_h = g.line_h;
	grp.same_line = g.same_line;
	grp.depth = regions_.size();
	groups_.push_back(grp);
	g.indent = grp.start.x;
	g.cursor = grp.start;
	g.same_line = false;
}

void Gui::end_group() {
	if (groups_.empty() || regions_.empty()) {
		return;
	}
	const Group grp = groups_.back();
	groups_.pop_back();
	Region &g = region();
	g.indent = grp.indent;
	g.line_y = grp.line_y;
	g.line_h = grp.line_h;
	g.same_line = true; // (the group sits where it started)
	g.cursor = grp.start;
	place({grp.max.x - grp.start.x, grp.max.y - grp.start.y});
}

void Gui::begin_child(std::string_view name, Vec2 size, bool border) {
	if (regions_.empty()) {
		return;
	}
	const uint32_t id = id_of(name);
	Region &parent = region();
	const float w = size.x > 0 ? size.x : available_width();
	float h = size.y;
	if (h <= 0) {
		const float bottom = parent.rect.y1 - theme.padding.y;
		h = std::max(20.0f, bottom - parent.cursor.y + h);
	}
	const Rect rect = place({w, h});
	Remembered &rem = remembered_[id];
	Region r;
	r.id = id;
	r.rect = rect;
	r.clip = intersect({rect.x0 + 1, rect.y0 + 1, rect.x1 - 1, rect.y1 - 1}, parent.clip);
	r.scroll = rem.scroll;
	r.content_origin = {rect.x0 + 6, rect.y0 + 6 - r.scroll};
	r.cursor = r.content_origin;
	r.indent = r.cursor.x;
	r.line_y = r.cursor.y;
	r.max = r.content_origin;
	r.hovered = parent.hovered && r.clip.contains(in_.mouse);
	r.list = parent.list;
	if (border) {
		r.list->fill(rect, rgba(0, 0, 0, 60), theme.rounding);
		r.list->stroke(rect, theme.border, theme.rounding);
	}
	r.list->clip(r.clip);
	regions_.push_back(r);
	ids_.push_back(id);
}

void Gui::end_child() {
	if (regions_.size() < 2) {
		return;
	}
	Region &r = region();
	Remembered &rem = remembered_[r.id];
	const float content_h = r.max.y - (r.rect.y0 + 6 - r.scroll) + 6;
	const float visible = r.rect.height();
	const float max_scroll = std::max(0.0f, content_h - visible);
	float scroll = r.scroll;
	if (r.hovered && in_.wheel != 0 && !wheel_used_) {
		scroll -= in_.wheel * 3 * line_height_;
		wheel_used_ = true;
	}
	if (max_scroll > 0) { // a scroll bar, which can be dragged
		const Rect track{r.rect.x1 - 8, r.rect.y0 + 4, r.rect.x1 - 4, r.rect.y1 - 4};
		const float thumb_h = std::max(20.0f, track.height() * visible / content_h);
		const uint32_t bar = r.id ^ 0x5c5c5c5cu;
		const bool over = r.hovered && track.contains(in_.mouse);
		if (over && in_.clicks[0] > 0) {
			active_ = bar;
		}
		if (active_ == bar && in_.down[0]) {
			scroll = (in_.mouse.y - track.y0 - thumb_h / 2) / std::max(1.0f, track.height() - thumb_h) * max_scroll;
		}
		scroll = std::clamp(scroll, 0.0f, max_scroll);
		const float ty = track.y0 + (track.height() - thumb_h) * (scroll / max_scroll);
		r.list->fill(track, rgba(255, 255, 255, 12), 2);
		r.list->fill({track.x0, ty, track.x1, ty + thumb_h}, active_ == bar || over ? theme.line_hover : theme.line, 2);
	}
	rem.scroll = std::clamp(scroll, 0.0f, max_scroll);
	rem.content_h = content_h;
	rem.seen = frame_;
	r.list->unclip();
	const Rect rect = r.rect;
	const bool hovered = r.hovered;
	regions_.pop_back();
	ids_.pop_back();
	last_rect_ = rect; // (the region is the last item now)
	last_hovered_ = last_hovered_any_ = hovered;
}

void Gui::next_width(float w) {
	next_width_ = w;
	has_next_width_ = true;
}

float Gui::width_for(float fallback) {
	if (!has_next_width_) {
		return fallback;
	}
	has_next_width_ = false;
	return next_width_ > 0 ? next_width_ : std::max(10.0f, available_width() + next_width_);
}

void Gui::push_id(std::string_view id) {
	ids_.push_back(id_of(id));
}

void Gui::pop_id() {
	if (!ids_.empty()) {
		ids_.pop_back();
	}
}

void Gui::push_spacing(Vec2 spacing) {
	spacings_.push_back(theme.spacing);
	theme.spacing = spacing;
}

void Gui::pop_spacing() {
	if (!spacings_.empty()) {
		theme.spacing = spacings_.back();
		spacings_.pop_back();
	}
}

void Gui::begin_disabled(bool disabled) {
	disabled_.push_back(disabled || (!disabled_.empty() && disabled_.back()));
	if (!regions_.empty()) {
		region().list->alpha = disabled_.back() ? theme.disabled_alpha : 1.0f;
	}
}

void Gui::end_disabled() {
	if (!disabled_.empty()) {
		disabled_.pop_back();
	}
	if (!regions_.empty()) {
		region().list->alpha = !disabled_.empty() && disabled_.back() ? theme.disabled_alpha : 1.0f;
	}
}

// --- Widgets -----------------------------------------------------------------------

bool Gui::hit(uint32_t id, const Rect &r, bool &held) {
	held = false;
	if (regions_.empty()) {
		return false;
	}
	const Region &g = region();
	const bool over = g.hovered && r.contains(in_.mouse) && g.clip.contains(in_.mouse);
	const bool disabled = !disabled_.empty() && disabled_.back();
	last_hovered_any_ = over;
	last_hovered_ = over && !disabled && (active_ == 0 || active_ == id);
	if (disabled) {
		return false;
	}
	if (last_hovered_ && in_.clicks[0] > 0) {
		active_ = id;
	}
	held = active_ == id && in_.down[0];
	if (active_ == id && !in_.down[0]) {
		active_ = 0;
		return over;
	}
	return false;
}

void Gui::text(std::string_view t, float scale) {
	text(t, theme.text, scale);
}

void Gui::text(std::string_view t, Color color, float scale) {
	const Vec2 size = text_size(t, scale);
	const float lift = align_next_text_ ? theme.frame.y : 0;
	align_next_text_ = false;
	const Rect r = place({size.x, size.y + 2 * lift});
	draw_list().text({r.x0, r.y0 + lift}, color, t, theme.font * scale);
}

void Gui::text_wrapped(std::string_view t, Color color) {
	const float wrap = std::max(40.0f, available_width());
	const Vec2 size = text_size(t, 1, wrap);
	const Rect r = place(size);
	draw_list().text({r.x0, r.y0}, color, t, theme.font, wrap);
}

void Gui::bullet(std::string_view t) {
	text("• " + std::string(t));
}

bool Gui::button(std::string_view label, Vec2 size) {
	const std::string_view s = shown(label);
	const Vec2 ts = text_size(s);
	const float w = size.x > 0 ? size.x : size.x < 0 ? std::max(10.0f, available_width() + size.x) : ts.x + 2 * theme.frame.x;
	const float h = size.y > 0 ? size.y : frame_height();
	const Rect r = place({w, h});
	bool held;
	const bool clicked = hit(id_of(label), r, held);
	DrawList &dl = draw_list();
	dl.fill(r, held ? theme.button_active : last_hovered_ ? theme.button_hover : theme.button, theme.rounding);
	dl.stroke(r, last_hovered_ || held ? theme.line_hover : theme.border, theme.rounding);
	dl.text({std::floor(r.x0 + (w - ts.x) / 2), std::floor(r.y0 + (h - ts.y) / 2)}, theme.text, s, theme.font);
	return clicked;
}

bool Gui::invisible_button(std::string_view id, Vec2 size) {
	const Rect r = place(size);
	bool held;
	return hit(id_of(id), r, held);
}

bool Gui::checkbox(std::string_view label, bool &value) {
	const std::string_view s = shown(label);
	const float box = frame_height();
	const Vec2 ts = text_size(s);
	const Rect r = place({box + (s.empty() ? 0 : theme.frame.x + ts.x), box});
	bool held;
	const bool clicked = hit(id_of(label), r, held);
	if (clicked) {
		value = !value;
	}
	DrawList &dl = draw_list();
	const Rect b{r.x0, r.y0, r.x0 + box, r.y0 + box};
	dl.fill(b, last_hovered_ ? theme.frame_hover : theme.frame_bg, theme.rounding);
	dl.stroke(b, last_hovered_ ? theme.line_hover : theme.line, theme.rounding);
	if (value) {
		const float in = std::floor(box * 0.28f);
		dl.fill({b.x0 + in, b.y0 + in, b.x1 - in, b.y1 - in}, theme.accent, 3);
	}
	dl.text({b.x1 + theme.frame.x, r.y0 + theme.frame.y}, theme.text, s, theme.font);
	return clicked;
}

bool Gui::slider(std::string_view id, float &value, float lo, float hi) {
	const float w = width_for(200);
	const Rect r = place({w, frame_height()});
	bool held;
	hit(id_of(id), r, held);
	const float grab = 12;
	bool changed = false;
	if (held && hi > lo) {
		const float t = std::clamp((in_.mouse.x - r.x0 - grab / 2) / std::max(1.0f, w - grab), 0.0f, 1.0f);
		const float v = lo + t * (hi - lo);
		changed = v != value;
		value = v;
	}
	const float t = hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f) : 0;
	DrawList &dl = draw_list();
	const bool lit = last_hovered_ || held;
	const float cy = std::floor((r.y0 + r.y1) / 2);
	const float gx = std::floor(r.x0 + t * (w - grab)); // the grab's left
	dl.fill({r.x0, cy - 1, r.x1, cy + 1}, theme.line, 1);
	dl.fill({r.x0, cy - 1, gx + grab / 2, cy + 1}, lit ? theme.accent : theme.line_hover, 1);
	const float d = lit ? grab + 2 : grab; // (a little bigger under the mouse)
	const float gcx = gx + grab / 2;
	dl.fill({gcx - d / 2, cy - d / 2, gcx + d / 2, cy + d / 2}, theme.accent, d / 2);
	return changed;
}

bool Gui::slider(std::string_view id, int &value, int lo, int hi) {
	float f = float(value);
	slider(id, f, float(lo), float(hi));
	const int v = std::clamp(int(std::lround(f)), lo, hi);
	const bool changed = v != value;
	value = v;
	return changed;
}

bool Gui::combo(std::string_view label, int &index, const std::vector<std::string> &options) {
	const uint32_t id = id_of(label);
	const std::string_view s = shown(label);
	const float w = width_for(200);
	const Vec2 ts = text_size(s);
	const Rect r = place({w + (s.empty() ? 0 : theme.frame.x + ts.x), frame_height()});
	const Rect box{r.x0, r.y0, r.x0 + w, r.y1};
	bool held;
	const bool clicked = hit(id, box, held);
	if (clicked) {
		open_popup_ = open_popup_ == id ? 0 : id;
		popup_claimed_ = true;
	}
	DrawList &dl = draw_list();
	const bool lit = last_hovered_ || open_popup_ == id;
	dl.fill(box, lit ? theme.frame_hover : theme.frame_bg, theme.rounding);
	dl.fill({box.x0 + 2, box.y1 - 1, box.x1 - 2, box.y1}, lit ? theme.line_hover : theme.line);
	const std::string current = index >= 0 && index < int(options.size()) ? options[size_t(index)] : "";
	dl.clip({box.x0, box.y0, box.x1 - 22, box.y1});
	dl.text({box.x0 + theme.frame.x, box.y0 + theme.frame.y}, theme.text, current, theme.font);
	dl.unclip();
	dl.text({box.x1 - 18, box.y0 + theme.frame.y}, lit ? theme.text : theme.text_dim, "▾", theme.font);
	dl.text({box.x1 + theme.frame.x, r.y0 + theme.frame.y}, theme.text, s, theme.font);

	bool changed = false;
	if (open_popup_ == id) {
		const float row = line_height_ + 6;
		float pw = w;
		for (const std::string &o : options) {
			pw = std::max(pw, text_size(o).x + 2 * theme.frame.x + 8);
		}
		const float ph = row * float(options.size()) + 6;
		float py = box.y1 + 2;
		if (py + ph > in_.height && box.y0 - ph - 2 >= 0) {
			py = box.y0 - ph - 2; // (above, if it doesn't fit below)
		}
		popup_rect_ = {box.x0, py, box.x0 + pw, py + ph};
		popup_drawn_ = true;
		popup_.fill(popup_rect_, theme.popup, theme.rounding);
		popup_.stroke(popup_rect_, theme.border, theme.rounding);
		const bool popup_hovered = hovered_window_ == POPUP_WINDOW || popup_rect_.contains(in_.mouse);
		for (size_t i = 0; i < options.size(); i++) {
			const Rect o{popup_rect_.x0 + 3, py + 3 + row * float(i), popup_rect_.x1 - 3, py + 3 + row * float(i + 1)};
			const bool over = popup_hovered && o.contains(in_.mouse);
			if (int(i) == index) {
				popup_.fill(o, theme.selected, theme.rounding - 1);
			}
			if (over) {
				popup_.fill(o, theme.frame_hover, theme.rounding - 1);
				popup_.fill({o.x0 + 1, o.y0 + 5, o.x0 + 3, o.y1 - 5}, theme.accent, 1);
				if (in_.clicks[0] > 0) {
					changed = index != int(i);
					index = int(i);
					open_popup_ = 0;
					popup_claimed_ = true;
				}
			}
			popup_.text({o.x0 + theme.frame.x, o.y0 + 3}, theme.text, options[i], theme.font);
		}
	}
	return changed;
}

bool Gui::selectable(std::string_view label, bool selected) {
	const std::string_view s = shown(label);
	const float w = std::max(available_width(), text_size(s).x);
	const Rect r = place({w, line_height_ + 4});
	bool held;
	hit(id_of(label), r, held);
	const bool pressed = last_hovered_ && in_.clicks[0] > 0; // (on the press: a double click shows at once)
	DrawList &dl = draw_list();
	if (selected || last_hovered_) {
		dl.fill(r, last_hovered_ ? theme.frame_hover : theme.selected, theme.rounding - 1);
	}
	if (selected) {
		dl.fill({r.x0 + 1, r.y0 + 4, r.x0 + 3, r.y1 - 4}, theme.accent, 1);
	}
	dl.text({r.x0 + 8, r.y0 + 2}, selected || last_hovered_ ? theme.text : rgba(225, 225, 225), s, theme.font);
	return pressed;
}

void Gui::caret_from_mouse(const std::string &value, float x0) {
	caret_ = value.size();
	for (size_t i = 0; i < value.size(); i = next_char(value, i)) {
		const size_t j = next_char(value, i);
		const float mid = (text_size(std::string_view(value).substr(0, i)).x +
						   text_size(std::string_view(value).substr(0, j)).x) / 2;
		if (in_.mouse.x < x0 + mid) {
			caret_ = i;
			return;
		}
	}
}

bool Gui::input_text(std::string_view label, std::string &value, size_t max_bytes, std::string_view hint,
		bool digits_only) {
	const uint32_t id = id_of(label);
	const std::string_view s = shown(label);
	const float w = width_for(200);
	const Vec2 ts = text_size(s);
	const Rect r = place({w + (s.empty() ? 0 : theme.frame.x + ts.x), frame_height()});
	const Rect box{r.x0, r.y0, r.x0 + w, r.y1};
	bool held;
	hit(id, box, held);
	const float text_x = box.x0 + theme.frame.x;
	if (last_hovered_ && in_.clicks[0] > 0) {
		if (focus_ != id) {
			caret_time_ = 0;
		}
		focus_ = id;
		focus_claimed_ = true;
		caret_from_mouse(value, text_x + 0);
	}
	if (focus_ == id && !disabled_.empty() && disabled_.back()) {
		focus_ = 0;
	}
	bool changed = false;
	if (focus_ == id) {
		caret_ = std::min(caret_, value.size());
		auto insert = [&](std::string_view add) {
			std::string clean;
			for (char c : add) {
				const unsigned char u = static_cast<unsigned char>(c);
				if (u < 32 || u == 127 || (digits_only && !(c >= '0' && c <= '9'))) {
					continue;
				}
				clean += c;
			}
			const std::string_view fits = fit_bytes(clean, max_bytes > value.size() ? max_bytes - value.size() : 0);
			if (!fits.empty()) {
				value.insert(caret_, fits);
				caret_ += fits.size();
				changed = true;
				caret_time_ = 0;
			}
		};
		insert(in_.text);
		for (int k : in_.keys) {
			switch (k) {
				case KEY_BACK:
					if (caret_ > 0) {
						const size_t p = prev_char(value, caret_);
						value.erase(p, caret_ - p);
						caret_ = p;
						changed = true;
					}
					break;
				case KEY_DELETE:
					if (caret_ < value.size()) {
						value.erase(caret_, next_char(value, caret_) - caret_);
						changed = true;
					}
					break;
				case KEY_LEFT:
					caret_ = prev_char(value, caret_);
					break;
				case KEY_RIGHT:
					caret_ = next_char(value, caret_);
					break;
				case KEY_HOME:
					caret_ = 0;
					break;
				case KEY_END:
					caret_ = value.size();
					break;
				case KEY_ENTER:
				case KEY_ESCAPE:
				case KEY_TAB:
					focus_ = 0;
					break;
				case KEY_V:
					if (in_.ctrl && clipboard) {
						insert(clipboard());
					}
					break;
				default:
					break;
			}
			caret_time_ = 0;
		}
	}
	DrawList &dl = draw_list();
	const bool lit = focus_ == id || last_hovered_;
	dl.fill(box, lit ? theme.frame_hover : theme.frame_bg, theme.rounding);
	dl.fill({box.x0 + 2, box.y1 - (focus_ == id ? 2 : 1), box.x1 - 2, box.y1}, lit ? theme.line_hover : theme.line);
	// The text, moved left when the caret would be past the box's end.
	const float room = w - 2 * theme.frame.x;
	const float caret_x = focus_ == id ? text_size(std::string_view(value).substr(0, std::min(caret_, value.size()))).x : 0;
	const float shift = std::max(0.0f, caret_x - room + 2);
	dl.clip({box.x0 + 2, box.y0, box.x1 - 2, box.y1});
	if (value.empty() && focus_ != id && !hint.empty()) {
		dl.text({text_x, box.y0 + theme.frame.y}, theme.text_dim, hint, theme.font);
	} else {
		dl.text({text_x - shift, box.y0 + theme.frame.y}, theme.text, value, theme.font);
	}
	if (focus_ == id && std::fmod(caret_time_, 1.0f) < 0.6f) {
		const float x = std::floor(text_x - shift + caret_x);
		dl.fill({x, box.y0 + theme.frame.y, x + 1, box.y1 - theme.frame.y}, theme.text);
	}
	dl.unclip();
	dl.text({box.x1 + theme.frame.x, r.y0 + theme.frame.y}, theme.text, s, theme.font);
	return changed;
}

void Gui::tooltip(std::string_view text) {
	tooltip_text_ = std::string(text);
}

} // namespace gc::ui
