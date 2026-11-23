// The settings screen: view distance, mouse sensitivity, frame rate limit,
// vsync, smooth lighting, sun shadows, render scale, volumes, the clock, key
// bindings. Changes apply at once (the game reads the settings every frame)
// and are saved when it closes. From the title menu and the pause menu.
module gocube::settings_screen;
import gocube::gui, gocube::game::settings, gocube::window, gocube::win;
import std::math;

const Color DIM @private = { 255, 255, 255, 153 };

struct SettingsScreen
{
	int capturing; // the action waiting for a key (-1: none)
	bool[256] was_down;
}

fn void SettingsScreen.open(&self) => self.capturing = -1;
fn bool SettingsScreen.waiting_for_key(&self) => self.capturing >= 0;

// A key's name ("W", "Space", "Left Shift"...; temp).
fn String key_name(int vk)
{
	switch (vk) // (names GetKeyNameText gives oddly or not at all)
	{
		case win::VK_SPACE: return "Space";
		case win::VK_SHIFT: return "Shift";
		case win::VK_CONTROL: return "Ctrl";
		case win::VK_MENU: return "Alt";
		default:
	}
	uint scan = win::map_virtual_key((uint)vk, 0); // (MAPVK_VK_TO_VSC)
	bool extended = vk == win::VK_LEFT || vk == win::VK_RIGHT || vk == win::VK_UP || vk == win::VK_DOWN || vk == win::VK_PRIOR ||
		vk == win::VK_NEXT || vk == win::VK_HOME || vk == win::VK_END || vk == 0x2D || vk == win::VK_DELETE || vk == 0x6F || vk == 0xA3 ||
		vk == 0xA5 || vk == 0x90;
	WChar[64] name;
	int n = win::get_key_name_text((int)(scan << 16 | (extended ? 1 << 24 : 0)), &name, 64);
	if (n <= 0) return string::tformat("Key %d", vk);
	return win::utf8_of(tmem, &name, n);
}

fn void heading(Gui* g, String text) @private
{
	g.space(6);
	g.text(text, 1.25f);
}

// A row of the settings: what it is on the left, its control (next) on the right.
fn void row(Gui* g, String label, float column) @private
{
	g.align_text_to_frame();
	g.text(label);
	g.same_line(column);
	g.set_next_width(-4);
}

// One frame of it; false once it closes (Done, or Esc while not waiting for
// a key), with the settings saved to `file`.
fn bool SettingsScreen.draw(&self, Gui* g, Settings* s, Window* window, bool esc_pressed, String file) => @pool()
{
	// Waiting for a key: the next one pressed (Esc cancels).
	bool keep_open = true;
	if (self.capturing >= 0)
	{
		for (int vk = 8; vk < 255; vk++)
		{
			bool down = window.key_down(vk);
			bool pressed = down && !self.was_down[vk];
			self.was_down[vk] = down;
			if (!pressed || (vk >= 0xA0 && vk <= 0xA5)) continue; // (the side-less codes come too)
			if (vk != win::VK_ESCAPE) s.keys[self.capturing] = vk;
			self.capturing = -1;
			break;
		}
	}
	else if (esc_pressed)
	{
		keep_open = false;
	}
	g.background.fill({ 0, 0, g.in.width, g.in.height }, gui::rgba(0, 0, 0, 150));
	g.begin_window("Settings##screen", { .pos = { g.in.width * 0.5f, g.in.height * 0.5f }, .pivot = { 0.5f, 0.5f },
		.size = { math::min(680.0f, g.in.width - 40), math::min(660.0f, g.in.height - 40) } });
	g.set_cursor_x((g.window_width() - g.text_size("Settings", 1.6f).x) * 0.5f - g.theme.padding.x);
	g.text("Settings", 1.6f);
	g.begin_child("settings", { 0, -g.frame_height() - g.theme.spacing.y - 6 }, false);
	float column = math::floor(g.available_width() * 0.4f);
	// View distance.
	row(g, string::tformat("View distance: %d blocks", s.view_distance), column);
	int view = 0;
	foreach (i, d : settings::VIEW_DISTANCES) if (d <= s.view_distance) view = (int)i;
	if (g.slider_int("##view", &view, 0, settings::VIEW_DISTANCES.len - 1)) s.view_distance = settings::VIEW_DISTANCES[view];
	// Mouse sensitivity.
	row(g, string::tformat("Mouse sensitivity: %.1f", s.mouse_sensitivity * 1000.0f), column);
	float sens = s.mouse_sensitivity * 1000.0f;
	if (g.slider("##sens", &sens, 0.5f, 10.0f)) s.mouse_sensitivity = math::round(sens * 10.0f) / 10000.0f;
	// Frame rate limit.
	row(g, "Frame rate limit", column);
	String[settings::FPS_LIMITS.len] fps_names;
	int at = 0;
	foreach (i, limit : settings::FPS_LIMITS)
	{
		fps_names[i] = limit == 0 ? "Unlimited" : string::tformat("%d FPS", limit);
		if (limit == s.max_fps) at = (int)i;
	}
	if (g.combo("##fps", &at, &fps_names)) s.max_fps = settings::FPS_LIMITS[at];
	row(g, "VSync", column);
	g.checkbox("Wait for the screen (no tearing)", &s.vsync);
	row(g, "Smooth lighting", column);
	g.checkbox("Blend light at block corners", &s.smooth_lighting);
	row(g, "Sun shadows", column);
	String[settings::SHADOW_MODES.len] shadow_names;
	at = 0;
	foreach (i, m : settings::SHADOW_MODES)
	{
		shadow_names[i] = m == "blocks" ? "Blocks (whole faces, cheap)" : "Off";
		if (s.shadow_mode == m) at = (int)i;
	}
	if (g.combo("##shadows", &at, &shadow_names)) s.shadow_mode = settings::SHADOW_MODES[at];
	row(g, "Render scale", column);
	String[settings::RENDER_SCALES.len] scale_names;
	at = 0;
	foreach (i, f : settings::RENDER_SCALES)
	{
		scale_names[i] = string::tformat("%d%%%s", (int)math::round(f * 100), f >= 1.0f ? " (sharpest)" : "");
		if (math::abs(f - s.render_scale) < 0.001f) at = (int)i;
	}
	if (g.combo("##scale", &at, &scale_names)) s.render_scale = settings::RENDER_SCALES[at];
	if (g.item_hovered()) g.tooltip("Draws the world at a lower resolution and stretches it: blurrier, but much less GPU.");
	String[3] volume_names = { "Master volume", "Effects volume", "Ambience volume" };
	float*[3] volumes = { &s.master_volume, &s.effects_volume, &s.ambience_volume };
	foreach (i, name : volume_names)
	{
		row(g, string::tformat("%s: %d%%", name, (int)math::round(*volumes[i] * 100)), column);
		int percent = (int)math::round(*volumes[i] * 20) * 5;
		g.push_id(name);
		if (g.slider_int("##v", &percent, 0, 100)) *volumes[i] = (float)(percent / 5 * 5) / 100.0f;
		g.pop_id();
	}
	row(g, "Clock", column);
	g.checkbox("24-hour clock (07:19 instead of 7:19 AM)", &s.clock_24h);
	// Key bindings.
	heading(g, "Keys (click, then press a key; Esc cancels)");
	foreach (i, a : settings::ACTIONS)
	{
		row(g, a.label, column);
		int k = s.keys[i];
		String text = self.capturing == (int)i ? "Press a key..." : key_name(k);
		if (self.capturing != (int)i && s.actions_using(k) > 1) text = string::tformat("%s  (also used!)", text);
		g.push_id(a.id);
		if (g.button(string::tformat("%s##key", text), { 200, 0 }))
		{
			self.capturing = (int)i;
			for (int vk = 0; vk < 256; vk++) self.was_down[vk] = window.key_down(vk); // (the click itself isn't a key)
		}
		g.pop_id();
	}
	g.text_wrapped("Mouse: look, hold left = mine, right = place, wheel/1-9 = hotbar · Esc = pause · F3 = details", DIM);
	g.end_child();
	// Reset keys, Done.
	g.set_cursor_x(g.window_width() - 2 * g.theme.padding.x - 228);
	if (g.button("Reset keys", { 110, 0 })) s.reset_keys();
	g.same_line();
	if (g.button("Done", { 110, 0 })) keep_open = false;
	g.end_window();
	if (!keep_open) s.save(file);
	return keep_open;
}
