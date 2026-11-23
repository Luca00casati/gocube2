// The base game's behaviour (the core mod's code: src/core_mod/core_mod.cpp):
// plants (growing, falling, support, leaf decay, trees sprouting), flowing
// water and lava, and the Block Eaters. It hears of block changes a tick
// after they happen (Game.tick) and runs every frame.
module gocube::game;
import gocube::game::blocks, gocube::game::player, gocube::game::inventory, gocube::game::drops, gocube::game::block_entities;
import gocube::game::creatures, gocube::game::rules, gocube::game::vitals, gocube::game::dye;
import gocube::content, gocube::data, gocube::world, gocube::voxel, gocube::maths;
import gocube::util::vec, gocube::util::map, gocube::util::rand, gocube::util::intern;
import std::math, std::sort;

// A cell as one integer (21 bits a coordinate, up to a million blocks out).
const long SPAN @private = 1 << 21;
const long HALF @private = SPAN >> 1;

fn long cell_key(int x, int y, int z) @private => (((long)x + HALF) * SPAN + ((long)y + HALF)) * SPAN + ((long)z + HALF);

fn BlockPos cell_of(long k) @private
{
	int z = (int)(k % SPAN - HALF);
	k /= SPAN;
	return { (int)(k / SPAN - HALF), (int)(k % SPAN - HALF), z };
}

const int[3][6] AROUND @private = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
const int[3][4] SIDES @private = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

// A set of cells that keeps the order they came in.
struct CellQueue
{
	Vec{long} order;
	sz head;
	Map{long, bool} in;
}

fn void CellQueue.add(&self, long k)
{
	if (self.in.has(k)) return;
	self.in[k] = true;
	self.order.push(k);
}

// Up to n of them, taken out (into `out`, cleared first).
fn void CellQueue.take(&self, sz n, Vec{long}* out)
{
	out.clear();
	while (self.head < self.order.len() && out.len() < n)
	{
		long k = self.order[self.head++];
		out.push(k);
		self.in.remove(k);
	}
	if (self.head == self.order.len())
	{
		self.order.clear();
		self.head = 0;
	}
	else if (self.head > 4096)
	{
		self.order.remove_first(self.head);
		self.head = 0;
	}
}

fn sz CellQueue.len(&self) => self.order.len() - self.head;

fn void CellQueue.free(&self)
{
	self.order.free();
	self.in.free();
	*self = {};
}

enum EaterState : char
{
	IDLE,
	WANDER,
	FLEE,
}

struct Eater
{
	EaterState state;
	double time;
	int health;
	bool has_heading;
	double heading;
	double flee_x, flee_z;
}

struct FluidLevel
{
	String block; // (interned)
	String fluid;
	int level;
}

struct Core
{
	// Plants.
	Map{long, double} growing;  // cell -> seconds left
	CellQueue check;            // cells whose support (or falling) to check
	CellQueue leaf_check;       // leaves to test for a nearby log
	Map{long, double} decaying; // leaves -> seconds until they fall
	double plant_timer, sprout_timer;
	bool logs_known;
	Vec{String} logs;           // every tree's log (interned)
	// Fluids.
	bool fluids_learned;
	Vec{FluidLevel} fluid_blocks; // fluid blocks: their fluid and level
	Map{String, int} steps;       // fluid -> levels lost per block
	String stone, cobblestone;
	CellQueue pending;            // cells to look at next, in the order they came
	Vec{long} batch;              // this tick's
	sz batch_head;
	double fluid_timer;
	// Creatures.
	Map{int, Eater} eaters;
	double spawn_timer;
}

fn void Core.free(&self)
{
	self.growing.free();
	self.check.free();
	self.leaf_check.free();
	self.decaying.free();
	self.logs.free();
	self.fluid_blocks.free();
	self.steps.free();
	self.pending.free();
	self.batch.free();
	self.eaters.free();
	*self = {};
}

fn double Game.random_unit(&self) @inline => self.random.unit();
fn double Game.rand_range(&self, double a, double b) => a + (b - a) * self.random.unit();

// The block's id at a cell ("" if it isn't loaded).
fn String Game.block_id(&self, int x, int y, int z)
{
	int id = self.world.block_id_at(x, y, z);
	return id < 0 ? "" : self.rules.id_of(id);
}

fn Data* Game.def_id(&self, String id) @inline => self.rules.def(id);

fn bool Game.set_block_id(&self, int x, int y, int z, String id)
{
	int index = self.content.block_index(id);
	if (index < 0) return false;
	return self.world.set_block({ x, y, z }, (BlockId)index);
}

fn bool Game.is_fluid_id(&self, String id) => id.len && self.def_id(id).has("fluid");
fn bool Game.is_leaves_id(&self, String id) => id.len && self.def_id(id).flag("leaves");

fn bool Game.is_log(&self, String id)
{
	if (!self.core.logs_known)
	{
		self.core.logs_known = true;
		foreach (&t : self.content.trees.entries)
		{
			if (t.def.get("log") && t.def.get("log").is_string()) self.core.logs.push(intern::intern(t.def.str("log")));
		}
	}
	foreach (l : self.core.logs)
	{
		if (l == id) return true;
	}
	return false;
}

fn void Game.drop_at_cell(&self, int x, int y, int z, String item, int count) => self.drops.spawn(item, count, { x + 0.5f, y + 0.3f, z + 0.5f });

fn RayHit Game.raycast_down(&self, float x, float y, float z) => self.world.raycast({ x, y, z }, { 0, -1, 0 }, 400);

// The players: the local one, and (the host) the others.
struct PlayerAt
{
	int peer;
	float[3] feet;
	bool alive;
}

fn void Game.players(&self, Vec{PlayerAt}* out)
{
	out.push({ 0, self.player.position, !self.dead });
	foreach (&o : self.others) out.push({ o.peer, o.feet, o.alive });
}

// --- Plants: growing, falling, support, leaf decay, trees sprouting ---------------------
//
// Falling: blocks with "falls" (sand, gravel) drop while there's air or a
// fluid under them.
// Support: plants ("plant_on") need one of those blocks below; blocks with
// "needs_support" need a solid block below or behind them (torches, ladders,
// doors); unsupported blocks pop off and drop their item.
// Leaf decay: leaves more than LEAF_REACH steps (through leaves) from a log
// fall after a short random delay, now and then dropping a sapling; placed
// leaves are a "_placed" variant that never decays.
// Growth: a block with "grows_into" turns into that block after about
// "grow_seconds"; a sapling ("grows_tree": a tree id, or {tree id: weight})
// grows into that tree, trying again later when there's no room. What's
// growing is saved with the world (world.json's "plants").
// Trees sprout by themselves: every SPROUT_EVERY seconds, near one player, a
// random spot SPROUT_MIN-SPROUT_MAX blocks away whose top block has
// "sprouts_trees" ({tree id: weight}: grass) and no log within SPROUT_SPACE
// grows one of those trees, if there's room.

const double PLANT_TICK @private = 1.0;
const double SPROUT_EVERY @private = 20.0;
const double SPROUT_MIN @private = 24.0;
const double SPROUT_MAX @private = 48.0;
const int SPROUT_SPACE @private = 4;
const double SKY_Y @private = 200;
const int LEAF_REACH @private = 4;
const double SAPLING_CHANCE @private = 0.05;
// At most this many cells (and leaves) are looked at a game tick, the rest later.
const sz MAX_CHECKS @private = 2048;
const sz MAX_LEAF_CHECKS @private = 128;

// Whether a log is within LEAF_REACH steps, going through leaves.
fn bool Game.near_log(&self, int x, int y, int z)
{
	Map{long, int} seen;
	defer seen.free();
	Vec{BlockPos} queue;
	defer queue.free();
	seen[cell_key(x, y, z)] = 0;
	queue.push({ x, y, z });
	for (sz q = 0; q < queue.len(); q++)
	{
		BlockPos p = queue[q];
		int dist = seen.get_or(cell_key(p.x, p.y, p.z), 0);
		foreach (a : AROUND)
		{
			int nx = p.x + a[0];
			int ny = p.y + a[1];
			int nz = p.z + a[2];
			long k = cell_key(nx, ny, nz);
			if (seen.has(k)) continue;
			String b = self.block_id(nx, ny, nz);
			if (self.is_log(b)) return true;
			if (dist + 1 < LEAF_REACH && self.is_leaves_id(b))
			{
				seen[k] = dist + 1;
				queue.push({ nx, ny, nz });
			}
		}
	}
	return false;
}

struct Weighted @private
{
	String id;
	double weight;
}

// A tree id from "grows_tree" or "sprouts_trees": the id itself, or picked by weight.
fn String Game.pick_tree(&self, Data* grows)
{
	if (grows.is_string()) return grows.s;
	Vec{Weighted} ids; // (sorted by id)
	defer ids.free();
	double total = 0;
	foreach (&f : grows.fields())
	{
		double w = f.value.is_number() ? f.value.as_double() : 0.0;
		ids.push({ f.key, w });
		total += w;
	}
	if (ids.is_empty()) return "";
	sort::quicksort(ids.array_view(), fn int(Weighted a, Weighted b) => content::compare_ids(a.id, b.id));
	double roll = self.random_unit() * total;
	foreach (w : ids)
	{
		roll -= w.weight;
		if (roll <= 0) return w.id;
	}
	return ids[ids.len() - 1].id;
}

// A log within SPROUT_SPACE (sideways) of the cell, at its height or one up.
fn bool Game.log_near(&self, int x, int y, int z)
{
	for (int dx = -SPROUT_SPACE; dx <= SPROUT_SPACE; dx++)
	{
		for (int dz = -SPROUT_SPACE; dz <= SPROUT_SPACE; dz++)
		{
			for (int dy = 0; dy <= 1; dy++)
			{
				if (self.is_log(self.block_id(x + dx, y + dy, z + dz))) return true;
			}
		}
	}
	return false;
}

// One try at a tree coming up near a player.
fn void Game.sprout(&self)
{
	Vec{PlayerAt} players;
	defer players.free();
	self.players(&players);
	PlayerAt p = players[(sz)math::floor(self.random_unit() * (double)players.len())];
	double angle = self.random_unit() * 2 * math::PI;
	double dist = SPROUT_MIN + (SPROUT_MAX - SPROUT_MIN) * self.random_unit();
	int x = (int)math::floor(p.feet[0] + math::cos(angle) * dist);
	int z = (int)math::floor(p.feet[2] + math::sin(angle) * dist);
	RayHit hit = self.raycast_down(x + 0.5f, (float)SKY_Y, z + 0.5f);
	if (!hit.hit) return;
	int y = hit.position.y + 1;
	Data* trees = self.rules.def_of(hit.block).get("sprouts_trees");
	if (!trees || !trees.is_object() || self.block_id(x, y, z) != "core:air" || self.log_near(x, y, z)) return;
	self.grow_tree({ x, y, z }, self.pick_tree(trees));
}

fn void Game.plants_block_changed(&self, BlockPos p, String id)
{
	Data* d = self.def_id(id);
	long k = cell_key(p.x, p.y, p.z);
	if (d.has("grows_into") || d.has("grows_tree"))
	{
		self.core.growing[k] = d.num("grow_seconds", 60.0) * (0.6 + 0.8 * self.random_unit());
	}
	else
	{
		self.core.growing.remove(k);
	}
	self.core.check.add(k);
	foreach (o : AROUND)
	{
		int nx = p.x + o[0];
		int ny = p.y + o[1];
		int nz = p.z + o[2];
		self.core.check.add(cell_key(nx, ny, nz));
		if (self.is_leaves_id(self.block_id(nx, ny, nz))) self.core.leaf_check.add(cell_key(nx, ny, nz));
	}
}

fn void Game.plants_tick(&self, double dt)
{
	Core* c = &self.core;
	c.sprout_timer += dt;
	if (c.sprout_timer >= SPROUT_EVERY)
	{
		c.sprout_timer = 0;
		self.sprout();
	}
	Vec{long} cells;
	defer cells.free();
	// Falling and support.
	c.check.take(MAX_CHECKS, &cells);
	foreach (k : cells)
	{
		BlockPos p = cell_of(k);
		String id = self.block_id(p.x, p.y, p.z);
		Data* d = self.def_id(id);
		if (d.flag("falls"))
		{
			String below = self.block_id(p.x, p.y - 1, p.z);
			if (below == "core:air" || self.is_fluid_id(below))
			{
				self.set_block_id(p.x, p.y, p.z, "core:air");
				self.set_block_id(p.x, p.y - 1, p.z, id); // (checked again next tick)
			}
		}
		else if (id.len && id != "core:air" && !self.rules.supported(self.world, id, p))
		{
			String drop = d.str("variant_of", id);
			if (Data* dr = d.get("drops")) drop = dr.is_string() ? dr.s : drop;
			drop = intern::intern(drop);
			Data* st = d.get("state");
			bool upper_door = d.str("shape") == "door" && st && st.str("half") == "upper";
			self.set_block_id(p.x, p.y, p.z, "core:air"); // (its neighbours get checked in turn)
			if (drop.len && !upper_door) self.drop_at_cell(p.x, p.y, p.z, drop, 1);
		}
	}
	// Leaves far from any log start to decay.
	c.leaf_check.take(MAX_LEAF_CHECKS, &cells);
	foreach (k : cells)
	{
		BlockPos p = cell_of(k);
		if (self.is_leaves_id(self.block_id(p.x, p.y, p.z)) && !c.decaying.has(k) && !self.near_log(p.x, p.y, p.z))
		{
			c.decaying[k] = 0.3 + 2.7 * self.random_unit();
		}
	}
	Vec{long} fall;
	defer fall.free();
	c.decaying.@each_ref(; long k, double* left)
	{
		*left -= dt;
		if (*left <= 0) fall.push(k);
	};
	foreach (k : fall)
	{
		c.decaying.remove(k);
		BlockPos p = cell_of(k);
		String leaf = self.block_id(p.x, p.y, p.z);
		if (self.is_leaves_id(leaf))
		{
			String sapling = intern::intern(self.def_id(leaf).str("sapling"));
			self.set_block_id(p.x, p.y, p.z, "core:air");
			if (sapling.len && self.random_unit() < SAPLING_CHANCE) self.drop_at_cell(p.x, p.y, p.z, sapling, 1);
		}
	}
	// Growth, once a second.
	c.plant_timer += dt;
	if (c.plant_timer < PLANT_TICK) return;
	Vec{long} done;
	defer done.free();
	double elapsed = c.plant_timer;
	c.growing.@each_ref(; long k, double* left)
	{
		*left -= elapsed;
		if (*left <= 0) done.push(k);
	};
	foreach (k : done)
	{
		c.growing.remove(k);
		BlockPos p = cell_of(k);
		Data* d = self.def_id(self.block_id(p.x, p.y, p.z));
		Data* into = d.get("grows_into");
		if (into && into.is_string())
		{
			self.set_block_id(p.x, p.y, p.z, intern::intern(into.s)); // (tracks the next stage)
		}
		else if (d.has("grows_tree") && !self.grow_tree(p, self.pick_tree(d.get("grows_tree"))))
		{
			c.growing[k] = d.num("grow_seconds", 60.0) * 0.5; // no room yet: later
		}
	}
	c.plant_timer = 0;
}

struct PlantEntry @private
{
	BlockPos p;
	double left;
}

// world.json's "plants": [[x, y, z, seconds left], ...].
fn Data Core.save_plants(&self)
{
	Vec{PlantEntry} list;
	defer list.free();
	self.growing.@each(; long k, double left) { list.push({ cell_of(k), left }); };
	sort::quicksort(list.array_view(), fn int(PlantEntry a, PlantEntry b)
	{
		if (a.p.x != b.p.x) return a.p.x < b.p.x ? -1 : 1;
		if (a.p.y != b.p.y) return a.p.y < b.p.y ? -1 : 1;
		return a.p.z < b.p.z ? -1 : (a.p.z > b.p.z ? 1 : 0);
	});
	Data out = data::array();
	foreach (e : list)
	{
		Data one = data::array();
		one.push_long(e.p.x);
		one.push_long(e.p.y);
		one.push_long(e.p.z);
		one.push_double(e.left);
		out.push(one);
	}
	return out;
}

fn void Core.load_plants(&self, Data* d)
{
	if (!d) return;
	foreach (&e : d.items())
	{
		if (e.is_array() && e.len() == 4 && e.at(0).is_number() && e.at(1).is_number() && e.at(2).is_number() && e.at(3).is_number())
		{
			self.growing[cell_key((int)math::floor(e.at(0).as_double()), (int)math::floor(e.at(1).as_double()),
				(int)math::floor(e.at(2).as_double()))] = e.at(3).as_double();
		}
	}
}

// --- Fluids: flowing water and lava ---------------------------------------------------
//
// Only cells near a recent change are looked at (every FLUID_TICK seconds), so
// a still sea costs nothing. A fluid is a block with "fluid" (its name) and
// "level": 8 = source, 7..1 = flowing, each block further losing one (or
// "flow_step": lava 2).
// Rules for a fluid cell:
//   - flowing fluid must be fed: from above (-> level 7) or by a sideways
//     neighbour one step higher; otherwise it shrinks and finally drains away
//   - it falls into air below (as level 7) before spreading sideways
//   - otherwise it spreads sideways into air, one step lower
//   - lava touching water hardens: a source into stone, flowing into
//     cobblestone; lava and water flowing into each other make cobblestone

const double FLUID_TICK @private = 0.25;
const sz MAX_CELLS_PER_TICK @private = 1024;
// A FLUID_TICK's cells are spread over game ticks, this many at a time (a
// big flood then spreads more slowly; a few blocks of water at once).
const sz CELLS_PER_CALL @private = 64;

fn void Game.learn_fluids(&self)
{
	Core* c = &self.core;
	c.fluids_learned = true;
	foreach (&e : self.content.blocks.entries)
	{
		Data* f = e.def.get("fluid");
		if (!f || !f.is_string()) continue;
		int level = math::clamp((int)math::floor(e.def.num("level", 8.0)), 1, 8);
		String fluid = intern::intern(f.s);
		c.fluid_blocks.push({ intern::intern(e.id), fluid, level });
		c.steps[fluid] = math::max(1, (int)math::floor(e.def.num("flow_step", 1.0)));
	}
	c.stone = self.content.blocks.has("core:stone") ? "core:stone" : "core:air";
	c.cobblestone = self.content.blocks.has("core:cobblestone") ? "core:cobblestone" : "core:air";
}

fn FluidLevel* Game.fluid_info(&self, String id)
{
	foreach (&f : self.core.fluid_blocks)
	{
		if (f.block == id) return f;
	}
	return null;
}

fn String Game.fluid_of(&self, String id)
{
	FluidLevel* f = self.fluid_info(id);
	return f ? f.fluid : "";
}

fn int Game.level_of(&self, String id)
{
	FluidLevel* f = self.fluid_info(id);
	return f ? f.level : 0;
}

fn String Game.block_for(&self, String fluid, int level)
{
	foreach (&f : self.core.fluid_blocks)
	{
		if (f.fluid == fluid && f.level == level) return f.block;
	}
	return "";
}

fn void Game.set_fluid(&self, int x, int y, int z, String fluid, int level)
{
	String there = self.fluid_of(self.block_id(x, y, z));
	if (there.len && there != fluid)
	{
		self.set_block_id(x, y, z, self.core.cobblestone);
		return;
	}
	String id = self.block_for(fluid, level);
	if (id.len) self.set_block_id(x, y, z, id);
}

fn String Game.fluid_at(&self, int x, int y, int z)
{
	String b = self.block_id(x, y, z);
	return self.fluid_of(b.len ? b : "core:air");
}

fn void Game.update_fluid(&self, int x, int y, int z)
{
	String here = self.block_id(x, y, z);
	FluidLevel* info = here.len ? self.fluid_info(here) : null;
	if (!info) return; // (not loaded, or not a fluid)
	String fluid = info.fluid;
	int level = info.level;
	int step = self.core.steps.get_or(fluid, 1);
	if (fluid == "lava")
	{
		foreach (a : AROUND)
		{
			if (a[1] != -1 && self.fluid_at(x + a[0], y + a[1], z + a[2]) == "water")
			{
				self.set_block_id(x, y, z, level == 8 ? self.core.stone : self.core.cobblestone);
				return;
			}
		}
	}
	if (level < 8)
	{
		// Flowing: is something still feeding it?
		int fed = 0;
		if (self.fluid_at(x, y + 1, z) == fluid) fed = 7;
		foreach (a : SIDES)
		{
			String n = self.block_id(x + a[0], y, z + a[2]);
			if (n.len && self.fluid_of(n) == fluid) fed = math::max(fed, self.level_of(n) - step);
		}
		if (fed <= 0)
		{
			self.set_block_id(x, y, z, "core:air"); // drains away
			return;
		}
		if (fed != level)
		{
			String id = self.block_for(fluid, fed);
			self.set_block_id(x, y, z, id.len ? id : here);
			return; // (changing looks at this cell and its neighbours again)
		}
	}
	// Fall first.
	String under = self.block_id(x, y - 1, z);
	if (!under.len) return; // (not loaded)
	String under_fluid = self.fluid_of(under);
	if (under == "core:air" || (under_fluid.len && under_fluid != fluid))
	{
		self.set_fluid(x, y - 1, z, fluid, 7);
		return;
	}
	if (under_fluid == fluid)
	{
		if (self.level_of(under) < 7) self.set_fluid(x, y - 1, z, fluid, 7);
		if (level < 8) return; // falling fluid lands on fluid: no sideways spread
	}
	// Then sideways, one step lower.
	int next_level = level - step;
	if (next_level < 1) return;
	foreach (a : SIDES)
	{
		int px = x + a[0];
		int pz = z + a[2];
		String n = self.block_id(px, y, pz);
		String nf = self.fluid_of(n);
		if (n.len && (n == "core:air" || (nf == fluid && self.level_of(n) < next_level) || (nf.len && nf != fluid)))
		{
			self.set_fluid(px, y, pz, fluid, next_level);
		}
	}
}

fn void Game.fluids_block_changed(&self, BlockPos p)
{
	self.core.pending.add(cell_key(p.x, p.y, p.z));
	foreach (o : AROUND) self.core.pending.add(cell_key(p.x + o[0], p.y + o[1], p.z + o[2]));
}

fn void Game.fluids_tick(&self, double dt)
{
	Core* c = &self.core;
	c.fluid_timer += dt;
	if (c.batch_head >= c.batch.len())
	{
		if (c.fluid_timer < FLUID_TICK || c.pending.len() == 0) return;
		c.fluid_timer = 0;
		if (!c.fluids_learned) self.learn_fluids();
		// This tick's cells (at most MAX_CELLS_PER_TICK; the rest next time).
		c.pending.take(MAX_CELLS_PER_TICK, &c.batch);
		c.batch_head = 0;
	}
	sz stop = math::min(c.batch.len(), c.batch_head + CELLS_PER_CALL);
	for (; c.batch_head < stop; c.batch_head++)
	{
		BlockPos p = cell_of(c.batch[c.batch_head]);
		self.update_fluid(p.x, p.y, p.z);
	}
}

// How many cells wait to be looked at (for tests).
fn sz Game.fluids_pending(&self) => self.core.pending.len() + (self.core.batch.len() - self.core.batch_head);

// --- Creatures (nothing here is hostile) -----------------------------------------------
// The game moves their bodies (gravity, steps, turning towards `heading` at
// `speed`); this decides where they go, spawns and despawns them, and handles
// hits.
//
// Block Eaters are rare: one comes out by day now and then (every
// SPAWN_EVERY seconds, at most MAX_EATERS) on grass open to the sky, 20-48
// blocks from a player, and goes when no player is within DESPAWN_DISTANCE.
// They graze (idle), wander, turn away from water and drops, and run off when
// hit. Grazing, now and then one eats the block in front of its feet: the
// block is gone. (One made of a block drops it when defeated.)

const int MAX_EATERS @private = 2;
const double SPAWN_EVERY @private = 45.0;
const double SPAWN_MIN @private = 20.0;
const double SPAWN_MAX @private = 48.0;
const double DESPAWN_DISTANCE @private = 80.0;
const String EATER @private = "core:block_eater";
const int EATER_HEALTH @private = 8;
const double WALK_SPEED @private = 1.2;
const double FLEE_SPEED @private = 3.4;
const double EAT_CHANCE @private = 0.3; // each time one starts grazing

fn void Game.block_sound_at(&self, String id, String kind, double x, double y, double z, float db = 0)
{
	Data* s = self.def_id(id).get("sound");
	if (s && s.is_string()) self.play_shared_sound(s.s, kind, { (float)x, (float)y, (float)z }, db);
}

// Water at the next step, or a fall of more than 3 blocks.
fn bool Game.danger_ahead(&self, Creature* c, double heading)
{
	int ax = (int)math::floor(c.pos[0] - math::sin(heading) * 0.9);
	int az = (int)math::floor(c.pos[2] - math::cos(heading) * 0.9);
	int ay = (int)math::floor(c.pos[1]);
	for (int dy = 1; dy >= -3; dy--)
	{
		String b = self.block_id(ax, ay + dy, az);
		if (self.is_fluid_id(b)) return true;
		if (b.len && b != "core:air") return false;
	}
	return true;
}

// Whether a Block Eater eats this block: solid, not a fluid, never storage
// (the contents would spill) nor half of a door or bed.
fn bool Game.edible(&self, String id)
{
	if (!id.len || id == "core:air") return false;
	Data* d = self.def_id(id);
	String shape = d.str("shape");
	return d.flag("solid", true) && !d.has("fluid") && !d.has("block_entity") && shape != "door" && shape != "bed";
}

// Eats the block in front of its feet: it's gone.
fn void Game.try_eat(&self, Creature* c)
{
	double yaw = c.yaw;
	int x = (int)math::floor(c.pos[0] - math::sin(yaw) * 0.9);
	int y = (int)math::floor(c.pos[1] - 0.5);
	int z = (int)math::floor(c.pos[2] - math::cos(yaw) * 0.9);
	String id = self.block_id(x, y, z);
	if (self.edible(id) && self.set_block_id(x, y, z, "core:air")) self.block_sound_at(id, "break", x + 0.5, y + 0.5, z + 0.5);
}

fn void Game.pick_state(&self, Eater* s, Creature* c)
{
	if (s.state == FLEE || self.random_unit() < 0.45)
	{
		s.state = IDLE; // graze a while
		s.time = self.rand_range(2.0, 5.0);
		if (self.random_unit() < EAT_CHANCE) self.try_eat(c);
	}
	else
	{
		s.state = WANDER;
		s.time = self.rand_range(2.5, 6.0);
		s.heading = (s.has_heading ? s.heading : (double)c.heading) + self.rand_range(-1.6, 1.6);
		s.has_heading = true;
	}
}

fn void Game.eater(&self, Eater* s, Creature* c, double dt)
{
	s.time -= dt;
	if (s.time <= 0) self.pick_state(s, c);
	double speed = 0;
	double heading = s.has_heading ? s.heading : (double)c.heading;
	if (s.state == WANDER)
	{
		speed = WALK_SPEED;
		if (c.hit_wall) heading += math::PI * 0.5 + self.random_unit() * math::PI; // blocked: try another way
	}
	else if (s.state == FLEE)
	{
		speed = FLEE_SPEED;
		double ax = c.pos[0] - s.flee_x;
		double az = c.pos[2] - s.flee_z;
		if (ax * ax + az * az > 0.01) heading = math::atan2(-ax, -az);
	}
	c.hit_wall = false; // (looked at)
	if (speed > 0 && self.danger_ahead(c, heading)) heading += math::PI * (0.5 + self.random_unit()); // water or a drop ahead: turn away
	s.heading = heading;
	s.has_heading = true;
	c.heading = (float)heading;
	c.speed = math::clamp((float)speed, 0.0f, 20.0f);
}

fn void Game.spawn_eater(&self, PlayerAt[] players)
{
	PlayerAt p = players[(sz)math::floor(self.random_unit() * (double)players.len)];
	double angle = self.random_unit() * 2 * math::PI;
	double dist = self.rand_range(SPAWN_MIN, SPAWN_MAX);
	int x = (int)math::floor(p.feet[0] + math::cos(angle) * dist);
	int z = (int)math::floor(p.feet[2] + math::sin(angle) * dist);
	RayHit hit = self.raycast_down(x + 0.5f, (float)SKY_Y, z + 0.5f);
	if (!hit.hit || self.rules.id_of(hit.block) != "core:grass") return; // only on grass, open to the sky
	int fy = hit.position.y + 1;
	if (!self.world.is_box_free({ x + 0.5f, (float)fy, z + 0.5f }, { 0.9f, 0.95f, 0.9f })) return;
	int id = self.creatures.spawn(self.content, EATER, { x + 0.5f, (float)fy, z + 0.5f });
	if (id) self.core.eaters[id] = { .state = IDLE, .health = EATER_HEALTH, .has_heading = true, .heading = self.random_unit() * 2 * math::PI };
}

fn void Game.despawn_far(&self, PlayerAt[] players)
{
	Vec{int} gone;
	defer gone.free();
	foreach (&c : self.creatures.all())
	{
		bool nearby = false;
		foreach (p : players)
		{
			double dx = (double)p.feet[0] - (double)c.pos[0];
			double dy = (double)p.feet[1] - (double)c.pos[1];
			double dz = (double)p.feet[2] - (double)c.pos[2];
			nearby = nearby || dx * dx + dy * dy + dz * dz < DESPAWN_DISTANCE * DESPAWN_DISTANCE;
		}
		if (!nearby || c.pos[1] < -200) gone.push(c.id);
	}
	foreach (id : gone)
	{
		self.creatures.remove(id);
		self.core.eaters.remove(id);
	}
}

fn void Game.creatures_tick(&self, double dt)
{
	foreach (&c : self.creatures.all())
	{
		if (c.kind != EATER) continue;
		bool made;
		Eater* s = self.core.eaters.slot(c.id, &made);
		if (made) *s = { .state = IDLE, .health = EATER_HEALTH, .has_heading = true, .heading = c.heading };
		self.eater(s, c, dt);
	}
	self.core.spawn_timer += dt;
	if (self.core.spawn_timer < SPAWN_EVERY) return;
	self.core.spawn_timer = 0;
	Vec{PlayerAt} players;
	defer players.free();
	self.players(&players);
	self.despawn_far(players.array_view());
	if (players.is_empty()) return;
	int eaters = 0;
	foreach (&c : self.creatures.all()) eaters += c.kind == EATER ? 1 : 0;
	if (self.day > 0.3 && eaters < MAX_EATERS) self.spawn_eater(players.array_view());
}

// A player (peer 0: the local one) standing at `from` hit a creature: hurt,
// knocked back, runs off; defeated, it drops what it's made of.
fn void Game.creature_hit(&self, int id, int damage, float[3] from, int attacker)
{
	Creature* c = self.creatures.find(id);
	if (!c || c.kind != EATER) return;
	double fx = from[0];
	double fz = from[2];
	double cx = c.pos[0];
	double cy = c.pos[1];
	double cz = c.pos[2];
	bool made;
	Eater* s = self.core.eaters.slot(id, &made);
	if (made) *s = { .state = IDLE, .health = EATER_HEALTH };
	String variant = c.variant;
	if (!variant.len)
	{
		self.play_shared_sound("core:effects", "hurt", { (float)cx, (float)cy, (float)cz }, -4.0f);
	}
	else
	{
		self.block_sound_at(variant, "break", cx, cy, cz);
	}
	s.health -= damage;
	s.state = FLEE;
	s.time = 3.0;
	s.flee_x = fx;
	s.flee_z = fz;
	double px = cx - fx;
	double pz = cz - fz;
	double len = math::sqrt(px * px + pz * pz);
	if (len > 1e-4)
	{
		px /= len;
		pz /= len;
	}
	else
	{
		px = pz = 0;
	}
	c.hurt();
	c.push({ (float)(px * 4.0), 5.0f, (float)(pz * 4.0) });
	if (s.health <= 0)
	{
		if (variant.len) self.drop_at_cell((int)math::floor(cx), (int)math::floor(cy + 0.5), (int)math::floor(cz), variant, 1);
		self.creatures.remove(id);
		self.core.eaters.remove(id);
	}
}

// --- Hooking it up ------------------------------------------------------------------

fn void Game.core_block_changed(&self, BlockPos p, BlockId id)
{
	String block = self.rules.id_of(id);
	self.plants_block_changed(p, block);
	self.fluids_block_changed(p);
}

fn void Game.core_tick(&self, double dt)
{
	self.plants_tick(dt);
	self.fluids_tick(dt);
	self.creatures_tick(dt);
}
