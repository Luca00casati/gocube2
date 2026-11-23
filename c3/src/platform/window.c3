// The game's one Win32 window.
module gocube::window;
import gocube::win, gocube::util::vec;
import std::io;

enum Button : char
{
	LEFT,
	RIGHT,
	MIDDLE,
}

const int BUTTONS = 3;

// For the UI (ui/gui): what happened since the last call.
struct UiEvents
{
	DString text;  // typed, UTF-8 (heap; free() it)
	Vec{int} keys; // keys pressed (virtual-key codes, repeats too)
	int[BUTTONS] clicks;
	float wheel;
}

fn void UiEvents.free(&self)
{
	self.text.free();
	self.keys.free();
}

struct Window
{
	Hwnd hwnd;
	int width, height;
	bool resized;
	bool open;
	bool captured;
	bool[BUTTONS] pressed;
	bool[BUTTONS] down;
	UiEvents ui;
	WChar high_surrogate;
	float wheel;
	float mouse_dx, mouse_dy;
}

macro WString class_name() @private => @wstring("GoCubeWindow");

fn void Window.free(&self)
{
	if (self.hwnd) win::destroy_window(self.hwnd);
	self.ui.free();
}

// `width` x `height`: the client area (scaled for the DPI); 0 x 0: three
// quarters of the screen each way, in its middle.
fn bool Window.create(&self, String title, int width, int height, bool visible) => @pool()
{
	Handle instance = win::get_module_handle(null);
	WndClassEx wc = { .size = WndClassEx::size, .style = win::CS_HREDRAW | win::CS_VREDRAW, .proc = &proc, .instance = instance,
		.cursor = win::load_cursor(null, 32512), .class_name = class_name() };
	if (!win::register_class_ex(&wc) && win::get_last_error() != win::ERROR_CLASS_ALREADY_EXISTS) return false;
	int x = win::CW_USEDEFAULT;
	int y = win::CW_USEDEFAULT;
	int w;
	int h;
	if (width <= 0 || height <= 0)
	{
		// Three quarters of the screen each way (its work area: without the
		// taskbar), in the middle of it.
		MonitorInfo mi = { .size = MonitorInfo::size };
		win::get_monitor_info(win::monitor_from_point({ 0, 0 }, win::MONITOR_DEFAULTTOPRIMARY), &mi);
		Rect work = mi.work;
		w = (work.right - work.left) * 3 / 4;
		h = (work.bottom - work.top) * 3 / 4;
		x = work.left + (work.right - work.left - w) / 2;
		y = work.top + (work.bottom - work.top - h) / 2;
	}
	else
	{
		// Size the client area, not the frame, scaled for the monitor's DPI.
		uint dpi = win::get_dpi_for_system();
		Rect rect = { 0, 0, win::mul_div(width, (int)dpi, 96), win::mul_div(height, (int)dpi, 96) };
		win::adjust_window_rect_ex_for_dpi(&rect, win::WS_OVERLAPPEDWINDOW, 0, 0, dpi);
		w = rect.right - rect.left;
		h = rect.bottom - rect.top;
	}
	self.hwnd = win::create_window_ex(0, class_name(), title.to_temp_wstring()!!, win::WS_OVERLAPPEDWINDOW, x, y, w, h, null, null, instance, self);
	if (!self.hwnd) return false;
	Rect client;
	win::get_client_rect(self.hwnd, &client);
	self.width = client.right - client.left;
	self.height = client.bottom - client.top;
	self.open = true;
	// Raw mouse motion (WM_INPUT): unaffected by pointer acceleration and the screen's edges.
	RawInputDevice mouse = { 0x01, 0x02, 0, self.hwnd }; // generic desktop, mouse
	win::register_raw_input_devices(&mouse, 1, RawInputDevice::size);
	if (visible) win::show_window(self.hwnd, win::SW_SHOWDEFAULT);
	return true;
}

// Handles pending messages; false once the window has been closed.
fn bool Window.pump(&self)
{
	Msg msg;
	while (win::peek_message(&msg, null, 0, 0, win::PM_REMOVE))
	{
		if (msg.message == win::WM_QUIT) self.open = false;
		win::translate_message(&msg);
		win::dispatch_message(&msg);
	}
	return self.open;
}

fn bool Window.minimized(&self) => self.width == 0 || self.height == 0;

// True once after the client area changed size.
fn bool Window.take_resized(&self)
{
	bool r = self.resized;
	self.resized = false;
	return r;
}

fn LResult proc(Hwnd hwnd, uint msg, WParam wp, LParam lp) @private
{
	if (msg == win::WM_NCCREATE)
	{
		CreateStruct* cs = (CreateStruct*)lp;
		win::set_window_long_ptr(hwnd, win::GWLP_USERDATA, (sz)cs.create_params);
		((Window*)cs.create_params).hwnd = hwnd; // (before CreateWindowExW returns)
	}
	Window* self = (Window*)win::get_window_long_ptr(hwnd, win::GWLP_USERDATA);
	return self ? self.handle(msg, wp, lp) : win::def_window_proc(hwnd, msg, wp, lp);
}

// Mouse capture for looking around: the cursor is hidden and held in the
// window, and raw mouse motion is collected. Losing focus releases it.
fn void Window.set_mouse_captured(&self, bool captured)
{
	if (captured == self.captured || !self.hwnd) return;
	self.captured = captured;
	if (captured)
	{
		Rect r;
		win::get_client_rect(self.hwnd, &r);
		win::map_window_points(self.hwnd, null, (Point*)&r, 2);
		win::clip_cursor(&r);
		while (win::show_cursor(0) >= 0) {}
	}
	else
	{
		win::clip_cursor(null);
		while (win::show_cursor(1) < 0) {}
	}
	self.mouse_dx = self.mouse_dy = 0;
}

// Raw mouse motion since the last call (counts, not pixels).
fn void Window.take_mouse_delta(&self, float* dx, float* dy)
{
	*dx = self.mouse_dx;
	*dy = self.mouse_dy;
	self.mouse_dx = self.mouse_dy = 0;
}

// True once after that mouse button was pressed in the window.
fn bool Window.take_pressed(&self, Button b)
{
	bool p = self.pressed[b.ordinal];
	self.pressed[b.ordinal] = false;
	return p;
}

// Mouse wheel notches since the last call (positive: away from you).
fn float Window.take_wheel(&self)
{
	float w = self.wheel;
	self.wheel = 0;
	return w;
}

// What happened since the last call (the caller frees it).
fn UiEvents Window.take_ui_events(&self)
{
	UiEvents e = self.ui;
	self.ui = {};
	return e;
}

// The mouse in the client area (false: outside it, or captured).
fn bool Window.mouse_position(&self, float* x, float* y)
{
	Point p;
	Rect r;
	if (!self.hwnd || self.captured || !win::is_window_visible(self.hwnd) || !win::get_cursor_pos(&p) || !win::screen_to_client(self.hwnd, &p) ||
		!win::get_client_rect(self.hwnd, &r) || p.x < 0 || p.y < 0 || p.x >= r.right || p.y >= r.bottom) return false;
	*x = (float)p.x;
	*y = (float)p.y;
	return true;
}

fn bool Window.button_down(&self, Button b) => self.down[b.ordinal];

// The clipboard's text (UTF-8, in `alloc`), "" if it has none.
fn String Window.clipboard_text(&self, Allocator alloc)
{
	String out = "";
	if (!win::is_clipboard_format_available(win::CF_UNICODETEXT) || !win::open_clipboard(self.hwnd)) return out;
	if (Handle h = win::get_clipboard_data(win::CF_UNICODETEXT))
	{
		if (WChar* w = win::global_lock(h))
		{
			out = win::utf8_of(alloc, w);
			win::global_unlock(h);
		}
	}
	win::close_clipboard();
	return out;
}

// Whether a key (a virtual-key code) is held, while the window has focus.
fn bool Window.key_down(&self, int vk) => self.hwnd && win::get_foreground_window() == self.hwnd && (win::get_async_key_state(vk) & 0x8000) != 0;

fn LResult Window.handle(&self, uint msg, WParam wp, LParam lp)
{
	switch (msg)
	{
		case win::WM_INPUT:
			RawInput raw;
			uint size = RawInput::size;
			if (win::get_raw_input_data((Handle)lp, win::RID_INPUT, &raw, &size, RawInputHeader::size) != uint::max &&
				raw.header.type == win::RIM_TYPEMOUSE && !(raw.flags & win::MOUSE_MOVE_ABSOLUTE) && self.captured)
			{
				self.mouse_dx += (float)raw.last_x;
				self.mouse_dy += (float)raw.last_y;
			}
			// (DefWindowProc cleans up)
		case win::WM_LBUTTONDOWN:
		case win::WM_RBUTTONDOWN:
		case win::WM_MBUTTONDOWN:
			int b = msg == win::WM_LBUTTONDOWN ? 0 : msg == win::WM_RBUTTONDOWN ? 1 : 2;
			self.pressed[b] = true;
			self.down[b] = true;
			if (!self.captured)
			{
				self.ui.clicks[b]++;
				win::set_capture(self.hwnd); // (a drag past the window's edge still ends here)
			}
			return 0;
		case win::WM_LBUTTONUP:
		case win::WM_RBUTTONUP:
		case win::WM_MBUTTONUP:
			self.down[msg == win::WM_LBUTTONUP ? 0 : msg == win::WM_RBUTTONUP ? 1 : 2] = false;
			if (!self.down[0] && !self.down[1] && !self.down[2] && win::get_capture() == self.hwnd) win::release_capture();
			return 0;
		case win::WM_MOUSEWHEEL:
			float notches = (float)(short)(wp >> 16) / (float)win::WHEEL_DELTA;
			self.wheel += notches;
			self.ui.wheel += notches;
			return 0;
		case win::WM_KEYDOWN:
		case win::WM_SYSKEYDOWN:
			if (self.ui.keys.len() < 64) self.ui.keys.push((int)wp);
			// (then WM_CHAR, through TranslateMessage)
		case win::WM_CHAR:
			uint u = (uint)wp;
			if (u >= 0xD800 && u < 0xDC00) // the first half of a pair: wait for the second
			{
				self.high_surrogate = (WChar)u;
				return 0;
			}
			if (u >= 0xDC00 && u < 0xE000)
			{
				if (!self.high_surrogate) return 0;
				u = 0x10000 + (((uint)self.high_surrogate - 0xD800) << 10) + (u - 0xDC00);
			}
			self.high_surrogate = 0;
			if (u >= 32 && u != 127 && self.ui.text.len() < 256)
			{
				if (!(void*)self.ui.text) self.ui.text.init(mem);
				self.ui.text.append_char32(u);
			}
			return 0;
		case win::WM_KILLFOCUS:
			self.set_mouse_captured(false);
			self.down = {};
			return 0;
		case win::WM_SIZE:
			int w = (int)(lp & 0xFFFF);
			int h = (int)((lp >> 16) & 0xFFFF);
			if (w != self.width || h != self.height)
			{
				self.width = w;
				self.height = h;
				self.resized = true;
				if (self.captured) // hold the cursor in the new client area
				{
					self.set_mouse_captured(false);
					self.set_mouse_captured(true);
				}
			}
			return 0;
		case win::WM_SYSCOMMAND:
			if ((wp & 0xFFF0) == win::SC_KEYMENU) return 0; // no beep and menu on Alt
		case win::WM_DESTROY:
			self.hwnd = null;
			win::post_quit_message(0);
			return 0;
		default:
	}
	return win::def_window_proc(self.hwnd, msg, wp, lp);
}
