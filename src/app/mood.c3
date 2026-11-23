// The lonely mood's sounds, chosen each frame from where the player is:
//  - wind, louder out in the open and up high, fading in forests and caves
//    (from the sky light where the eyes are, and the height);
//  - now and then underground (little sky light), a strange sound somewhere
//    in the dark: a rumble, scraping, a drip, steps, a moan, falling pebbles
//    (content "core:cave"), every CAVE_GAP_MIN..MAX seconds;
//  - rarely, a slow piece of music (content "core:music"): the first a few
//    minutes in, then MUSIC_GAP_MIN..MAX seconds of silence after each.
module gocube::app;
import gocube::audio, gocube::world, gocube::util::rand, std::math;

const float MUSIC_FIRST_MIN @private = 90.0f;
const float MUSIC_FIRST_MAX @private = 240.0f;
const float MUSIC_GAP_MIN @private = 600.0f;
const float MUSIC_GAP_MAX @private = 1200.0f;
const float CAVE_FIRST_MIN @private = 45.0f;
const float CAVE_FIRST_MAX @private = 120.0f;
const float CAVE_GAP_MIN @private = 90.0f;
const float CAVE_GAP_MAX @private = 240.0f;
const int UNDERGROUND_SKY @private = 4;   // sky light at the eyes at or below it: underground
const float CAVE_SOUND_DB @private = -4.0f;

struct Mood
{
	Rng random;
	bool started;
	float music_wait; // seconds until the next piece (counted while none plays)
	float cave_wait;  // seconds of being underground until the next cave sound
	float wind;       // 0..1, smoothed
}

fn void Mood.update(&self, Audio* audio, World* world, float[3] eye, float dt)
{
	if (!self.started)
	{
		self.random = rand::seed_random();
		self.music_wait = self.random.range(MUSIC_FIRST_MIN, MUSIC_FIRST_MAX);
		self.cave_wait = self.random.range(CAVE_FIRST_MIN, CAVE_FIRST_MAX);
		self.started = true;
	}
	int sky;
	int block;
	world.light_at((int)math::floor(eye[0]), (int)math::floor(eye[1]), (int)math::floor(eye[2]), &sky, &block);
	// Wind: the open sky, more of it up high.
	float open = (float)sky / 15.0f;
	float height = math::clamp((eye[1] - 10.0f) / 50.0f, 0.0f, 1.0f);
	float target = open * open * (0.3f + 0.7f * height);
	self.wind += (target - self.wind) * (1.0f - math::exp(-0.8f * dt));
	audio.set_wind(self.wind);
	// Something in the dark, somewhere around, not too near.
	if (sky <= UNDERGROUND_SKY)
	{
		self.cave_wait -= dt;
		if (self.cave_wait <= 0.0f)
		{
			float angle = self.random.range(0.0f, 6.2831853f);
			float distance = self.random.range(8.0f, 16.0f);
			float[3] at = { eye[0] + math::cos(angle) * distance, eye[1] + self.random.range(-4.0f, 4.0f), eye[2] + math::sin(angle) * distance };
			audio.play("core:cave", "sounds", at, CAVE_SOUND_DB);
			self.cave_wait = self.random.range(CAVE_GAP_MIN, CAVE_GAP_MAX);
		}
	}
	// Music, rarely.
	if (!audio.music_playing())
	{
		self.music_wait -= dt;
		if (self.music_wait <= 0.0f)
		{
			audio.play_music("core:music", "pieces");
			self.music_wait = self.random.range(MUSIC_GAP_MIN, MUSIC_GAP_MAX);
		}
	}
}
