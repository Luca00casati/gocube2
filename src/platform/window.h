#pragma once
// The game's one Win32 window.

#include <windows.h>

#include <string>
#include <vector>

namespace gc {

class Window {
public:
	Window() = default;
	Window(const Window &) = delete;
	Window &operator=(const Window &) = delete;
	~Window();

	// `width` x `height`: the client area (scaled for the DPI); 0 x 0: three
	// quarters of the screen each way, in its middle.
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

	// For the UI (ui/gui.h): what happened since the last call.
	struct UiEvents {
		std::string text;      // typed, UTF-8
		std::vector<int> keys; // keys pressed (virtual-key codes, repeats too)
		int clicks[BUTTONS] = {};
		float wheel = 0;
	};
	UiEvents take_ui_events();
	// The mouse in the client area (false: outside it, or captured).
	bool mouse_position(float &x, float &y) const;
	bool button_down(Button b) const { return down_[b]; }
	// The clipboard's text (UTF-8), if it has some.
	std::string clipboard_text() const;

private:
	static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

	HWND hwnd_ = nullptr;
	int width_ = 0, height_ = 0;
	bool resized_ = false;
	bool open_ = false;
	bool captured_ = false;
	bool pressed_[BUTTONS] = {};
	bool down_[BUTTONS] = {};
	UiEvents ui_;
	wchar_t high_surrogate_ = 0;
	float wheel_ = 0;
	float mouse_dx_ = 0, mouse_dy_ = 0;
};

} // namespace gc
