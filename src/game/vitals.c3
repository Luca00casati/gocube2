// The host: a joined survival player's health, by the same rules as the
// local player's (Game): nothing in the world hurts (no falls, burns or
// drowning); only a command (/kill) does, and health comes back after a
// while without damage. The client only shows it.
module gocube::game::vitals;
import gocube::game::blocks, gocube::game::player, gocube::game::rules, gocube::world, gocube::data;
import std::math;

const int MAX_HEALTH = 20;
// (Game's own health, in single player, heals the same.)
const float REGEN_DELAY = 4.0f;    // seconds without damage before healing
const float REGEN_INTERVAL = 2.5f; // seconds per health point healed
const float MAX_BREATH = 10.0f;

struct Vitals
{
	int health;
	float breath;
	bool dead;
	bool changed; // to send to the client
	float since_damage, regen_timer;
}

fn Vitals new() => { .health = MAX_HEALTH, .breath = MAX_BREATH, .changed = true };

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
	self.since_damage = 0;
	self.changed = true;
}

// A new position the host accepted (feet). (Nothing in the world hurts:
// falls don't.) `held_up`: flying or in bed.
fn void Vitals.moved(&self, World* world, BlockRules* rules, float[3] feet, bool held_up)
{
}

// Time passing: health comes back after a while without damage (only a
// command hurts). `in_bed`: lying down.
fn void Vitals.tick(&self, World* world, BlockRules* rules, float[3] feet, bool in_bed, float dt)
{
	if (self.dead) return;
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
