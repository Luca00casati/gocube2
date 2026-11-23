// user32 (windows, messages, input, the clipboard) and the few kernel32
// calls that go with it, as Windows declares them (x64 layouts).
module gocube::win @link("user32");

alias Hwnd = void*;
alias WParam = usz;
alias LParam = sz;
alias LResult = sz;
alias WndProc = fn LResult(Hwnd, uint, WParam, LParam);

struct Point
{
	int x, y;
}

struct Rect
{
	int left, top, right, bottom;
}

struct WndClassEx
{
	uint size, style;
	WndProc proc;
	int cls_extra, wnd_extra;
	Handle instance, icon, cursor, background;
	WString menu_name, class_name;
	Handle icon_small;
}

struct Msg
{
	Hwnd hwnd;
	uint message;
	WParam wparam;
	LParam lparam;
	Dword time;
	Point pt;
	Dword private_;
}

struct MonitorInfo
{
	Dword size;
	Rect monitor, work;
	Dword flags;
}

struct RawInputDevice
{
	ushort usage_page, usage;
	Dword flags;
	Hwnd target;
}

struct RawInputHeader
{
	Dword type, size;
	Handle device;
	WParam wparam;
}

// RAWINPUT with its RAWMOUSE (the union's largest member on x64).
struct RawInput
{
	RawInputHeader header;
	ushort flags;
	ushort pad_;
	uint buttons;
	uint raw_buttons;
	int last_x, last_y;
	uint extra;
}

struct CreateStruct
{
	void* create_params;
	// (the rest isn't read)
}

const uint CS_VREDRAW = 1;
const uint CS_HREDRAW = 2;
const Dword ERROR_CLASS_ALREADY_EXISTS = 1410;
const int CW_USEDEFAULT = (int)0x80000000;
const Dword MONITOR_DEFAULTTOPRIMARY = 1;
const Dword WS_OVERLAPPEDWINDOW = 0x00CF0000;
const int SW_SHOWDEFAULT = 10;
const uint PM_REMOVE = 1;
const int GWLP_USERDATA = -21;
const uint RID_INPUT = 0x10000003;
const Dword RIM_TYPEMOUSE = 0;
const ushort MOUSE_MOVE_ABSOLUTE = 1;
const uint CF_UNICODETEXT = 13;
const int WHEEL_DELTA = 120;
const usz SC_KEYMENU = 0xF100;

const uint WM_DESTROY = 0x0002;
const uint WM_SIZE = 0x0005;
const uint WM_KILLFOCUS = 0x0008;
const uint WM_QUIT = 0x0012;
const uint WM_NCCREATE = 0x0081;
const uint WM_INPUT = 0x00FF;
const uint WM_KEYDOWN = 0x0100;
const uint WM_CHAR = 0x0102;
const uint WM_SYSKEYDOWN = 0x0104;
const uint WM_SYSCOMMAND = 0x0112;
const uint WM_LBUTTONDOWN = 0x0201;
const uint WM_LBUTTONUP = 0x0202;
const uint WM_RBUTTONDOWN = 0x0204;
const uint WM_RBUTTONUP = 0x0205;
const uint WM_MBUTTONDOWN = 0x0207;
const uint WM_MBUTTONUP = 0x0208;
const uint WM_MOUSEWHEEL = 0x020A;

// Virtual keys the game names (letters and digits are their ASCII codes).
const int VK_LBUTTON = 0x01;
const int VK_RBUTTON = 0x02;
const int VK_BACK = 0x08;
const int VK_TAB = 0x09;
const int VK_RETURN = 0x0D;
const int VK_SHIFT = 0x10;
const int VK_CONTROL = 0x11;
const int VK_MENU = 0x12;
const int VK_ESCAPE = 0x1B;
const int VK_SPACE = 0x20;
const int VK_PRIOR = 0x21;
const int VK_NEXT = 0x22;
const int VK_END = 0x23;
const int VK_HOME = 0x24;
const int VK_LEFT = 0x25;
const int VK_UP = 0x26;
const int VK_RIGHT = 0x27;
const int VK_DOWN = 0x28;
const int VK_DELETE = 0x2E;
const int VK_F1 = 0x70;
const int VK_F2 = 0x71;
const int VK_F3 = 0x72;
const int VK_F11 = 0x7A;
const int VK_LSHIFT = 0xA0;
const int VK_LCONTROL = 0xA2;

extern fn Handle get_module_handle(WString name) @cname("GetModuleHandleW");
extern fn int mul_div(int a, int b, int c) @cname("MulDiv");
extern fn int wide_char_to_multi_byte(uint page, Dword flags, WChar* w, int wn, char* out, int n, void* d, void* used) @cname("WideCharToMultiByte");
extern fn void* global_lock(Handle h) @cname("GlobalLock");
extern fn Bool global_unlock(Handle h) @cname("GlobalUnlock");

extern fn ushort register_class_ex(WndClassEx* wc) @cname("RegisterClassExW");
extern fn Handle load_cursor(Handle instance, usz name) @cname("LoadCursorW");
extern fn Handle monitor_from_point(Point p, Dword flags) @cname("MonitorFromPoint");
extern fn Bool get_monitor_info(Handle monitor, MonitorInfo* info) @cname("GetMonitorInfoW");
extern fn uint get_dpi_for_system() @cname("GetDpiForSystem");
extern fn uint get_dpi_for_window(Hwnd hwnd) @cname("GetDpiForWindow");
extern fn Bool adjust_window_rect_ex_for_dpi(Rect* r, Dword style, Bool menu, Dword ex_style, uint dpi) @cname("AdjustWindowRectExForDpi");
extern fn Hwnd create_window_ex(Dword ex_style, WString cls, WString title, Dword style, int x, int y, int w, int h, Hwnd parent, Handle menu, Handle instance, void* param) @cname("CreateWindowExW");
extern fn Bool destroy_window(Hwnd hwnd) @cname("DestroyWindow");
extern fn Bool get_client_rect(Hwnd hwnd, Rect* r) @cname("GetClientRect");
extern fn Bool get_window_rect(Hwnd hwnd, Rect* r) @cname("GetWindowRect");
extern fn Bool register_raw_input_devices(RawInputDevice* devices, uint count, uint size) @cname("RegisterRawInputDevices");
extern fn uint get_raw_input_data(Handle raw, uint command, void* data, uint* size, uint header_size) @cname("GetRawInputData");
extern fn Bool show_window(Hwnd hwnd, int cmd) @cname("ShowWindow");
extern fn Bool peek_message(Msg* msg, Hwnd hwnd, uint min, uint max, uint remove) @cname("PeekMessageW");
extern fn Bool translate_message(Msg* msg) @cname("TranslateMessage");
extern fn LResult dispatch_message(Msg* msg) @cname("DispatchMessageW");
extern fn sz set_window_long_ptr(Hwnd hwnd, int index, sz value) @cname("SetWindowLongPtrW");
extern fn sz get_window_long_ptr(Hwnd hwnd, int index) @cname("GetWindowLongPtrW");
extern fn LResult def_window_proc(Hwnd hwnd, uint msg, WParam wp, LParam lp) @cname("DefWindowProcW");
extern fn void post_quit_message(int code) @cname("PostQuitMessage");
extern fn int map_window_points(Hwnd from, Hwnd to, Point* points, uint count) @cname("MapWindowPoints");
extern fn Bool clip_cursor(Rect* r) @cname("ClipCursor");
extern fn int show_cursor(Bool show) @cname("ShowCursor");
extern fn Bool is_window_visible(Hwnd hwnd) @cname("IsWindowVisible");
extern fn Bool get_cursor_pos(Point* p) @cname("GetCursorPos");
extern fn Bool set_cursor_pos(int x, int y) @cname("SetCursorPos");
extern fn Bool screen_to_client(Hwnd hwnd, Point* p) @cname("ScreenToClient");
extern fn Bool is_clipboard_format_available(uint format) @cname("IsClipboardFormatAvailable");
extern fn Bool open_clipboard(Hwnd hwnd) @cname("OpenClipboard");
extern fn Handle get_clipboard_data(uint format) @cname("GetClipboardData");
extern fn Bool close_clipboard() @cname("CloseClipboard");
extern fn Hwnd get_foreground_window() @cname("GetForegroundWindow");
extern fn short get_async_key_state(int vk) @cname("GetAsyncKeyState");
extern fn Hwnd set_capture(Hwnd hwnd) @cname("SetCapture");
extern fn Hwnd get_capture() @cname("GetCapture");
extern fn Bool release_capture() @cname("ReleaseCapture");
extern fn Bool set_window_text(Hwnd hwnd, WString text) @cname("SetWindowTextW");
extern fn Bool set_process_dpi_awareness_context(sz awareness) @cname("SetProcessDpiAwarenessContext");
extern fn int message_box(Hwnd hwnd, WString text, WString caption, uint type) @cname("MessageBoxW");
extern fn int get_key_name_text(int lparam, WChar* out, int n) @cname("GetKeyNameTextW");
extern fn uint map_virtual_key(uint code, uint map_type) @cname("MapVirtualKeyW");

const sz DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4;
const uint MB_ICONERROR = 0x10;

// UTF-16 (a whole string, or `n` units) as UTF-8, in `alloc`.
fn String utf8_of(Allocator alloc, WChar* w, int n = -1)
{
	int len = wide_char_to_multi_byte(65001, 0, w, n, null, 0, null, null);
	if (len <= 0) return "";
	char[] out = alloc::alloc_array(alloc, char, (sz)len + 1);
	wide_char_to_multi_byte(65001, 0, w, n, out.ptr, len, null, null);
	if (n < 0) len--; // (the terminator)
	out[len] = 0;
	return (String)out[:len];
}
