// The player: walking, sprinting, sneaking, jumping, swimming, climbing ladders
// and flying, in fixed physics steps (STEP); breaking and placing blocks (Game
// adds survival's mining time, inventory and health).
module gocube::game::player;
import gocube::game::blocks, gocube::world, gocube::content, gocube::voxel, gocube::data, gocube::maths, gocube::util::vec, gocube::util::intern;
import std::math;

// A block the player broke or placed.
struct BlockChange
{
	BlockPos pos;
	String id; // the block broken, or placed (interned)
	bool placed;
}

alias BlockChanges = Vec{BlockChange};

struct PlayerInput
{
	float forward, right; // -1..1 each
	bool jump, sneak, sprint;
}

const float STEP = 1.0f / 60.0f; // physics step, seconds
const float EYE_HEIGHT = 1.62f;
const float HEIGHT = 1.8f;
const float RADIUS = 0.3f;
const float REACH = 6.0f;

const float WALK_SPEED @private = 4.3f;
const float SPRINT_SPEED @private = 5.6f;
const float SNEAK_SPEED @private = 1.3f;
const float FLY_SPEED @private = 11.0f;
const float FLY_SPRINT_SPEED @private = 22.0f;
const float JUMP_VELOCITY @private = 8.4f; // with GRAVITY: ~1.26 blocks high, enough to climb one block
const float GRAVITY @private = 28.0f;
const float MAX_FALL_SPEED @private = 60.0f;
const float ACCEL_GROUND @private = 14.0f;
const float ACCEL_AIR @private = 3.0f;
const float SWIM_SPEED_FACTOR @private = 0.6f;
const float CLIMB_SPEED @private = 3.0f;
const float SWIM_UP_SPEED @private = 3.5f;
const float WATER_CLIMB_SPEED @private = 8.0f;
const float SINK_SPEED @private = 2.0f;

alias OccupiedFn = fn bool(void* context, BlockPos cell);

struct Player
{
	float[3] position; // feet
	float[3] velocity;
	float yaw, pitch;  // radians: yaw 0 looks towards -z, positive turns left; pitch up
	bool flying;
	bool on_floor, in_water, on_ladder;
	bool sneaking;     // in the last step (shorter footsteps)
	bool against_wall;
}

fn float[3] Player.eye(&self) => { self.position[0], self.position[1] + EYE_HEIGHT, self.position[2] };

// Unit view direction.
fn float[3] Player.look(&self)
	=> { -math::sin(self.yaw) * math::cos(self.pitch), math::sin(self.pitch), -math::cos(self.yaw) * math::cos(self.pitch) };

fn BlockId block_at_f(World* world, float x, float y, float z) @private
	=> world.block_at(maths::floor_i(x), maths::floor_i(y), maths::floor_i(z));

// One physics step (STEP seconds).
fn void Player.step(&self, PlayerInput in, World* world, BlockRules* rules)
{
	float dt = STEP;
	self.sneaking = in.sneak;
	// The wish direction from the input, turned by yaw only (looking down doesn't slow you).
	float ix = in.right;
	float iz = -in.forward;
	float il = math::sqrt(ix * ix + iz * iz);
	if (il > 1.0f)
	{
		ix /= il;
		iz /= il;
	}
	float c = math::cos(self.yaw);
	float s = math::sin(self.yaw);
	float wish_x = ix * c + iz * s;
	float wish_z = -ix * s + iz * c;

	float[3] p = self.position;
	self.in_water = rules.is_fluid(block_at_f(world, p[0], p[1] + 0.9f, p[2]));
	self.on_ladder = rules.climbable(block_at_f(world, p[0], p[1], p[2])) || rules.climbable(block_at_f(world, p[0], p[1] + 1.0f, p[2]));

	if (self.flying)
	{
		float speed = in.sprint ? FLY_SPRINT_SPEED : FLY_SPEED;
		float vertical = (float)(int)in.jump - (float)(int)in.sneak;
		float[3] target = { wish_x * speed, vertical * speed, wish_z * speed };
		float k = 1.0f - math::exp(-10.0f * dt);
		for (int a = 0; a < 3; a++) self.velocity[a] += (target[a] - self.velocity[a]) * k;
	}
	else
	{
		float speed = in.sneak ? SNEAK_SPEED : (in.sprint ? SPRINT_SPEED : WALK_SPEED);
		float accel = self.on_floor ? ACCEL_GROUND : ACCEL_AIR;
		if (self.in_water)
		{
			speed *= SWIM_SPEED_FACTOR;
			accel = ACCEL_GROUND;
		}
		float k = 1.0f - math::exp(-accel * dt);
		self.velocity[0] += (wish_x * speed - self.velocity[0]) * k;
		self.velocity[2] += (wish_z * speed - self.velocity[2]) * k;
		if (self.on_ladder)
		{
			// Climbing: up with Space or walking into it, down slowly otherwise;
			// sneaking holds on.
			if (in.jump || in.forward > 0.1f)
			{
				self.velocity[1] = CLIMB_SPEED;
			}
			else if (in.sneak)
			{
				self.velocity[1] = 0.0f;
			}
			else
			{
				self.velocity[1] = math::max(self.velocity[1] - GRAVITY * dt, -CLIMB_SPEED);
			}
		}
		else if (self.in_water)
		{
			// Buoyant: sink slowly, Space swims up.
			self.velocity[1] = math::max(self.velocity[1] - GRAVITY * 0.3f * dt, -SINK_SPEED);
			if (in.jump)
			{
				self.velocity[1] = math::min(self.velocity[1] + 20.0f * dt, SWIM_UP_SPEED);
				// At the surface (head out of the water): climb out over the edge.
				bool head_out = !rules.is_fluid(block_at_f(world, p[0], p[1] + 1.8f, p[2]));
				if (self.against_wall && head_out) self.velocity[1] = WATER_CLIMB_SPEED;
			}
		}
		else
		{
			self.velocity[1] = math::max(self.velocity[1] - GRAVITY * dt, -MAX_FALL_SPEED);
			if (self.on_floor && in.jump) self.velocity[1] = JUMP_VELOCITY;
		}
	}

	float[3] size = { RADIUS * 2, HEIGHT, RADIUS * 2 };
	float[3] motion = { self.velocity[0] * dt, self.velocity[1] * dt, self.velocity[2] * dt };
	MoveResult r = world.move_box(self.position, size, motion, 0.0f);
	float[3] moved = { r.position[0] - self.position[0], r.position[1] - self.position[1], r.position[2] - self.position[2] };
	self.position = r.position;
	self.on_floor = r.on_floor;
	self.against_wall = r.hit_wall && math::hypot(wish_x, wish_z) > 0.1f;
	// Lose the velocity that ran into something.
	if (r.on_floor || r.hit_ceiling) self.velocity[1] = 0.0f;
	if (r.hit_wall)
	{
		if (math::abs(moved[0]) < math::abs(motion[0]) - 1e-4f) self.velocity[0] = 0.0f;
		if (math::abs(moved[2]) < math::abs(motion[2]) - 1e-4f) self.velocity[2] = 0.0f;
	}
	if (self.flying && self.on_floor && in.sneak) self.flying = false; // flying down onto the ground ends flight
}

// The block aimed at, within REACH.
fn RayHit Player.target(&self, World* world) => world.raycast(self.eye(), self.look(), REACH);

fn BlockId air_of(BlockRules* rules) @private => (BlockId)math::max(0, rules.index_of("core:air"));

// Breaks the block aimed at (a door or bed goes as a whole). False if none.
// What was broken is added to `changes` if given.
fn bool Player.break_target(&self, World* world, BlockRules* rules, BlockChanges* changes = null)
{
	RayHit hit = self.target(world);
	if (!hit.hit) return false;
	String broken = intern::intern(rules.id_of(hit.block));
	world.set_block(hit.position, air_of(rules));
	if (changes) changes.push({ hit.position, broken, false });
	BlockPos other;
	if (rules.other_part(broken, hit.position, &other)) // a door or bed goes as a whole
	{
		String there = intern::intern(rules.id_of(world.block_at(other.x, other.y, other.z)));
		if (rules.base_of(there) == rules.base_of(broken))
		{
			world.set_block(other, air_of(rules));
			if (changes) changes.push({ other, there, false });
		}
	}
	return true;
}

// Whether a player standing with feet at `feet` overlaps the block `cell`.
fn bool overlaps_block(float[3] feet, BlockPos cell)
{
	float[3] mn = { feet[0] - RADIUS, feet[1], feet[2] - RADIUS };
	float[3] mx = { feet[0] + RADIUS, feet[1] + HEIGHT, feet[2] + RADIUS };
	float[3] cmn = { (float)cell.x, (float)cell.y, (float)cell.z };
	for (int k = 0; k < 3; k++)
	{
		if (mx[k] <= cmn[k] || mn[k] >= cmn[k] + 1.0f) return false;
	}
	return true;
}

// Right click: opens or closes a door aimed at (unless sneaking), else places
// `block` against the face aimed at. False if nothing happened. What was
// placed is added to `changes` if given. `occupied` (with its context):
// whether someone else stands in a cell (no solid block goes there).
fn bool Player.use_or_place(&self, World* world, BlockRules* rules, String dyed_block, bool sneak, BlockChanges* changes = null,
	OccupiedFn occupied = null, void* occupied_context = null) => @pool()
{
	String block = content::undyed(dyed_block); // (placed with its dye)
	Dye dye = content::dye_of(dyed_block);
	RayHit hit = self.target(world);
	if (!hit.hit) return false;
	// Doors open and close (sneak to place against them instead).
	if (!sneak)
	{
		Placement toggle;
		defer toggle.free();
		rules.toggle_door(rules.id_of(hit.block), hit.position, &toggle);
		if (!toggle.is_empty())
		{
			foreach (t : toggle) // (a dyed door stays dyed)
			{
				world.set_block(t.pos, (BlockId)math::max(0, rules.index_of(t.id)), world.dye_at(t.pos.x, t.pos.y, t.pos.z));
			}
			return true;
		}
	}
	if (!rules.is_placeable(block)) return false;
	// Placing into tall grass or a flower replaces it.
	if (rules.replaceable(hit.block))
	{
		hit.previous = hit.position;
		hit.normal = { 0, 1, 0 };
	}
	float[3] e = self.eye();
	float[3] d = self.look();
	float hit_y = e[1] + d[1] * hit.distance;
	Placement places;
	defer places.free();
	rules.placement(block, &hit, d, hit_y, &places);
	if (places.is_empty()) return false;
	foreach (pl : places)
	{
		int there = world.block_id_at(pl.pos.x, pl.pos.y, pl.pos.z);
		if (there < 0 || (there != 0 && !rules.is_fluid(there) && !rules.replaceable(there)))
		{
			return false; // occupied (e.g. no room for a door's upper half), or not loaded
		}
		// A torch or ladder needs something to hang on; a door's upper half
		// stands on its lower half, placed along with it.
		bool on_lower = false;
		foreach (p2 : places)
		{
			on_lower = on_lower || (p2.pos == pl.pos + (BlockPos){ 0, -1, 0 } && rules.base_of(p2.id) == rules.base_of(pl.id));
		}
		if (!on_lower && !rules.supported(world, pl.id, pl.pos)) return false;
		Data* def = rules.def(pl.id);
		Data* solid_field = def.get("solid");
		bool solid = !solid_field || !solid_field.is_boolean() || solid_field.b;
		if (solid && (overlaps_block(self.position, pl.pos) || (occupied && occupied(occupied_context, pl.pos))))
		{
			return false; // it would be inside you, or someone else
		}
	}
	foreach (pl : places)
	{
		int index = rules.index_of(pl.id);
		if (index > 0)
		{
			world.set_block(pl.pos, (BlockId)index, dye);
			if (changes) changes.push({ pl.pos, intern::intern(pl.id), true });
		}
	}
	return true;
}
