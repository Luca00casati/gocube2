// World generation: terrain from a 3D density field (hills, cliffs,
// overhangs, now and then a floating bit), seas with sand and gravel beaches
// and sea floors, bare stone where the dirt runs out, snow up high, caves,
// lava deep down and trees. Thread-safe once configured; the same seed always gives the
// same world, block for block (worlds made by older versions carry on
// seamlessly, only with no new ore veins; test/voxel/generator_test.c3 checks it).
module gocube::voxel;
import std::math, gocube::util::vec, gocube::util::map, std::thread, std::atomic;

// Terrain blocks the base shape is made of.
struct GeneratorIds
{
	BlockId stone, dirt, grass, sand, water, lava, gravel, snow;
	BlockId cobblestone; // the empty structures' (0: none built)
}

// Tree shapes.
enum TreeStyle : char
{
	ROUND_OAK,
	BIG_OAK,
	CACTUS,
}

// One block of a tree, in world coordinates (only_air: leaves, which don't
// replace anything).
struct TreeBlock
{
	int x, y, z;
	BlockId id;
	bool only_air;
}

// One kind of tree (defined by mods).
struct TreeKind
{
	BlockId log, leaves;  // leaves may be 0 (cactus)
	int min_trunk, max_trunk;
	TreeStyle style;
	int percent;          // of the tree cells on grass (the commoner kinds are tried first)
}

struct Tree
{
	int x, z, ground, trunk;
	int kind;  // index into the tree kinds
}

alias TreeList = Vec{Tree};
alias TreeBlockList = Vec{TreeBlock};

const int SEA_LEVEL = -4;
const float CAVE_WIDTH = 0.11f;
const float CAVE_PERIOD = 48.0f;
const float CAVE_Y_SQUASH = 1.6f;
const int CAVE_MIN_Y = -92;
const int LAVA_LEVEL = -76;      // caves below this are full of lava
const float CAVERN_PERIOD = 40.0f;
const int CAVERN_MAX_Y = -18;    // big caverns only deep down
const int TREE_CELL = 8;         // at most one tree in each TREE_CELL x TREE_CELL
const int TREE_REACH = 5;        // how far a tree's leaves spread
// Huge hollows: big dark chambers at middle depths, under the land.
const float HOLLOW_PERIOD = 70.0f;
const int HOLLOW_MAX_Y = 12;
const int HOLLOW_MIN_Y = -64;
const int HOLLOW_ROOF = 7;       // at least this much ground over one
// Ravines: long, narrow cuts down from the surface, here and there.
const float RAVINE_SCALE = 0.006f;
const int RAVINE_DEPTH = 34;
// Floating islands, over low land and the sea (rare); perfectly flat
// patches of land (rare).
const int ISLAND_Y = 36;
const float FLAT_Y = 10.5f;      // the flat patches' top: y 10
// Empty structures: at most one in each STRUCTURE_CELL x STRUCTURE_CELL,
// STRUCTURE_PERCENT of them; no further than STRUCTURE_REACH from its spot.
const int STRUCTURE_CELL = 80;
const int STRUCTURE_PERCENT = 24;
// Dead-straight tunnels deep down: at most one in each TUNNEL_CELL x
// TUNNEL_CELL, TUNNEL_PERCENT of them, TUNNEL_LENGTH long, 1 x 2.
const int TUNNEL_CELL = 160;
const int TUNNEL_PERCENT = 35;
const int TUNNEL_LENGTH = 100;
const int STRUCTURE_REACH = 12;
// The density lattice: a value every LX, LY, LZ blocks (interpolated
// between), from BOTTOM_Y up LY_COUNT points; solid where above 0.
const int LX = 4;
const int LY = 8;
const int LZ = 4;
const int BOTTOM_Y = -96;
const int LY_COUNT = 30;
const int TOP_LIMIT = 100;       // terrain stays under it (room for trees below the world's top, 127)
// The land: hills, how much it is raised, mountain ranges on top, bare stone
// from ROCK_ABOVE up and snow from SNOW_ABOVE up (a little ragged).
const float HILL_HEIGHT = 14.0f;
const float LAND_BASE = 2.0f;
const float MOUNTAINS = 34.0f;
const int ROCK_ABOVE = 46;
const int SNOW_ABOVE = 56;

const int NX @private = CHUNK / LX + 1; // lattice points across a chunk column
const int NZ @private = CHUNK / LZ + 1;
const int COLUMN_CACHE @private = 256;

// A chunk column: the density at the lattice points over its 32 x 32
// columns (the whole height of the world) and each column's topmost solid
// block. Shared by the chunks stacked in it (the latest
// COLUMN_CACHE kept), counted: the last to let go frees it.
struct Columns @private
{
	Atomic{int} refs;
	int[CHUNK_AREA] heights;
	int max_h;
	float[LY_COUNT * NZ * NX] density; // ((LY_COUNT x NZ) x NX)
}

fn void Columns.release(&self)
{
	if (self.refs.sub(1) == 1) free(self);
}

struct Generator
{
	uint seed;
	GeneratorIds ids;
	Vec{TreeKind} trees;
	Vec{int} tree_rolls;      // the tree kinds in the order a tree cell tries them
	Mutex columns_mutex;
	bool has_mutex;
	Map{long, Columns*} columns;
	Vec{long} columns_order; // oldest first
}

fn void Generator.configure(&self, uint seed, GeneratorIds ids, TreeKind[] trees)
{
	if (!self.has_mutex)
	{
		self.columns_mutex.init()!!;
		self.has_mutex = true;
	}
	self.seed = seed;
	self.ids = ids;
	self.trees.clear();
	self.trees.push_all(trees);
	self.tree_rolls.clear();
	for (int i = 0; i < trees.len; i++) self.tree_rolls.push(i);
	// Commoner first; equal ones in their order (an insertion sort: stable).
	for (sz i = 1; i < self.tree_rolls.len(); i++)
	{
		for (sz j = i; j > 0 && trees[self.tree_rolls[j]].percent > trees[self.tree_rolls[j - 1]].percent; j--)
		{
			int t = self.tree_rolls[j];
			self.tree_rolls[j] = self.tree_rolls[j - 1];
			self.tree_rolls[j - 1] = t;
		}
	}
	self.forget_cached();
}

fn void Generator.free(&self)
{
	self.forget_cached();
	self.trees.free();
	self.tree_rolls.free();
	self.columns.free();
	self.columns_order.free();
	if (self.has_mutex) self.columns_mutex.destroy();
	self.has_mutex = false;
}

fn void Generator.forget_cached(&self)
{
	if (!self.has_mutex) return;
	self.columns_mutex.lock();
	defer self.columns_mutex.unlock();
	foreach (k : self.columns_order)
	{
		if (try c = self.columns.get(k)) c.release();
	}
	self.columns.clear();
	self.columns_order.clear();
}

fn int Generator.tree_kind_count(&self) => (int)self.trees.len();

fn long column_key(int x, int z) @private => ((long)x << 32) ^ (long)(uint)z;

fn int floor_div(int v, int d) => v >= 0 ? v / d : -((-v + d - 1) / d);

fn float smoothstep01(float t) @private
{
	t = math::clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Where mountain ranges rise: a slow noise picks the ranges, a ridged one
// shapes their crests and valleys. 0 outside ranges, up to 1 on a crest.
fn float Generator.mountain_shape(&self, int x, int z)
{
	float m = fbm2((float)x * 0.0022f, (float)z * 0.0022f, self.seed + 601, 3);
	float t = math::clamp((m - 0.02f) / 0.28f, 0.0f, 1.0f);
	float range = t * t * (3.0f - 2.0f * t);
	if (range <= 0.0f) return 0.0f;
	float ridge = 1.0f - math::abs(fbm2((float)x * 0.0095f, (float)z * 0.0095f, self.seed + 603, 4)) * 1.6f;
	float r = math::clamp(ridge, 0.0f, 1.0f);
	return range * (0.3f + 0.7f * r * r);
}

// The density at a lattice point: above 0 is solid. The land's shape from 2D
// noise (continents and seas, hills, mountain ranges) gives the
// height it leans to; 3D noise moves it up and down, more where the land is
// rough (a slow noise): there it makes cliffs, overhangs and arches, now and
// then a floating piece; where it's smooth, rolling hills and plains.
// (The 2D part, the same at every height: worked out once per column.)
struct LandColumn @private
{
	float target; // the height the land leans to
	float squash; // blocks for the density to go from 0 to 1 (the 3D part's reach)
	float rough;
	float flat;   // 1: a perfectly flat patch (no 3D noise)
	float island; // 1: floating islands may hang here
}

fn LandColumn Generator.land_column(&self, int x, int z)
{
	float fx = (float)x;
	float fz = (float)z;
	float continent = fbm2(fx * 0.0032f, fz * 0.0032f, self.seed + 701, 4);
	float hills = fbm2(fx * 0.011f, fz * 0.011f, self.seed + 703, 4);
	float rough = smoothstep01((fbm2(fx * 0.0055f, fz * 0.0055f, self.seed + 705, 3) + 0.1f) / 0.5f);
	float target = (float)SEA_LEVEL - 5.0f + LAND_BASE + continent * 52.0f + hills * HILL_HEIGHT * (0.7f + 0.9f * rough);
	target += rough * 18.0f; // rough land is highland
	target += self.mountain_shape(x, z) * MOUNTAINS * 1.3f;
	float squash = 8.0f + rough * 26.0f;
	// Now and then the land goes perfectly flat, as if someone levelled it.
	float flat = smoothstep01((noise2(fx * 0.006f, fz * 0.006f, self.seed + 821) - 0.42f) / 0.08f);
	if (flat > 0.0f)
	{
		target += (FLAT_Y - target) * flat;
		squash += (1.0f - squash) * flat;
		rough *= 1.0f - flat;
	}
	// Floating islands over low land and the sea, in rare places.
	float island = smoothstep01((fbm2(fx * 0.0045f, fz * 0.0045f, self.seed + 811, 2) - 0.42f) / 0.1f) *
		math::clamp((22.0f - target) / 12.0f, 0.0f, 1.0f);
	return { target, squash, rough, flat, island };
}

fn float Generator.density(&self, LandColumn* c, int x, int y, int z)
{
	float n = fbm3((float)x * 0.017f, (float)y * 0.028f, (float)z * 0.017f, self.seed + 707, 4);
	float d = (c.target - (float)y) / c.squash + n * (0.3f + 0.9f * c.rough) * (1.0f - c.flat);
	if (c.island > 0.0f && y > ISLAND_Y - 20 && y < ISLAND_Y + 12)
	{
		// Flat-topped lumps, ragged underneath: solid where the 3D noise is
		// high, in a band that's thick at its top and thins out downwards.
		float band = y >= ISLAND_Y ? 1.0f - (float)(y - ISLAND_Y) / 6.0f : 1.0f - (float)(ISLAND_Y - y) / 16.0f;
		d = math::max(d, (n * 2.4f + band - 1.0f) * c.island * 3.0f);
	}
	if (y > TOP_LIMIT - 20) d -= (float)(y - (TOP_LIMIT - 20)) / 10.0f; // nothing reaches the top
	if (y < BOTTOM_Y + 24) d += (float)(BOTTOM_Y + 24 - y) / 6.0f;     // solid at the bottom
	return d;
}

// The chunk column at (ox, oz), from the cache or made; release() it.
fn Columns* Generator.columns_at(&self, int ox, int oz)
{
	long k = column_key(ox, oz);
	self.columns_mutex.lock();
	if (try c = self.columns.get(k))
	{
		c.refs.add(1);
		self.columns_mutex.unlock();
		return c;
	}
	self.columns_mutex.unlock();
	Columns* c = mem::new(Columns);
	c.refs.store(1);
	self.fill_columns(c, ox, oz);
	self.columns_mutex.lock();
	defer self.columns_mutex.unlock();
	if (!self.columns.has(k)) // (else made at the same time by another thread: this one isn't kept)
	{
		c.refs.add(1); // (the cache's)
		self.columns[k] = c;
		self.columns_order.push(k);
		if (self.columns_order.len() > COLUMN_CACHE)
		{
			long oldest = self.columns_order[0];
			self.columns_order.remove_at(0);
			if (try old = self.columns.get(oldest)) old.release();
			self.columns.remove(oldest);
		}
	}
	return c;
}

// The density at a column (lattice cell ix, iz; fx, fz across it), at lattice level j.
fn float Columns.level(&self, int ix, int iz, float fx, float fz, int j) @inline
{
	float* d = &self.density[(j * NZ + iz) * NX + ix];
	float a = d[0] + (d[1] - d[0]) * fx;
	float b = d[NX] + (d[NX + 1] - d[NX]) * fx;
	return a + (b - a) * fz;
}

// A chunk column's lattice (the whole height), and from it each column's
// topmost solid block.
fn void Generator.fill_columns(&self, Columns* c, int ox, int oz)
{
	LandColumn[NZ * NX] land;
	for (int k = 0; k < NZ; k++)
	{
		for (int i = 0; i < NX; i++) land[k * NX + i] = self.land_column(ox + i * LX, oz + k * LZ);
	}
	for (int j = 0; j < LY_COUNT; j++)
	{
		for (int k = 0; k < NZ; k++)
		{
			for (int i = 0; i < NX; i++)
			{
				c.density[(j * NZ + k) * NX + i] = self.density(&land[k * NX + i], ox + i * LX, BOTTOM_Y + j * LY, oz + k * LZ);
			}
		}
	}
	int top = BOTTOM_Y + (LY_COUNT - 1) * LY;
	c.max_h = -(1 << 30);
	for (int z = 0; z < CHUNK; z++)
	{
		for (int x = 0; x < CHUNK; x++)
		{
			int i = z * CHUNK + x;
			int ix = x / LX;
			int iz = z / LZ;
			float fx = (float)(x % LX) / (float)LX;
			float fz = (float)(z % LZ) / (float)LZ;
			int h = BOTTOM_Y;
			float above = c.level(ix, iz, fx, fz, LY_COUNT - 1);
			for (int j = LY_COUNT - 2; j >= 0 && h == BOTTOM_Y; j--)
			{
				float below = c.level(ix, iz, fx, fz, j);
				if (below > 0.0f || above > 0.0f)
				{
					for (int dy = LY - 1; dy >= 0; dy--)
					{
						if (below + (above - below) * (float)dy / (float)LY > 0.0f)
						{
							h = BOTTOM_Y + j * LY + dy;
							break;
						}
					}
				}
				above = below;
			}
			h = math::min(h, top);
			c.heights[i] = h;
			c.max_h = math::max(c.max_h, h);
		}
	}
}

// The topmost solid block of the terrain at a column (before trees).
fn int Generator.height_at(&self, int x, int z)
{
	int ox = floor_div(x, CHUNK) * CHUNK;
	int oz = floor_div(z, CHUNK) * CHUNK;
	Columns* c = self.columns_at(ox, oz);
	defer c.release();
	return c.heights[(z - oz) * CHUNK + (x - ox)];
}

// The ground's top block at a column whose topmost solid block is at h
// (beaches, sea floor, bare stone, grass), and the filler
// under it and how deep it goes (0: stone right under).
fn BlockId Generator.top_block(&self, int x, int h, int z, int* filler_depth, BlockId* filler)
{
	float fx = (float)x;
	float fz = (float)z;
	float sandy = noise2(fx * 0.055f, fz * 0.055f, self.seed + 711);
	float stony = noise2(fx * 0.048f, fz * 0.048f, self.seed + 713);
	float depth = noise2(fx * 0.08f, fz * 0.08f, self.seed + 715) + 0.5f * noise2(fx * 0.23f, fz * 0.23f, self.seed + 717);
	*filler_depth = math::clamp((int)math::floor(3.6f + depth * 3.0f), 0, 5);
	*filler = self.ids.dirt;
	BlockId sand = self.ids.sand ? self.ids.sand : self.ids.dirt;
	BlockId gravel = self.ids.gravel ? self.ids.gravel : sand;
	if (h < SEA_LEVEL - 1) // the sea floor
	{
		*filler_depth = math::max(*filler_depth, 2);
		*filler = stony > 0.25f ? gravel : sandy > -0.25f ? sand : self.ids.dirt;
		return *filler;
	}
	if (h <= SEA_LEVEL + 2) // the shore: beaches of sand, some of gravel
	{
		if (stony > 0.32f)
		{
			*filler = gravel;
			*filler_depth = math::max(*filler_depth, 2);
			return gravel;
		}
		if (sandy > -0.35f)
		{
			*filler = sand;
			*filler_depth = math::max(*filler_depth, 3);
			return sand;
		}
	}
	if (*filler_depth == 0)
	{
		*filler = self.ids.stone;
		return self.ids.stone; // the dirt ran out: bare stone
	}
	int jitter = (int)(hash_point(x, 7, z, self.seed + 77) % 4); // (a ragged line, not a level one)
	if (h >= ROCK_ABOVE + jitter)
	{
		*filler = self.ids.stone;
		return self.ids.stone;
	}
	return self.ids.grass;
}

// Whether a cave goes through (x, y, z), under land whose top is at `land`.
fn bool Generator.is_cave(&self, int x, int y, int z, int land)
{
	Noise3Cell[4] cells;
	return self.is_cave_cached(x, y, z, land, &cells);
}

// The same, keeping each noise's lattice cell in `cells` between calls
// (called for every block underground, nearby ones one after another).
fn bool Generator.is_cave_cached(&self, int x, int y, int z, int land, Noise3Cell[4]* cells)
{
	float s = 1.0f / CAVE_PERIOD;
	float ys = (float)y * CAVE_Y_SQUASH * s;
	float fx = (float)x;
	float fz = (float)z;
	float a = noise3_cached(fx * s, ys, fz * s, self.seed + 101, &(*cells)[0]);
	// (|a| alone too wide already: no tunnel, whatever b is)
	if (math::abs(a) < CAVE_WIDTH &&
		math::abs(a) + math::abs(noise3_cached(fx * s, ys, fz * s, self.seed + 202, &(*cells)[1])) < CAVE_WIDTH)
	{
		return true;
	}
	// Big caverns deep down.
	if (y < CAVERN_MAX_Y)
	{
		float c = 1.0f / CAVERN_PERIOD;
		float n = noise3_cached(fx * c, (float)y * 1.5f * c, fz * c, self.seed + 404, &(*cells)[2]);
		float fade = math::clamp((float)(CAVERN_MAX_Y - y) / 14.0f, 0.0f, 1.0f);
		if (n * fade > 0.36f) return true;
	}
	// Huge hollows, nearer the surface, with ground still over them.
	if (y < HOLLOW_MAX_Y && y > HOLLOW_MIN_Y && y < land - HOLLOW_ROOF)
	{
		float h = 1.0f / HOLLOW_PERIOD;
		float n = noise3_cached(fx * h, (float)y * 1.7f * h, fz * h, self.seed + 505, &(*cells)[3]);
		float fade = math::min(math::clamp((float)(HOLLOW_MAX_Y - y) / 10.0f, 0.0f, 1.0f), math::clamp((float)(y - HOLLOW_MIN_Y) / 10.0f, 0.0f, 1.0f));
		fade = math::min(fade, math::clamp((float)(land - HOLLOW_ROOF - y) / 6.0f, 0.0f, 1.0f));
		return n * fade > 0.4f;
	}
	return false;
}

// How far down a ravine cuts at a column whose land is at `land`: carve from
// the returned height up (the land's height + 1: no ravine).
fn int Generator.ravine_floor(&self, int x, int z, int land)
{
	if (land <= SEA_LEVEL + 3) return land + 1;
	float fx = (float)x;
	float fz = (float)z;
	float where = noise2(fx * 0.0025f, fz * 0.0025f, self.seed + 803); // (only some places have them)
	if (where < 0.15f) return land + 1;
	float v = math::abs(noise2(fx * RAVINE_SCALE, fz * RAVINE_SCALE, self.seed + 801));
	float width = 0.022f * math::min((where - 0.15f) / 0.1f, 1.0f);
	if (v >= width) return land + 1;
	float depth = (1.0f - v / width);
	return land - (int)(depth * depth * (float)RAVINE_DEPTH) - 3;
}

// Trees whose leaves may reach columns [x0, x1) x [z0, z1).
fn void Generator.trees_near(&self, int x0, int z0, int x1, int z1, TreeList* out)
{
	for (int cz = floor_div(z0 - TREE_REACH, TREE_CELL); cz <= floor_div(z1 + TREE_REACH, TREE_CELL); cz++)
	{
		for (int cx = floor_div(x0 - TREE_REACH, TREE_CELL); cx <= floor_div(x1 + TREE_REACH, TREE_CELL); cx++)
		{
			uint r = hash_point(cx, 7, cz, self.seed + 77);
			int tx = cx * TREE_CELL + 1 + (int)((r >> 8) % (uint)(TREE_CELL - 2));
			int tz = cz * TREE_CELL + 1 + (int)((r >> 12) % (uint)(TREE_CELL - 2));
			// Pick a tree kind by their chances; most cells get none.
			int roll = (int)(r % 100);
			int kind = -1;
			foreach (k : self.tree_rolls)
			{
				if (roll < self.trees[k].percent)
				{
					kind = k;
					break;
				}
				roll -= self.trees[k].percent;
			}
			if (kind < 0) continue;
			int ground = self.height_at(tx, tz);
			if (ground <= SEA_LEVEL + 1 || self.is_cave(tx, ground, tz, ground) || self.ravine_floor(tx, tz, ground) <= ground) continue; // beach, sea or a cave mouth
			int depth;
			BlockId filler;
			if (self.top_block(tx, ground, tz, &depth, &filler) != self.ids.grass) continue; // a beach, gravel, bare stone
			TreeKind tk = self.trees[kind];
			int span = math::max(1, tk.max_trunk - tk.min_trunk + 1);
			out.push({ .x = tx, .z = tz, .ground = ground, .trunk = tk.min_trunk + (int)((r >> 16) % (uint)span), .kind = kind });
		}
	}
}

// Fills a chunk whose minimum corner is (ox, oy, oz) (in blocks).
fn void Generator.generate(&self, ChunkData* chunk, int ox, int oy, int oz)
{
	chunk.clear();
	Columns* cols = self.columns_at(ox, oz); // (with the chunks above and below)
	defer cols.release();
	const int TREE_ROOM = 18;
	int top_y = oy + CHUNK - 1;
	if (oy > math::max(cols.max_h, SEA_LEVEL) + TREE_ROOM) return; // all air
	bool terrain = oy <= math::max(cols.max_h, SEA_LEVEL) + 1;
	Noise3Cell[4] cave_cells;
	// From a little above the chunk (the filler under a surface just above
	// it) to one below (snow on a surface just under it).
	int scan_top = math::min(top_y + 8, BOTTOM_Y + (LY_COUNT - 1) * LY - 1);
	for (int z = 0; z < CHUNK && terrain; z++)
	{
		for (int x = 0; x < CHUNK; x++)
		{
			int i = z * CHUNK + x;
			int wx = ox + x;
			int wz = oz + z;
			int ix = x / LX;
			int iz = z / LZ;
			float fx = (float)(x % LX) / (float)LX;
			float fz = (float)(z % LZ) / (float)LZ;
			int land = cols.heights[i]; // the column's topmost solid block
			bool caves_allowed = land > SEA_LEVEL + 2;
			int ravine = self.ravine_floor(wx, wz, land);
			int filler_depth = 0;
			BlockId filler = AIR;
			// Blocks since the last surface, going down (-1: in air or water).
			// Solid just above the scan: say deep in the ground (stone).
			int run = scan_top + 1 <= land && cols.solid_at(ix, iz, fx, fz, scan_top + 1) ? 1 << 20 : -1;
			for (int wy = scan_top; wy >= oy - 1; wy--)
			{
				bool solid = wy <= land && cols.solid_at(ix, iz, fx, fz, wy);
				BlockId b;
				if (!solid)
				{
					run = -1;
					b = wy <= SEA_LEVEL ? self.ids.water : AIR;
				}
				else
				{
					run = run < (1 << 20) ? run + 1 : run;
					if (run == 0)
					{
						b = self.top_block(wx, wy, wz, &filler_depth, &filler);
						// A thin cover (snow) on high ground, on the block above.
						if (self.ids.snow && b == self.ids.grass && wy + 1 >= oy && wy + 1 <= top_y &&
							wy >= SNOW_ABOVE + (int)(hash_point(wx, 9, wz, self.seed + 79) % 3) &&
							chunk.get(x, wy + 1 - oy, z) == AIR)
						{
							chunk.set(x, wy + 1 - oy, z, self.ids.snow);
						}
					}
					else if (run <= filler_depth)
					{
						b = filler;
					}
					else
					{
						b = self.ids.stone;
					}
					// Caves: on land, or deep under the sea.
					if (wy >= oy && wy > CAVE_MIN_Y && (caves_allowed || wy < land - 8) && self.is_cave_cached(wx, wy, wz, land, &cave_cells))
					{
						b = wy <= LAVA_LEVEL && self.ids.lava ? self.ids.lava : AIR;
					}
					else if (wy >= ravine)
					{
						b = AIR;
					}
				}
				if (wy >= oy && wy <= top_y && b != AIR) chunk.set(x, wy - oy, z, b);
			}
		}
	}
	{
		TunnelList tunnels;
		defer tunnels.free();
		self.tunnels_near(ox, oz, ox + CHUNK, oz + CHUNK, &tunnels);
		TreeSink sink = { .chunk = chunk, .ox = ox, .oy = oy, .oz = oz };
		foreach (t : tunnels)
		{
			if (t.y + 1 < oy || t.y > top_y) continue;
			for (int s = 0; s < TUNNEL_LENGTH; s++)
			{
				int tx = t.along_x ? t.x + s : t.x;
				int tz = t.along_x ? t.z : t.z + s;
				if (tx < ox || tx >= ox + CHUNK || tz < oz || tz >= oz + CHUNK) continue;
				sink.force(tx, t.y, tz, AIR);
				sink.force(tx, t.y + 1, tz, AIR);
			}
		}
	}
	if (top_y >= SEA_LEVEL + 2)
	{
		TreeList trees;
		defer trees.free();
		self.trees_near(ox, oz, ox + CHUNK, oz + CHUNK, &trees);
		TreeSink sink = { .chunk = chunk, .ox = ox, .oy = oy, .oz = oz };
		foreach (t : trees) self.build_tree(t, &sink);
	}
	if (self.ids.cobblestone)
	{
		StructureList found;
		defer found.free();
		self.structures_near(ox, oz, ox + CHUNK, oz + CHUNK, &found);
		TreeSink sink = { .chunk = chunk, .ox = ox, .oy = oy, .oz = oz };
		foreach (st : found) self.build_structure(st, &sink);
	}
	chunk.compact();
}

// Solid at height y, at a column (lattice cell ix, iz; fx, fz across it).
fn bool Columns.solid_at(&self, int ix, int iz, float fx, float fz, int y)
{
	int j = math::clamp(floor_div(y - BOTTOM_Y, LY), 0, LY_COUNT - 2);
	float fy = (float)(y - (BOTTOM_Y + j * LY)) / (float)LY;
	float lo = self.level(ix, iz, fx, fz, j);
	return lo + (self.level(ix, iz, fx, fz, j + 1) - lo) * fy > 0.0f;
}

// Where a tree's blocks go: into a chunk (those inside it), or a list.
struct TreeSink @private
{
	ChunkData* chunk;
	int ox, oy, oz;
	TreeBlockList* out;
}

fn void TreeSink.put(&self, int wx, int wy, int wz, BlockId id, bool only_air)
{
	if (id == AIR) return;
	if (self.out)
	{
		self.out.push({ .x = wx, .y = wy, .z = wz, .id = id, .only_air = only_air });
		return;
	}
	int x = wx - self.ox;
	int y = wy - self.oy;
	int z = wz - self.oz;
	if (x < 0 || y < 0 || z < 0 || x >= CHUNK || y >= CHUNK || z >= CHUNK) return;
	if (only_air && self.chunk.get(x, y, z) != AIR) return;
	self.chunk.set(x, y, z, id);
}

// The blocks of a tree of `kind` standing on the block at (x, ground, z)
// (one growing by itself). Its height and shape come from the position.
fn void Generator.tree_blocks(&self, int kind, int x, int ground, int z, TreeBlockList* out)
{
	if (kind < 0 || kind >= (int)self.trees.len()) return;
	TreeKind tk = self.trees[kind];
	uint r = hash_point(x, ground, z, self.seed + 67);
	int span = math::max(1, tk.max_trunk - tk.min_trunk + 1);
	Tree t = { .x = x, .z = z, .ground = ground, .trunk = tk.min_trunk + (int)(r % (uint)span), .kind = kind };
	TreeSink sink = { .out = out };
	self.build_tree(t, &sink);
}

// A rough ball of leaves (a little ragged, from the position hash).
fn void Generator.leaf_blob(&self, TreeSink* put, BlockId leaves, int cx, int cy, int cz, float r)
{
	int ri = (int)math::ceil(r);
	for (int dy = -ri; dy <= ri; dy++)
	{
		for (int dz = -ri; dz <= ri; dz++)
		{
			for (int dx = -ri; dx <= ri; dx++)
			{
				float d = (float)(dx * dx + dz * dz) + (float)(dy * dy) * 1.5f;
				float jitter = (float)(hash_point(cx + dx, cy + dy, cz + dz, self.seed + 61) % 100) / 100.0f;
				if (d <= r * r - jitter * r) put.put(cx + dx, cy + dy, cz + dz, leaves, true);
			}
		}
	}
}

fn int round_i(float v) @private @inline => (int)math::round(v);

// A block inside the chunk, whatever is there (air too: a structure's inside).
fn void TreeSink.force(&self, int wx, int wy, int wz, BlockId id)
{
	int x = wx - self.ox;
	int y = wy - self.oy;
	int z = wz - self.oz;
	if (x < 0 || y < 0 || z < 0 || x >= CHUNK || y >= CHUNK || z >= CHUNK || !self.chunk) return;
	self.chunk.set(x, y, z, id);
}

// --- Empty structures ----------------------------------------------------------------
// Rare, small and abandoned, with nothing in them and nothing to say who
// made them: a lone pillar, a dirt hut, a shaft down to a tunnel that goes
// nowhere, a broken wall; and things that are just wrong: sand hanging in
// the air, a perfect cube of stone, a tree that never had leaves.

enum StructureKind : char
{
	PILLAR,
	HUT,
	SHAFT,
	WALL,
	FLOATING_SAND,
	CUBE,
	DEAD_TREE,
}

struct Structure
{
	int x, z, ground;
	StructureKind kind;
	uint r; // (its shape's dice)
}

alias StructureList = Vec{Structure};

struct StraightTunnel
{
	int x, y, z;   // its first block (the lower of its two)
	bool along_x;  // else along z
}

alias TunnelList = Vec{StraightTunnel};

// Straight tunnels that pass through columns [x0, x1) x [z0, z1).
fn void Generator.tunnels_near(&self, int x0, int z0, int x1, int z1, TunnelList* out)
{
	for (int cz = floor_div(z0 - TUNNEL_LENGTH, TUNNEL_CELL); cz <= floor_div(z1, TUNNEL_CELL); cz++)
	{
		for (int cx = floor_div(x0 - TUNNEL_LENGTH, TUNNEL_CELL); cx <= floor_div(x1, TUNNEL_CELL); cx++)
		{
			uint r = hash_point(cx, 17, cz, self.seed + 97);
			if (r % 100 >= TUNNEL_PERCENT) continue;
			StraightTunnel t = { .x = cx * TUNNEL_CELL + (int)((r >> 8) % (uint)TUNNEL_CELL), .y = -46 + (int)((r >> 16) % 22),
				.z = cz * TUNNEL_CELL + (int)((r >> 20) % (uint)TUNNEL_CELL), .along_x = ((r >> 30) & 1) != 0 };
			int ex = t.along_x ? t.x + TUNNEL_LENGTH : t.x + 1;
			int ez = t.along_x ? t.z + 1 : t.z + TUNNEL_LENGTH;
			if (ex <= x0 || t.x >= x1 || ez <= z0 || t.z >= z1) continue;
			out.push(t);
		}
	}
}

// Structures that may reach columns [x0, x1) x [z0, z1).
fn void Generator.structures_near(&self, int x0, int z0, int x1, int z1, StructureList* out)
{
	for (int cz = floor_div(z0 - STRUCTURE_REACH, STRUCTURE_CELL); cz <= floor_div(z1 + STRUCTURE_REACH, STRUCTURE_CELL); cz++)
	{
		for (int cx = floor_div(x0 - STRUCTURE_REACH, STRUCTURE_CELL); cx <= floor_div(x1 + STRUCTURE_REACH, STRUCTURE_CELL); cx++)
		{
			uint r = hash_point(cx, 13, cz, self.seed + 91);
			if (r % 100 >= STRUCTURE_PERCENT) continue;
			int px = cx * STRUCTURE_CELL + 16 + (int)((r >> 8) % (uint)(STRUCTURE_CELL - 32));
			int pz = cz * STRUCTURE_CELL + 16 + (int)((r >> 16) % (uint)(STRUCTURE_CELL - 32));
			if (px + STRUCTURE_REACH < x0 || px - STRUCTURE_REACH >= x1 || pz + STRUCTURE_REACH < z0 || pz - STRUCTURE_REACH >= z1) continue;
			int ground = self.height_at(px, pz);
			if (ground <= SEA_LEVEL + 1 || ground > TOP_LIMIT - 16 || self.is_cave(px, ground, pz, ground)) continue; // the sea, a cave mouth
			out.push({ .x = px, .z = pz, .ground = ground, .kind = StructureKind::from_ordinal((int)((r >> 24) % (uint)StructureKind::len)), .r = hash_point(px, ground, pz, self.seed + 93) });
		}
	}
}

fn void Generator.build_structure(&self, Structure st, TreeSink* put)
{
	BlockId cobble = self.ids.cobblestone;
	BlockId dirt = self.ids.dirt;
	int g = st.ground;
	switch (st.kind)
	{
		case PILLAR:
			// 2 x 2, 4 to 8 high, sunk into the ground; its top a little broken.
			int h = 4 + (int)(st.r % 5);
			for (int dz = 0; dz < 2; dz++)
			{
				for (int dx = 0; dx < 2; dx++)
				{
					int top = g + h - (int)((st.r >> (uint)(4 + (dz * 2 + dx) * 2)) % 2);
					for (int y = g - 3; y <= top; y++) put.force(st.x + dx, y, st.z + dz, cobble);
				}
			}
		case HUT:
			// 5 x 5 of dirt, 3 high inside a roof, a doorway and nothing else;
			// its walls go down into the ground.
			int door = (int)(st.r % 4);
			for (int dz = -2; dz <= 2; dz++)
			{
				for (int dx = -2; dx <= 2; dx++)
				{
					bool wall = dx == -2 || dx == 2 || dz == -2 || dz == 2;
					for (int y = g - 3; y <= g; y++) put.put(st.x + dx, y, st.z + dz, dirt, true); // (a footing)
					put.force(st.x + dx, g, st.z + dz, dirt); // the floor
					for (int y = g + 1; y <= g + 3; y++) put.force(st.x + dx, y, st.z + dz, wall ? dirt : AIR);
					put.force(st.x + dx, g + 4, st.z + dz, dirt); // the roof
				}
			}
			int[2][4] doors = { { 0, -2 }, { 2, 0 }, { 0, 2 }, { -2, 0 } };
			for (int y = g + 1; y <= g + 2; y++) put.force(st.x + doors[door][0], y, st.z + doors[door][1], AIR);
		case SHAFT:
			// A cobblestone rim around a 1 x 2 hole straight down, then a
			// tunnel along, 2 high, that ends in stone.
			int depth = 12 + (int)(st.r % 6);
			int run = 6 + (int)((st.r >> 4) % 6);
			bool along_x = ((st.r >> 8) & 1) != 0;
			for (int dz = -1; dz <= 2; dz++)
			{
				for (int dx = -1; dx <= 1; dx++)
				{
					bool hole = dx == 0 && (dz == 0 || dz == 1);
					put.force(st.x + dx, g, st.z + dz, hole ? AIR : cobble);
					for (int y = g + 1; y <= g + 3; y++) if (hole) put.force(st.x + dx, y, st.z + dz, AIR);
				}
			}
			for (int y = g - depth; y < g; y++)
			{
				put.force(st.x, y, st.z, AIR);
				put.force(st.x, y, st.z + 1, AIR);
			}
			for (int s = 1; s <= run; s++)
			{
				int tx = along_x ? st.x + s : st.x;
				int tz = along_x ? st.z : st.z + 1 + s;
				for (int y = g - depth; y <= g - depth + 1; y++) put.force(tx, y, tz, AIR);
			}
		case FLOATING_SAND:
			// A column of sand hanging a few blocks over the ground.
			if (!self.ids.sand) return;
			int lift = 3 + (int)(st.r % 3);
			int tall = 2 + (int)((st.r >> 3) % 3);
			for (int y = g + lift; y < g + lift + tall; y++) put.force(st.x, y, st.z, self.ids.sand);
		case CUBE:
			// 3 x 3 x 3 of stone, perfect, on whatever ground is there.
			for (int dy = -2; dy <= 3; dy++)
			{
				for (int dz = -1; dz <= 1; dz++)
				{
					for (int dx = -1; dx <= 1; dx++)
					{
						if (dy <= 0)
						{
							put.put(st.x + dx, g + dy, st.z + dz, self.ids.stone, true); // (a footing under it)
						}
						else
						{
							put.force(st.x + dx, g + dy, st.z + dz, self.ids.stone);
						}
					}
				}
			}
		case DEAD_TREE:
			// A bare trunk with a stub or two, no leaves at all.
			if (!self.trees.len()) return;
			BlockId log = self.trees[0].log;
			int trunk = 5 + (int)(st.r % 4);
			for (int y = g + 1; y <= g + trunk; y++) put.force(st.x, y, st.z, log);
			int[2][4] sides = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			int stubs = 1 + (int)((st.r >> 4) % 2);
			for (int b = 0; b < stubs; b++)
			{
				int[2] d = sides[(st.r >> (uint)(6 + b * 3)) % 4];
				put.force(st.x + d[0], g + trunk - 1 - b * 2, st.z + d[1], log);
			}
		case WALL:
			// 7 long, 2 high, some of it fallen.
			bool along_x = (st.r & 1) != 0;
			for (int s = -3; s <= 3; s++)
			{
				int wx = along_x ? st.x + s : st.x;
				int wz = along_x ? st.z : st.z + s;
				for (int y = g - 2; y <= g; y++) put.force(wx, y, wz, cobble);
				for (int y = g + 1; y <= g + 2; y++)
				{
					uint k = hash_point(wx, y, wz, self.seed + 95) % 100;
					if (k < (y == g + 2 ? 45u : 12u)) continue; // (gaps, more at the top)
					put.force(wx, y, wz, cobble);
				}
			}
	}
}

fn void Generator.build_tree(&self, Tree t, TreeSink* put)
{
	TreeKind k = self.trees[t.kind];
	int base = t.ground + 1;
	uint h = hash_point(t.x, t.ground, t.z, self.seed + 63);
	switch (k.style)
	{
		case CACTUS:
			for (int dy = 0; dy < t.trunk; dy++) put.put(t.x, base + dy, t.z, k.log, true);
			return;
		case BIG_OAK:
			// A tall trunk with 2-3 branches, each ending in a ball of leaves,
			// and a big crown on top.
			int branches = 2 + (int)(h % 2);
			for (int b = 0; b < branches; b++)
			{
				float angle = (float)((h >> (uint)(4 + b * 5)) % 360) * 0.01745f + (float)b * 2.1f;
				int start = base + t.trunk / 2 + b;
				float bx = (float)t.x;
				float bz = (float)t.z;
				int by = start;
				for (int s = 1; s <= 3; s++)
				{
					bx += math::cos(angle);
					bz += math::sin(angle);
					if (s % 2 == 0) by++;
					put.put(round_i(bx), by, round_i(bz), k.log, false);
				}
				self.leaf_blob(put, k.leaves, round_i(bx), by + 1, round_i(bz), 2.3f);
			}
			self.leaf_blob(put, k.leaves, t.x, base + t.trunk, t.z, 2.8f);
		case ROUND_OAK:
			// A rounded, uneven crown: a main ball at the top of the trunk and
			// 2-3 smaller ones around it, one on a short branch.
			int clumps = 2 + (int)(h % 2);
			for (int c = 0; c < clumps; c++)
			{
				float angle = (float)((h >> (uint)(3 + c * 6)) % 360) * 0.01745f + (float)c * 2.4f;
				float dist = 1.4f + (float)((h >> (uint)(9 + c * 3)) % 3) * 0.4f;
				int cx = t.x + round_i(math::cos(angle) * dist);
				int cz = t.z + round_i(math::sin(angle) * dist);
				int cy = base + t.trunk - 2 + (int)((h >> (uint)(14 + c * 2)) % 3);
				if (c == 0) put.put(t.x + round_i(math::cos(angle)), cy - 1, t.z + round_i(math::sin(angle)), k.log, false);
				self.leaf_blob(put, k.leaves, cx, cy, cz, 1.7f + (float)((h >> (uint)(20 + c)) % 2) * 0.3f);
			}
			self.leaf_blob(put, k.leaves, t.x, base + t.trunk, t.z, 2.4f);
	}
	for (int dy = 0; dy < t.trunk; dy++) put.put(t.x, base + dy, t.z, k.log, false);
}
