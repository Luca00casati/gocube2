// Creatures' bodies: where they are, how they move (gravity, walking up steps,
// turning towards where they head), how they look while they do (the walk
// cycle, a squash or a red flash when hurt) and being hit. What they do is
// game::core's (it spawns them, sets where they head and how fast, and
// handles hits).
//
// A kind is content ("creatures"): "model" (how it's drawn: "block_eater" or
// "figure": a person), "box" [width, height, depth] (collision, picking),
// "gravity", "step_height" (walks up that much), "turn_rate" (how fast it
// turns towards its heading), "jump" and "jump_when_blocked" (leaps over what
// stops it), "light_height" (where its light is taken).
module gocube::game::creatures;
import gocube::game::figure, gocube::content, gocube::data, gocube::world, gocube::maths, gocube::util::vec, gocube::util::intern;
import std::math;

struct Creature
{
	int id;
	String kind;       // its definition's id ("core:block_eater"; interned)
	String model;      // (interned)
	String variant;    // what it's made of: "" its own look, or a block id (interned)
	float[3] pos;      // feet (bottom centre)
	float[3] prev;     // before the last physics step (drawing interpolates)
	float[3] vel;
	float yaw;         // facing (radians; 0 = -z), turning towards heading
	float prev_yaw;
	float heading;     // where its behaviour wants it to go
	float speed;       // m/s along heading
	bool on_floor;
	bool hit_wall;     // walked into something since its behaviour last looked
	bool blocked;      // this step: wanted to move and barely did (legs stop)
	bool walking;
	// Looks: time left of a hurt squash, a red flash (0..1), a strike playing.
	float hurt_time, flash, attacking;
	int strikes;       // goes up with every strike (the figure plays one)
	float walk_phase;
	// A figure's (model "figure"): its pose, how fast it seems to go.
	Figure figure;
	float anim_speed;
	int seen_strikes;
	// From its kind.
	float[3] box;
	float gravity, step_height, turn_rate, jump, light_height;
	bool jump_when_blocked;
	// A client's copy of the host's creature: it glides to where the host
	// last said it was (no physics here).
	bool remote;
	float[3] target;
	float target_yaw;
}

// A creature as the host sends it to clients.
struct CreatureState
{
	int id;
	String kind, variant; // (interned)
	float[3] pos;
	float yaw;
	bool walking, on_floor, blocked;
	float flash, hurt_time;
	int strikes;
}

const int MAX = 64;

struct Creatures
{
	Vec{Creature} list;
	int next_id;
}

fn void Creatures.free(&self) => self.list.free();

fn float float_field(Data* def, String key, float fallback) @private
{
	Data* v = def.get(key);
	return v && v.is_number() ? (float)v.as_double() : fallback;
}

// A creature of `kind` with its feet at `at`; its id, or 0 (unknown kind, too many).
fn int Creatures.spawn(&self, Content* content, String kind, float[3] at, String variant = "")
{
	Data* def = content.creatures.get(kind);
	if (!def || self.list.len() >= MAX) return 0;
	if (self.next_id == 0) self.next_id = 1;
	Creature c = { .id = self.next_id++, .kind = intern::intern(kind), .model = intern::intern(def.str("model")),
		.variant = intern::intern(variant), .pos = at, .prev = at, .figure = figure::new(), .box = { 0.6f, 1.8f, 0.6f } };
	Data* box = def.get("box");
	if (box && box.is_array() && box.len() == 3)
	{
		for (int k = 0; k < 3; k++)
		{
			if (box.at(k).is_number()) c.box[k] = (float)box.at(k).as_double();
		}
	}
	c.gravity = float_field(def, "gravity", 24);
	c.step_height = float_field(def, "step_height", 1);
	c.turn_rate = float_field(def, "turn_rate", 8);
	c.jump = float_field(def, "jump", 0);
	c.light_height = float_field(def, "light_height", 0.5f);
	c.jump_when_blocked = def.flag("jump_when_blocked");
	self.list.push(c);
	return c.id;
}

fn bool Creatures.remove(&self, int id)
{
	foreach (i, &c : self.list)
	{
		if (c.id == id)
		{
			self.list.remove_at(i);
			return true;
		}
	}
	return false;
}

fn Creature* Creatures.find(&self, int id)
{
	foreach (&c : self.list)
	{
		if (c.id == id) return c;
	}
	return null;
}

fn Creature[] Creatures.all(&self) => self.list.array_view();

// The host: every creature's state.
fn void Creatures.states(&self, Vec{CreatureState}* out)
{
	foreach (&c : self.list)
	{
		out.push({ .id = c.id, .kind = c.kind, .variant = c.variant, .pos = c.pos, .yaw = c.yaw, .walking = c.walking,
			.on_floor = c.on_floor, .blocked = c.blocked, .flash = c.flash, .hurt_time = c.hurt_time, .strikes = c.strikes });
	}
}

// A client: the host's (creatures come, go and glide to where it says).
fn void Creatures.apply_states(&self, Content* content, CreatureState[] states)
{
	for (sz i = 0; i < self.list.len();)
	{
		bool found = false;
		foreach (s : states) found = found || s.id == self.list[i].id;
		if (found)
		{
			i++;
		}
		else
		{
			self.list.remove_at(i);
		}
	}
	foreach (s : states)
	{
		Creature* c = self.find(s.id);
		if (!c || c.kind != s.kind)
		{
			if (c) self.remove(s.id);
			int made = self.spawn(content, s.kind, s.pos, s.variant);
			c = made ? self.find(made) : null;
			if (!c) continue; // a kind we don't have
			c.id = s.id; // (the host's id: hits name it)
			c.remote = true;
			c.yaw = c.prev_yaw = s.yaw;
		}
		c.target = s.pos;
		c.target_yaw = s.yaw;
		c.variant = intern::intern(s.variant);
		c.walking = s.walking;
		c.on_floor = s.on_floor;
		c.blocked = s.blocked;
		c.flash = math::max(c.flash, s.flash);
		c.hurt_time = math::max(c.hurt_time, s.hurt_time);
		c.strikes = s.strikes;
	}
}

// One physics step for every creature (its body only: what it decides is game::core's).
fn void Creatures.step(&self, float dt, World* world)
{
	foreach (&c : self.list)
	{
		c.prev = c.pos;
		c.prev_yaw = c.yaw;
		if (c.remote) // glide to where the host says it is
		{
			float t = 1.0f - math::exp(-12.0f * dt);
			for (int k = 0; k < 3; k++)
			{
				c.pos[k] += (c.target[k] - c.pos[k]) * t;
				c.vel[k] = dt > 0 ? (c.pos[k] - c.prev[k]) / dt : 0.0f;
			}
			c.yaw = maths::lerp_angle(c.yaw, c.target_yaw, t);
			continue;
		}
		float dx = -math::sin(c.heading);
		float dz = -math::cos(c.heading);
		c.vel[0] = dx * c.speed;
		c.vel[2] = dz * c.speed;
		c.vel[1] = math::max(c.vel[1] - c.gravity * dt, -40.0f);
		MoveResult r = world.move_box(c.pos, c.box, { c.vel[0] * dt, c.vel[1] * dt, c.vel[2] * dt }, c.step_height);
		c.hit_wall = c.hit_wall || r.hit_wall;
		if (c.jump_when_blocked && r.hit_wall && r.on_floor && c.speed > 0)
		{
			c.vel[1] = c.jump; // over whatever is in the way
		}
		else if (r.on_floor)
		{
			c.vel[1] = 0;
		}
		c.on_floor = r.on_floor;
		c.blocked = c.speed > 0 && math::hypot(r.position[0] - c.pos[0], r.position[2] - c.pos[2]) < c.speed * dt * 0.3f;
		c.pos = r.position;
		c.walking = c.speed > 0;
		c.yaw = maths::lerp_angle(c.yaw, c.heading, 1.0f - math::exp(-c.turn_rate * dt));
	}
}

// Each frame: the looks (walk cycle, squash, flash, strike).
fn void Creatures.animate(&self, float dt)
{
	foreach (&c : self.list)
	{
		c.walk_phase += dt * (c.walking ? 9.0f : 0.0f);
		c.hurt_time = math::max(0.0f, c.hurt_time - dt);
		c.flash = math::max(0.0f, c.flash - dt * 3.0f);
		c.attacking = math::max(0.0f, c.attacking - dt);
		if (c.model != "figure") continue;
		// The figure's pose from how it moves.
		float smooth = 1.0f - math::exp(-10.0f * dt);
		float moving = c.blocked ? 0.0f : math::hypot(c.vel[0], c.vel[2]);
		c.anim_speed += (moving - c.anim_speed) * smooth;
		float vspeed = c.on_floor ? 0.0f : c.vel[1];
		c.figure.move_speed = c.anim_speed;
		c.figure.on_floor = math::abs(vspeed) < 1.5f;
		if (c.strikes != c.seen_strikes)
		{
			c.seen_strikes = c.strikes;
			c.figure.swing();
		}
		c.figure.update(dt);
	}
}

// The creature a ray meets first within max_distance (its box), or 0.
fn int Creatures.pick(&self, float[3] origin, float[3] dir, float max_distance, float* distance)
{
	int best = 0;
	*distance = max_distance;
	foreach (&c : self.list)
	{
		// Slab test against its box.
		float[3] mn = { c.pos[0] - c.box[0] / 2, c.pos[1], c.pos[2] - c.box[2] / 2 };
		float[3] mx = { c.pos[0] + c.box[0] / 2, c.pos[1] + c.box[1], c.pos[2] + c.box[2] / 2 };
		float t0 = 0;
		float t1 = *distance;
		bool hit = true;
		for (int k = 0; k < 3 && hit; k++)
		{
			if (math::abs(dir[k]) < 1e-8f)
			{
				hit = origin[k] >= mn[k] && origin[k] <= mx[k];
				continue;
			}
			float a = (mn[k] - origin[k]) / dir[k];
			float b = (mx[k] - origin[k]) / dir[k];
			if (a > b) @swap(a, b);
			t0 = math::max(t0, a);
			t1 = math::min(t1, b);
			hit = t0 <= t1;
		}
		if (hit && t0 < *distance)
		{
			*distance = t0;
			best = c.id;
		}
	}
	return best;
}

// What behaviour does to a creature: a knock back (its velocity), a squash,
// a red flash, a strike.
fn void Creature.push(&self, float[3] v)
{
	for (int k = 0; k < 3; k++) self.vel[k] = math::clamp(v[k], -40.0f, 40.0f);
}

fn void Creature.hurt(&self) => self.hurt_time = 0.3f;
fn void Creature.flash_red(&self) => self.flash = 1.0f;

fn void Creature.attack(&self)
{
	self.attacking = 0.35f;
	self.strikes++;
}

fn void Creature.set_variant(&self, String variant)
{
	if (variant == self.variant) return;
	self.variant = intern::intern(variant);
	self.hurt_time = 0.3f; // a little squash as it changes
}
