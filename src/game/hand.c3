// The first-person arm and held item: a sleeve, a hand coming out of its cuff
// and the item in it, off the bottom right of the view. Swings when you hit,
// place, use or mine (a swing asked for during another plays right after it,
// whole), bobs while walking.
//
// build() gives their triangles in camera space (x right, y up, -z forward),
// coloured by the light where the eyes are: the renderer draws them over the
// world, the item over the arm.
module gocube::game::hand;
import gocube::game::creature_models, gocube::content, gocube::content::item_models, gocube::world, gocube::mat, gocube::maths;
import gocube::util::vec;
import std::math;

// The player gives a little light of their own, day and night: this level
// where the eyes are, one less per block away (counted along x, y and z, as
// placed light spreads), whatever is in hand. (Through walls too: cheap, and
// only what you see is lit.) The terrain shader does the same (Frame.player_light).
const int PLAYER_LIGHT = 6; // (small: the dark stays a threat)

// The player's light at `at` from `eye` (0..15).
fn int player_light_at(float[3] eye, float[3] at)
{
	int d = 0;
	for (int k = 0; k < 3; k++) d += math::abs((int)math::floor(at[k]) - (int)math::floor(eye[k]));
	return math::max(PLAYER_LIGHT - d, 0);
}

// Each light level is 80% as bright as the one above; level 0 is dark.
fn float light_curve(float level01) => level01 <= 0.0f ? 0.0f : math::pow(0.8f, 15.0f - level01 * 15.0f);

const float[3] BLOCK_LIGHT_COLOR = { 1.0f, 0.85f, 0.62f };

// Light for what isn't terrain (dropped and held items, the hand), worked out
// like the terrain shader does: sky light times `sky_light` (the day's),
// placed light and the player's own (from `eye`) in the block light's colour,
// the brighter of the two, never below `least`. Linear rgb.
fn float[3] light_for(World* world, float[3] sky_light, float[3] at, float[3] eye, float least)
{
	int sky;
	int block;
	world.light_at((int)math::floor(at[0]), (int)math::floor(at[1]), (int)math::floor(at[2]), &sky, &block);
	float s = light_curve((float)sky / 15.0f);
	float b = light_curve((float)math::max(block, player_light_at(eye, at)) / 15.0f);
	float[3] out;
	for (int k = 0; k < 3; k++) out[k] = math::max(math::max(sky_light[k] * s, BLOCK_LIGHT_COLOR[k] * b), least);
	return out;
}

const float SWING_TIME = 0.3f;

// The pose at a moment of a swing: rotation about the shoulder, push (camera
// space) and the held item's tilt, as Euler angles.
struct Pose
{
	float[3] rotation, push, tilt;
}

const float[3] REST @private = { 0.36f, -0.34f, -0.52f }; // where the hand rests (camera space)
// The arm turns about the shoulder, off the bottom right of the screen, so
// the sleeve always stays behind the hand.
const float[3] SHOULDER @private = { 0.52f, -0.62f, 0.02f };
const float HAND_WIND @private = 0.25f;
const float HAND_HIT @private = 0.5f; // (the strike: as Figure's, but quicker)
const Pose SWING_REST @private = {};
const Pose SWING_UP @private = { { 0.1f, -0.05f, 0.0f }, { 0.0f, 0.015f, 0.03f }, { 0.35f, 0.0f, 0.0f } };
const Pose SWING_DOWN @private = { { 0.12f, 0.32f, 0.1f }, { -0.02f, 0.06f, -0.06f }, { -0.8f, 0.0f, 0.0f } };

fn Pose mix(Pose a, Pose b, float u) @private
{
	Pose p;
	for (int k = 0; k < 3; k++)
	{
		p.rotation[k] = a.rotation[k] + (b.rotation[k] - a.rotation[k]) * u;
		p.push[k] = a.push[k] + (b.push[k] - a.push[k]) * u;
		p.tilt[k] = a.tilt[k] + (b.tilt[k] - a.tilt[k]) * u;
	}
	return p;
}

// The pose at `t` (0..1 of a swing).
fn Pose swing_pose(float t)
{
	if (t < HAND_WIND) return mix(SWING_REST, SWING_UP, maths::ease(t / HAND_WIND));
	if (t < HAND_HIT)
	{
		float u = (t - HAND_WIND) / (HAND_HIT - HAND_WIND);
		return mix(SWING_UP, SWING_DOWN, u * u); // speeds up into the blow
	}
	return mix(SWING_DOWN, SWING_REST, maths::ease((t - HAND_HIT) / (1.0f - HAND_HIT)));
}

struct Hand
{
	float swing_t;     // time into the swing (-1: none)
	bool swing_next;
	float bob;
	float bob_amount;  // 0 standing .. 1 walking (smoothed, so the bob never snaps)
	Pose pose;
	float[3] bob_offset;
}

fn Hand new() => { .swing_t = -1 };

fn void Hand.swing(&self)
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

fn bool Hand.swinging(&self) => self.swing_t >= 0;

// Each frame: `mining` keeps it swinging; `walking`: on the floor and moving.
fn void Hand.update(&self, float dt, bool mining, bool walking)
{
	// Keep swinging while mining, one whole swing after another.
	if (mining && self.swing_t < 0) self.swing_t = 0;
	self.pose = SWING_REST;
	if (self.swing_t >= 0)
	{
		self.swing_t += dt;
		if (self.swing_t >= SWING_TIME)
		{
			self.swing_t = self.swing_next ? 0.0f : -1.0f;
			self.swing_next = false;
		}
		if (self.swing_t >= 0) self.pose = swing_pose(self.swing_t / SWING_TIME);
	}
	// Walking bob, faded in and out (never snapping when you stop).
	self.bob_amount += ((walking ? 1.0f : 0.0f) - self.bob_amount) * (1.0f - math::exp(-8.0f * dt));
	self.bob += dt * 9.0f * self.bob_amount;
	self.bob_offset = { math::cos(self.bob * 0.5f) * 0.012f * self.bob_amount, -math::abs(math::sin(self.bob * 0.5f)) * 0.02f * self.bob_amount, 0 };
}

// A box of `size` centred on its origin, through `m` (the hand shader shades
// each face by its normal).
fn void add_box(Vec{WorldVertex}* out, float[3] size, Matrix m, float[3] color) @private
{
	foreach (n : creature_models::NORMALS)
	{
		// Two axes across the face.
		float[3] a = math::abs(n[1]) > 0.5f ? { 1, 0, 0 } : { 0, 1, 0 };
		float[3] b = mat::cross(n, a);
		float[3][4] c;
		float[4] su = { -1, 1, 1, -1 };
		float[4] sv = { -1, -1, 1, 1 };
		for (int i = 0; i < 4; i++)
		{
			float[3] p;
			for (int k = 0; k < 3; k++) p[k] = (n[k] + a[k] * su[i] + b[k] * sv[i]) * size[k] / 2;
			c[i] = m.point(p);
		}
		foreach (i : creature_models::QUAD)
		{
			WorldVertex v = { .pos = c[i], .normal = n, .color = { color[0], color[1], color[2], 1 } };
			out.push(v);
		}
	}
}

// The arm and the item (textured: the colour is the light; `light`: linear).
// The arm is the player's right arm in its skin (`skin`: the renderer's slot)
// into `skinned`; without one (null), a sleeve and hand into `arm`
// (untextured: the colour is their tint times the light).
fn void Hand.build(&self, ItemModels* models, String item, float[3] light, Vec{WorldVertex}* arm, Vec{WorldVertex}* held,
	CreatureVertices* skinned = null, int skin = -1)
{
	Matrix hand = mat::node({ SHOULDER[0] + self.bob_offset[0] + self.pose.push[0], SHOULDER[1] + self.bob_offset[1] + self.pose.push[1],
		SHOULDER[2] + self.bob_offset[2] + self.pose.push[2] }, self.pose.rotation);
	Matrix grip = mat::node({ REST[0] - SHOULDER[0], REST[1] - SHOULDER[1], REST[2] - SHOULDER[2] }) * hand;
	Matrix arm_m = mat::node({ 0.0f, -0.02f, 0.12f }, { 0.15f, -0.12f, 0.0f }) * grip;
	Matrix fist_m = mat::node({ 0, 0, -0.22f }) * arm_m;
	// (Colours as they are: items.hlsl writes the result out as the world's shaders do.)
	float[3] sleeve = { 0.34f * light[0], 0.46f * light[1], 0.72f * light[2] };
	float[3] tone = { 0.86f * light[0], 0.68f * light[1], 0.54f * light[2] };
	if (skinned)
	{
		// The skin's right arm, raised forward: its hand where the fist is (12
		// pixels of 0.04 m, the hand's end 0.28 m ahead of the sleeve's
		// middle), its front facing up.
		float p = 0.04f;
		creature_models::build_skinned_arm(mat::node({ 0.0f, 0.0f, -0.28f + 6 * p }, { 1.5707963f, 0.0f, 0.0f }) * arm_m, p, light, skin, skinned);
	}
	else
	{
		add_box(arm, { 0.135f, 0.135f, 0.42f }, arm_m, sleeve); // wider than the hand: a cuff around the wrist
		add_box(arm, { 0.1f, 0.1f, 0.12f }, fist_m, tone);
	}
	if (!item.len) return;
	Matrix tilt = mat::node({ 0, 0, 0 }, self.pose.tilt) * grip;
	Matrix item_m;
	if (models.is_block(item))
	{
		item_m = mat::node({ -0.04f, 0.04f, -0.2f }, { 0.1f, 0.75f, 0.0f }, { 1.1f, 1.1f, 1.1f }) * tilt;
	}
	else
	{
		item_m = mat::node({ -0.02f, 0.1f, -0.2f }, { 0.0f, -1.35f, 0.35f }, { 0.85f, 0.85f, 0.85f }) * tilt; // held edge-on, tip up
	}
	foreach (mv : models.model(item))
	{
		WorldVertex v = { .pos = item_m.point(mv.pos), .normal = mv.normal, .uv = mv.uv, .color = { light[0], light[1], light[2], 1 } };
		held.push(v);
	}
}
