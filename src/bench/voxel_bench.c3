// Benchmarks (`c3c benchmark`; not part of the game build): the noise, a chunk's light, and meshing it
// with and without block shadows, on seed 777's terrain around the origin
// (the base game's content). Each prints its mean time per call.
module gocube::bench::voxel_bench @feat(BENCHMARKING); // (only in `c3c benchmark`)
import gocube::content, gocube::voxel, gocube::util::vec, gocube::util::map;

const String CONTENT_DIR = "content";

// The chunks a lit chunk needs (3 x 3 columns, from below the land to the
// sky), made once for all the benchmarks.
struct Scene
{
	bool ready;
	Content content;
	BlockTable table;
	Generator generator;
	ChunkData*[3][3][7] chunks; // [y + 3][z + 1][x + 1]: chunk y -3..3
	ColumnTops[3][3] tops;      // [z + 1][x + 1]
	LightRegion light;
	Surface opaque, cutout, fluid;
}

Scene scene;

fn void setup() @init
{
	set_benchmark_max_iterations(150);
	set_benchmark_warmup_iterations(2);
}

fn Scene* get() @private
{
	Scene* s = &scene;
	if (s.ready) return s;
	DString why;
	defer why.free();
	if (!s.content.load(CONTENT_DIR, &why)) abort("content: %s", why.str_view());
	s.content.freeze();
	s.table = s.content.build_block_table();
	s.content.configure_generator(&s.generator, 777);
	for (int y = 0; y < 7; y++)
	{
		for (int z = 0; z < 3; z++)
		{
			for (int x = 0; x < 3; x++)
			{
				ChunkData* c = mem::new(ChunkData);
				s.generator.generate(c, (x - 1) * voxel::CHUNK, (y - 3) * voxel::CHUNK, (z - 1) * voxel::CHUNK);
				s.chunks[y][z][x] = c;
			}
		}
	}
	BlockInfo[] blocks = s.table.blocks.array_view();
	for (int z = 0; z < 3; z++)
	{
		for (int x = 0; x < 3; x++)
		{
			s.tops[z][x][..] = voxel::NO_TOP;
			for (int y = 6; y >= 0; y--)
			{
				ichar[voxel::CHUNK_AREA] local;
				voxel::chunk_local_tops(s.chunks[y][z][x], blocks, &local);
				for (int i = 0; i < voxel::CHUNK_AREA; i++)
				{
					if (s.tops[z][x][i] == voxel::NO_TOP && local[i] >= 0) s.tops[z][x][i] = (short)((y - 3) * voxel::CHUNK + local[i]);
				}
			}
		}
	}
	s.ready = true;
	return s;
}

// The neighbourhood of chunk (0, cy, 0); outside -3..3 null (as the world's
// edge: air above, solid below).
fn void hood(Scene* s, int cy, ChunkData*[27]* out, ColumnTops*[9]* tops) @private
{
	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++)
			{
				int y = cy + dy + 3;
				(*out)[voxel::neighbourhood_index(dx, dy, dz)] = y >= 0 && y < 7 ? s.chunks[y][dz + 1][dx + 1] : null;
			}
		}
	}
	for (int dz = -1; dz <= 1; dz++)
	{
		for (int dx = -1; dx <= 1; dx++) (*tops)[(dz + 1) * 3 + (dx + 1)] = &s.tops[dz + 1][dx + 1];
	}
}

// The chunks that have both land and sky in them (where light and shadows
// cost the most): seed 777's surface is at about y 0..40.
const int[3] SURFACE_CHUNKS = { 0, 1, -1 };

// --- Noise ------------------------------------------------------------------

float sink;

fn void noise2_64k() @benchmark
{
	float sum = 0;
	for (int z = 0; z < 256; z++)
	{
		for (int x = 0; x < 256; x++) sum += voxel::noise2(x * 0.037f, z * 0.041f, 7);
	}
	sink = sum;
}

fn void noise3_64k() @benchmark
{
	float sum = 0;
	for (int y = 0; y < 16; y++)
	{
		for (int z = 0; z < 64; z++)
		{
			for (int x = 0; x < 64; x++) sum += voxel::noise3(x * 0.037f, y * 0.043f, z * 0.041f, 7);
		}
	}
	sink = sum;
}

fn void fbm3_4oct_16k() @benchmark
{
	float sum = 0;
	for (int y = 0; y < 4; y++)
	{
		for (int z = 0; z < 64; z++)
		{
			for (int x = 0; x < 64; x++) sum += voxel::fbm3(x * 0.017f, y * 0.028f, z * 0.017f, 7, 4);
		}
	}
	sink = sum;
}

// A chunk column's terrain, from nothing (the generator forgets its cache).
fn void generate_surface_chunk() @benchmark
{
	Scene* s = get();
	s.generator.forget_cached();
	ChunkData c;
	defer c.free();
	s.generator.generate(&c, 0, 0, 0);
}

// --- Light and shadows ------------------------------------------------------

fn void light_3_surface_chunks() @benchmark
{
	Scene* s = get();
	foreach (cy : SURFACE_CHUNKS)
	{
		ChunkData*[27] h;
		ColumnTops*[9] t;
		hood(s, cy, &h, &t);
		voxel::compute_light(&s.light, &h, &t, cy, s.table.blocks.array_view());
	}
}

fn void mesh(bool shadows) @private
{
	Scene* s = get();
	foreach (cy : SURFACE_CHUNKS)
	{
		ChunkData*[27] h;
		ColumnTops*[9] t;
		hood(s, cy, &h, &t);
		voxel::compute_light(&s.light, &h, &t, cy, s.table.blocks.array_view());
		ChunkData*[voxel::FACE_COUNT] faces = { h[voxel::neighbourhood_index(1, 0, 0)], h[voxel::neighbourhood_index(-1, 0, 0)],
			h[voxel::neighbourhood_index(0, 1, 0)], h[voxel::neighbourhood_index(0, -1, 0)],
			h[voxel::neighbourhood_index(0, 0, 1)], h[voxel::neighbourhood_index(0, 0, -1)] };
		MeshOptions o = { .smooth_lighting = true, .sun_shadows = shadows, .sun_x = 0.5f, .sun_y = 0.866f, .tops = &t,
			.origin_y = cy * voxel::CHUNK };
		s.opaque.clear();
		s.cutout.clear();
		s.fluid.clear();
		voxel::mesh_chunk(h[voxel::neighbourhood_index(0, 0, 0)], faces, &h, s.table.blocks.array_view(), &s.table.atlas.layout,
			&s.light, &o, &s.opaque, &s.cutout, &s.fluid);
	}
}

fn void light_and_mesh_3_chunks() @benchmark => mesh(false);
fn void light_and_mesh_3_chunks_shadows() @benchmark => mesh(true);
