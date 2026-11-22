#pragma once
// Small math the game shares: angles (pi, a full turn, turning the short
// way), easing, and rounding toward minus infinity.

#include <algorithm>
#include <cmath>

namespace gc {

inline constexpr float PI = 3.14159265f;
inline constexpr float TAU = 2 * PI;

// From `from` toward `to` by `w` (0..1), the short way round.
inline float lerp_angle(float from, float to, float w) {
	float d = std::fmod(to - from, TAU);
	if (d > PI) {
		d -= TAU;
	} else if (d < -PI) {
		d += TAU;
	}
	return from + d * w;
}

// 0..1 eased in and out (smoothstep); clamped.
inline float ease(float x) {
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

// The block a coordinate is in.
inline int floor_i(float v) {
	return int(std::floor(v));
}

// v / d rounded toward minus infinity (d > 0): the chunk a block is in.
inline int floor_div(int v, int d) {
	return v >= 0 ? v / d : -((-v + d - 1) / d);
}

} // namespace gc
