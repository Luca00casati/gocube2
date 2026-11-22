#include "game/figure.h"

#include "maths.h"

#include <algorithm>
#include <cmath>

namespace gc {

namespace {

// A strike: winding up ends at WIND, the chop lands at HIT, then the arm eases
// back. The arm at the top of the wind-up and where the chop lands:
// {shoulder (up/forward), out to the side (+) or across (-), elbow bend}.
constexpr float SWING_WIND = 0.4f, SWING_HIT = 0.62f;
constexpr float SWING_TOP[3] = {2.5f, 0.4f, 1.25f};
constexpr float SWING_LAND[3] = {0.3f, -0.45f, 0.5f};

float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

} // namespace

void Figure::swing() {
	if (swing_ < 0) {
		swing_ = 0;
	} else {
		swing_next_ = true;
	}
}

void Figure::update(float dt) {
	auto k = [&](float rate) { return 1.0f - std::exp(-rate * dt); };
	const float target_amount = on_floor ? std::clamp(move_speed / RUN_SPEED, 0.0f, 1.0f) : 0.0f;
	amount_ = lerp(amount_, target_amount, k(10));
	air_ = lerp(air_, on_floor ? 0.0f : 1.0f, k(12));
	sneak_ = lerp(sneak_, sneaking && !lying ? 1.0f : 0.0f, k(12));
	lie = lerp(lie, lying ? 1.0f : 0.0f, k(8));
	// Steps follow the distance walked, so the feet don't slide.
	if (on_floor) {
		phase_ = std::fmod(phase_ + move_speed * dt / STRIDE * TAU, TAU);
	} else if (amount_ < 0.05f) {
		phase_ = lerp(phase_, 0.0f, k(6));
	}
	if (swing_ >= 0) {
		swing_ += dt;
		if (swing_ >= SWING_TIME) {
			swing_ = swing_next_ ? 0.0f : -1.0f;
			swing_next_ = false;
		}
	}
	pose(dt);
}

void Figure::pose(float dt) {
	auto k = [&](float rate) { return 1.0f - std::exp(-rate * dt); };
	const float walk = amount_;
	const float run = std::clamp((amount_ - 0.55f) / 0.45f, 0.0f, 1.0f); // past a walk: running
	const float s = std::sin(phase_), c = std::cos(phase_);

	// Legs swing forward and back; the knee bends as the leg comes forward (more
	// running); in the air one knee is pulled up.
	const float leg_amp = 0.55f * walk + 0.35f * run;
	float legs[2][2];
	for (int i = 0; i < 2; i++) {
		const float side = i == 0 ? -1.0f : 1.0f;
		const float swing_leg = s * side * leg_amp;
		const float knee = std::max(0.0f, -c * side) * (0.5f * walk + 0.7f * run);
		const float air_leg = (side < 0 ? 0.55f : -0.25f) * air_;
		const float air_knee = (side < 0 ? 1.0f : 0.35f) * air_;
		legs[i][0] = swing_leg + air_leg + 0.55f * sneak_;
		legs[i][1] = knee + air_knee + 0.9f * sneak_;
	}
	// Arms swing against the legs; elbows bend more running; in the air they
	// lift out a little.
	const float arm_amp = 0.7f * walk + 0.4f * run;
	float arms[2][3];
	for (int i = 0; i < 2; i++) {
		const float side = i == 0 ? -1.0f : 1.0f;
		arms[i][0] = -s * side * arm_amp + 0.25f * sneak_;
		arms[i][1] = side * (0.06f + 0.35f * air_);
		arms[i][2] = 0.15f + 0.9f * run + 0.2f * sneak_;
	}
	// The right arm strikes like an axe blow: winds up (elbow beside the head),
	// chops down and across, speeding up, then eases back.
	float twist_swing = 0;
	if (swing_ >= 0) {
		const float t = swing_ / SWING_TIME;
		const float rest[3] = {arms[1][0], arms[1][1], arms[1][2]};
		float p[3];
		if (t < SWING_WIND) {
			const float u = ease(t / SWING_WIND);
			p[0] = lerp(rest[0], SWING_TOP[0], u);
			p[1] = lerp(rest[1], SWING_TOP[1], u);
			p[2] = lerp(rest[2], SWING_TOP[2], std::min(1.0f, u * 2.0f)); // the elbow bends first
			twist_swing = -0.2f * u;
		} else if (t < SWING_HIT) {
			const float u = (t - SWING_WIND) / (SWING_HIT - SWING_WIND);
			const float fall = u * u; // speeds up into the blow
			p[0] = lerp(SWING_TOP[0], SWING_LAND[0], fall);
			p[1] = lerp(SWING_TOP[1], SWING_LAND[1], fall);
			p[2] = lerp(SWING_TOP[2], SWING_LAND[2], fall * fall); // and straightens last
			twist_swing = lerp(-0.2f, 0.35f, fall);
		} else {
			const float u = ease((t - SWING_HIT) / (1.0f - SWING_HIT));
			p[0] = lerp(SWING_LAND[0], rest[0], u);
			p[1] = lerp(SWING_LAND[1], rest[1], u);
			p[2] = lerp(SWING_LAND[2], rest[2], u);
			twist_swing = lerp(0.35f, 0.0f, u);
		}
		std::copy(p, p + 3, arms[1]);
	}

	const float lean = 0.12f * walk + 0.2f * run + 0.5f * sneak_;
	const float bob = -std::abs(c) * 0.04f * walk - 0.05f * run - 0.16f * sneak_;
	const float twist = s * 0.12f * walk + twist_swing; // shoulders turn with the stride

	auto blend = [&](Joint j, float x, float y, float z, float rate) {
		const float w = k(rate);
		rot[j][0] = lerp_angle(rot[j][0], x, w);
		rot[j][1] = lerp_angle(rot[j][1], y, w);
		rot[j][2] = lerp_angle(rot[j][2], z, w);
	};
	const float r = 18.0f;
	blend(HIPS, 0, -twist * 0.4f, 0, r);
	hips_y = lerp(hips_y, HIP_Y + bob, k(r));
	blend(TORSO_J, -lean, twist, 0, r);
	// The head keeps looking where the figure looks, whatever the body does.
	blend(HEAD, std::clamp(pitch, -1.2f, 1.2f) + lean * 0.8f, -twist, 0, 25.0f);
	const Joint leg[2] = {LEG_L, LEG_R}, knee[2] = {KNEE_L, KNEE_R}, arm[2] = {ARM_L, ARM_R},
				elbow[2] = {ELBOW_L, ELBOW_R};
	for (int i = 0; i < 2; i++) {
		blend(leg[i], legs[i][0], 0, 0, r);
		blend(knee[i], -legs[i][1], 0, 0, r);
		if (i == 1 && swing_ >= 0) { // the strike sets the arm exactly
			rot[ARM_R][0] = arms[1][0];
			rot[ARM_R][1] = 0;
			rot[ARM_R][2] = arms[1][1];
			rot[ELBOW_R][0] = arms[1][2];
			rot[ELBOW_R][1] = rot[ELBOW_R][2] = 0;
		} else {
			blend(arm[i], arms[i][0], 0, arms[i][1], r);
			blend(elbow[i], arms[i][2], 0, 0, r);
		}
	}
}

} // namespace gc
