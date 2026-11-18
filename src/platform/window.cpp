#include "platform/window.h"

#include <algorithm>

namespace gc {

namespace {
constexpr wchar_t CLASS_NAME[] = L"GoCubeWindow";
}

Window::~Window() {
	if (hwnd_) {
		DestroyWindow(hwnd_);
	}
}

bool Window::create(const wchar_t *title, int width, int height, bool visible) {
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = &Window::proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = CLASS_NAME;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}

	int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w, h;
	MONITORINFO mi = {sizeof(mi)};
	if (width <= 0 || height <= 0) {
		// Three quarters of the screen each way (its work area: without the
		// taskbar), in the middle of it.
		GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
		const RECT &work = mi.rcWork;
		w = (work.right - work.left) * 3 / 4;
		h = (work.bottom - work.top) * 3 / 4;
		x = work.left + (work.right - work.left - w) / 2;
		y = work.top + (work.bottom - work.top - h) / 2;
	} else {
		// Size the client area, not the frame, scaled for the monitor's DPI.
		const UINT dpi = GetDpiForSystem();
		RECT rect = {0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
		AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
		w = rect.right - rect.left;
		h = rect.bottom - rect.top;
	}

	hwnd_ = CreateWindowExW(0, CLASS_NAME, title, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, instance, this);
	if (!hwnd_) {
		return false;
	}
	RECT client;
	GetClientRect(hwnd_, &client);
	width_ = client.right - client.left;
	height_ = client.bottom - client.top;
	open_ = true;
	// Raw mouse motion (WM_INPUT): unaffected by pointer acceleration and the screen's edges.
	RAWINPUTDEVICE mouse = {0x01, 0x02, 0, hwnd_}; // generic desktop, mouse
	RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
	if (visible) {
		ShowWindow(hwnd_, SW_SHOWDEFAULT);
	}
	return true;
}

bool Window::pump() {
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		if (msg.message == WM_QUIT) {
			open_ = false;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return open_;
}

LRESULT CALLBACK Window::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_NCCREATE) {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		static_cast<Window *>(cs->lpCreateParams)->hwnd_ = hwnd; // (before CreateWindowExW returns)
	}
	auto *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void Window::set_mouse_captured(bool captured) {
	if (captured == captured_ || !hwnd_) {
		return;
	}
	captured_ = captured;
	if (captured) {
		RECT r;
		GetClientRect(hwnd_, &r);
		MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT *>(&r), 2);
		ClipCursor(&r);
		while (ShowCursor(FALSE) >= 0) {
		}
	} else {
		ClipCursor(nullptr);
		while (ShowCursor(TRUE) < 0) {
		}
	}
	mouse_dx_ = mouse_dy_ = 0;
}

Window::UiEvents Window::take_ui_events() {
	UiEvents e = std::move(ui_);
	ui_ = UiEvents{};
	return e;
}

bool Window::mouse_position(float &x, float &y) const {
	POINT p;
	RECT r;
	if (!hwnd_ || captured_ || !IsWindowVisible(hwnd_) || !GetCursorPos(&p) || !ScreenToClient(hwnd_, &p) || !GetClientRect(hwnd_, &r) ||
			p.x < 0 || p.y < 0 || p.x >= r.right || p.y >= r.bottom) {
		return false;
	}
	x = float(p.x);
	y = float(p.y);
	return true;
}

std::string Window::clipboard_text() const {
	std::string out;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(hwnd_)) {
		return out;
	}
	if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
		if (const auto *w = static_cast<const wchar_t *>(GlobalLock(h))) {
			const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n > 1) {
				out.resize(size_t(n));
				WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
				out.resize(size_t(n - 1));
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
	return out;
}

bool Window::key_down(int vk) const {
	return hwnd_ && GetForegroundWindow() == hwnd_ && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

LRESULT Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_INPUT: {
			RAWINPUT raw;
			UINT size = sizeof(raw);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
					raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && captured_) {
				mouse_dx_ += float(raw.data.mouse.lLastX);
				mouse_dy_ += float(raw.data.mouse.lLastY);
			}
			break; // DefWindowProc cleans up
		}
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN: {
			const Button b = msg == WM_LBUTTONDOWN ? LEFT : msg == WM_RBUTTONDOWN ? RIGHT : MIDDLE;
			pressed_[b] = true;
			down_[b] = true;
			if (!captured_) {
				ui_.clicks[b]++;
				SetCapture(hwnd_); // (a drag past the window's edge still ends here)
			}
			return 0;
		}
		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
			down_[msg == WM_LBUTTONUP ? LEFT : msg == WM_RBUTTONUP ? RIGHT : MIDDLE] = false;
			if (!down_[LEFT] && !down_[RIGHT] && !down_[MIDDLE] && GetCapture() == hwnd_) {
				ReleaseCapture();
			}
			return 0;
		case WM_MOUSEWHEEL: {
			const float notches = float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
			wheel_ += notches;
			ui_.wheel += notches;
			return 0;
		}
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (ui_.keys.size() < 64) {
				ui_.keys.push_back(int(wp));
			}
			break; // (then WM_CHAR, through TranslateMessage)
		case WM_CHAR: {
			wchar_t w[2] = {wchar_t(wp), 0};
			int n = 1;
			if (w[0] >= 0xd800 && w[0] < 0xdc00) { // the first half of a pair: wait for the second
				high_surrogate_ = w[0];
				return 0;
			}
			if (w[0] >= 0xdc00 && w[0] < 0xe000) {
				if (!high_surrogate_) {
					return 0;
				}
				w[1] = w[0];
				w[0] = high_surrogate_;
				n = 2;
			}
			high_surrogate_ = 0;
			if (w[0] >= 32 && w[0] != 127 && ui_.text.size() < 256) {
				char out[8];
				const int m = WideCharToMultiByte(CP_UTF8, 0, w, n, out, sizeof(out), nullptr, nullptr);
				ui_.text.append(out, size_t(std::max(m, 0)));
			}
			return 0;
		}
		case WM_KILLFOCUS:
			set_mouse_captured(false);
			down_[LEFT] = down_[RIGHT] = down_[MIDDLE] = false;
			return 0;
		case WM_SIZE: {
			const int w = LOWORD(lp), h = HIWORD(lp);
			if (w != width_ || h != height_) {
				width_ = w;
				height_ = h;
				resized_ = true;
				if (captured_) { // hold the cursor in the new client area
					set_mouse_captured(false);
					set_mouse_captured(true);
				}
			}
			return 0;
		}
		case WM_SYSCOMMAND:
			if ((wp & 0xfff0) == SC_KEYMENU) { // no beep and menu on Alt
				return 0;
			}
			break;
		case WM_DESTROY:
			hwnd_ = nullptr;
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace gc
