// The time of day (0..1, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset) and what
// follows from it: daylight, the sky's colours, where the sun is. A day lasts
// day_length seconds (a real day by default: new worlds start at the
// computer's clock time). (src/game/day_cycle.h)
module gocube::game::day_cycle;
import gocube::win, std::math;

alias Rgb = float[<3>];

fn Rgb lerp(Rgb a, Rgb b, float t) => a + (b - a) * t;

const double DEFAULT_DAY_LENGTH = 86400.0;
const float NIGHT_LIGHT = 0.02f;    // sky light at night: dark, bring a torch
const double MORNING = 0.26;        // what sleeping wakes you at
const float PI = 3.14159265358979f;
// (Colours: linear, as the shaders get them.)
const Rgb NIGHT_TINT = { 0.6f, 0.7f, 1.0f }; // moonlight is a cool blue
const Rgb DAY_TOP = { 0.32f, 0.52f, 0.86f };
const Rgb DAY_HORIZON = { 0.68f, 0.79f, 0.92f };
const Rgb DUSK_HORIZON = { 0.95f, 0.55f, 0.32f };
const Rgb NIGHT_TOP = { 0.015f, 0.02f, 0.06f };
const Rgb NIGHT_HORIZON = { 0.05f, 0.07f, 0.14f };
const Rgb NIGHT_FOG = { 0.006f, 0.008f, 0.02f };
const float NIGHT_FOG_BEGIN = 4.0f;
const float NIGHT_FOG_END = 40.0f;

struct DayCycle
{
	double time;
	double day_length;
	float[2] cloud_offset; // drifting clouds, in cloud cells
}

fn DayCycle new() => { .time = 0.3, .day_length = DEFAULT_DAY_LENGTH };

fn void DayCycle.update(&self, double dt)
{
	self.time = (self.time + dt / math::max(10.0, self.day_length)) % 1.0;
	self.cloud_offset[0] += 0.35f * (float)dt;
	self.cloud_offset[1] += 0.12f * (float)dt;
}

// The computer's clock as a time of day.
fn double system_time()
{
	SystemTime t;
	win::get_local_time(&t);
	return (double)(t.hour * 3600 + t.minute * 60 + t.second) / 86400.0;
}

fn float smoothstep(float a, float b, float x) @private
{
	float t = math::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

fn float DayCycle.angle(&self) => (float)(self.time - 0.25) * 2.0f * PI; // the sun above the eastern horizon
fn float DayCycle.sun_height(&self) => math::sin(self.angle());
fn float DayCycle.day_amount(&self) => smoothstep(-0.18f, 0.22f, self.sun_height());
fn float DayCycle.daylight(&self) => NIGHT_LIGHT + (1.0f - NIGHT_LIGHT) * self.day_amount();
fn Rgb DayCycle.sky_tint(&self) => lerp(NIGHT_TINT, { 1, 1, 1 }, self.day_amount());
fn float DayCycle.sun_amount(&self) => self.day_amount() * math::clamp(self.sun_height() * 3.0f, 0.0f, 1.0f);
fn float DayCycle.dusk(&self) => math::clamp(1.0f - math::abs(self.sun_height()) / 0.3f, 0.0f, 1.0f);
// Towards the sun: it rises in the east (+x), overhead at noon, sets in the west.
fn float[3] DayCycle.sun_dir(&self) => { math::cos(self.angle()), math::sin(self.angle()), 0 };

fn Rgb DayCycle.haze(&self)
{
	float day = self.day_amount();
	return lerp(lerp(NIGHT_FOG, DAY_HORIZON, day), DUSK_HORIZON, self.dusk() * 0.4f * day);
}

fn Rgb DayCycle.sky_top(&self) => lerp(NIGHT_TOP, DAY_TOP, self.day_amount());
fn Rgb DayCycle.sky_horizon(&self) => lerp(lerp(NIGHT_HORIZON, DAY_HORIZON, self.day_amount()), DUSK_HORIZON, self.dusk() * 0.65f);
// Depth fog: from 0.45 of the view distance, thick at 0.95, by day; much nearer at night.
fn float DayCycle.fog_begin(&self, float view) => NIGHT_FOG_BEGIN + (view * 0.45f - NIGHT_FOG_BEGIN) * self.day_amount();
fn float DayCycle.fog_end(&self, float view) => NIGHT_FOG_END + (view * 0.95f - NIGHT_FOG_END) * self.day_amount();

// "7:05 AM" or "07:05" (temp).
fn String clock_text(double time, bool use_24h)
{
	int minutes = (int)((time % 1.0) * 24.0 * 60.0);
	int hour = minutes / 60;
	int minute = minutes % 60;
	if (use_24h) return string::tformat("%02d:%02d", hour, minute);
	return string::tformat("%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
}
