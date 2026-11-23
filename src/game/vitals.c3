// The host: a joined survival player's health, worked out from where its
// client says it is, by the same rules as the local player's (Game): falls,
// fluids that hurt (lava), falling out of the world, drowning, healing after
// a while without damage. The client only shows it.
module gocube::game::vitals;
import gocube::game::blocks, gocube::game::player, gocube::game::rules, gocube::world, gocube::data;
import std::math;

const int MAX_HEALTH = 20;
// (Game's own health, in single player, heals and drowns the same.)
const float REGEN_DELAY = 4.0f;    // seconds without damage before healing
const float REGEN_INTERVAL = 2.5f; // seconds per health point healed
const int DROWN_DAMAGE = 2;        // per second without breath
const float MAX_BREATH = 10.0f;

struct Vitals
{
	int health;
	float breath;
	bool dead;
	bool changed; // to send to the client
	bool has_peak;
	float fall_peak;
	float since_damage, regen_timer, drown_timer, burn_timer, void_timer;
}

fn Vitals new() => { .health = MAX_HEALTH, .breath = MAX_BREATH, .changed = true, .burn_timer = 0.5f, .void_timer = 0.5f };

fn Data* def_at(World* world, BlockRules* rules, float x, float y, float z) @private
	=> rules.def_of(world.block_at((int)math::floor(x), (int)math::floor(y), (int)math::floor(z)));

fn void Vitals.hurt(&self, int amount)
{
	if (self.dead || amount <= 0) return;
	self.health = math::max(0, self.health - amount);
	self.since_damage = 0;
	self.regen_timer = 0;
	self.dead = self.health == 0;
	self.changed = true;
}

fn void Vitals.respawn(&self)
{
	self.health = MAX_HEALTH;
	self.breath = MAX_BREATH;
	self.dead = false;
	self.has_peak = false;
	self.since_damage = 0;
	self.drown_timer = 0;
	self.changed = true;
}

// A new position the host accepted (feet). `held_up`: flying or in bed (no
// fall). A fall ends on the ground, in a fluid or on a ladder.
fn void Vitals.moved(&self, World* world, BlockRules* rules, float[3] feet, bool held_up)
{
	if (self.dead) return;
	bool in_fluid = def_at(world, rules, feet[0], feet[1] + 0.9f, feet[2]).has("fluid");
	bool climbing = def_at(world, rules, feet[0], feet[1], feet[2]).flag("climbable") ||
		def_at(world, rules, feet[0], feet[1] + 1.0f, feet[2]).flag("climbable");
	float[3] body = { player::RADIUS * 2, player::HEIGHT, player::RADIUS * 2 };
	if (held_up || in_fluid || climbing || !self.has_peak)
	{
		self.fall_peak = feet[1];
		self.has_peak = true;
	}
	else if (!world.is_box_free({ feet[0], feet[1] - 0.05f, feet[2] }, body)) // on the ground
	{
		float fall = self.fall_peak - feet[1];
		if (fall > 0.6f) self.hurt(rules::fall_damage(fall));
		self.fall_peak = feet[1];
	}
	else
	{
		self.fall_peak = math::max(self.fall_peak, feet[1]);
	}
}

// Time passing: fluids that hurt burn, below the world hurts, breath runs out
// with the head in a fluid, health comes back. `in_bed`: lying down (nothing
// burns or drowns).
fn void Vitals.tick(&self, World* world, BlockRules* rules, float[3] feet, bool in_bed, float dt)
{
	if (self.dead) return;
	if (feet[1] < (float)world::BOTTOM_Y)
	{
		self.void_timer += dt;
		if (self.void_timer >= 0.5f)
		{
			self.void_timer = 0;
			self.hurt(rules::VOID_DAMAGE);
		}
	}
	else
	{
		self.void_timer = 0.5f; // (at once, the moment it falls out)
	}
	Data* body = in_bed ? null : def_at(world, rules, feet[0], feet[1] + 0.4f, feet[2]);
	if (body && body.get("damage") && body.get("damage").is_number())
	{
		self.burn_timer += dt;
		if (self.burn_timer >= 0.5f)
		{
			self.burn_timer = 0;
			self.hurt((int)body.num("damage"));
		}
	}
	else
	{
		self.burn_timer = 0.5f; // the first touch hurts at once
	}
	if (!in_bed && def_at(world, rules, feet[0], feet[1] + player::EYE_HEIGHT, feet[2]).has("fluid"))
	{
		self.breath = math::max(0.0f, self.breath - dt);
		if (self.breath <= 0)
		{
			self.drown_timer += dt;
			if (self.drown_timer >= 1.0f)
			{
				self.drown_timer = 0;
				self.hurt(DROWN_DAMAGE);
			}
		}
	}
	else
	{
		self.breath = math::min(MAX_BREATH, self.breath + dt * 3.0f);
		self.drown_timer = 0;
	}
	self.since_damage += dt;
	if (self.health < MAX_HEALTH && self.since_damage >= REGEN_DELAY)
	{
		self.regen_timer += dt;
		if (self.regen_timer >= REGEN_INTERVAL)
		{
			self.regen_timer = 0;
			self.health++;
			self.changed = true;
		}
	}
}
