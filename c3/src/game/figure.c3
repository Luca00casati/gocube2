// A jointed figure made of boxes (players, creatures with the "figure" model),
// animated from how it moves. Feet at the origin, facing -z, 1.8 m tall.
//
// Joints: hips -> torso (bends at the waist) -> head, shoulders -> upper arm
// -> elbow -> forearm, and hips -> thigh -> knee -> shin. Each frame the owner
// sets the inputs; the figure blends towards the pose they ask for (joint by
// joint), so changes never snap. swing() plays a strike with the right arm.
module gocube::game::figure;
import gocube::maths, std::math;

const float HIP_Y = 0.76f;
const float THIGH = 0.38f;
const float SHIN = 0.38f;
const float TORSO = 0.62f;
const float UPPER_ARM = 0.3f;
const float FOREARM = 0.32f;
const float STRIDE = 1.5f;     // metres a full walk cycle (two steps)
const float RUN_SPEED = 5.0f;  // the stride is fully stretched at this speed
const float SWING_TIME = 0.4f;

enum Joint : char
{
	HIPS, TORSO_J, HEAD, ARM_L, ARM_R, ELBOW_L, ELBOW_R, LEG_L, LEG_R, KNEE_L, KNEE_R
}

const int JOINTS = 11;

// A strike: winding up ends at WIND, the chop lands at HIT, then the arm eases
// back. The arm at the top of the wind-up and where the chop lands:
// {shoulder (up/forward), out to the side (+) or across (-), elbow bend}.
const float SWING_WIND @private = 0.4f;
const float SWING_HIT @private = 0.62f;
const float[3] SWING_TOP @private = { 2.5f, 0.4f, 1.25f };
const float[3] SWING_LAND @private = { 0.3f, -0.45f, 0.5f };

struct Figure
{
	// Inputs.
	float move_speed;  // horizontal m/s
	bool on_floor, sneaking, lying;
	float pitch;       // looking up (+) or down (-)
	// The pose and the hips' height.
	float[3][JOINTS] rot;
	float hips_y;
	float lie;         // 0 standing .. 1 lying down
	float phase, amount, air, sneak;
	float swing_t;     // -1: no strike
	bool swing_next;
}

fn Figure new() => { .on_floor = true, .hips_y = HIP_Y, .swing_t = -1 };

fn float lerp(float a, float b, float t) @private @inline => a + (b - a) * t;
fn float rate(float r, float dt) @private @inline => 1.0f - math::exp(-r * dt);

// One asked for during another plays right after it.
fn void Figure.swing(&self)
{
	if (self.swing_t < 0)
	{
		self.swing_t = 0;
	}
	else
	{
		self.swing_next = true;
	}
}

fn bool Figure.swinging(&self) => self.swing_t >= 0;

fn void Figure.update(&self, float dt)
{
	float target_amount = self.on_floor ? math::clamp(self.move_speed / RUN_SPEED, 0.0f, 1.0f) : 0.0f;
	self.amount = lerp(self.amount, target_amount, rate(10, dt));
	self.air = lerp(self.air, self.on_floor ? 0.0f : 1.0f, rate(12, dt));
	self.sneak = lerp(self.sneak, self.sneaking && !self.lying ? 1.0f : 0.0f, rate(12, dt));
	self.lie = lerp(self.lie, self.lying ? 1.0f : 0.0f, rate(8, dt));
	// Steps follow the distance walked, so the feet don't slide.
	if (self.on_floor)
	{
		self.phase = (self.phase + self.move_speed * dt / STRIDE * maths::TAU) % maths::TAU;
	}
	else if (self.amount < 0.05f)
	{
		self.phase = lerp(self.phase, 0.0f, rate(6, dt));
	}
	if (self.swing_t >= 0)
	{
		self.swing_t += dt;
		if (self.swing_t >= SWING_TIME)
		{
			self.swing_t = self.swing_next ? 0.0f : -1.0f;
			self.swing_next = false;
		}
	}
	self.pose(dt);
}

fn void Figure.blend(&self, Joint j, float x, float y, float z, float w)
{
	float[3]* r = &self.rot[j.ordinal];
	(*r)[0] = maths::lerp_angle((*r)[0], x, w);
	(*r)[1] = maths::lerp_angle((*r)[1], y, w);
	(*r)[2] = maths::lerp_angle((*r)[2], z, w);
}

fn void Figure.pose(&self, float dt)
{
	float walk = self.amount;
	float run = math::clamp((self.amount - 0.55f) / 0.45f, 0.0f, 1.0f); // past a walk: running
	float s = math::sin(self.phase);
	float c = math::cos(self.phase);

	// Legs swing forward and back; the knee bends as the leg comes forward (more
	// running); in the air one knee is pulled up.
	float leg_amp = 0.55f * walk + 0.35f * run;
	float[2][2] legs;
	for (int i = 0; i < 2; i++)
	{
		float side = i == 0 ? -1.0f : 1.0f;
		float swing_leg = s * side * leg_amp;
		float knee = math::max(0.0f, -c * side) * (0.5f * walk + 0.7f * run);
		float air_leg = (side < 0 ? 0.55f : -0.25f) * self.air;
		float air_knee = (side < 0 ? 1.0f : 0.35f) * self.air;
		legs[i][0] = swing_leg + air_leg + 0.55f * self.sneak;
		legs[i][1] = knee + air_knee + 0.9f * self.sneak;
	}
	// Arms swing against the legs; elbows bend more running; in the air they
	// lift out a little.
	float arm_amp = 0.7f * walk + 0.4f * run;
	float[3][2] arms;
	for (int i = 0; i < 2; i++)
	{
		float side = i == 0 ? -1.0f : 1.0f;
		arms[i][0] = -s * side * arm_amp + 0.25f * self.sneak;
		arms[i][1] = side * (0.06f + 0.35f * self.air);
		arms[i][2] = 0.15f + 0.9f * run + 0.2f * self.sneak;
	}
	// The right arm strikes like an axe blow: winds up (elbow beside the head),
	// chops down and across, speeding up, then eases back.
	float twist_swing = 0;
	if (self.swing_t >= 0)
	{
		float t = self.swing_t / SWING_TIME;
		float[3] rest = arms[1];
		float[3] p;
		if (t < SWING_WIND)
		{
			float u = maths::ease(t / SWING_WIND);
			p[0] = lerp(rest[0], SWING_TOP[0], u);
			p[1] = lerp(rest[1], SWING_TOP[1], u);
			p[2] = lerp(rest[2], SWING_TOP[2], math::min(1.0f, u * 2.0f)); // the elbow bends first
			twist_swing = -0.2f * u;
		}
		else if (t < SWING_HIT)
		{
			float u = (t - SWING_WIND) / (SWING_HIT - SWING_WIND);
			float fall = u * u; // speeds up into the blow
			p[0] = lerp(SWING_TOP[0], SWING_LAND[0], fall);
			p[1] = lerp(SWING_TOP[1], SWING_LAND[1], fall);
			p[2] = lerp(SWING_TOP[2], SWING_LAND[2], fall * fall); // and straightens last
			twist_swing = lerp(-0.2f, 0.35f, fall);
		}
		else
		{
			float u = maths::ease((t - SWING_HIT) / (1.0f - SWING_HIT));
			p[0] = lerp(SWING_LAND[0], rest[0], u);
			p[1] = lerp(SWING_LAND[1], rest[1], u);
			p[2] = lerp(SWING_LAND[2], rest[2], u);
			twist_swing = lerp(0.35f, 0.0f, u);
		}
		arms[1] = p;
	}

	float lean = 0.12f * walk + 0.2f * run + 0.5f * self.sneak;
	float bob = -math::abs(c) * 0.04f * walk - 0.05f * run - 0.16f * self.sneak;
	float twist = s * 0.12f * walk + twist_swing; // shoulders turn with the stride

	float r = rate(18.0f, dt);
	self.blend(HIPS, 0, -twist * 0.4f, 0, r);
	self.hips_y = lerp(self.hips_y, HIP_Y + bob, r);
	self.blend(TORSO_J, -lean, twist, 0, r);
	// The head keeps looking where the figure looks, whatever the body does.
	self.blend(HEAD, math::clamp(self.pitch, -1.2f, 1.2f) + lean * 0.8f, -twist, 0, rate(25.0f, dt));
	Joint[2] leg = { LEG_L, LEG_R };
	Joint[2] knee = { KNEE_L, KNEE_R };
	Joint[2] arm = { ARM_L, ARM_R };
	Joint[2] elbow = { ELBOW_L, ELBOW_R };
	for (int i = 0; i < 2; i++)
	{
		self.blend(leg[i], legs[i][0], 0, 0, r);
		self.blend(knee[i], -legs[i][1], 0, 0, r);
		if (i == 1 && self.swing_t >= 0) // the strike sets the arm exactly
		{
			self.rot[Joint.ARM_R.ordinal] = { arms[1][0], 0, arms[1][1] };
			self.rot[Joint.ELBOW_R.ordinal] = { arms[1][2], 0, 0 };
		}
		else
		{
			self.blend(arm[i], arms[i][0], 0, arms[i][1], r);
			self.blend(elbow[i], arms[i][2], 0, 0, r);
		}
	}
}
