// Worlds generate the same every time: unedited chunks are generated again
// each time a world is opened, so a change in what comes out shifts caves,
// ores and light in worlds already played. The fingerprint (block data and
// light around the origin) catches any change; when one is meant, record the
// new value. And what the land is like (seas, beaches, overhangs, trees).

#include "content/content.h"
#include "helpers.h"
#include "voxel/generator.h"
#include "voxel/light.h"
#include "voxel/storage.h"

#include <doctest/doctest.h>

#include <windows.h>
#include <bcrypt.h>

#include <map>
#include <memory>
#include <string>
#include <tuple>

using namespace gocube;

namespace {

constexpr const char *WORLD_FINGERPRINT = "bd803aed75d4246e"; // seed 777
constexpr int CHUNK_Y_MIN = -3, CHUNK_Y_MAX = 3;                    // the world's height

struct Sha256 {
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	Sha256() {
		BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
	}
	~Sha256() {
		BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	void update(const void *data, size_t size) {
		if (size) {
			BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size), 0);
		}
	}
	std::string hex16() {
		unsigned char out[32];
		BCryptFinishHash(hash, out, sizeof(out), 0);
		static const char *digits = "0123456789abcdef";
		std::string s;
		for (int i = 0; i < 8; i++) {
			s += digits[out[i] >> 4];
			s += digits[out[i] & 15];
		}
		return s;
	}
};

int floor_div(int v, int d) {
	return v >= 0 ? v / d : -((-v + d - 1) / d);
}

// The chunks around the origin, generated and lit the way the world does it
// (whole columns for sky light, a full neighbourhood per chunk).
struct TestWorld {
	const std::vector<BlockInfo> &blocks;
	const Generator &generator;
	std::map<std::tuple<int, int, int>, std::shared_ptr<ChunkData>> chunks;
	std::map<std::pair<int, int>, std::shared_ptr<ColumnTops>> tops;
	std::map<std::tuple<int, int, int>, std::vector<uint8_t>> light; // sky << 4 | block, per block

	const ChunkData *chunk(int cx, int cy, int cz) {
		if (cy < CHUNK_Y_MIN || cy > CHUNK_Y_MAX) {
			return nullptr; // outside the world: air above, solid below
		}
		auto &c = chunks[{cx, cy, cz}];
		if (!c) {
			c = std::make_shared<ChunkData>();
			generator.generate(*c, cx * CHUNK, cy * CHUNK, cz * CHUNK);
		}
		return c.get();
	}

	const ColumnTops *column_tops(int cx, int cz) {
		auto &t = tops[{cx, cz}];
		if (!t) {
			t = std::make_shared<ColumnTops>();
			t->fill(NO_TOP);
			for (int cy = CHUNK_Y_MAX; cy >= CHUNK_Y_MIN; cy--) {
				std::array<int8_t, CHUNK_AREA> local;
				chunk_local_tops(*chunk(cx, cy, cz), blocks, local);
				for (int i = 0; i < CHUNK_AREA; i++) {
					if ((*t)[i] == NO_TOP && local[i] >= 0) {
						(*t)[i] = static_cast<int16_t>(cy * CHUNK + local[i]);
					}
				}
			}
		}
		return t.get();
	}

	const std::vector<uint8_t> &chunk_light(int cx, int cy, int cz) {
		auto &packed = light[{cx, cy, cz}];
		if (packed.empty()) {
			const ChunkData *hood[27];
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						hood[neighbourhood_index(dx, dy, dz)] = chunk(cx + dx, cy + dy, cz + dz);
					}
				}
			}
			const ColumnTops *t[9];
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					t[(dz + 1) * 3 + (dx + 1)] = column_tops(cx + dx, cz + dz);
				}
			}
			LightRegion region;
			compute_light(region, hood, t, cy, blocks);
			packed.resize(CHUNK_VOLUME);
			for (int y = 0; y < CHUNK; y++) {
				for (int z = 0; z < CHUNK; z++) {
					for (int x = 0; x < CHUNK; x++) {
						packed[(y * CHUNK + z) * CHUNK + x] = uint8_t(region.sky_at(x, y, z) << 4 | region.block_at(x, y, z));
					}
				}
			}
		}
		return packed;
	}

	// GoCubeWorld::light_at: (sky, block) as 0..1.
	void light_at(int x, int y, int z, float out[2]) {
		const int cx = floor_div(x, CHUNK), cy = floor_div(y, CHUNK), cz = floor_div(z, CHUNK);
		const uint8_t v = chunk_light(cx, cy, cz)[((y - cy * CHUNK) * CHUNK + (z - cz * CHUNK)) * CHUNK + (x - cx * CHUNK)];
		out[0] = (v >> 4) / float(LIGHT_MAX);
		out[1] = (v & 15) / float(LIGHT_MAX);
	}

	// The chunks' bytes and the light at points between them, hashed.
	std::string fingerprint() {
		Sha256 sha;
		for (int cx = -1; cx <= 1; cx++) {
			for (int cz = -1; cz <= 1; cz++) {
				for (int cy = -3; cy <= 3; cy++) {
					const std::vector<uint8_t> bytes = encode_chunk(*chunk(cx, cy, cz));
					sha.update(bytes.data(), bytes.size());
				}
			}
		}
		std::vector<float> values;
		for (int x = -16; x < 16; x += 2) {
			for (int z = -16; z < 16; z += 2) {
				for (int y = -20; y < 60; y += 2) {
					float v[2];
					light_at(x, y, z, v);
					values.push_back(v[0]);
					values.push_back(v[1]);
				}
			}
		}
		sha.update(values.data(), values.size() * sizeof(float));
		return sha.hex16();
	}
};

std::string fingerprint(uint32_t seed) {
	gc::Content content;
	load_base_game(content);
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	Generator generator;
	gc::configure_generator(generator, content, seed);
	TestWorld world{table.blocks, generator};
	return world.fingerprint();
}

} // namespace

TEST_CASE("a world (seed 777) is generated as it always was") {
	CHECK(fingerprint(777) == WORLD_FINGERPRINT);
}

TEST_CASE("ore salts are djb2 string hashes") {
	CHECK(gc::string_hash("") == 5381u);
	CHECK(gc::string_hash("a") == 5381u * 33 + 'a');
}

TEST_CASE("the land: seas, beaches, overhangs, bare stone, trees") {
	gc::Content content;
	load_base_game(content);
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	Generator generator;
	gc::configure_generator(generator, content, 777);
	auto id = [&](const char *name) { return BlockId(content.block_index(name)); };
	const BlockId grass = id("core:grass"), sand = id("core:sand"), gravel = id("core:gravel"), stone = id("core:stone"),
				  water = id("core:water"), log = id("core:log");
	int columns = 0, sea = 0, sand_tops = 0, gravel_tops = 0, stone_tops = 0, grass_tops = 0, overhangs = 0, trees = 0;
	int highest = -1000;
	ChunkData c;
	std::map<std::tuple<int, int, int>, ChunkData> chunks;
	auto at = [&](int x, int y, int z) {
		const int cx = int(std::floor(x / 32.0)), cy = int(std::floor(y / 32.0)), cz = int(std::floor(z / 32.0));
		auto key = std::make_tuple(cx, cy, cz);
		auto it = chunks.find(key);
		if (it == chunks.end()) {
			it = chunks.emplace(key, ChunkData()).first;
			generator.generate(it->second, cx * 32, cy * 32, cz * 32);
		}
		return it->second.get(x - cx * 32, y - cy * 32, z - cz * 32);
	};
	for (int x = -256; x < 256; x += 8) {
		for (int z = -256; z < 256; z += 8) {
			columns++;
			const int h = generator.height_at(x, z);
			highest = std::max(highest, h);
			const BlockId top = at(x, h, z);
			sea += h < Generator::SEA_LEVEL && at(x, Generator::SEA_LEVEL, z) == water;
			sand_tops += top == sand;
			gravel_tops += top == gravel;
			stone_tops += top == stone && h > Generator::SEA_LEVEL;
			grass_tops += top == grass;
			// An overhang: solid, then air, then solid again further down, above the sea.
			bool seen_air = false;
			for (int y = h - 1; y > Generator::SEA_LEVEL; y--) {
				const BlockId b = at(x, y, z);
				if (b == AIR) {
					seen_air = true;
				} else if (seen_air && b != water) {
					overhangs++;
					break;
				}
			}
		}
	}
	for (int x = -128; x < 128; x++) { // trees: a log on the grass
		for (int z = -128; z < 128; z++) {
			const int h = generator.height_at(x, z);
			trees += at(x, h + 1, z) == log && at(x, h, z) == grass;
		}
	}
	// (Seed 777: 14% sea, 16% sand, 4% gravel, under 1% bare stone, 73% grass;
	// 14% with something overhanging, 60 trees; up to 61.)
	CHECK(sea > columns / 15);
	CHECK(sand_tops > columns / 15);
	CHECK(gravel_tops > columns / 60);
	CHECK(stone_tops > 0);
	CHECK(grass_tops > columns / 2);
	CHECK(overhangs > columns / 20);
	CHECK(trees > 20);
	CHECK(highest > 45);
	CHECK(highest < Generator::TOP_LIMIT);
}
