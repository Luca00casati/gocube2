// Chunk storage and the block type table shared by the generator, mesher and
// world. (src/voxel/chunk.h)
module gocube::voxel;
import gocube::util::vec, gocube::util::map;

const CHUNK = 32;
const CHUNK_AREA = CHUNK * CHUNK;
const CHUNK_VOLUME = CHUNK * CHUNK * CHUNK;

alias BlockId = ushort;
const BlockId AIR = 0;

// A block's dye (from the dye vat): 0xFFRRGGBB, or NO_DYE (its own colours).
alias Dye = uint;
const Dye NO_DYE = 0;

fn Dye make_dye(int r, int g, int b)
{
	return 0xFF000000 | ((uint)r & 255) << 16 | ((uint)g & 255) << 8 | ((uint)b & 255);
}

// Face order used everywhere: +X, -X, +Y, -Y, +Z, -Z.
enum Face : char
{
	PX, NX, PY, NY, PZ, NZ
}
const FACE_COUNT = 6;

// How a block is drawn and collides.
enum Shape : char
{
	CUBE,  // a full cube (the fast path)
	BOXES, // a list of boxes inside the cell (slabs, stairs, torches, doors...)
	CROSS, // two crossed quads (plants)
	FENCE, // a post with rails towards fences and solid blocks next to it
}

// An axis-aligned box in block-local coordinates (0..1; collision may reach
// higher, e.g. fences are 1.5 tall).
struct Box
{
	float[3] mn;
	float[3] mx;
	float ty;      // texture offset up: side faces are textured as if the box sat ty lower
	float tx, tz;  // and across: as if it sat tx, tz further along x, z (a wall torch: as if in the middle)
	bool undyed;   // keeps its own colours when the block is dyed (the dye vat's tub)
}

alias BoxList = Vec{Box};

struct BlockInfo
{
	bool solid;          // collides
	bool opaque;         // hides neighbour faces, blocks skylight
	bool fluid;          // drawn on the transparent fluid surface
	bool cutout;         // textures with see-through pixels: drawn on the cutout surface
	char emit;           // block light it gives off (0..15)
	char fluid_level;    // fluids: 8 = source (full), 1..7 = flowing (lower surface)
	bool random_rotation; // top/bottom texture turned 0/90/180/270 degrees by position
	bool tint;           // green parts vary in colour across the land (grass, leaves)
	Shape shape;
	BoxList model;       // Shape.BOXES: what is drawn
	BoxList collision;   // what movement bumps into (empty: nothing)
	int[FACE_COUNT] tiles; // atlas tile per face
}

// A new block type's defaults (a full source for fluids).
fn BlockInfo block_info() => { .fluid_level = 8 };

fn void BlockInfo.free(&self)
{
	self.model.free();
	self.collision.free();
}

// Fence geometry. Connections: towards -z (north), +x (east), +z (south), -x (west).
fn void fence_boxes(bool north, bool east, bool south, bool west, bool for_collision, BoxList* out)
{
	float top = for_collision ? 1.5f : 1.0f;
	float a = 6.0f / 16.0f;
	float b = 10.0f / 16.0f;
	out.push({ .mn = { a, 0.0f, a }, .mx = { b, top, b } });
	bool[4] dirs = { north, east, south, west };
	for (int d = 0; d < 4; d++)
	{
		if (!dirs[d]) continue;
		// Extent along the connection: from the post to the cell edge.
		float w0 = for_collision ? a : 7.0f / 16.0f;
		float w1 = for_collision ? b : 9.0f / 16.0f;
		float[3] lo = { w0, 0, w0 };
		float[3] hi = { w1, 0, w1 };
		switch (d)
		{
			case 0: lo[2] = 0.0f; hi[2] = a;
			case 1: lo[0] = b; hi[0] = 1.0f;
			case 2: lo[2] = b; hi[2] = 1.0f;
			case 3: lo[0] = 0.0f; hi[0] = a;
		}
		if (for_collision)
		{
			out.push({ .mn = { lo[0], 0.0f, lo[2] }, .mx = { hi[0], top, hi[2] } });
		}
		else
		{
			foreach (y0 : (float[2]){ 6.0f / 16.0f, 12.0f / 16.0f })
			{
				out.push({ .mn = { lo[0], y0, lo[2] }, .mx = { hi[0], y0 + 3.0f / 16.0f, hi[2] } });
			}
		}
	}
}

// 32^3 block ids. Uniform chunks (all air, all stone...) store a single
// value (the zero value: uniform, all air). Dyed blocks' colours, by index
// (few: most chunks have none).
struct ChunkData
{
	bool mixed;            // false: every block is uniform_value
	BlockId uniform_value;
	BlockId[] blocks;      // CHUNK_VOLUME, when mixed
	Map{ushort, Dye} dyes;
}

fn int chunk_index(int x, int y, int z) @inline => (y * CHUNK + z) * CHUNK + x;

fn void ChunkData.free(&self)
{
	if (self.blocks.ptr) free(self.blocks.ptr);
	self.blocks = {};
	self.dyes.free();
	self.mixed = false;
	self.uniform_value = AIR;
}

// Back to all air.
fn void ChunkData.clear(&self) => self.free();

fn BlockId ChunkData.get(&self, int x, int y, int z) @inline
{
	return self.mixed ? self.blocks[chunk_index(x, y, z)] : self.uniform_value;
}

fn Dye ChunkData.dye(&self, int x, int y, int z)
{
	if (self.dyes.is_empty()) return NO_DYE;
	return self.dyes.get((ushort)chunk_index(x, y, z)) ?? NO_DYE;
}

// (A dye on air is dropped.)
fn void ChunkData.set_dye(&self, int x, int y, int z, Dye d)
{
	ushort i = (ushort)chunk_index(x, y, z);
	if (d == NO_DYE || self.get(x, y, z) == AIR)
	{
		if (!self.dyes.is_empty()) self.dyes.remove(i);
	}
	else
	{
		self.dyes[i] = d;
	}
}

// A new block there loses the dye of the one before.
fn void ChunkData.set(&self, int x, int y, int z, BlockId v)
{
	if (!self.dyes.is_empty()) self.dyes.remove((ushort)chunk_index(x, y, z));
	if (!self.mixed)
	{
		if (v == self.uniform_value) return;
		if (!self.blocks.ptr) self.blocks = mem::alloc_array(BlockId, CHUNK_VOLUME);
		self.blocks[..] = self.uniform_value;
		self.mixed = true;
	}
	self.blocks[chunk_index(x, y, z)] = v;
}

// Collapse back to a single value if every block is the same.
fn void ChunkData.compact(&self)
{
	if (!self.mixed) return;
	BlockId first = self.blocks[0];
	foreach (b : self.blocks)
	{
		if (b != first) return;
	}
	free(self.blocks.ptr);
	self.blocks = {};
	self.mixed = false;
	self.uniform_value = first;
}
