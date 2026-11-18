#include "generator.h"
#include "noise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gocube {

namespace {

int64_t column_key(int x, int z) {
	return (int64_t(x) << 32) ^ int64_t(uint32_t(z));
}

int floor_div(int v, int d) {
	return v >= 0 ? v / d : -((-v + d - 1) / d);
}

// Fractal sum of 3D noise, roughly [-1, 1].
float fbm3(float x, float y, float z, uint32_t seed, int octaves) {
	float sum = 0.0f, amp = 1.0f, norm = 0.0f;
	for (int i = 0; i < octaves; i++) {
		sum += noise3(x, y, z, seed + static_cast<uint32_t>(i) * 1013u) * amp;
		norm += amp;
		amp *= 0.5f;
		x *= 2.0f;
		y *= 2.0f;
		z *= 2.0f;
	}
	return sum / norm;
}

float smoothstep01(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

constexpr int NX = CHUNK / Generator::LX + 1, NZ = CHUNK / Generator::LZ + 1; // lattice points across a chunk column

} // namespace

void Generator::forget_cached() {
	std::lock_guard<std::mutex> lock(_columns_mutex);
	_columns.clear();
	_columns_order.clear();
}

void Generator::configure(uint32_t seed, const GeneratorIds &ids, const std::vector<OreKind> &ores,
		const std::vector<TreeKind> &trees) {
	_seed = seed;
	_ids = ids;
	_ores = ores;
	_trees = trees;
	forget_cached();
}

void Generator::set_biomes(const std::vector<BiomeKind> &biomes) {
	_biomes = biomes;
	forget_cached();
}

// With no biome configured: plain grass land, no trees.
const BiomeKind &Generator::biome(int index) const {
	if (!_biomes.empty()) {
		return _biomes[std::clamp(index, 0, int(_biomes.size()) - 1)];
	}
	static thread_local BiomeKind plain;
	plain.surface = _ids.grass;
	plain.filler = _ids.dirt;
	return plain;
}

void Generator::climate(int x, int z, float &temperature, float &humidity) const {
	constexpr float F = 0.0016f, J = 0.045f;
	temperature = fbm2(x * F, z * F, _seed + 311u, 3) * 1.7f + fbm2(x * J, z * J, _seed + 313u, 2) * 0.05f;
	humidity = fbm2(x * F, z * F, _seed + 317u, 3) * 1.7f + fbm2(x * J, z * J, _seed + 319u, 2) * 0.05f;
}

int Generator::biome_at(int x, int z) const {
	if (_biomes.size() < 2) {
		return 0;
	}
	float t, h;
	climate(x, z, t, h);
	int best = 0;
	float best_d = INFINITY;
	for (size_t i = 0; i < _biomes.size(); i++) {
		const float dt = t - _biomes[i].temperature, dh = h - _biomes[i].humidity;
		const float d = dt * dt + dh * dh;
		if (d < best_d) {
			best_d = d;
			best = int(i);
		}
	}
	return best;
}

// Where mountain ranges rise: a slow noise picks the ranges, a ridged one
// shapes their crests and valleys. 0 outside ranges, up to 1 on a crest.
float Generator::mountain_shape(int x, int z) const {
	const float m = fbm2(x * 0.0022f, z * 0.0022f, _seed + 601u, 3);
	const float t = std::clamp((m - 0.02f) / 0.28f, 0.0f, 1.0f);
	const float range = t * t * (3.0f - 2.0f * t);
	if (range <= 0.0f) {
		return 0.0f;
	}
	const float ridge = 1.0f - std::abs(fbm2(x * 0.0095f, z * 0.0095f, _seed + 603u, 4)) * 1.6f;
	const float r = std::clamp(ridge, 0.0f, 1.0f);
	return range * (0.3f + 0.7f * r * r);
}

// The biomes' hill height, base and mountains, blended by closeness in climate space.
void Generator::blend(int x, int z, float &amp, float &base, float &mountains) const {
	if (_biomes.empty()) {
		amp = 20.0f;
		base = mountains = 0.0f;
		return;
	}
	float t, h;
	climate(x, z, t, h);
	float wsum = 0.0f;
	amp = base = mountains = 0.0f;
	for (const BiomeKind &b : _biomes) {
		const float dt = t - b.temperature, dh = h - b.humidity;
		const float e = dt * dt + dh * dh + 0.02f;
		const float w = 1.0f / (e * e * e);
		wsum += w;
		amp += w * b.height;
		base += w * b.base;
		mountains += w * b.mountains;
	}
	amp /= wsum;
	base /= wsum;
	mountains /= wsum;
}

// The density at a lattice point: above 0 is solid. The land's shape from 2D
// noise (continents and seas, hills, the biomes' mountain ranges) gives the
// height it leans to; 3D noise moves it up and down, more where the land is
// rough (a slow noise): there it makes cliffs, overhangs and arches, now and
// then a floating piece; where it's smooth, rolling hills and plains.
float Generator::density(int x, int y, int z) const {
	float amp, base, mountains;
	blend(x, z, amp, base, mountains);
	const float continent = fbm2(x * 0.0032f, z * 0.0032f, _seed + 701u, 4);
	const float hills = fbm2(x * 0.011f, z * 0.011f, _seed + 703u, 4);
	const float rough = smoothstep01((fbm2(x * 0.0055f, z * 0.0055f, _seed + 705u, 3) + 0.1f) / 0.5f);
	float target = float(SEA_LEVEL) - 5.0f + base + continent * 52.0f + hills * amp * (0.7f + 0.9f * rough);
	target += rough * 18.0f; // rough land is highland
	if (mountains > 0.0f) {
		target += mountain_shape(x, z) * mountains * 1.3f;
	}
	const float squash = 8.0f + rough * 26.0f; // blocks for the density to go from 0 to 1 (the 3D part's reach)
	const float n = fbm3(x * 0.017f, y * 0.028f, z * 0.017f, _seed + 707u, 4);
	float d = (target - float(y)) / squash + n * (0.3f + 0.9f * rough);
	if (y > TOP_LIMIT - 20) {
		d -= float(y - (TOP_LIMIT - 20)) / 10.0f; // nothing reaches the top
	}
	if (y < BOTTOM_Y + 24) {
		d += float(BOTTOM_Y + 24 - y) / 6.0f; // solid at the bottom
	}
	return d;
}

std::shared_ptr<const Generator::Columns> Generator::columns(int ox, int oz) const {
	const int64_t k = column_key(ox, oz);
	{
		std::lock_guard<std::mutex> lock(_columns_mutex);
		auto it = _columns.find(k);
		if (it != _columns.end()) {
			return it->second;
		}
	}
	auto c = std::make_shared<Columns>();
	fill_columns(*c, ox, oz);
	std::lock_guard<std::mutex> lock(_columns_mutex);
	if (_columns.emplace(k, c).second) {
		_columns_order.push_back(k);
		if (_columns_order.size() > COLUMN_CACHE) {
			_columns.erase(_columns_order.front());
			_columns_order.pop_front();
		}
	}
	return c;
}

// A chunk column's lattice (the whole height), and from it each column's
// topmost solid block and biome.
void Generator::fill_columns(Columns &c, int ox, int oz) const {
	c.density.assign(size_t(NX) * NZ * LY_COUNT, 0.0f);
	for (int j = 0; j < LY_COUNT; j++) {
		for (int k = 0; k < NZ; k++) {
			for (int i = 0; i < NX; i++) {
				c.density[(size_t(j) * NZ + k) * NX + i] = density(ox + i * LX, BOTTOM_Y + j * LY, oz + k * LZ);
			}
		}
	}
	const int top = BOTTOM_Y + (LY_COUNT - 1) * LY;
	c.max_h = -(1 << 30);
	for (int z = 0; z < CHUNK; z++) {
		for (int x = 0; x < CHUNK; x++) {
			const int i = z * CHUNK + x;
			const int ix = x / LX, iz = z / LZ;
			const float fx = float(x % LX) / LX, fz = float(z % LZ) / LZ;
			auto level = [&](int j) { // the density at this column, at lattice level j
				const float *d = &c.density[(size_t(j) * NZ + iz) * NX + ix];
				const float a = d[0] + (d[1] - d[0]) * fx, b = d[NX] + (d[NX + 1] - d[NX]) * fx;
				return a + (b - a) * fz;
			};
			int h = BOTTOM_Y;
			float above = level(LY_COUNT - 1);
			for (int j = LY_COUNT - 2; j >= 0 && h == BOTTOM_Y; j--) {
				const float below = level(j);
				if (below > 0.0f || above > 0.0f) {
					for (int dy = LY - 1; dy >= 0; dy--) {
						if (below + (above - below) * float(dy) / LY > 0.0f) {
							h = BOTTOM_Y + j * LY + dy;
							break;
						}
					}
				}
				above = below;
			}
			h = std::min(h, top);
			c.heights[i] = h;
			c.biomes[i] = biome_at(ox + x, oz + z);
			c.max_h = std::max(c.max_h, h);
		}
	}
}

int Generator::height_at(int x, int z) const {
	const int ox = floor_div(x, CHUNK) * CHUNK, oz = floor_div(z, CHUNK) * CHUNK;
	return columns(ox, oz)->heights[(z - oz) * CHUNK + (x - ox)];
}

BlockId Generator::top_block(int x, int h, int z, const BiomeKind &b, int &filler_depth, BlockId &filler) const {
	const float sandy = noise2(x * 0.055f, z * 0.055f, _seed + 711u);
	const float stony = noise2(x * 0.048f, z * 0.048f, _seed + 713u);
	const float depth = noise2(x * 0.08f, z * 0.08f, _seed + 715u) + 0.5f * noise2(x * 0.23f, z * 0.23f, _seed + 717u);
	filler_depth = std::clamp(int(std::floor(3.6f + depth * 3.0f)), 0, 5);
	filler = b.filler;
	const BlockId sand = _ids.sand ? _ids.sand : b.filler, gravel = _ids.gravel ? _ids.gravel : sand;
	if (h < SEA_LEVEL - 1) { // the sea floor
		filler_depth = std::max(filler_depth, 2);
		filler = stony > 0.25f ? gravel : sandy > -0.25f ? sand : b.filler;
		return filler;
	}
	if (h <= SEA_LEVEL + 2) { // the shore: beaches of sand, some of gravel
		if (stony > 0.32f) {
			filler = gravel;
			filler_depth = std::max(filler_depth, 2);
			return gravel;
		}
		if (sandy > -0.35f) {
			filler = sand;
			filler_depth = std::max(filler_depth, 3);
			return sand;
		}
	}
	if (filler_depth == 0) {
		filler = _ids.stone;
		return _ids.stone; // the dirt ran out: bare stone
	}
	const int jitter = int(hash_point(x, 7, z, _seed + 77u) % 4u); // (a ragged line, not a level one)
	if (b.cap && h >= b.cap_above + jitter) {
		return b.cap;
	}
	if (b.rock && h >= b.rock_above + jitter) {
		filler = b.rock;
		return b.rock;
	}
	return b.surface;
}

bool Generator::is_cave(int x, int y, int z) const {
	Noise3Cell cells[3];
	return is_cave(x, y, z, cells);
}

// Called for every block underground, nearby ones one after another (see
// generate): each noise keeps its lattice cell's hashes.
bool Generator::is_cave(int x, int y, int z, Noise3Cell *cells) const {
	Noise3Cell &cell_a = cells[0], &cell_b = cells[1], &cell_c = cells[2];
	const float s = 1.0f / CAVE_PERIOD;
	const float ys = y * CAVE_Y_SQUASH * s;
	const float a = noise3(x * s, ys, z * s, _seed + 101u, cell_a);
	// (|a| alone too wide already: no tunnel, whatever b is)
	if (std::fabs(a) < CAVE_WIDTH && std::fabs(a) + std::fabs(noise3(x * s, ys, z * s, _seed + 202u, cell_b)) < CAVE_WIDTH) {
		return true;
	}
	// Big caverns deep down.
	if (!_biomes.empty() && y < CAVERN_MAX_Y) {
		const float c = 1.0f / CAVERN_PERIOD;
		const float n = noise3(x * c, y * 1.5f * c, z * c, _seed + 404u, cell_c);
		const float fade = std::clamp((CAVERN_MAX_Y - y) / 14.0f, 0.0f, 1.0f);
		return n * fade > 0.36f;
	}
	return false;
}

void Generator::trees_near(int x0, int z0, int x1, int z1, std::vector<Tree> &out) const {
	if (_biomes.empty()) {
		return;
	}
	auto cell_floor = [](int v) { return floor_div(v, TREE_CELL); };
	for (int cz = cell_floor(z0 - TREE_REACH); cz <= cell_floor(z1 + TREE_REACH); cz++) {
		for (int cx = cell_floor(x0 - TREE_REACH); cx <= cell_floor(x1 + TREE_REACH); cx++) {
			const uint32_t r = hash_point(cx, 7, cz, _seed + 77u);
			const int tx = cx * TREE_CELL + 1 + static_cast<int>((r >> 8) % (TREE_CELL - 2));
			const int tz = cz * TREE_CELL + 1 + static_cast<int>((r >> 12) % (TREE_CELL - 2));
			// Pick a tree kind by the biome's chances; most cells get none.
			const BiomeKind &tb = biome(biome_at(tx, tz));
			int roll = static_cast<int>(r % 100u), kind = -1;
			for (const auto &bt : tb.trees) {
				if (roll < bt.second) {
					kind = bt.first;
					break;
				}
				roll -= bt.second;
			}
			if (kind < 0 || kind >= int(_trees.size())) {
				continue;
			}
			const int ground = height_at(tx, tz);
			if (ground <= SEA_LEVEL + 1 || is_cave(tx, ground, tz)) {
				continue; // beach, sea or a cave mouth
			}
			int depth;
			BlockId filler;
			if (top_block(tx, ground, tz, tb, depth, filler) != tb.surface) {
				continue; // a beach, gravel, bare stone
			}
			const TreeKind &tk = _trees[kind];
			const int span = std::max(1, tk.max_trunk - tk.min_trunk + 1);
			out.push_back(Tree{ tx, tz, ground, tk.min_trunk + static_cast<int>((r >> 16) % uint32_t(span)), kind });
		}
	}
}

void Generator::generate(ChunkData &chunk, int ox, int oy, int oz) const {
	chunk = ChunkData();
	const std::shared_ptr<const Columns> shared = columns(ox, oz); // (with the chunks above and below)
	const Columns &cols = *shared;
	constexpr int TREE_ROOM = 18;
	const int top_y = oy + CHUNK - 1;
	if (oy > std::max(cols.max_h, SEA_LEVEL) + TREE_ROOM) {
		return; // all air
	}
	const bool terrain = oy <= std::max(cols.max_h, SEA_LEVEL) + 1;
	Noise3Cell cave_cells[3];
	// From a little above the chunk (the filler under a surface just above
	// it) to one below (snow on a surface just under it).
	const int scan_top = std::min(top_y + 8, BOTTOM_Y + (LY_COUNT - 1) * LY - 1);
	for (int z = 0; z < CHUNK && terrain; z++) {
		for (int x = 0; x < CHUNK; x++) {
			const int i = z * CHUNK + x;
			const int wx = ox + x, wz = oz + z;
			const BiomeKind &bm = biome(cols.biomes[i]);
			const int ix = x / LX, iz = z / LZ;
			const float fx = float(x % LX) / LX, fz = float(z % LZ) / LZ;
			auto at = [&](int j) {
				const float *d = &cols.density[(size_t(j) * NZ + iz) * NX + ix];
				const float a = d[0] + (d[1] - d[0]) * fx, b = d[NX] + (d[NX + 1] - d[NX]) * fx;
				return a + (b - a) * fz;
			};
			auto solid_at = [&](int y) {
				const int j = std::clamp(floor_div(y - BOTTOM_Y, LY), 0, LY_COUNT - 2);
				const float fy = float(y - (BOTTOM_Y + j * LY)) / LY;
				const float lo = at(j);
				return lo + (at(j + 1) - lo) * fy > 0.0f;
			};
			const int land = cols.heights[i]; // the column's topmost solid block
			const bool caves_allowed = land > SEA_LEVEL + 2;
			int filler_depth = 0;
			BlockId filler = AIR;
			// Blocks since the last surface, going down (-1: in air or water).
			// Solid just above the scan: say deep in the ground (stone).
			int run = scan_top + 1 <= land && solid_at(scan_top + 1) ? 1 << 20 : -1;
			for (int wy = scan_top; wy >= oy - 1; wy--) {
				const bool solid = wy <= land && solid_at(wy);
				BlockId b;
				if (!solid) {
					run = -1;
					b = wy <= SEA_LEVEL ? _ids.water : AIR;
					if (b == _ids.water && wy == SEA_LEVEL && bm.frozen && _ids.ice) {
						b = _ids.ice;
					}
				} else {
					run = run < (1 << 20) ? run + 1 : run;
					if (run == 0) {
						b = top_block(wx, wy, wz, bm, filler_depth, filler);
						// A thin cover (snow) on high ground, on the block above.
						if (bm.cover && b == bm.surface && wy + 1 >= oy && wy + 1 <= top_y &&
								wy >= bm.cover_above + int(hash_point(wx, 9, wz, _seed + 79u) % 3u) &&
								chunk.get(x, wy + 1 - oy, z) == AIR) {
							chunk.set(x, wy + 1 - oy, z, bm.cover);
						}
					} else if (run <= filler_depth) {
						b = filler;
					} else {
						b = _ids.stone;
					}
					// Caves: on land, or deep under the sea.
					if (wy >= oy && wy > CAVE_MIN_Y && (caves_allowed || wy < land - 8) && is_cave(wx, wy, wz, cave_cells)) {
						b = wy <= LAVA_LEVEL && _ids.lava ? _ids.lava : AIR;
					}
				}
				if (wy >= oy && wy <= top_y && b != AIR) {
					chunk.set(x, wy - oy, z, b);
				}
			}
		}
	}
	place_ores(chunk, ox, oy, oz);
	if (top_y >= SEA_LEVEL + 2) {
		std::vector<Tree> trees;
		trees_near(ox, oz, ox + CHUNK, oz + CHUNK, trees);
		for (const Tree &t : trees) {
			place_tree(chunk, ox, oy, oz, t);
		}
	}
	chunk.compact();
}

// Veins start in some chunk and wander up to ~max_size blocks, so they can reach
// into neighbours: each chunk replays the veins of its 3x3x3 neighbourhood and
// keeps the blocks that land inside it. Only stone is replaced.
void Generator::place_ores(ChunkData &chunk, int ox, int oy, int oz) const {
	if (_ores.empty() || (chunk.uniform && chunk.uniform_value == AIR)) {
		return;
	}
	static const int STEPS[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	const int ccx = ox / CHUNK, ccy = oy / CHUNK, ccz = oz / CHUNK; // origins are multiples of CHUNK
	for (int dy = -1; dy <= 1; dy++) {
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				const int sx = (ccx + dx) * CHUNK, sy = (ccy + dy) * CHUNK, sz = (ccz + dz) * CHUNK;
				for (size_t k = 0; k < _ores.size(); k++) {
					const OreKind &ore = _ores[k];
					if (sy > ore.max_y || sy + CHUNK <= ore.min_y) {
						continue;
					}
					for (int v = 0; v < ore.veins_per_chunk; v++) {
						uint32_t h = hash_point(ccx + dx, (ccy + dy) * 64 + v, ccz + dz, _seed + ore.salt);
						int x = sx + int(h % CHUNK), y = sy + int((h >> 5) % CHUNK), z = sz + int((h >> 10) % CHUNK);
						if (y < ore.min_y || y > ore.max_y) {
							continue;
						}
						const int size = ore.min_size + int((h >> 15) % uint32_t(ore.max_size - ore.min_size + 1));
						for (int s = 0; s < size; s++) {
							const int lx = x - ox, ly = y - oy, lz = z - oz;
							if (lx >= 0 && ly >= 0 && lz >= 0 && lx < CHUNK && ly < CHUNK && lz < CHUNK &&
									chunk.get(lx, ly, lz) == ore.replaces) {
								chunk.set(lx, ly, lz, ore.id);
							}
							h = hash_point(x, y, z, h);
							const int *step = STEPS[h % 6];
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

void Generator::place_tree(ChunkData &chunk, int ox, int oy, int oz, const Tree &t) const {
	build_tree(t, [&](int wx, int wy, int wz, BlockId id, bool only_air) {
		const int x = wx - ox, y = wy - oy, z = wz - oz;
		if (id == AIR || x < 0 || y < 0 || z < 0 || x >= CHUNK || y >= CHUNK || z >= CHUNK) {
			return;
		}
		if (only_air && chunk.get(x, y, z) != AIR) {
			return;
		}
		chunk.set(x, y, z, id);
	});
}

void Generator::tree_blocks(int kind, int x, int ground, int z, std::vector<TreeBlock> &out) const {
	if (kind < 0 || kind >= int(_trees.size())) {
		return;
	}
	const TreeKind &tk = _trees[kind];
	const uint32_t r = hash_point(x, ground, z, _seed + 67u);
	const int span = std::max(1, tk.max_trunk - tk.min_trunk + 1);
	const Tree t{ x, z, ground, tk.min_trunk + int(r % uint32_t(span)), kind };
	build_tree(t, [&](int wx, int wy, int wz, BlockId id, bool only_air) {
		if (id != AIR) {
			out.push_back(TreeBlock{ wx, wy, wz, id, only_air });
		}
	});
}

template <typename Put>
void Generator::build_tree(const Tree &t, Put &&put) const {
	const TreeKind &k = _trees[t.kind];
	const int base = t.ground + 1;
	auto leaf = [&](int x, int y, int z) { put(x, y, z, k.leaves, true); };
	// A rough ball of leaves (a little ragged, from the position hash).
	auto blob = [&](int cx, int cy, int cz, float r) {
		const int ri = int(std::ceil(r));
		for (int dy = -ri; dy <= ri; dy++) {
			for (int dz = -ri; dz <= ri; dz++) {
				for (int dx = -ri; dx <= ri; dx++) {
					const float d = float(dx * dx + dz * dz) + dy * dy * 1.5f;
					const float jitter = (hash_point(cx + dx, cy + dy, cz + dz, _seed + 61u) % 100u) / 100.0f;
					if (d <= r * r - jitter * r) {
						leaf(cx + dx, cy + dy, cz + dz);
					}
				}
			}
		}
	};
	const uint32_t h = hash_point(t.x, t.ground, t.z, _seed + 63u);
	switch (k.style) {
		case TREE_CACTUS: {
			for (int dy = 0; dy < t.trunk; dy++) {
				put(t.x, base + dy, t.z, k.log, true);
			}
			return;
		}
		case TREE_BIG_OAK: {
			// A tall trunk with 2-3 branches, each ending in a ball of leaves,
			// and a big crown on top.
			const int branches = 2 + int(h % 2u);
			for (int b = 0; b < branches; b++) {
				const float angle = (h >> (4 + b * 5)) % 360u * 0.01745f + b * 2.1f;
				const int start = base + t.trunk / 2 + b;
				float bx = float(t.x), bz = float(t.z);
				int by = start;
				for (int s = 1; s <= 3; s++) {
					bx += std::cos(angle);
					bz += std::sin(angle);
					if (s % 2 == 0) {
						by++;
					}
					put(int(std::round(bx)), by, int(std::round(bz)), k.log, false);
				}
				blob(int(std::round(bx)), by + 1, int(std::round(bz)), 2.3f);
			}
			blob(t.x, base + t.trunk, t.z, 2.8f);
			break;
		}
		default: {
			// A rounded, uneven crown: a main ball at the top of the trunk and
			// 2-3 smaller ones around it, one on a short branch.
			const int clumps = 2 + int(h % 2u);
			for (int c = 0; c < clumps; c++) {
				const float angle = ((h >> (3 + c * 6)) % 360u) * 0.01745f + c * 2.4f;
				const float dist = 1.4f + ((h >> (9 + c * 3)) % 3u) * 0.4f;
				const int cx = t.x + int(std::round(std::cos(angle) * dist));
				const int cz = t.z + int(std::round(std::sin(angle) * dist));
				const int cy = base + t.trunk - 2 + int((h >> (14 + c * 2)) % 3u);
				if (c == 0) {
					put(t.x + int(std::round(std::cos(angle))), cy - 1, t.z + int(std::round(std::sin(angle))), k.log, false);
				}
				blob(cx, cy, cz, 1.7f + ((h >> (20 + c)) % 2u) * 0.3f);
			}
			blob(t.x, base + t.trunk, t.z, 2.4f);
			break;
		}
	}
	for (int dy = 0; dy < t.trunk; dy++) {
		put(t.x, base + dy, t.z, k.log, false);
	}
}

} // namespace gocube
