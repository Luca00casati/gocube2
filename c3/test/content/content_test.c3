// The base game's content: its block table and atlas, and a world of seed
// 777, exactly as older versions made them (the numbers they recorded).
module gocube::content::content_test;
import gocube::content, gocube::content::image, gocube::data, gocube::voxel, gocube::util::vec, gocube::util::map;
import std::io, std::hash::sha256, std::sort;

// Where the game's content is, from where the tests run (c3/).
const String CONTENT_DIR = "../content";

fn void load_base_game(Content* content)
{
	DString why;
	defer why.free();
	assert(content.load(CONTENT_DIR, &why), "%s", why.str_view());
	content.freeze();
}

struct Fnv
{
	ulong h;
}

fn void Fnv.put(&self, void* data, sz size)
{
	char* p = data;
	for (sz i = 0; i < size; i++) self.h = (self.h ^ p[i]) * 1099511628211;
}

fn void Fnv.put_int(&self, long v) => self.put(&v, 8);

fn void block_table_hash() @test
{
	Content content;
	defer content.free();
	load_base_game(&content);
	BlockTable table = content.build_block_table();
	defer table.free();
	Fnv f = { 0xcbf29ce484222325 };
	foreach (id : content.block_ids())
	{
		f.put(id.ptr, id.len);
		f.put_int(0);
	}
	foreach (&b : table.blocks)
	{
		f.put_int(b.solid ? 1 : 0);
		f.put_int(b.opaque ? 1 : 0);
		f.put_int(b.fluid ? 1 : 0);
		f.put_int(b.cutout ? 1 : 0);
		f.put_int(b.emit);
		f.put_int(b.fluid_level);
		f.put_int(b.random_rotation ? 1 : 0);
		f.put_int(b.tint ? 1 : 0);
		f.put_int(b.shape.ordinal);
		foreach (t : b.tiles) f.put_int(t);
		foreach (boxes : (BoxList*[2]){ &b.model, &b.collision })
		{
			f.put_int(boxes.len());
			foreach (&x : *boxes)
			{
				f.put(&x.mn, 12);
				f.put(&x.mx, 12);
				f.put(&x.ty, 4);
				f.put(&x.tx, 4);
				f.put(&x.tz, 4);
				f.put_int(x.undyed ? 1 : 0);
			}
		}
	}
	// The icons, by id (as a sorted map goes through them).
	Vec{String} ids;
	defer ids.free();
	table.icons.@each(; String k, int v) { ids.push(k); };
	sort::quicksort(ids.array_view(), fn int(String a, String b) => content::compare_ids(a, b));
	foreach (id : ids)
	{
		f.put(id.ptr, id.len);
		f.put_int(table.icon(id));
	}
	f.put_int(table.atlas.width);
	f.put_int(table.atlas.height);
	f.put(table.atlas.rgba.ptr, table.atlas.rgba.len);
	assert(f.h == 0x190569335bc1a6d9, "the block table's hash is %x", f.h);
}

fn void png_read_with_alpha() @test
{
	// 3 x 1: an opaque pixel, a clear one and a half see-through one.
	char[*] png = x"89504e470d0a1a0a0000000d49484452000000030000000108060000001be014b4000000134944415478da63105030f8cf000427528c1a001234033e740f606c0000000049454e44ae426082";
	Image im;
	defer im.free();
	assert(image::parse_png(&png, &im));
	assert(im.width == 3 && im.height == 1);
	assert(im.at(0, 0)[0] == 0x10 && im.at(0, 0)[2] == 0x30 && im.at(0, 0)[3] == 255);
	assert(im.at(1, 0)[3] == 0);
	assert(im.at(2, 0)[0] == 200 && im.at(2, 0)[3] == 128); // (not premultiplied)
	assert(!image::parse_png({}, &im));
	assert(!image::parse_png("P6 1 1 255 abc", &im)); // not a PNG
	assert(!image::parse_png(png[:40], &im));         // cut short
	// And written: read back the same.
	Vec{char} out;
	defer out.free();
	image::encode_png(3, 1, im.rgba, true, &out);
	Image back;
	defer back.free();
	assert(image::parse_png(out.array_view(), &back));
	assert(back.rgba == im.rgba);
}

// --- Seed 777, as older versions had it ------------------------------------

const String WORLD_FINGERPRINT = "79025b3a4669200c";
const int CHUNK_Y_MIN = -3;
const int CHUNK_Y_MAX = 3;

// The chunks around the origin, generated and lit the way the world does it
// (whole columns for sky light, a full neighbourhood per chunk).
struct GenWorld
{
	BlockInfo[] blocks;
	Generator* generator;
	Map{long, ChunkData*} chunks;
	Map{long, ColumnTops*} tops;
	Map{long, char[]} light; // sky << 4 | block, per block
}

fn long key3(int x, int y, int z) => ((long)(x & 0xFFFFF) << 40) | ((long)(y & 0xFFFFF) << 20) | (long)(z & 0xFFFFF);

fn ChunkData* GenWorld.chunk(&self, int cx, int cy, int cz)
{
	if (cy < CHUNK_Y_MIN || cy > CHUNK_Y_MAX) return null; // outside the world: air above, solid below
	long k = key3(cx, cy, cz);
	if (try c = self.chunks.get(k)) return c;
	ChunkData* c = mem::new(ChunkData);
	self.generator.generate(c, cx * voxel::CHUNK, cy * voxel::CHUNK, cz * voxel::CHUNK);
	self.chunks[k] = c;
	return c;
}

fn ColumnTops* GenWorld.column_tops(&self, int cx, int cz)
{
	long k = key3(cx, 0, cz);
	if (try t = self.tops.get(k)) return t;
	ColumnTops* t = mem::new(ColumnTops);
	(*t)[..] = voxel::NO_TOP;
	for (int cy = CHUNK_Y_MAX; cy >= CHUNK_Y_MIN; cy--)
	{
		ichar[voxel::CHUNK_AREA] local;
		voxel::chunk_local_tops(self.chunk(cx, cy, cz), self.blocks, &local);
		for (int i = 0; i < voxel::CHUNK_AREA; i++)
		{
			if ((*t)[i] == voxel::NO_TOP && local[i] >= 0) (*t)[i] = (short)(cy * voxel::CHUNK + local[i]);
		}
	}
	self.tops[k] = t;
	return t;
}

fn char[] GenWorld.chunk_light(&self, int cx, int cy, int cz)
{
	long k = key3(cx, cy, cz);
	if (try l = self.light.get(k)) return l;
	ChunkData*[27] hood;
	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++) hood[voxel::neighbourhood_index(dx, dy, dz)] = self.chunk(cx + dx, cy + dy, cz + dz);
		}
	}
	ColumnTops*[9] t;
	for (int dz = -1; dz <= 1; dz++)
	{
		for (int dx = -1; dx <= 1; dx++) t[(dz + 1) * 3 + (dx + 1)] = self.column_tops(cx + dx, cz + dz);
	}
	LightRegion region;
	defer region.free();
	voxel::compute_light(&region, &hood, &t, cy, self.blocks);
	char[] packed = mem::alloc_array(char, voxel::CHUNK_VOLUME);
	for (int y = 0; y < voxel::CHUNK; y++)
	{
		for (int z = 0; z < voxel::CHUNK; z++)
		{
			for (int x = 0; x < voxel::CHUNK; x++)
			{
				packed[(y * voxel::CHUNK + z) * voxel::CHUNK + x] = region.sky_at(x, y, z) << 4 | region.block_at(x, y, z);
			}
		}
	}
	self.light[k] = packed;
	return packed;
}

fn void GenWorld.free(&self)
{
	self.chunks.@each(; long k, ChunkData* c)
	{
		c.free();
		free(c);
	};
	self.tops.@each(; long k, ColumnTops* t) { free(t); };
	self.light.@each(; long k, char[] l) { free(l.ptr); };
	self.chunks.free();
	self.tops.free();
	self.light.free();
}

fn String GenWorld.fingerprint(&self)
{
	Sha256 sha;
	sha.init();
	for (int cx = -1; cx <= 1; cx++)
	{
		for (int cz = -1; cz <= 1; cz++)
		{
			for (int cy = -3; cy <= 3; cy++)
			{
				Bytes bytes;
				defer bytes.free();
				voxel::encode_chunk(self.chunk(cx, cy, cz), &bytes);
				sha.update(bytes.array_view());
			}
		}
	}
	Vec{float} values;
	defer values.free();
	for (int x = -16; x < 16; x += 2)
	{
		for (int z = -16; z < 16; z += 2)
		{
			for (int y = -20; y < 60; y += 2)
			{
				int cx = voxel::floor_div(x, voxel::CHUNK);
				int cy = voxel::floor_div(y, voxel::CHUNK);
				int cz = voxel::floor_div(z, voxel::CHUNK);
				char v = self.chunk_light(cx, cy, cz)[((y - cy * voxel::CHUNK) * voxel::CHUNK + (z - cz * voxel::CHUNK)) * voxel::CHUNK + (x - cx * voxel::CHUNK)];
				values.push((float)(v >> 4) / (float)voxel::LIGHT_MAX);
				values.push((float)(v & 15) / (float)voxel::LIGHT_MAX);
			}
		}
	}
	sha.update(((char*)values.items)[:values.len() * 4]);
	char[32] out = sha.final();
	return string::tformat("%02x%02x%02x%02x%02x%02x%02x%02x", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
}

fn void world_777_as_it_always_was() @test
{
	Content content;
	defer content.free();
	load_base_game(&content);
	BlockTable table = content.build_block_table();
	defer table.free();
	Generator generator;
	defer generator.free();
	content.configure_generator(&generator, 777);
	GenWorld world = { .blocks = table.blocks.array_view(), .generator = &generator };
	defer world.free();
	String f = world.fingerprint();
	assert(f == WORLD_FINGERPRINT, "the world's fingerprint is %s", f);
}
