// The generator configured by hand to use everything it has (ores, every
// tree style, biomes with frozen seas, mountains, bare rock, caps and snow),
// its output hashed (FNV-1a, 64 bits): the same configuration and hash as the
// game's earlier versions recorded, so worlds made before stay the same.
module gocube::voxel::generator_test;
import gocube::voxel, gocube::util::vec;

const ulong HAND_GENERATOR_HASH = 0x36fff692a3aedbe3;

fn ulong fnv(ulong h, void* data, sz size)
{
	char* p = data;
	for (sz i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211;
	return h;
}

fn void hand_configure(Generator* g)
{
	GeneratorIds ids = { .stone = 1, .dirt = 2, .grass = 3, .sand = 4, .water = 5, .lava = 6, .ice = 7, .gravel = 8 };
	OreKind[*] ores = {
		{ .id = 20, .replaces = 1, .min_y = -90, .max_y = 10, .veins_per_chunk = 6, .min_size = 3, .max_size = 9, .salt = 1234 },
		{ .id = 21, .replaces = 1, .min_y = -90, .max_y = -30, .veins_per_chunk = 2, .min_size = 2, .max_size = 5, .salt = 99 },
	};
	TreeKind[*] trees = {
		{ .log = 30, .leaves = 31, .min_trunk = 4, .max_trunk = 6, .style = ROUND_OAK },
		{ .log = 32, .leaves = 33, .min_trunk = 6, .max_trunk = 9, .style = BIG_OAK },
		{ .log = 34, .leaves = 0, .min_trunk = 2, .max_trunk = 4, .style = CACTUS },
	};
	g.configure(4242, ids, &ores, &trees);

	BiomeKind plains = voxel::biome_kind();
	plains.height = 14;
	plains.surface = 3;
	plains.filler = 2;
	plains.trees.push({ 0, 8 });

	BiomeKind forest = voxel::biome_kind();
	forest.temperature = 0.2f;
	forest.humidity = 0.6f;
	forest.height = 18;
	forest.surface = 3;
	forest.filler = 2;
	forest.trees.push({ 0, 30 });
	forest.trees.push({ 1, 10 });

	BiomeKind desert = voxel::biome_kind();
	desert.temperature = 0.7f;
	desert.humidity = -0.6f;
	desert.height = 10;
	desert.surface = 4;
	desert.filler = 4;
	desert.trees.push({ 2, 6 });

	BiomeKind peaks = voxel::biome_kind();
	peaks.temperature = -0.6f;
	peaks.humidity = 0.1f;
	peaks.height = 26;
	peaks.base = 6;
	peaks.surface = 3;
	peaks.filler = 2;
	peaks.frozen = true;
	peaks.mountains = 60;
	peaks.rock = 1;
	peaks.rock_above = 40;
	peaks.cap = 9;
	peaks.cap_above = 70;
	peaks.cover = 10;
	peaks.cover_above = 45;
	peaks.trees.push({ 0, 5 });

	BiomeKind[*] biomes = { plains, forest, desert, peaks };
	g.set_biomes(&biomes);
	foreach (&b : biomes) b.trees.free();
}

fn void hand_generator_hash() @test
{
	Generator g;
	defer g.free();
	hand_configure(&g);
	ulong h = 0xcbf29ce484222325; // (FNV's offset basis)
	Vec{int[2]} columns;
	defer columns.free();
	for (int cz = -1; cz <= 1; cz++)
	{
		for (int cx = -1; cx <= 1; cx++) columns.push({ cx, cz });
	}
	// The land on a coarse grid, and a chunk column in each biome (the first found).
	bool[4] found;
	for (int z = -3000; z <= 3000; z += 250)
	{
		for (int x = -3000; x <= 3000; x += 250)
		{
			int biome = g.biome_at(x, z);
			int height = g.height_at(x, z);
			h = fnv(h, &biome, 4);
			h = fnv(h, &height, 4);
			if (biome >= 0 && biome < 4 && !found[biome])
			{
				found[biome] = true;
				columns.push({ voxel::floor_div(x, voxel::CHUNK), voxel::floor_div(z, voxel::CHUNK) });
			}
		}
	}
	assert(found[0] && found[1] && found[2] && found[3], "every biome on the grid");
	ChunkData c;
	defer c.free();
	foreach (col : columns)
	{
		for (int cy = -3; cy <= 3; cy++)
		{
			g.generate(&c, col[0] * voxel::CHUNK, cy * voxel::CHUNK, col[1] * voxel::CHUNK);
			for (int y = 0; y < voxel::CHUNK; y++)
			{
				for (int z = 0; z < voxel::CHUNK; z++)
				{
					for (int x = 0; x < voxel::CHUNK; x++)
					{
						BlockId b = c.get(x, y, z);
						h = fnv(h, &b, 2);
					}
				}
			}
		}
	}
	for (int kind = 0; kind < 3; kind++)
	{
		TreeBlockList blocks;
		defer blocks.free();
		g.tree_blocks(kind, 5, 20, -7, &blocks);
		foreach (t : blocks)
		{
			char only_air = t.only_air ? 1 : 0;
			h = fnv(h, &t.x, 4);
			h = fnv(h, &t.y, 4);
			h = fnv(h, &t.z, 4);
			h = fnv(h, &t.id, 2);
			h = fnv(h, &only_air, 1);
		}
	}
	assert(h == HAND_GENERATOR_HASH, "the generator's hash is %x, not %x", h, HAND_GENERATOR_HASH);
}

// A chunk set block by block, collapsing back to one value, and dyes.
fn void chunk_data() @test
{
	ChunkData c;
	defer c.free();
	assert(c.get(3, 4, 5) == voxel::AIR);
	c.set(3, 4, 5, 7);
	assert(c.mixed && c.get(3, 4, 5) == 7 && c.get(0, 0, 0) == voxel::AIR);
	c.set_dye(3, 4, 5, voxel::make_dye(255, 0, 0));
	assert(c.dye(3, 4, 5) == 0xFFFF0000);
	c.set_dye(0, 0, 0, voxel::make_dye(0, 255, 0)); // (on air: dropped)
	assert(c.dye(0, 0, 0) == voxel::NO_DYE);
	c.set(3, 4, 5, voxel::AIR); // (the dye goes with the block)
	assert(c.dye(3, 4, 5) == voxel::NO_DYE);
	c.compact();
	assert(!c.mixed && c.get(3, 4, 5) == voxel::AIR);
}
