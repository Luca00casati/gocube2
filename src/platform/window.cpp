#include "platform/window.h"

#include <imgui.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

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

	// Size the client area, not the frame, scaled for the monitor's DPI.
	const UINT dpi = GetDpiForSystem();
	RECT rect = {0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
	AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);

	hwnd_ = CreateWindowExW(0, CLASS_NAME, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
			rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
	if (!hwnd_) {
		return false;
	}
	RECT client;
	GetClientRect(hwnd_, &client);
	width_ = client.right - client.left;
	height_ = client.bottom - client.top;
	open_ = true;
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
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
		return 1;
	}
	if (msg == WM_NCCREATE) {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		static_cast<Window *>(cs->lpCreateParams)->hwnd_ = hwnd; // (before CreateWindowExW returns)
	}
	auto *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_SIZE: {
			const int w = LOWORD(lp), h = HIWORD(lp);
			if (w != width_ || h != height_) {
				width_ = w;
				height_ = h;
				resized_ = true;
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
