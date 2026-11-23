// The time of day (0..1, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset) and what
// follows from it: daylight, the sky's colours, where the sun is. The moon is
// always opposite the sun: at night it is the light that shines (and casts
// the block shadows), weaker and blue. A whole day lasts day_length seconds
// (40 minutes by default): DAY_SHARE of it the sun is up (20 minutes), the
// rest is night (20). New worlds start in the morning.
//
// Days are counted (from midnight): the moon goes through MOON_PHASES, full
// on day 0, new halfway; the night is as bright as the moon. At sunset an
// orange-red haze comes in, and the night falls quickly after it.
module gocube::game::day_cycle;
import std::math;

alias Rgb = float[<3>];

fn Rgb lerp(Rgb a, Rgb b, float t) => a + (b - a) * t;

const double DEFAULT_DAY_LENGTH = 40.0 * 60.0;
const double DAY_SHARE = 0.5;           // sunrise to sunset (time 0.25..0.75)
const double NIGHT_SHARE = 1.0 - DAY_SHARE;
const float NIGHT_LIGHT = 0.48f;    // sky light on a full moon's night: dim, but you can see the land (shaded in sRGB: about a tenth of the day)
const float NEW_MOON_LIGHT = 0.16f; // and with no moon: very dark
const float MOON_LIGHT = 0.6f;      // how strongly the full moon shines (the sun: 1)
const int MOON_PHASES = 8;
const double MORNING = 0.26;        // what sleeping wakes you at
const float PI = 3.14159265358979f;
// (Colours: linear, as the shaders get them.)
const Rgb NIGHT_TINT = { 0.6f, 0.7f, 1.0f }; // moonlight is a cool blue
const Rgb DAY_TOP = { 0.2f, 0.38f, 1.0f };       // a deep, plain blue
const Rgb DAY_HORIZON = { 0.6f, 0.74f, 1.0f };    // pale and hazy: the fog's colour
const Rgb DUSK_HORIZON = { 0.95f, 0.55f, 0.32f };
const Rgb DUSK_HAZE = { 0.78f, 0.3f, 0.14f };     // the sunset's fog: orange-red
const Rgb NIGHT_TOP = { 0.02f, 0.035f, 0.09f };
const Rgb NIGHT_HORIZON = { 0.07f, 0.1f, 0.2f };
const Rgb NIGHT_FOG = { 0.06f, 0.085f, 0.17f };  // a moonlit haze (near the horizon's: no dark band between)
const float DAY_FOG_BEGIN = 0.12f;   // of the view distance
const float DAY_FOG_END = 0.6f;
const float NIGHT_FOG_BEGIN = 0.08f;
const float NIGHT_FOG_END = 0.45f;

struct DayCycle
{
	double time;  // 0..1 (see above)
	long days;    // days since the world began (counted at midnight)
	double day_length;
	float[2] cloud_offset; // drifting clouds, in cloud cells
}

fn DayCycle new() => { .time = 0.3, .day_length = DEFAULT_DAY_LENGTH };

// The sky's half from sunrise to sunset passes in DAY_SHARE of the day's
// seconds, the night's half in the rest.
fn void DayCycle.update(&self, double dt)
{
	bool sun_up = self.time >= 0.25 && self.time < 0.75;
	double half = math::max(10.0, self.day_length) * (sun_up ? DAY_SHARE : NIGHT_SHARE);
	double t = self.time + dt * 0.5 / half;
	if (t >= 1.0) self.days++;
	self.time = t % 1.0;
	self.cloud_offset[0] += 0.35f * (float)dt;
	self.cloud_offset[1] += 0.12f * (float)dt;
}

fn float smoothstep(float a, float b, float x) @private
{
	float t = math::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Sets the time of day; earlier than it is now: the next day's (sleeping,
// "/time set").
fn void DayCycle.set_time(&self, double t)
{
	t = ((t % 1.0) + 1.0) % 1.0;
	if (t < self.time) self.days++;
	self.time = t;
}

// The days and the time in one number (days + time: saved, and sent to
// other players), and back.
fn double DayCycle.clock(&self) => (double)self.days + self.time;
fn void DayCycle.set_clock(&self, double c)
{
	double d = math::floor(c);
	self.days = (long)math::max(d, 0.0);
	self.time = math::clamp(c - d, 0.0, 0.999999);
}

// The moon's phase: 0 full .. MOON_PHASES / 2 new .. (one a day), and how
// bright it is (1 full, 0 new).
fn int DayCycle.moon_phase(&self) => (int)(((self.days % MOON_PHASES) + MOON_PHASES) % MOON_PHASES);
fn float DayCycle.moon_brightness(&self) => 0.5f + 0.5f * math::cos((float)self.moon_phase() * 2.0f * PI / (float)MOON_PHASES);

fn float DayCycle.angle(&self) => (float)(self.time - 0.25) * 2.0f * PI; // the sun above the eastern horizon
fn float DayCycle.sun_height(&self) => math::sin(self.angle());
fn float DayCycle.day_amount(&self) => smoothstep(-0.07f, 0.22f, self.sun_height()); // (soon dark once the sun is down)
fn float DayCycle.night_light(&self) => NEW_MOON_LIGHT + (NIGHT_LIGHT - NEW_MOON_LIGHT) * self.moon_brightness();
fn float DayCycle.daylight(&self) => self.night_light() + (1.0f - self.night_light()) * self.day_amount();
fn Rgb DayCycle.sky_tint(&self) => lerp(NIGHT_TINT, { 1, 1, 1 }, self.day_amount());
fn float DayCycle.sun_amount(&self) => self.day_amount() * math::clamp(self.sun_height() * 3.0f, 0.0f, 1.0f);
fn float DayCycle.dusk(&self) => math::clamp(1.0f - math::abs(self.sun_height()) / 0.3f, 0.0f, 1.0f);
// Towards the sun: it rises in the east (+x), overhead at noon, sets in the west.
fn float[3] DayCycle.sun_dir(&self) => { math::cos(self.angle()), math::sin(self.angle()), 0 };

// The moon: opposite the sun, rising as it sets.
fn float DayCycle.moon_amount(&self) => MOON_LIGHT * self.moon_brightness() * (1.0f - self.day_amount()) * math::clamp(-self.sun_height() * 3.0f, 0.0f, 1.0f);

// What lights the land: the sun by day, the moon by night. How much it
// shines (0 at dusk and dawn, either way), and where it is, in radians above
// the eastern horizon (0..PI while it's up).
fn float DayCycle.light_amount(&self) => self.sun_amount() + self.moon_amount();
fn float DayCycle.light_angle(&self)
{
	float a = self.angle();
	if (self.sun_height() >= 0.0f) return a;
	return a < 0.0f ? a + PI : a - PI;
}
fn float[3] DayCycle.light_dir(&self) => { math::cos(self.light_angle()), math::sin(self.light_angle()), 0 };

// The fog's colour: the horizon's, an orange-red haze at sunset (and
// sunrise), the moonlit haze at night (darker without a moon).
fn Rgb DayCycle.haze(&self)
{
	float day = self.day_amount();
	Rgb night = NIGHT_FOG * (0.35f + 0.65f * self.moon_brightness());
	return lerp(lerp(night, DAY_HORIZON, day), DUSK_HAZE, self.dusk() * 0.8f * math::sqrt(day));
}

fn Rgb DayCycle.sky_top(&self) => lerp(NIGHT_TOP, DAY_TOP, self.day_amount());
fn Rgb DayCycle.sky_horizon(&self) => lerp(lerp(NIGHT_HORIZON, DAY_HORIZON, self.day_amount()), DUSK_HORIZON, self.dusk() * 0.65f);
// Depth fog, near and moody: from DAY_FOG_BEGIN of the view distance,
// thick at DAY_FOG_END, by day; nearer still at night (the moonlit haze).
// The sunset's haze is nearer still.
fn float DayCycle.fog_begin(&self, float view) => view * (NIGHT_FOG_BEGIN + (DAY_FOG_BEGIN - NIGHT_FOG_BEGIN) * self.day_amount()) * (1.0f - 0.4f * self.dusk());
fn float DayCycle.fog_end(&self, float view) => view * (NIGHT_FOG_END + (DAY_FOG_END - NIGHT_FOG_END) * self.day_amount()) * (1.0f - 0.3f * self.dusk());

// "7:05 AM" or "07:05" (temp).
fn String clock_text(double time, bool use_24h)
{
	int minutes = (int)((time % 1.0) * 24.0 * 60.0);
	int hour = minutes / 60;
	int minute = minutes % 60;
	if (use_24h) return string::tformat("%02d:%02d", hour, minute);
	return string::tformat("%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
}
