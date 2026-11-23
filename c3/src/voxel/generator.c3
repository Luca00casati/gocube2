// World generation: terrain from a 3D density field (hills, cliffs,
// overhangs, now and then a floating bit), seas with sand and gravel beaches
// and sea floors, bare stone where the dirt runs out, biomes, caves, lava deep
// down, ore veins and trees. Thread-safe once configured; the same seed
// always gives the same world, block for block the same as the C++ game's
// (c3/test/voxel/generator_test.c3 checks it).
module gocube::voxel;
import std::math, gocube::util::vec, gocube::util::map, std::thread, std::atomic;

// Terrain blocks the base shape is made of.
struct GeneratorIds
{
	BlockId stone, dirt, grass, sand, water, lava, ice, gravel;
}

// One kind of ore (defined by mods): veins are random walks through `replaces`.
struct OreKind
{
	BlockId id;
	BlockId replaces;     // usually stone
	int min_y, max_y;     // vein starting points lie in this range
	int veins_per_chunk;  // attempts per 32^3 chunk
	int min_size, max_size; // blocks per vein (before clipping)
	uint salt;            // from the ore's id, so adding mods doesn't move veins
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
}

// A biome's chance of a tree kind (percent of tree cells).
struct TreeChance
{
	int kind;
	int percent;
}

// One biome (defined by mods): where it is in climate space, its ground and
// what grows there. The nearest biome to a column's climate wins; the land's
// height is blended between biomes so borders don't make cliffs.
struct BiomeKind
{
	float temperature, humidity; // roughly -1..1
	float height;                // hill height (blocks)
	float base;                  // raises or lowers the land
	BlockId surface, filler;     // top block and the ones under it
	Vec{TreeChance} trees;
	bool frozen;                 // ice on water
	float mountains;             // height of mountain ranges on top of the hills (0: none)
	BlockId rock;                // bare top (and filler) from rock_above up
	int rock_above;
	BlockId cap;                 // top of the highest peaks, from cap_above up
	int cap_above;
	BlockId cover;               // a thin layer on top of the ground (snow), from cover_above up
	int cover_above;
}

// A biome with the defaults (no rock, cap or cover).
fn BiomeKind biome_kind()
{
	return { .height = 20.0f, .rock_above = 1 << 20, .cap_above = 1 << 20, .cover_above = 1 << 20 };
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
// The density lattice: a value every LX, LY, LZ blocks (interpolated
// between), from BOTTOM_Y up LY_COUNT points; solid where above 0.
const int LX = 4;
const int LY = 8;
const int LZ = 4;
const int BOTTOM_Y = -96;
const int LY_COUNT = 30;
const int TOP_LIMIT = 100;       // terrain stays under it (room for trees below the world's top, 127)

const int NX @private = CHUNK / LX + 1; // lattice points across a chunk column
const int NZ @private = CHUNK / LZ + 1;
const int COLUMN_CACHE @private = 256;

// A chunk column: the density at the lattice points over its 32 x 32
// columns (the whole height of the world), each column's topmost solid
// block and biome. Shared by the chunks stacked in it (the latest
// COLUMN_CACHE kept), counted: the last to let go frees it.
struct Columns @private
{
	Atomic{int} refs;
	int[CHUNK_AREA] heights;
	int[CHUNK_AREA] biomes;
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
	Vec{OreKind} ores;
	Vec{TreeKind} trees;
	Vec{BiomeKind} biomes;
	BiomeKind plain;          // with no biome configured: plain grass land, no trees
	Mutex columns_mutex;
	bool has_mutex;
	Map{long, Columns*} columns;
	Vec{long} columns_order; // oldest first
}

fn void Generator.configure(&self, uint seed, GeneratorIds ids, OreKind[] ores, TreeKind[] trees)
{
	if (!self.has_mutex)
	{
		self.columns_mutex.init()!!;
		self.has_mutex = true;
	}
	self.seed = seed;
	self.ids = ids;
	self.ores.clear();
	self.ores.push_all(ores);
	self.trees.clear();
	self.trees.push_all(trees);
	self.plain = biome_kind();
	self.plain.surface = ids.grass;
	self.plain.filler = ids.dirt;
	self.forget_cached();
}

// Copies the biomes (and their tree chances).
fn void Generator.set_biomes(&self, BiomeKind[] biomes)
{
	self.free_biomes();
	foreach (b : biomes)
	{
		BiomeKind copy = b;
		copy.trees = {};
		copy.trees.push_all(b.trees.array_view());
		self.biomes.push(copy);
	}
	self.forget_cached();
}

fn void Generator.free_biomes(&self)
{
	foreach (&b : self.biomes) b.trees.free();
	self.biomes.clear();
}

fn void Generator.free(&self)
{
	self.forget_cached();
	self.free_biomes();
	self.biomes.free();
	self.ores.free();
	self.trees.free();
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

fn BiomeKind* Generator.biome(&self, int index)
{
	if (self.biomes.is_empty()) return &self.plain;
	return self.biomes.get_ref(math::clamp(index, 0, (int)self.biomes.len() - 1));
}

fn void Generator.climate(&self, int x, int z, float* temperature, float* humidity)
{
	const float F = 0.0016f;
	const float J = 0.045f;
	float fx = (float)x;
	float fz = (float)z;
	*temperature = fbm2(fx * F, fz * F, self.seed + 311, 3) * 1.7f + fbm2(fx * J, fz * J, self.seed + 313, 2) * 0.05f;
	*humidity = fbm2(fx * F, fz * F, self.seed + 317, 3) * 1.7f + fbm2(fx * J, fz * J, self.seed + 319, 2) * 0.05f;
}

// The biome of a column (index into the biomes; 0 if none are configured).
fn int Generator.biome_at(&self, int x, int z)
{
	if (self.biomes.len() < 2) return 0;
	float t;
	float h;
	self.climate(x, z, &t, &h);
	int best = 0;
	float best_d = float::inf;
	foreach (i, &b : self.biomes)
	{
		float dt = t - b.temperature;
		float dh = h - b.humidity;
		float d = dt * dt + dh * dh;
		if (d < best_d)
		{
			best_d = d;
			best = (int)i;
		}
	}
	return best;
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

// The biomes' hill height, base and mountains, blended by closeness in climate space.
fn void Generator.blend(&self, int x, int z, float* amp, float* base, float* mountains)
{
	if (self.biomes.is_empty())
	{
		*amp = 20.0f;
		*base = 0.0f;
		*mountains = 0.0f;
		return;
	}
	float t;
	float h;
	self.climate(x, z, &t, &h);
	float wsum = 0.0f;
	float a = 0.0f;
	float bs = 0.0f;
	float m = 0.0f;
	foreach (&b : self.biomes)
	{
		float dt = t - b.temperature;
		float dh = h - b.humidity;
		float e = dt * dt + dh * dh + 0.02f;
		float w = 1.0f / (e * e * e);
		wsum += w;
		a += w * b.height;
		bs += w * b.base;
		m += w * b.mountains;
	}
	*amp = a / wsum;
	*base = bs / wsum;
	*mountains = m / wsum;
}

// The density at a lattice point: above 0 is solid. The land's shape from 2D
// noise (continents and seas, hills, the biomes' mountain ranges) gives the
// height it leans to; 3D noise moves it up and down, more where the land is
// rough (a slow noise): there it makes cliffs, overhangs and arches, now and
// then a floating piece; where it's smooth, rolling hills and plains.
fn float Generator.density(&self, int x, int y, int z)
{
	float amp;
	float base;
	float mountains;
	self.blend(x, z, &amp, &base, &mountains);
	float fx = (float)x;
	float fz = (float)z;
	float continent = fbm2(fx * 0.0032f, fz * 0.0032f, self.seed + 701, 4);
	float hills = fbm2(fx * 0.011f, fz * 0.011f, self.seed + 703, 4);
	float rough = smoothstep01((fbm2(fx * 0.0055f, fz * 0.0055f, self.seed + 705, 3) + 0.1f) / 0.5f);
	float target = (float)SEA_LEVEL - 5.0f + base + continent * 52.0f + hills * amp * (0.7f + 0.9f * rough);
	target += rough * 18.0f; // rough land is highland
	if (mountains > 0.0f) target += self.mountain_shape(x, z) * mountains * 1.3f;
	float squash = 8.0f + rough * 26.0f; // blocks for the density to go from 0 to 1 (the 3D part's reach)
	float n = fbm3(fx * 0.017f, (float)y * 0.028f, fz * 0.017f, self.seed + 707, 4);
	float d = (target - (float)y) / squash + n * (0.3f + 0.9f * rough);
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
// topmost solid block and biome.
fn void Generator.fill_columns(&self, Columns* c, int ox, int oz)
{
	for (int j = 0; j < LY_COUNT; j++)
	{
		for (int k = 0; k < NZ; k++)
		{
			for (int i = 0; i < NX; i++)
			{
				c.density[(j * NZ + k) * NX + i] = self.density(ox + i * LX, BOTTOM_Y + j * LY, oz + k * LZ);
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
			c.biomes[i] = self.biome_at(ox + x, oz + z);
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
// (beaches, sea floor, bare stone, the biome's surface), and the filler
// under it and how deep it goes (0: stone right under).
fn BlockId Generator.top_block(&self, int x, int h, int z, BiomeKind* b, int* filler_depth, BlockId* filler)
{
	float fx = (float)x;
	float fz = (float)z;
	float sandy = noise2(fx * 0.055f, fz * 0.055f, self.seed + 711);
	float stony = noise2(fx * 0.048f, fz * 0.048f, self.seed + 713);
	float depth = noise2(fx * 0.08f, fz * 0.08f, self.seed + 715) + 0.5f * noise2(fx * 0.23f, fz * 0.23f, self.seed + 717);
	*filler_depth = math::clamp((int)math::floor(3.6f + depth * 3.0f), 0, 5);
	*filler = b.filler;
	BlockId sand = self.ids.sand ? self.ids.sand : b.filler;
	BlockId gravel = self.ids.gravel ? self.ids.gravel : sand;
	if (h < SEA_LEVEL - 1) // the sea floor
	{
		*filler_depth = math::max(*filler_depth, 2);
		*filler = stony > 0.25f ? gravel : sandy > -0.25f ? sand : b.filler;
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
	if (b.cap && h >= b.cap_above + jitter) return b.cap;
	if (b.rock && h >= b.rock_above + jitter)
	{
		*filler = b.rock;
		return b.rock;
	}
	return b.surface;
}

fn bool Generator.is_cave(&self, int x, int y, int z)
{
	Noise3Cell[3] cells;
	return self.is_cave_cached(x, y, z, &cells);
}

// The same, keeping each noise's lattice cell in `cells` between calls
// (called for every block underground, nearby ones one after another).
fn bool Generator.is_cave_cached(&self, int x, int y, int z, Noise3Cell[3]* cells)
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
	if (!self.biomes.is_empty() && y < CAVERN_MAX_Y)
	{
		float c = 1.0f / CAVERN_PERIOD;
		float n = noise3_cached(fx * c, (float)y * 1.5f * c, fz * c, self.seed + 404, &(*cells)[2]);
		float fade = math::clamp((float)(CAVERN_MAX_Y - y) / 14.0f, 0.0f, 1.0f);
		return n * fade > 0.36f;
	}
	return false;
}

// Trees whose leaves may reach columns [x0, x1) x [z0, z1).
fn void Generator.trees_near(&self, int x0, int z0, int x1, int z1, TreeList* out)
{
	if (self.biomes.is_empty()) return;
	for (int cz = floor_div(z0 - TREE_REACH, TREE_CELL); cz <= floor_div(z1 + TREE_REACH, TREE_CELL); cz++)
	{
		for (int cx = floor_div(x0 - TREE_REACH, TREE_CELL); cx <= floor_div(x1 + TREE_REACH, TREE_CELL); cx++)
		{
			uint r = hash_point(cx, 7, cz, self.seed + 77);
			int tx = cx * TREE_CELL + 1 + (int)((r >> 8) % (uint)(TREE_CELL - 2));
			int tz = cz * TREE_CELL + 1 + (int)((r >> 12) % (uint)(TREE_CELL - 2));
			// Pick a tree kind by the biome's chances; most cells get none.
			BiomeKind* tb = self.biome(self.biome_at(tx, tz));
			int roll = (int)(r % 100);
			int kind = -1;
			foreach (bt : tb.trees)
			{
				if (roll < bt.percent)
				{
					kind = bt.kind;
					break;
				}
				roll -= bt.percent;
			}
			if (kind < 0 || kind >= (int)self.trees.len()) continue;
			int ground = self.height_at(tx, tz);
			if (ground <= SEA_LEVEL + 1 || self.is_cave(tx, ground, tz)) continue; // beach, sea or a cave mouth
			int depth;
			BlockId filler;
			if (self.top_block(tx, ground, tz, tb, &depth, &filler) != tb.surface) continue; // a beach, gravel, bare stone
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
	Noise3Cell[3] cave_cells;
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
			BiomeKind* bm = self.biome(cols.biomes[i]);
			int ix = x / LX;
			int iz = z / LZ;
			float fx = (float)(x % LX) / (float)LX;
			float fz = (float)(z % LZ) / (float)LZ;
			int land = cols.heights[i]; // the column's topmost solid block
			bool caves_allowed = land > SEA_LEVEL + 2;
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
					if (b == self.ids.water && wy == SEA_LEVEL && bm.frozen && self.ids.ice) b = self.ids.ice;
				}
				else
				{
					run = run < (1 << 20) ? run + 1 : run;
					if (run == 0)
					{
						b = self.top_block(wx, wy, wz, bm, &filler_depth, &filler);
						// A thin cover (snow) on high ground, on the block above.
						if (bm.cover && b == bm.surface && wy + 1 >= oy && wy + 1 <= top_y &&
							wy >= bm.cover_above + (int)(hash_point(wx, 9, wz, self.seed + 79) % 3) &&
							chunk.get(x, wy + 1 - oy, z) == AIR)
						{
							chunk.set(x, wy + 1 - oy, z, bm.cover);
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
					if (wy >= oy && wy > CAVE_MIN_Y && (caves_allowed || wy < land - 8) && self.is_cave_cached(wx, wy, wz, &cave_cells))
					{
						b = wy <= LAVA_LEVEL && self.ids.lava ? self.ids.lava : AIR;
					}
				}
				if (wy >= oy && wy <= top_y && b != AIR) chunk.set(x, wy - oy, z, b);
			}
		}
	}
	self.place_ores(chunk, ox, oy, oz);
	if (top_y >= SEA_LEVEL + 2)
	{
		TreeList trees;
		defer trees.free();
		self.trees_near(ox, oz, ox + CHUNK, oz + CHUNK, &trees);
		TreeSink sink = { .chunk = chunk, .ox = ox, .oy = oy, .oz = oz };
		foreach (t : trees) self.build_tree(t, &sink);
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

const int[3][6] ORE_STEPS @private = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

// Veins start in some chunk and wander up to ~max_size blocks, so they can reach
// into neighbours: each chunk replays the veins of its 3x3x3 neighbourhood and
// keeps the blocks that land inside it. Only stone is replaced.
fn void Generator.place_ores(&self, ChunkData* chunk, int ox, int oy, int oz)
{
	if (self.ores.is_empty() || (!chunk.mixed && chunk.uniform_value == AIR)) return;
	int ccx = ox / CHUNK; // (origins are multiples of CHUNK)
	int ccy = oy / CHUNK;
	int ccz = oz / CHUNK;
	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++)
			{
				int sx = (ccx + dx) * CHUNK;
				int sy = (ccy + dy) * CHUNK;
				int oz2 = (ccz + dz) * CHUNK;
				foreach (&ore : self.ores)
				{
					if (sy > ore.max_y || sy + CHUNK <= ore.min_y) continue;
					for (int v = 0; v < ore.veins_per_chunk; v++)
					{
						uint h = hash_point(ccx + dx, (ccy + dy) * 64 + v, ccz + dz, self.seed + ore.salt);
						int x = sx + (int)(h % CHUNK);
						int y = sy + (int)((h >> 5) % CHUNK);
						int z = oz2 + (int)((h >> 10) % CHUNK);
						if (y < ore.min_y || y > ore.max_y) continue;
						int size = ore.min_size + (int)((h >> 15) % (uint)(ore.max_size - ore.min_size + 1));
						for (int s = 0; s < size; s++)
						{
							int lx = x - ox;
							int ly = y - oy;
							int lz = z - oz;
							if (lx >= 0 && ly >= 0 && lz >= 0 && lx < CHUNK && ly < CHUNK && lz < CHUNK &&
								chunk.get(lx, ly, lz) == ore.replaces)
							{
								chunk.set(lx, ly, lz, ore.id);
							}
							h = hash_point(x, y, z, h);
							int[3] step = ORE_STEPS[h % 6];
							x += step[0];
							y += step[1];
							z += step[2];
						}
					}
				}
			}
		}
	}
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
