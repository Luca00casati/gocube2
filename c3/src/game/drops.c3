// Items lying in the world: they pop out of broken blocks, fall, merge with
// the same item nearby, drift to the player and go into their inventory,
// and disappear after LIFETIME. (src/game/drops.*)
module gocube::game::drops;
import gocube::game::inventory, gocube::world, gocube::voxel, gocube::content, gocube::content::item_models, gocube::util::vec, gocube::util::rand, gocube::util::intern;
import std::math;

const float BOX = 0.25f;          // collision box size
const float GRAVITY = 20.0f;
const float PICKUP_DELAY = 0.5f;  // not grabbed the instant it pops out
const float MAGNET_RADIUS = 2.6f;
const float PICKUP_RADIUS = 1.2f;
const float LIFETIME = 300.0f;
const float MERGE_DISTANCE = 0.8f;
// Thrown (Q): not picked up for THROW_DELAY (it would come straight back),
// and never merged into another drop.
const float THROW_DELAY = 1.5f;

struct Drop
{
	String item; // interned
	int count;
	float[3] pos; // bottom centre
	float[3] vel;
	float age;
	float turn;  // the model's starting spin (radians)
	float delay; // no one picks it up before this age
}

// Someone who picks items up: feet, and the inventory they go into.
struct Taker
{
	int peer; // 0: the local player
	float[3] feet;
	bool alive;
	Inventory* inventory;
}

struct Picked
{
	int peer;
	Stack stack;
}

alias LightFn = fn float[3](void* context, float[3] at);

struct ItemDrops
{
	Vec{Drop} drops;
	Rng random;
}

fn void ItemDrops.init(&self) => *self = { .random = rand::seed_random() };
fn void ItemDrops.free(&self) => self.drops.free();

fn void ItemDrops.spawn(&self, String item, int count, float[3] at)
{
	if (count <= 0 || item.len == 0) return;
	foreach (&d : self.drops)
	{
		float dx = d.pos[0] - at[0];
		float dy = d.pos[1] - at[1];
		float dz = d.pos[2] - at[2];
		if (d.item == item && dx * dx + dy * dy + dz * dz < MERGE_DISTANCE * MERGE_DISTANCE)
		{
			d.count += count;
			return;
		}
	}
	Drop d = { .item = intern::intern(item), .count = count, .pos = at, .delay = PICKUP_DELAY };
	d.vel[0] = self.random.range(-1.5f, 1.5f);
	d.vel[1] = 3.0f;
	d.vel[2] = self.random.range(-1.5f, 1.5f);
	d.turn = self.random.range(0.0f, 6.2831853f);
	self.drops.push(d);
}

// Thrown (Q): from `at`, at `vel`.
fn void ItemDrops.throw_item(&self, String item, int count, float[3] at, float[3] vel)
{
	if (count <= 0 || item.len == 0) return;
	Drop d = { .item = intern::intern(item), .count = count, .pos = at, .vel = vel, .delay = THROW_DELAY };
	d.turn = self.random.range(0.0f, 6.2831853f);
	self.drops.push(d);
}

// One physics step: falling, drifting to the nearest taker, picking up into
// their inventory. What was picked up, by whom, is added to `picked`.
fn void ItemDrops.step(&self, float dt, World* world, Taker[] takers, Vec{Picked}* picked)
{
	float[3] size = { BOX, BOX, BOX };
	for (sz i = 0; i < self.drops.len();)
	{
		Drop* d = self.drops.get_ref(i);
		d.age += dt;
		if (d.age > LIFETIME)
		{
			self.drops.remove_at(i);
			continue;
		}
		float[3] to;
		float dist = 1e9f;
		Taker* taker = null; // the nearest one with room for it
		if (d.age > d.delay)
		{
			foreach (&t : takers)
			{
				if (!t.alive || !t.inventory || !t.inventory.can_fit(d.item)) continue;
				float[3] v = { t.feet[0] - d.pos[0], t.feet[1] + 0.9f - d.pos[1], t.feet[2] - d.pos[2] };
				float l = math::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				if (l < dist)
				{
					dist = l;
					to = v;
					taker = t;
				}
			}
		}
		if (taker && dist < PICKUP_RADIUS)
		{
			int left = taker.inventory.add(d.item, d.count);
			if (left < d.count && picked) picked.push({ taker.peer, { d.item, d.count - left } });
			d.count = left;
			if (d.count == 0)
			{
				self.drops.remove_at(i);
				continue;
			}
		}
		if (taker && dist < MAGNET_RADIUS && dist > 1e-4f)
		{
			for (int k = 0; k < 3; k++) // drift towards the player
			{
				d.vel[k] = to[k] / dist * 6.0f;
				d.pos[k] += d.vel[k] * dt;
			}
		}
		else
		{
			d.vel[1] = math::max(d.vel[1] - GRAVITY * dt, -30.0f);
			d.vel[0] *= 0.92f;
			d.vel[2] *= 0.92f;
			MoveResult r = world.move_box(d.pos, size, { d.vel[0] * dt, d.vel[1] * dt, d.vel[2] * dt });
			d.pos = r.position;
			if (r.on_floor)
			{
				d.vel[0] *= 0.5f;
				d.vel[1] = 0;
				d.vel[2] *= 0.5f;
			}
		}
		i++;
	}
}

// Spins and bobs (a client's: no physics runs).
fn void ItemDrops.age(&self, float dt)
{
	foreach (&d : self.drops) d.age += dt;
}

// A client: the host's drops, as they are now (shown, not simulated).
fn void ItemDrops.set_all(&self, Drop[] drops)
{
	self.drops.clear();
	self.drops.push_all(drops);
}

// Their models (world coordinates; ItemModels), bobbing and slowly spinning,
// each coloured by `light` at it (linear rgb: the light where it is, for
// shaders/items.hlsl).
fn void ItemDrops.build_mesh(&self, ItemModels* models, LightFn light, void* light_context, Vec{WorldVertex}* out)
{
	foreach (&d : self.drops)
	{
		float[3] lit = light(light_context, { d.pos[0], d.pos[1] + 0.3f, d.pos[2] });
		// The model: BOX up from the drop's feet, bobbing, and spinning slowly
		// about y from a random start.
		float spin = d.turn + d.age * 1.2f;
		float c = math::cos(spin);
		float s = math::sin(spin);
		float[3] origin = { d.pos[0], d.pos[1] + BOX + 0.06f + 0.06f * math::sin(d.age * 2.5f), d.pos[2] };
		Dye dye = content::dye_of(d.item);
		foreach (&mv : models.model(d.item))
		{
			WorldVertex w;
			w.pos = { origin[0] + mv.pos[0] * c + mv.pos[2] * s, origin[1] + mv.pos[1], origin[2] - mv.pos[0] * s + mv.pos[2] * c };
			w.normal = mv.normal; // (shaded by the model's own normal)
			w.uv = mv.uv;
			w.color = { lit[0], lit[1], lit[2], 1 };
			w.set_dye(dye);
			out.push(w);
		}
	}
}
