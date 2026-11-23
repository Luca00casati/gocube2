// The lonely, uncanny mood, chosen each frame from where the player is:
//  - wind, louder out in the open and up high, fading in forests and caves
//    (from the sky light where the eyes are, and the height); now and then
//    it stops altogether for a while (a silence);
//  - now and then underground (little sky light), a strange sound somewhere
//    in the dark: a rumble, scraping, a drip, steps, a moan, falling pebbles
//    (content "core:cave"); and underground, now and then, your footsteps
//    come back a little late from behind you;
//  - rarely, far off and out of sight, a block breaking or being placed,
//    with nobody there;
//  - rarely, a slow piece of music (content "core:music");
//  - a low hum near the empty structures (the generator's);
//  - now and then the fog closes in for a few minutes, then lifts;
//  - some nights, a small warm light far off on the land, gone when you
//    come near (and by morning).
// --mood-test makes all of it come quickly, to record and check.
module gocube::app;
import gocube::audio, gocube::world, gocube::voxel, gocube::util::rand, gocube::util::vec, std::math;

const float MUSIC_FIRST_MIN @private = 90.0f;
const float MUSIC_FIRST_MAX @private = 240.0f;
const float MUSIC_GAP_MIN @private = 600.0f;
const float MUSIC_GAP_MAX @private = 1200.0f;
const float CAVE_FIRST_MIN @private = 45.0f;
const float CAVE_FIRST_MAX @private = 120.0f;
const float CAVE_GAP_MIN @private = 90.0f;
const float CAVE_GAP_MAX @private = 240.0f;
const float PHANTOM_GAP_MIN @private = 480.0f;   // a sound with nobody there
const float PHANTOM_GAP_MAX @private = 1200.0f;
const float SILENCE_GAP_MIN @private = 600.0f;   // the wind stops
const float SILENCE_GAP_MAX @private = 1200.0f;
const float SILENCE_MIN @private = 45.0f;
const float SILENCE_MAX @private = 90.0f;
const float FOG_GAP_MIN @private = 900.0f;       // the fog closes in
const float FOG_GAP_MAX @private = 1800.0f;
const float FOG_MIN @private = 120.0f;
const float FOG_MAX @private = 240.0f;
const float FOG_THICK @private = 0.4f;           // the fog's distances, then
const float LIGHT_CHANCE @private = 0.5f;        // of a night having the far light
const float LIGHT_NEAREST @private = 40.0f;      // nearer than this: gone
const float ECHO_CHANCE @private = 0.3f;
const float HUM_REACH @private = 22.0f;          // blocks from a structure the hum is heard
const int UNDERGROUND_SKY @private = 4;          // sky light at the eyes at or below it: underground
const float CAVE_SOUND_DB @private = -4.0f;

struct Echo @private
{
	float wait;
	String set; // (the content's)
	float[3] at;
}

struct Mood
{
	Rng random;
	bool started;
	bool test;         // (--mood-test: everything comes quickly)
	float music_wait;  // seconds until the next piece (counted while none plays)
	float cave_wait;   // seconds of being underground until the next cave sound
	float phantom_wait;
	float silence_wait, silence_left;
	float fog_wait, fog_left;
	float wind;        // 0..1, smoothed
	float open;        // the sky light at the eyes, 0..1, smoothed (underground: about 0)
	float fog;         // the fog's distances times this (1: as the day says)
	float hum, hum_check;
	Vec{Echo} echoes;
	// The far light: where it is, if it's there tonight.
	bool light_on, light_tried;
	float[3] light;
}

fn void Mood.free(&self) => self.echoes.free();

fn float Mood.wait(&self, float lo, float hi, float test_lo, float test_hi) @private
	=> self.test ? self.random.range(test_lo, test_hi) : self.random.range(lo, hi);

fn void Mood.start(&self) @private
{
	self.random = rand::seed_random();
	self.music_wait = self.test ? 0.5f : self.random.range(MUSIC_FIRST_MIN, MUSIC_FIRST_MAX);
	self.cave_wait = self.wait(CAVE_FIRST_MIN, CAVE_FIRST_MAX, 2, 3);
	self.phantom_wait = self.wait(PHANTOM_GAP_MIN, PHANTOM_GAP_MAX, 5, 8);
	self.silence_wait = self.wait(SILENCE_GAP_MIN, SILENCE_GAP_MAX, 10, 12);
	self.fog_wait = self.wait(FOG_GAP_MIN, FOG_GAP_MAX, 4, 6);
	self.open = 1.0f;
	self.fog = 1.0f;
	self.started = true;
}

// A point `distance` away from `eye` in a random direction (level, give or take).
fn float[3] Mood.around(&self, float[3] eye, float distance, float dy) @private
{
	float angle = self.random.range(0.0f, 6.2831853f);
	return { eye[0] + math::cos(angle) * distance, eye[1] + self.random.range(-dy, dy), eye[2] + math::sin(angle) * distance };
}

// `day`: 0 night .. 1 day.
fn void Mood.update(&self, Audio* audio, World* world, Generator* generator, float[3] eye, float day, float dt)
{
	if (!self.started) self.start();
	int sky;
	int block;
	world.light_at((int)math::floor(eye[0]), (int)math::floor(eye[1]), (int)math::floor(eye[2]), &sky, &block);
	float open = (float)sky / 15.0f;
	self.open += (open - self.open) * (1.0f - math::exp(-1.5f * dt));
	bool underground = sky <= UNDERGROUND_SKY;

	// Wind: the open sky, more of it up high; none during a silence.
	if (self.silence_left > 0.0f)
	{
		self.silence_left -= dt;
	}
	else
	{
		self.silence_wait -= dt;
		if (self.silence_wait <= 0.0f)
		{
			self.silence_left = self.wait(SILENCE_MIN, SILENCE_MAX, 6, 8);
			self.silence_wait = self.wait(SILENCE_GAP_MIN, SILENCE_GAP_MAX, 30, 40);
		}
	}
	float height = math::clamp((eye[1] - 10.0f) / 50.0f, 0.0f, 1.0f);
	float target = self.silence_left > 0.0f ? 0.0f : open * open * (0.3f + 0.7f * height);
	self.wind += (target - self.wind) * (1.0f - math::exp(-0.8f * dt));
	audio.set_wind(self.wind);

	// Something in the dark, somewhere around, not too near.
	if (underground)
	{
		self.cave_wait -= dt;
		if (self.cave_wait <= 0.0f)
		{
			audio.play("core:cave", "sounds", self.around(eye, self.random.range(8.0f, 16.0f), 4.0f), CAVE_SOUND_DB);
			self.cave_wait = self.wait(CAVE_GAP_MIN, CAVE_GAP_MAX, 4, 7);
		}
	}
	// Late footsteps.
	for (sz i = 0; i < self.echoes.len();)
	{
		Echo* e = self.echoes.get_ref(i);
		e.wait -= dt;
		if (e.wait > 0.0f)
		{
			i++;
			continue;
		}
		audio.play(e.set, "step", e.at, -16.0f);
		self.echoes.remove_at(i);
	}
	// Somebody breaking or placing a block, far off, nowhere to be seen.
	self.phantom_wait -= dt;
	if (self.phantom_wait <= 0.0f)
	{
		String[2][4] sounds = { { "core:stone", "break" }, { "core:wood", "break" }, { "core:wood", "place" }, { "core:gravel", "break" } };
		String[2] s = sounds[(sz)(self.random.unit() * 4.0) % 4];
		audio.play(s[0], s[1], self.around(eye, self.random.range(22.0f, 29.0f), 3.0f), -4.0f);
		self.phantom_wait = self.wait(PHANTOM_GAP_MIN, PHANTOM_GAP_MAX, 7, 10);
	}
	// Music, rarely.
	if (!audio.music_playing())
	{
		self.music_wait -= dt;
		if (self.music_wait <= 0.0f)
		{
			audio.play_music("core:music", "pieces");
			self.music_wait = self.wait(MUSIC_GAP_MIN, MUSIC_GAP_MAX, 60, 90);
		}
	}
	// The hum near an empty structure (looked for once a second).
	self.hum_check -= dt;
	if (self.hum_check <= 0.0f)
	{
		self.hum_check = 1.0f;
		StructureList found;
		defer found.free();
		int reach = (int)HUM_REACH;
		generator.structures_near((int)eye[0] - reach, (int)eye[2] - reach, (int)eye[0] + reach + 1, (int)eye[2] + reach + 1, &found);
		float nearest = HUM_REACH;
		foreach (s : found)
		{
			float dx = (float)s.x + 0.5f - eye[0];
			float dy = (float)s.ground + 2.0f - eye[1];
			float dz = (float)s.z + 0.5f - eye[2];
			nearest = math::min(nearest, math::sqrt(dx * dx + dy * dy * 0.5f + dz * dz));
		}
		float near = 1.0f - nearest / HUM_REACH;
		self.hum = near * near;
	}
	audio.set_hum(self.hum);
	// The fog closes in, then lifts.
	if (self.fog_left > 0.0f)
	{
		self.fog_left -= dt;
	}
	else
	{
		self.fog_wait -= dt;
		if (self.fog_wait <= 0.0f)
		{
			self.fog_left = self.wait(FOG_MIN, FOG_MAX, 8, 10);
			self.fog_wait = self.wait(FOG_GAP_MIN, FOG_GAP_MAX, 30, 40);
		}
	}
	float fog_target = self.fog_left > 0.0f ? FOG_THICK : 1.0f;
	self.fog += (fog_target - self.fog) * (1.0f - math::exp(-0.25f * dt));
	// The far light: some nights, on the land, out where the fog nearly hides it.
	if (day > 0.5f)
	{
		self.light_on = false;
		self.light_tried = false;
	}
	else if (day < 0.15f && !self.light_tried)
	{
		if (self.test || self.random.unit() < LIGHT_CHANCE)
		{
			// A few spots a frame, until one on the ground can be seen from here.
			for (int attempt = 0; attempt < 6 && !self.light_on; attempt++)
			{
				float[3] p = self.around(eye, self.random.range(55.0f, 80.0f), 0.0f);
				int x = (int)math::floor(p[0]);
				int z = (int)math::floor(p[2]);
				for (int y = 110; y > -40; y--)
				{
					int id = world.block_id_at(x, y, z);
					if (id < 0) break; // (not loaded)
					if (id == voxel::AIR) continue;
					float[3] at = { (float)x + 0.5f, (float)y + 1.4f, (float)z + 0.5f };
					if (in_sight(world, eye, at))
					{
						self.light = at;
						self.light_on = true;
						self.light_tried = true;
					}
					break;
				}
			}
		}
		else
		{
			self.light_tried = true;
		}
	}
	if (self.light_on)
	{
		float dx = self.light[0] - eye[0];
		float dz = self.light[2] - eye[2];
		if (dx * dx + dz * dz < LIGHT_NEAREST * LIGHT_NEAREST) self.light_on = false; // (gone, and not back tonight)
	}
}

// Whether nothing solid is between `eye` and `at` (looked at block by block).
fn bool in_sight(World* world, float[3] eye, float[3] at) @private
{
	float[3] d = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
	float length = math::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	for (float t = 1.0f; t < length - 1.0f; t += 0.5f)
	{
		float u = t / length;
		int id = world.block_id_at((int)math::floor(eye[0] + d[0] * u), (int)math::floor(eye[1] + d[1] * u), (int)math::floor(eye[2] + d[2] * u));
		if (id > 0) return false; // (solid, or anything not air; unloaded counts as clear)
	}
	return true;
}

// A footstep just played (its sound set): underground, now and then it
// comes back late, from behind.
fn void Mood.heard_step(&self, String set, float[3] eye, float[3] forward)
{
	if (!self.started || self.open > 0.25f || self.random.unit() >= (self.test ? 0.9 : (double)ECHO_CHANCE) || self.echoes.len() > 4) return;
	float[3] at = { eye[0] - forward[0] * 7.0f, eye[1] - 1.0f, eye[2] - forward[2] * 7.0f };
	self.echoes.push({ .wait = self.random.range(0.35f, 0.7f), .set = set, .at = at });
}

// The far light's little glowing block (to draw with no fog), if it's there.
fn void Mood.light_mesh(&self, Vec{WorldVertex}* out)
{
	if (!self.light_on) return;
	const float H = 0.45f;
	float[4] warm = { 1.0f, 0.8f, 0.5f, 1.0f };
	float[3][6] normals = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	foreach (n : normals)
	{
		float[3] a = math::abs(n[1]) > 0.5f ? { 1, 0, 0 } : { 0, 1, 0 };
		float[3] b = { n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0] };
		float[3][4] c;
		float[4] su = { -1, 1, 1, -1 };
		float[4] sv = { -1, -1, 1, 1 };
		for (int i = 0; i < 4; i++)
		{
			for (int k = 0; k < 3; k++) c[i][k] = self.light[k] + (n[k] + a[k] * su[i] + b[k] * sv[i]) * H;
		}
		foreach (i : (int[6]){ 0, 1, 2, 0, 2, 3 })
		{
			out.push({ .pos = c[i], .normal = n, .color = warm });
		}
	}
}
