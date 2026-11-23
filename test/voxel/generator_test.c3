// The generator configured by hand (every tree style, with mountains, bare
// rock and snow), its output hashed (FNV-1a, 64 bits), so worlds made before
// stay the same (the hash last changed when biomes went).
module gocube::voxel::generator_test;
import gocube::voxel, gocube::util::vec;

const ulong HAND_GENERATOR_HASH = 0x4a085ea686637dde;

fn ulong fnv(ulong h, void* data, sz size)
{
	char* p = data;
	for (sz i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211;
	return h;
}

fn void hand_configure(Generator* g)
{
	GeneratorIds ids = { .stone = 1, .dirt = 2, .grass = 3, .sand = 4, .water = 5, .lava = 6, .gravel = 8, .snow = 10 };
	TreeKind[*] trees = {
		{ .log = 30, .leaves = 31, .min_trunk = 4, .max_trunk = 6, .style = ROUND_OAK, .percent = 8 },
		{ .log = 32, .leaves = 33, .min_trunk = 6, .max_trunk = 9, .style = BIG_OAK, .percent = 10 },
		{ .log = 34, .leaves = 0, .min_trunk = 2, .max_trunk = 4, .style = CACTUS, .percent = 6 },
	};
	g.configure(4242, ids, &trees);
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
	// The land on a coarse grid, and a chunk column on its highest point.
	int top = -1000;
	int[2] peak;
	for (int z = -3000; z <= 3000; z += 250)
	{
		for (int x = -3000; x <= 3000; x += 250)
		{
			int height = g.height_at(x, z);
			h = fnv(h, &height, 4);
			if (height > top)
			{
				top = height;
				peak = { voxel::floor_div(x, voxel::CHUNK), voxel::floor_div(z, voxel::CHUNK) };
			}
		}
	}
	assert(top > voxel::SNOW_ABOVE, "a snowy peak on the grid (%d)", top);
	columns.push(peak);
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

// A chunk set block by block, collapsing back to one value.
fn void chunk_data() @test
{
	ChunkData c;
	defer c.free();
	assert(c.get(3, 4, 5) == voxel::AIR);
	c.set(3, 4, 5, 7);
	assert(c.mixed && c.get(3, 4, 5) == 7 && c.get(0, 0, 0) == voxel::AIR);
	c.set(3, 4, 5, voxel::AIR);
	c.compact();
	assert(!c.mixed && c.get(3, 4, 5) == voxel::AIR);
}

// Chunk files: written without dyes ("GCC1"); a file from when blocks could be
// dyed ("GCC2") reads as its blocks, the dyes skipped.
fn void chunk_files_old_and_new() @test
{
	ChunkData c;
	defer c.free();
	c.set(1, 2, 3, 7);
	Vec{char} bytes;
	defer bytes.free();
	voxel::encode_chunk(&c, &bytes);
	assert(bytes.array_view()[:4] == "GCC1");
	ChunkData back;
	defer back.free();
	assert(voxel::decode_chunk(bytes.array_view(), &back));
	assert(back.get(1, 2, 3) == 7 && back.get(0, 0, 0) == voxel::AIR);
	// Uniform 5, one dyed block (index 0, red).
	char[*] dyed = { 'G', 'C', 'C', '2', 1, 5, 0, 1, 0, 0, 0, 0, 0, 255, 0, 0 };
	ChunkData old;
	defer old.free();
	assert(voxel::decode_chunk(&dyed, &old));
	assert(!old.mixed && old.get(0, 0, 0) == 5);
	assert(!voxel::decode_chunk(dyed[:15], &old)); // (cut short: not a chunk)
}
