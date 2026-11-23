// Small math the game shares: angles (pi, a full turn, turning the short
// way), easing, rounding toward minus infinity, 3-vectors. (src/maths.h)
module gocube::maths;
import std::math;

const float PI = 3.14159265f;
const float TAU = 2 * PI;

alias Vec3 = float[<3>];

// From `from` toward `to` by `w` (0..1), the short way round.
fn float lerp_angle(float from, float to, float w)
{
	float d = (to - from) % TAU; // (as C's fmod: truncated)
	if (d > PI)
	{
		d -= TAU;
	}
	else if (d < -PI)
	{
		d += TAU;
	}
	return from + d * w;
}

// 0..1 eased in and out (smoothstep); clamped.
fn float ease(float x)
{
	x = math::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

// The block a coordinate is in.
fn int floor_i(float v) @inline => (int)math::floor(v);

// v / d rounded toward minus infinity (d > 0): the chunk a block is in.
fn int floor_div(int v, int d) @inline => v >= 0 ? v / d : -((-v + d - 1) / d);

fn float length3(Vec3 v) => math::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
fn Vec3 cross(Vec3 a, Vec3 b) => { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
fn float dot(Vec3 a, Vec3 b) => a.x * b.x + a.y * b.y + a.z * b.z;
