#pragma once
// The game's one Win32 window.

#include <windows.h>

namespace gc {

class Window {
public:
	Window() = default;
	Window(const Window &) = delete;
	Window &operator=(const Window &) = delete;
	~Window();

	bool create(const wchar_t *title, int width, int height, bool visible);
	// Handles pending messages; false once the window has been closed.
	bool pump();

	HWND hwnd() const { return hwnd_; }
	int width() const { return width_; }
	int height() const { return height_; }
	bool minimized() const { return width_ == 0 || height_ == 0; }
	// True once after the client area changed size.
	bool take_resized() {
		const bool r = resized_;
		resized_ = false;
		return r;
	}

private:
	static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

	HWND hwnd_ = nullptr;
	int width_ = 0, height_ = 0;
	bool resized_ = false;
	bool open_ = false;
};

} // namespace gc
