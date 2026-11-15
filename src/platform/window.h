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

	// Mouse capture for looking around: the cursor is hidden and held in the
	// window, and raw mouse motion is collected. Losing focus releases it.
	void set_mouse_captured(bool captured);
	bool mouse_captured() const { return captured_; }
	// Raw mouse motion since the last call (counts, not pixels).
	void take_mouse_delta(float &dx, float &dy) {
		dx = mouse_dx_;
		dy = mouse_dy_;
		mouse_dx_ = mouse_dy_ = 0;
	}
	enum Button { LEFT, RIGHT, MIDDLE, BUTTONS };
	// True once after that mouse button was pressed in the window.
	bool take_pressed(Button b) {
		const bool p = pressed_[b];
		pressed_[b] = false;
		return p;
	}
	// Mouse wheel notches since the last call (positive: away from you).
	float take_wheel() {
		const float w = wheel_;
		wheel_ = 0;
		return w;
	}
	// Whether a key (a virtual-key code) is held, while the window has focus.
	bool key_down(int vk) const;

private:
	static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

	HWND hwnd_ = nullptr;
	int width_ = 0, height_ = 0;
	bool resized_ = false;
	bool open_ = false;
	bool captured_ = false;
	bool pressed_[BUTTONS] = {};
	float wheel_ = 0;
	float mouse_dx_ = 0, mouse_dy_ = 0;
};

} // namespace gc
