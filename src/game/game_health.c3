// Game: health, damage, death and respawn, spawning on dry ground.
module gocube::game;
import gocube::game::blocks, gocube::game::player, gocube::game::inventory, gocube::game::drops, gocube::game::block_entities;
import gocube::game::rules, gocube::game::vitals;
import gocube::content, gocube::data, gocube::world, gocube::voxel, gocube::maths;
import gocube::util::vec, gocube::util::map, gocube::util::rand, gocube::util::intern;
import std::math, std::sort;

const int SPAWN_SEARCH @private = 64;
const int TOP_Y @private = 127; // the highest block y
const float VOID_DEPTH @private = 16.0f; // blocks below the world's bottom before you're brought back

fn void Game.damage(&self, int amount, String source = "")
{
	if (self.creative || self.dead || amount <= 0) return;
	self.health = math::max(0, self.health - amount);
	self.since_damage = 0;
	self.regen_timer = 0;
	self.since_hurt = 0;
	self.play_sound("core:effects", "hurt", self.player.eye());
	if (self.health == 0)
	{
		self.dead = true;
		self.player.velocity = { 0, 0, 0 };
	}
}

fn Data* Game.block_def_at(&self, float x, float y, float z)
	=> self.rules.def_of(self.world.block_at(maths::floor_i(x), maths::floor_i(y), maths::floor_i(z)));

// Landing faster than this (m/s, downward) makes a sound: a jump onto a
// block (about 4.3) and anything bigger, not stepping down a slab.
const float LANDING_SPEED @private = 2.5f;

// After each physics step: landing, footsteps and splashes; healing; out of
// the void. (Nothing in the world hurts: no falls, burns or drowning.)
fn void Game.after_move(&self, float dt)
{
	Player* p = self.player;
	float y = p.position[1];
	// The block under the feet (footsteps sound like it).
	String below = self.rules.id_of(self.world.block_at(maths::floor_i(p.position[0]), maths::floor_i(y - 0.1f), maths::floor_i(p.position[2])));
	// Landing: the sound of the block landed on, louder the harder.
	if (p.on_floor && !self.was_on_floor && !p.in_water && !p.flying && !p.on_ladder && -self.last_vy > LANDING_SPEED)
	{
		float hard = math::clamp((-self.last_vy - LANDING_SPEED) / 12.0f, 0.0f, 1.0f);
		self.play_block_sound(below, "step", p.position, -11.0f + 6.0f * hard);
	}
	self.was_on_floor = p.on_floor;

	// Into water fast: a splash. Walking: a footstep every STRIDE metres
	// (sooner sneaking), in the sound of the block underfoot.
	float vy = math::min(p.velocity[1], self.last_vy);
	if (p.in_water && !self.was_in_water && vy < -4.0f) self.play_sound("core:effects", "splash", p.position);
	self.was_in_water = p.in_water;
	self.last_vy = p.velocity[1];
	if (self.feet_known && p.on_floor && !p.in_water)
	{
		const float STRIDE = 1.7f;
		self.step_distance += math::hypot(p.position[0] - self.last_feet[0], p.position[2] - self.last_feet[2]);
		if (self.step_distance >= STRIDE * (p.sneaking ? 0.6f : 1.0f))
		{
			self.step_distance = 0;
			self.play_block_sound(below, "step", p.position, -13.0f); // (quiet: the silence matters)
		}
	}
	self.last_feet = p.position;
	self.feet_known = true;

	// Fallen out of the world: back where you'd come back to life.
	if (!self.dead && y < (float)world::BOTTOM_Y - VOID_DEPTH)
	{
		self.respawn();
		return;
	}
	if (self.creative || self.dead || self.remote) return; // (a client: the host keeps our health)
	// Healing after a while without damage.
	self.since_damage += dt;
	if (self.health < MAX_HEALTH && self.since_damage >= vitals::REGEN_DELAY)
	{
		self.regen_timer += dt;
		if (self.regen_timer >= vitals::REGEN_INTERVAL)
		{
			self.regen_timer = 0;
			self.health++;
		}
	}
}

fn void Game.unstuck(&self)
{
	float[3] size = { player::RADIUS * 2, player::HEIGHT, player::RADIUS * 2 };
	for (int dy = 0; dy < 256; dy++)
	{
		float[3] p = { self.player.position[0], self.player.position[1] + (float)dy, self.player.position[2] };
		if (self.world.is_box_free(p, size))
		{
			self.player.position[1] = p[1];
			return;
		}
	}
}

// Back to life: at the bed if it's still there, else at the world spawn.
fn void Game.respawn(&self)
{
	float[3] at = { 0.5f, 40.0f, 0.5f };
	if (self.has_world_spawn) at = self.world_spawn;
	if (self.has_bed)
	{
		if (self.rules.def_of(self.world.block_at(self.bed.x, self.bed.y, self.bed.z)).flag("bed"))
		{
			at = { self.bed.x + 0.5f, self.bed.y + 0.5625f, self.bed.z + 0.5f };
		}
		else
		{
			self.has_bed = false;
			self.show_notice("Your bed was missing, so you're back at the world spawn");
		}
	}
	self.health = MAX_HEALTH;
	self.breath = MAX_BREATH;
	self.dead = false;
	self.player.velocity = { 0, 0, 0 };
	self.player.position = at;
	if (self.world.is_loaded(maths::floor_i(at[0]), maths::floor_i(at[1]), maths::floor_i(at[2]))) self.unstuck();
	if (self.remote) self.remote.respawned(); // (the host gives us our health back)
}

// The nearest column (searching outward, every second one, up to
// SPAWN_SEARCH blocks) whose top is grass
// and not under water; else the nearest dry one. False while the ground
// isn't loaded yet.
fn bool Game.try_place_on_ground(&self)
{
	int ox = maths::floor_i(self.player.position[0]);
	int oz = maths::floor_i(self.player.position[2]);
	if (!self.world.is_loaded(ox, 0, oz)) return false;
	int grass = self.content.block_index("core:grass");
	bool has_fallback = false;
	float[3] fallback;
	for (int radius = 0; radius <= SPAWN_SEARCH; radius += 2)
	{
		for (int dz = -radius; dz <= radius; dz += 2)
		{
			for (int dx = -radius; dx <= radius; dx += 2)
			{
				if (math::max(math::abs(dx), math::abs(dz)) != radius) continue; // only the ring at this radius
				int x = ox + dx;
				int z = oz + dz;
				if (!self.world.is_loaded(x, TOP_Y, z) || !self.world.is_loaded(x, -96, z))
				{
					if (radius == 0) return false; // wait for the column under the player
					continue;
				}
				RayHit hit = self.world.raycast({ x + 0.5f, (float)TOP_Y, z + 0.5f }, { 0, -1, 0 }, 400);
				if (!hit.hit || self.rules.is_fluid(self.world.block_at(hit.previous.x, hit.previous.y, hit.previous.z))) continue;
				float[3] feet = { x + 0.5f, (float)(hit.position.y + 1), z + 0.5f };
				if ((int)hit.block == grass)
				{
					self.player.position = feet;
					return true;
				}
				if (!has_fallback)
				{
					fallback = feet;
					has_fallback = true;
				}
			}
		}
	}
	if (has_fallback) self.player.position = fallback;
	return true;
}
