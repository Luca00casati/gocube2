// Block-by-block light: sky light (0..15, from the open sky) and block light
// (0..15, from glowing blocks). Computed for one chunk at a time over the chunk
// plus a margin of LIGHT_MAX blocks, which is as far as light can travel, so a
// chunk's light only depends on its 27-chunk neighbourhood. Thread-safe.
module gocube::voxel;
import gocube::util::vec, std::math;

const int LIGHT_MAX = 15;

// World y of the topmost opaque block for each (x, z) of a chunk column, or NO_TOP.
alias ColumnTops = short[CHUNK_AREA];
const short NO_TOP = -32768;

// Index into the 3x3x3 chunk neighbourhood; (0, 0, 0) is the chunk itself.
fn int neighbourhood_index(int dx, int dy, int dz) @inline => ((dy + 1) * 3 + (dz + 1)) * 3 + (dx + 1);

const int LIGHT_MARGIN = LIGHT_MAX;
const int LIGHT_SIZE = CHUNK + 2 * LIGHT_MARGIN;
const int LIGHT_CELLS = LIGHT_SIZE * LIGHT_SIZE * LIGHT_SIZE;

// A chunk's light, over the chunk and its margin. Reused: compute_light
// fills it (free() when done with it).
struct LightRegion
{
	char[] sky, block, opaque;
	Vec{int} sky_queue, block_queue; // (working space)
}

fn int light_index(int x, int y, int z) @inline => (y * LIGHT_SIZE + z) * LIGHT_SIZE + x;

// Chunk-local coordinates, valid from -LIGHT_MARGIN to CHUNK + LIGHT_MARGIN - 1.
fn char LightRegion.sky_at(&self, int x, int y, int z) @inline
	=> self.sky[light_index(x + LIGHT_MARGIN, y + LIGHT_MARGIN, z + LIGHT_MARGIN)];
fn char LightRegion.block_at(&self, int x, int y, int z) @inline
	=> self.block[light_index(x + LIGHT_MARGIN, y + LIGHT_MARGIN, z + LIGHT_MARGIN)];
fn bool LightRegion.opaque_at(&self, int x, int y, int z) @inline
	=> self.opaque[light_index(x + LIGHT_MARGIN, y + LIGHT_MARGIN, z + LIGHT_MARGIN)] != 0;

fn void LightRegion.free(&self)
{
	if (self.sky.ptr) free(self.sky.ptr);
	if (self.block.ptr) free(self.block.ptr);
	if (self.opaque.ptr) free(self.opaque.ptr);
	self.sky = {};
	self.block = {};
	self.opaque = {};
	self.sky_queue.free();
	self.block_queue.free();
}

fn void LightRegion.reset(&self, char sky)
{
	if (!self.sky.ptr)
	{
		self.sky = mem::alloc_array(char, LIGHT_CELLS);
		self.block = mem::alloc_array(char, LIGHT_CELLS);
		self.opaque = mem::alloc_array(char, LIGHT_CELLS);
	}
	self.sky[..] = sky;
	self.block[..] = 0;
	self.opaque[..] = 0;
	self.sky_queue.clear();
	self.block_queue.clear();
}

// Breadth-first flood: each step into a non-opaque cell loses one level.
fn void flood(char[] light, Vec{int}* queue, char[] opaque) @private
{
	const int S = LIGHT_SIZE;
	const int SS = S * S;
	for (sz head = 0; head < queue.len(); head++)
	{
		int i = (*queue)[head];
		char l = light[i];
		if (l <= 1) continue;
		char next = l - 1;
		int x = i % S;
		int z = (i / S) % S;
		int y = i / SS;
		if (x > 0) visit(light, queue, opaque, i - 1, next);
		if (x < S - 1) visit(light, queue, opaque, i + 1, next);
		if (z > 0) visit(light, queue, opaque, i - S, next);
		if (z < S - 1) visit(light, queue, opaque, i + S, next);
		if (y > 0) visit(light, queue, opaque, i - SS, next);
		if (y < S - 1) visit(light, queue, opaque, i + SS, next);
	}
}

fn void visit(char[] light, Vec{int}* queue, char[] opaque, int j, char next) @private @inline
{
	if (!opaque[j] && light[j] < next)
	{
		light[j] = next;
		queue.push(j);
	}
}

// Whether the region around chunk_y is all open sky (see compute_light).
fn bool open_sky(ChunkData*[27]* chunks, ColumnTops*[9]* tops, int chunk_y, BlockInfo[] blocks) @private
{
	sz count = blocks.len;
	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++)
			{
				ChunkData* c = (*chunks)[neighbourhood_index(dx, dy, dz)];
				if (!c)
				{
					if (dy < 0) return false; // below the world: solid
					continue; // above it: air
				}
				if (c.mixed || c.uniform_value >= count) return false;
				BlockInfo* b = &blocks[c.uniform_value];
				if (b.opaque || b.emit > 0) return false;
			}
		}
	}
	int bottom = chunk_y * CHUNK - LIGHT_MARGIN; // world y of the region's lowest cells
	for (int i = 0; i < 9; i++)
	{
		ColumnTops* t = (*tops)[i];
		if (!t) continue;
		foreach (top : *t)
		{
			if (top >= bottom) return false;
		}
	}
	return true;
}

fn int part_lo(int o) @private @inline => o < 0 ? 0 : (o == 0 ? LIGHT_MARGIN : LIGHT_MARGIN + CHUNK);
fn int part_hi(int o) @private @inline => o < 0 ? LIGHT_MARGIN : (o == 0 ? LIGHT_MARGIN + CHUNK : LIGHT_SIZE);

// chunks: 27 entries (neighbourhood_index); null = outside the world's
// vertical range (above: open air, below: solid).
// tops: 9 entries, index (dz + 1) * 3 + (dx + 1), for the surrounding columns.
fn void compute_light(LightRegion* out, ChunkData*[27]* chunks, ColumnTops*[9]* tops, int chunk_y, BlockInfo[] blocks)
{
	const int S = LIGHT_SIZE;
	const int M = LIGHT_MARGIN;
	// Open sky (the common case above the land): nothing opaque or glowing in
	// the whole neighbourhood and every column's top below the region, so
	// sky light is full everywhere and there is no block light to spread.
	if (open_sky(chunks, tops, chunk_y, blocks))
	{
		out.reset((char)LIGHT_MAX);
		return;
	}
	out.reset(0);
	char[] opaque = out.opaque;

	// Region coordinate -> (which neighbour chunk, local coordinate), per axis.
	int[S] chunk_offset;
	int[S] local;
	for (int r = 0; r < S; r++)
	{
		int l = r - M;
		chunk_offset[r] = l < 0 ? -1 : (l >= CHUNK ? 1 : 0);
		local[r] = l - chunk_offset[r] * CHUNK;
	}

	// Opaque cells and glowing blocks, one neighbour chunk's part of the region
	// at a time (a uniform one in whole rows).
	sz count = blocks.len;
	for (int oy = -1; oy <= 1; oy++)
	{
		for (int oz = -1; oz <= 1; oz++)
		{
			for (int ox = -1; ox <= 1; ox++)
			{
				ChunkData* c = (*chunks)[neighbourhood_index(ox, oy, oz)];
				int x0 = part_lo(ox);
				int x1 = part_hi(ox);
				if (!c || !c.mixed)
				{
					// Below the world: solid; above it: air.
					BlockId id = c ? c.uniform_value : AIR;
					bool solid = c ? (id < count && blocks[id].opaque) : oy < 0;
					char emit = c && id < count ? blocks[id].emit : 0;
					if (!solid && emit == 0) continue; // (already clear)
					for (int y = part_lo(oy); y < part_hi(oy); y++)
					{
						for (int z = part_lo(oz); z < part_hi(oz); z++)
						{
							int row = light_index(x0, y, z);
							if (solid) opaque[row : x1 - x0] = 1;
							if (emit > 0)
							{
								for (int i = row; i < row + (x1 - x0); i++)
								{
									out.block[i] = emit;
									out.block_queue.push(i);
								}
							}
						}
					}
					continue;
				}
				for (int y = part_lo(oy); y < part_hi(oy); y++)
				{
					for (int z = part_lo(oz); z < part_hi(oz); z++)
					{
						int src = chunk_index(local[x0], local[y], local[z]);
						int i = light_index(x0, y, z);
						for (int x = x0; x < x1; x++, i++, src++)
						{
							BlockId id = c.blocks[src];
							if (id >= count) continue;
							BlockInfo* b = &blocks[id];
							opaque[i] = b.opaque ? 1 : 0;
							if (b.emit > 0)
							{
								out.block[i] = b.emit;
								out.block_queue.push(i);
							}
						}
					}
				}
			}
		}
	}

	// Sky: full light straight down each column until its topmost opaque block
	// (the lowest lit cell of each column kept).
	short[S * S] lowest;
	lowest[..] = (short)S;
	for (int z = 0; z < S; z++)
	{
		for (int x = 0; x < S; x++)
		{
			ColumnTops* t = (*tops)[(chunk_offset[z] + 1) * 3 + (chunk_offset[x] + 1)];
			int top = t ? (*t)[local[z] * CHUNK + local[x]] : NO_TOP;
			for (int y = S - 1; y >= 0; y--)
			{
				int wy = chunk_y * CHUNK + y - M;
				int i = light_index(x, y, z);
				if (wy <= top || opaque[i]) break;
				out.sky[i] = (char)LIGHT_MAX;
				lowest[z * S + x] = (short)y;
			}
		}
	}
	// The flood starts only from lit cells next to an unlit open one: from any
	// other, every neighbour is lit already or opaque (the same light comes out,
	// without going through the whole open sky). A column's cells can only have
	// one below them unlit at its lowest; beside them, where the next column's
	// lit part ends higher up.
	for (int z = 0; z < S; z++)
	{
		for (int x = 0; x < S; x++)
		{
			int low = lowest[z * S + x];
			if (low >= S) continue; // (no sky in this column)
			// Only where a neighbouring column isn't lit as far down as this one can there be a dark neighbour.
			int deepest = low;
			if (x > 0) deepest = math::max(deepest, (int)lowest[z * S + x - 1]);
			if (x < S - 1) deepest = math::max(deepest, (int)lowest[z * S + x + 1]);
			if (z > 0) deepest = math::max(deepest, (int)lowest[(z - 1) * S + x]);
			if (z < S - 1) deepest = math::max(deepest, (int)lowest[(z + 1) * S + x]);
			int y_top = math::min(S - 1, deepest == S ? S - 1 : deepest);
			for (int y = low; y <= y_top; y++)
			{
				int i = light_index(x, y, z);
				if ((y > 0 && dark(out, i - S * S)) || (x > 0 && dark(out, i - 1)) || (x < S - 1 && dark(out, i + 1)) ||
					(z > 0 && dark(out, i - S)) || (z < S - 1 && dark(out, i + S)))
				{
					out.sky_queue.push(i);
				}
			}
		}
	}
	flood(out.sky, &out.sky_queue, opaque);
	flood(out.block, &out.block_queue, opaque);
}

fn bool dark(LightRegion* r, int j) @private @inline => !r.opaque[j] && r.sky[j] < LIGHT_MAX - 1;

// Topmost opaque local y per column inside one chunk, or -1.
fn void chunk_local_tops(ChunkData* chunk, BlockInfo[] blocks, ichar[CHUNK_AREA]* out)
{
	sz count = blocks.len;
	if (!chunk.mixed)
	{
		BlockId id = chunk.uniform_value;
		(*out)[..] = id < count && blocks[id].opaque ? (ichar)(CHUNK - 1) : -1;
		return;
	}
	for (int z = 0; z < CHUNK; z++)
	{
		for (int x = 0; x < CHUNK; x++)
		{
			ichar top = -1;
			for (int y = CHUNK - 1; y >= 0; y--)
			{
				BlockId id = chunk.get(x, y, z);
				if (id < count && blocks[id].opaque)
				{
					top = (ichar)y;
					break;
				}
			}
			(*out)[z * CHUNK + x] = top;
		}
	}
}
