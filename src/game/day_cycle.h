#pragma once
// The time of day (0..1, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset) and what
// follows from it: daylight, the sky's colours, where the sun is. As the
// Godot version's DayCycle. A day lasts day_length seconds (a real day by
// default: new worlds start at the computer's clock time).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace gc {

struct Rgb {
	float r, g, b;
	Rgb lerp(const Rgb &o, float t) const { return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t}; }
};

class DayCycle {
public:
	static constexpr double DEFAULT_DAY_LENGTH = 86400.0;
	static constexpr float NIGHT_LIGHT = 0.02f;    // sky light at night: dark, bring a torch
	static constexpr double MORNING = 0.26;        // what sleeping wakes you at
	static constexpr float PI = 3.14159265358979f;
	// (Colours as the Godot version's: linear, as its shaders get them.)
	static constexpr Rgb NIGHT_TINT = {0.6f, 0.7f, 1.0f}; // moonlight is a cool blue
	static constexpr Rgb DAY_TOP = {0.32f, 0.52f, 0.86f};
	static constexpr Rgb DAY_HORIZON = {0.68f, 0.79f, 0.92f};
	static constexpr Rgb DUSK_HORIZON = {0.95f, 0.55f, 0.32f};
	static constexpr Rgb NIGHT_TOP = {0.015f, 0.02f, 0.06f};
	static constexpr Rgb NIGHT_HORIZON = {0.05f, 0.07f, 0.14f};
	static constexpr Rgb NIGHT_FOG = {0.006f, 0.008f, 0.02f};
	static constexpr float NIGHT_FOG_BEGIN = 4.0f, NIGHT_FOG_END = 40.0f;

	double time = 0.3;
	double day_length = DEFAULT_DAY_LENGTH;
	float cloud_offset[2] = {0, 0}; // drifting clouds, in cloud cells

	void update(double dt) {
		time = std::fmod(time + dt / std::max(10.0, day_length), 1.0);
		cloud_offset[0] += 0.35f * float(dt);
		cloud_offset[1] += 0.12f * float(dt);
	}
	// The computer's clock as a time of day.
	static double system_time() {
		const std::time_t t = std::time(nullptr);
		std::tm tm;
		localtime_s(&tm, &t);
		return (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) / 86400.0;
	}

	float angle() const { return float(time - 0.25) * 2.0f * PI; } // the sun above the eastern horizon
	float sun_height() const { return std::sin(angle()); }
	float day_amount() const { return smoothstep(-0.18f, 0.22f, sun_height()); }
	float daylight() const { return NIGHT_LIGHT + (1.0f - NIGHT_LIGHT) * day_amount(); }
	Rgb sky_tint() const { return NIGHT_TINT.lerp({1, 1, 1}, day_amount()); }
	float sun_amount() const { return day_amount() * std::clamp(sun_height() * 3.0f, 0.0f, 1.0f); }
	float dusk() const { return std::clamp(1.0f - std::fabs(sun_height()) / 0.3f, 0.0f, 1.0f); }
	// Towards the sun: it rises in the east (+x), overhead at noon, sets in the west.
	void sun_dir(float out[3]) const {
		out[0] = std::cos(angle());
		out[1] = std::sin(angle());
		out[2] = 0;
	}
	Rgb haze() const {
		const float day = day_amount();
		return NIGHT_FOG.lerp(DAY_HORIZON, day).lerp(DUSK_HORIZON, dusk() * 0.4f * day);
	}
	Rgb sky_top() const { return NIGHT_TOP.lerp(DAY_TOP, day_amount()); }
	Rgb sky_horizon() const {
		return NIGHT_HORIZON.lerp(DAY_HORIZON, day_amount()).lerp(DUSK_HORIZON, dusk() * 0.65f);
	}
	// Depth fog: from 0.45 of the view distance, thick at 0.95, by day (the
	// Godot version's main._set_fog_range); much nearer at night.
	float fog_begin(float view) const { return NIGHT_FOG_BEGIN + (view * 0.45f - NIGHT_FOG_BEGIN) * day_amount(); }
	float fog_end(float view) const { return NIGHT_FOG_END + (view * 0.95f - NIGHT_FOG_END) * day_amount(); }

	// "7:05 AM" or "07:05".
	static void clock_text(double time, bool use_24h, char *out, size_t size) {
		const int minutes = int(std::fmod(time, 1.0) * 24.0 * 60.0);
		const int hour = minutes / 60, minute = minutes % 60;
		if (use_24h) {
			snprintf(out, size, "%02d:%02d", hour, minute);
		} else {
			snprintf(out, size, "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
		}
	}

private:
	static float smoothstep(float a, float b, float x) {
		const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}
};

} // namespace gc
