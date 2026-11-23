// Turns a chunk (plus its 6 neighbours, for faces on the border) into vertex
// arrays: an opaque surface, a cutout surface (blocks whose textures have
// see-through pixels) and a fluid surface. Thread-safe.
module gocube::voxel;
import gocube::util::vec, std::math;

struct AtlasLayout
{
	int cols, rows;     // tiles in the atlas grid
	int cell;           // each tile owns a cell x cell area of repeated copies
	int face_x, face_y; // which copy inside the cell faces use
}

const AtlasLayout DEFAULT_ATLAS_LAYOUT = { 1, 1, 4, 1, 1 };

struct MeshOptions
{
	// Per-corner light: each vertex averages the 4 cells touching it on the
	// face's air side, and solid cells there darken the corner (ambient
	// occlusion). Off: one flat light value per face (the classic look).
	bool smooth_lighting;
	// World block position of the chunk's (0, 0, 0) block, for per-block
	// variation that must not change when the chunk is remeshed.
	int origin_x, origin_y, origin_z;
	// Block shadows from the sun: each face is either in the sun or in shadow,
	// by whether a line from the cell in front of it towards the sun meets an
	// opaque block (see sun_reaches). The sun moves in the x-y plane: towards
	// it is (sun_x, sun_y), sun_y > 0. Marked in the vertex colour's alpha.
	bool sun_shadows;
	float sun_x, sun_y;
	// The 3x3 columns' topmost opaque blocks around the chunk (index
	// (dz + 1) * 3 + (dx + 1)), for shadows from above the light region.
	ColumnTops*[9]* tops;
}

struct Surface
{
	Vec{float} positions; // xyz, chunk-local
	Vec{float} normals;   // xyz
	Vec{float} uvs;       // uv
	// rgba: r = sky light, g = block light (0..1), b = corner occlusion (1 = none),
	// a = 1 - 0.5 (green varies: grass, leaves) - 0.25 (in a block's shadow).
	Vec{float} colors;
	Vec{Dye} dyes;        // per vertex: its block's dye (NO_DYE: its own colours)
	Vec{int} indices;
}

fn bool Surface.is_empty(&self) => self.indices.is_empty();
fn sz Surface.vertex_count(&self) => self.colors.len() / 4;

fn void Surface.clear(&self)
{
	self.positions.clear();
	self.normals.clear();
	self.uvs.clear();
	self.colors.clear();
	self.dyes.clear();
	self.indices.clear();
}

fn void Surface.free(&self)
{
	self.positions.free();
	self.normals.free();
	self.uvs.free();
	self.colors.free();
	self.dyes.free();
	self.indices.free();
}

struct FaceDef
{
	int dx, dy, dz;         // normal / direction to the neighbour
	float[3][4] corners;    // quad corners relative to the block's min corner
	float[2][4] st;         // texture coords (0..1 inside the tile) per corner
	int[6] order;           // triangle indices giving clockwise (front) winding
	int[6] flipped;         // same winding, using the other diagonal
	int ua, va;             // tangent axes
	int[4] cu, cv;          // corner position along (ua, va): 0 or 1
}

FaceDef[FACE_COUNT] faces @private;

// Builds the 6 face definitions once. Corners go around the face in (u, v)
// tangent order; the winding is then chosen so triangles are clockwise when
// seen from outside (front-facing).
fn void build_faces() @init @private
{
	int[3][FACE_COUNT] normals = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (int i = 0; i < FACE_COUNT; i++)
	{
		FaceDef* d = &faces[i];
		d.dx = normals[i][0];
		d.dy = normals[i][1];
		d.dz = normals[i][2];
		int axis = d.dx ? 0 : (d.dy ? 1 : 2);
		float plane = (d.dx + d.dy + d.dz) > 0 ? 1.0f : 0.0f;
		// Tangents: X faces -> (z, y); Y faces -> (x, z); Z faces -> (x, y).
		int ua = axis == 0 ? 2 : 0;
		int va = axis == 1 ? 2 : 1;
		d.ua = ua;
		d.va = va;
		float[2][4] uv = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		for (int c = 0; c < 4; c++)
		{
			float[3] p;
			p[axis] = plane;
			p[ua] = uv[c][0];
			p[va] = uv[c][1];
			d.cu[c] = (int)uv[c][0];
			d.cv[c] = (int)uv[c][1];
			d.corners[c] = p;
			d.st[c][0] = uv[c][0];
			// top/bottom: v along z; sides: v from the top down
			d.st[c][1] = axis == 1 ? uv[c][1] : 1.0f - uv[c][1];
		}
		// Winding: (c1 - c0) x (c2 - c0) must point away from the normal.
		float[3] e1;
		float[3] e2;
		for (int k = 0; k < 3; k++)
		{
			e1[k] = d.corners[1][k] - d.corners[0][k];
			e2[k] = d.corners[2][k] - d.corners[0][k];
		}
		float cx = e1[1] * e2[2] - e1[2] * e2[1];
		float cy = e1[2] * e2[0] - e1[0] * e2[2];
		float cz = e1[0] * e2[1] - e1[1] * e2[0];
		bool ccw_from_outside = cx * (float)d.dx + cy * (float)d.dy + cz * (float)d.dz > 0.0f;
		int[6] cw = { 0, 2, 1, 0, 3, 2 };
		int[6] ccw = { 0, 1, 2, 0, 2, 3 };
		int[6] cw_flip = { 1, 3, 2, 1, 0, 3 };
		int[6] ccw_flip = { 1, 2, 3, 1, 3, 0 };
		d.order = ccw_from_outside ? cw : ccw;
		d.flipped = ccw_from_outside ? cw_flip : ccw_flip;
	}
}

fn FaceDef* face_def(int f) @inline => &faces[f];

// What meshing one chunk looks at.
struct MeshInput
{
	ChunkData* chunk;
	ChunkData*[FACE_COUNT] neighbours; // FACE order; null: outside the world (above: air, below: solid)
	ChunkData*[27]* hood;              // the 3x3x3 neighbourhood (neighbourhood_index), or null
	BlockInfo[] blocks;
	BlockId solid_fallback;            // any opaque block: "solid ground" below the world
}

// Block at chunk-local coordinates that may be one step outside the chunk.
fn BlockId MeshInput.sample(&self, int x, int y, int z)
{
	if (x >= 0 && y >= 0 && z >= 0 && x < CHUNK && y < CHUNK && z < CHUNK) return self.chunk.get(x, y, z);
	ChunkData* n;
	if (x >= CHUNK)
	{
		n = self.neighbours[Face.PX.ordinal];
		x -= CHUNK;
	}
	else if (x < 0)
	{
		n = self.neighbours[Face.NX.ordinal];
		x += CHUNK;
	}
	else if (y >= CHUNK)
	{
		if (!self.neighbours[Face.PY.ordinal]) return AIR; // above the world
		n = self.neighbours[Face.PY.ordinal];
		y -= CHUNK;
	}
	else if (y < 0)
	{
		n = self.neighbours[Face.NY.ordinal];
		y += CHUNK;
	}
	else if (z >= CHUNK)
	{
		n = self.neighbours[Face.PZ.ordinal];
		z -= CHUNK;
	}
	else
	{
		n = self.neighbours[Face.NZ.ordinal];
		z += CHUNK;
	}
	return n ? n.get(x, y, z) : self.solid_fallback;
}

// A block next to this chunk, also diagonally across an edge or corner of it
// (from the 3x3x3 neighbourhood; AIR if that isn't given).
fn BlockId MeshInput.sample_near(&self, int x, int y, int z)
{
	int dx = x < 0 ? -1 : (x >= CHUNK ? 1 : 0);
	int dy = y < 0 ? -1 : (y >= CHUNK ? 1 : 0);
	int dz = z < 0 ? -1 : (z >= CHUNK ? 1 : 0);
	if (math::abs(dx) + math::abs(dy) + math::abs(dz) <= 1) return self.sample(x, y, z);
	if (!self.hood) return AIR;
	ChunkData* n = (*self.hood)[neighbourhood_index(dx, dy, dz)];
	if (!n) return dy > 0 ? AIR : self.solid_fallback; // above / below the world
	return n.get(x - dx * CHUNK, y - dy * CHUNK, z - dz * CHUNK);
}

// A stable quarter-turn count (0..3) for a world block position.
fn int block_rotation(int x, int y, int z) @private
{
	uint h = (uint)x * 0x8da6b343 ^ (uint)y * 0xd8163841 ^ (uint)z * 0xcb1ab31f;
	h ^= h >> 15;
	h *= 0x2c1b3c6d;
	h ^= h >> 12;
	return (int)(h >> 30);
}

// Light for each of the 4 corners: {sky, block, occlusion}.
struct CornerLight @private
{
	float[4] sky, block, ao;
}

fn void tile_origin(AtlasLayout* atlas, int tile, float* u0, float* v0, float* tw, float* th) @private @inline
{
	*tw = 1.0f / (float)(atlas.cols * atlas.cell);
	*th = 1.0f / (float)(atlas.rows * atlas.cell);
	*u0 = (float)((tile % atlas.cols) * atlas.cell + atlas.face_x) * *tw;
	*v0 = (float)((tile / atlas.cols) * atlas.cell + atlas.face_y) * *th;
}

// top_height: where the face's upper corners sit (1 = full block).
// rotation: quarter turns of the texture within the face (0..3).
// corner_tops (fluids): the height of each upper corner instead, by its
// position in the cell: [x + 2 * z] for x, z in {0, 1}.
fn void emit_face(Surface* s, FaceDef* d, float bx, float by, float bz, AtlasLayout* atlas, int tile, CornerLight* cl,
	int rotation = 0, float* corner_tops = null) @private
{
	int base = (int)(s.positions.len() / 3);
	float u0;
	float v0;
	float tw;
	float th;
	tile_origin(atlas, tile, &u0, &v0, &tw, &th);
	for (int c = 0; c < 4; c++)
	{
		s.positions.push(bx + d.corners[c][0]);
		float top = 1.0f;
		if (corner_tops) top = corner_tops[(int)(d.corners[c][0] + 0.5f) + 2 * (int)(d.corners[c][2] + 0.5f)];
		s.positions.push(by + (d.corners[c][1] > 0.5f ? top : d.corners[c][1]));
		s.positions.push(bz + d.corners[c][2]);
		s.normals.push((float)d.dx);
		s.normals.push((float)d.dy);
		s.normals.push((float)d.dz);
		float su = d.st[c][0];
		float sv = d.st[c][1];
		for (int r = 0; r < rotation; r++)
		{
			float t = su;
			su = 1.0f - sv;
			sv = t;
		}
		s.uvs.push(u0 + su * tw);
		s.uvs.push(v0 + sv * th);
		s.colors.push(cl.sky[c]);
		s.colors.push(cl.block[c]);
		s.colors.push(cl.ao[c]);
		s.colors.push(1.0f);
	}
	// Split the quad along the diagonal whose corners are more alike, so light
	// gradients don't show a crease.
	float a = cl.ao[0] * (cl.sky[0] + cl.block[0]) + cl.ao[2] * (cl.sky[2] + cl.block[2]);
	float b = cl.ao[1] * (cl.sky[1] + cl.block[1]) + cl.ao[3] * (cl.sky[3] + cl.block[3]);
	int[6]* order = a < b ? &d.flipped : &d.order;
	for (int k = 0; k < 6; k++) s.indices.push(base + (*order)[k]);
}

// A quad with flat light: corners pos[4] (chunk-local), texture coords st[4]
// inside the tile, triangle order (front-facing winding).
fn void emit_quad(Surface* s, float[3][4]* pos, float[2][4]* st, float[3] normal, AtlasLayout* atlas, int tile,
	float sky, float blk, int[6] order) @private
{
	int base = (int)(s.positions.len() / 3);
	float u0;
	float v0;
	float tw;
	float th;
	tile_origin(atlas, tile, &u0, &v0, &tw, &th);
	for (int c = 0; c < 4; c++)
	{
		s.positions.push_all(&(*pos)[c]);
		s.normals.push_all(&normal);
		s.uvs.push(u0 + (*st)[c][0] * tw);
		s.uvs.push(v0 + (*st)[c][1] * th);
		s.colors.push(sky);
		s.colors.push(blk);
		s.colors.push(1.0f);
		s.colors.push(1.0f);
	}
	for (int k = 0; k < 6; k++) s.indices.push(base + order[k]);
}

// Smooth light at one corner of a face: average over the cell in front of the
// face and its 3 neighbours around that corner (skipping solid ones); solid
// neighbours also darken the corner.
fn void corner_light(LightRegion* light, int fx, int fy, int fz, int ua, int va, int du, int dv,
	float* sky, float* blk, float* ao) @private
{
	int[3] p = { fx, fy, fz };
	int[3] pu = p;
	int[3] pv = p;
	int[3] puv = p;
	pu[ua] += du;
	pv[va] += dv;
	puv[ua] += du;
	puv[va] += dv;
	bool side_u = light.opaque_at(pu[0], pu[1], pu[2]);
	bool side_v = light.opaque_at(pv[0], pv[1], pv[2]);
	// Light can't reach the diagonal cell around two solid sides.
	bool diag = (side_u && side_v) || light.opaque_at(puv[0], puv[1], puv[2]);
	float s = light.sky_at(p[0], p[1], p[2]);
	float b = light.block_at(p[0], p[1], p[2]);
	int n = 1;
	if (!side_u)
	{
		s += light.sky_at(pu[0], pu[1], pu[2]);
		b += light.block_at(pu[0], pu[1], pu[2]);
		n++;
	}
	if (!side_v)
	{
		s += light.sky_at(pv[0], pv[1], pv[2]);
		b += light.block_at(pv[0], pv[1], pv[2]);
		n++;
	}
	if (!diag)
	{
		s += light.sky_at(puv[0], puv[1], puv[2]);
		b += light.block_at(puv[0], puv[1], puv[2]);
		n++;
	}
	*sky = s / ((float)n * (float)LIGHT_MAX);
	*blk = b / ((float)n * (float)LIGHT_MAX);
	int occluders = (side_u && side_v) ? 3 : (int)side_u + (int)side_v + (int)diag;
	*ao = 1.0f - 0.24f * (float)occluders;
}

// Whether the sun reaches the cell (cx, cy, cz) (chunk-local): a line from the
// cell's centre towards the sun, stepped block by block, meets no opaque block.
// Inside the light region every block is checked; above it, each column's
// topmost block (mountains, tall trees further up). Past the region's sides
// nothing is known, and the sun gets through. max_top: the highest column top
// around (int::max if unknown): above it nothing can be in the way.
fn bool sun_reaches(LightRegion* light, MeshOptions* o, int max_top, int cx, int cy, int cz) @private
{
	if (light.sky_at(cx, cy, cz) == 0) return false; // closed in: no sky, no sun
	const int LO = -LIGHT_MARGIN;
	const int HI = CHUNK + LIGHT_MARGIN - 1;
	int step_x = o.sun_x > 0.0f ? 1 : -1;
	float dt_x = math::abs(o.sun_x) > 1e-6f ? 1.0f / math::abs(o.sun_x) : 1e30f;
	float dt_y = 1.0f / o.sun_y;
	float t_x = 0.5f * dt_x; // to the next cell boundary
	float t_y = 0.5f * dt_y;
	int x = cx;
	int y = cy;
	int oz = cz < 0 ? -1 : (cz >= CHUNK ? 1 : 0);
	for (int i = 0; i < 1024; i++)
	{
		if (t_x < t_y)
		{
			x += step_x;
			t_x += dt_x;
		}
		else
		{
			y++;
			t_y += dt_y;
		}
		if (x < LO || x > HI || o.origin_y + y > max_top) return true;
		if (y <= HI)
		{
			if (light.opaque_at(x, y, cz)) return false;
			continue;
		}
		int ox = x < 0 ? -1 : (x >= CHUNK ? 1 : 0);
		ColumnTops* t = o.tops ? (*o.tops)[(oz + 1) * 3 + (ox + 1)] : null;
		if (!t) return true;
		if (o.origin_y + y <= (*t)[(cz - oz * CHUNK) * CHUNK + (x - ox * CHUNK)]) return false;
	}
	return true;
}

// The vertices added since the last call get dye `d`.
fn void dye_to(Surface* s, Dye d) @private
{
	sz n = s.vertex_count();
	while (s.dyes.len() < n) s.dyes.push(d);
}

// Sets the alpha of the vertices from `first` on (see Surface.colors).
fn void set_alpha(Surface* s, sz first, float alpha) @private
{
	for (sz v = first; v < s.vertex_count(); v++) s.colors[v * 4 + 3] = alpha;
}

fn void light_for(LightRegion* light, BlockInfo* info, int lx, int ly, int lz, float* sky, float* blk) @private
{
	if (!light)
	{
		*sky = 1.0f;
		*blk = 0.0f;
	}
	else
	{
		*sky = (float)light.sky_at(lx, ly, lz) / (float)LIGHT_MAX;
		*blk = (float)light.block_at(lx, ly, lz) / (float)LIGHT_MAX;
	}
	if (info.emit > 0) *blk = math::max(*blk, (float)info.emit / (float)LIGHT_MAX);
}

fn bool MeshInput.fence_connects(&self, int nx, int ny, int nz)
{
	BlockId n = self.sample(nx, ny, nz);
	return n < self.blocks.len && n != AIR && (self.blocks[n].shape == FENCE || self.blocks[n].opaque);
}

// Everything that isn't a full cube: boxes, crossed plants, fences. Light is
// flat per face: the cell in front for faces on the block's edge, else the
// block's own cell (shaped blocks let light in).
fn void emit_shape(Surface* s, MeshInput* in, BlockInfo* info, BlockId id, Dye dye, int x, int y, int z,
	AtlasLayout* atlas, LightRegion* light) @private
{
	BlockInfo[] blocks = in.blocks;
	float fx = (float)x;
	float fy = (float)y;
	float fz = (float)z;
	if (info.shape == CROSS)
	{
		float sky;
		float blk;
		light_for(light, info, x, y, z, &sky, &blk);
		const float A = 0.15f;
		const float B = 0.85f;
		float[3][4][2] quads = {
			{ { A, 0, A }, { B, 0, B }, { B, 1, B }, { A, 1, A } },
			{ { A, 0, B }, { B, 0, A }, { B, 1, A }, { A, 1, B } },
		};
		float[2][4] st = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
		float[3] normal = { 0, 0, 1 }; // shaded like a north/south side
		int[6] front = { 0, 1, 2, 0, 2, 3 };
		int[6] back = { 0, 2, 1, 0, 3, 2 };
		foreach (&q : quads)
		{
			float[3][4] pos;
			for (int c = 0; c < 4; c++) pos[c] = { fx + (*q)[c][0], fy + (*q)[c][1], fz + (*q)[c][2] };
			emit_quad(s, &pos, &st, normal, atlas, info.tiles[0], sky, blk, front);
			emit_quad(s, &pos, &st, normal, atlas, info.tiles[0], sky, blk, back);
		}
		return;
	}
	BoxList fence;
	defer fence.free();
	Box[] boxes = info.model.array_view();
	if (info.shape == FENCE)
	{
		fence_boxes(in.fence_connects(x, y, z - 1), in.fence_connects(x + 1, y, z), in.fence_connects(x, y, z + 1),
			in.fence_connects(x - 1, y, z), false, &fence);
		boxes = fence.array_view();
	}
	foreach (&bx : boxes)
	{
		for (int f = 0; f < FACE_COUNT; f++)
		{
			FaceDef* d = &faces[f];
			int axis = d.dx ? 0 : (d.dy ? 1 : 2);
			bool positive = (d.dx + d.dy + d.dz) > 0;
			bool on_edge = positive ? bx.mx[axis] >= 1.0f : bx.mn[axis] <= 0.0f;
			float sky;
			float blk;
			if (on_edge)
			{
				BlockId n = in.sample(x + d.dx, y + d.dy, z + d.dz);
				if (n < blocks.len && (blocks[n].opaque || (n == id && !info.opaque && blocks[n].shape == CUBE)))
				{
					continue; // covered by the neighbour
				}
				light_for(light, info, x + d.dx, y + d.dy, z + d.dz, &sky, &blk);
			}
			else
			{
				light_for(light, info, x, y, z, &sky, &blk);
			}
			float[3][4] pos;
			float[2][4] st;
			float[3] shift = { bx.tx, 0.0f, bx.tz };
			for (int c = 0; c < 4; c++)
			{
				float[3] p;
				for (int k = 0; k < 3; k++) p[k] = d.corners[c][k] > 0.5f ? bx.mx[k] : bx.mn[k];
				pos[c] = { fx + p[0], fy + p[1], fz + p[2] };
				st[c][0] = p[d.ua] - shift[d.ua];
				st[c][1] = axis == 1 ? p[d.va] - shift[d.va] : 1.0f - (p[d.va] - bx.ty);
			}
			float[3] normal = { (float)d.dx, (float)d.dy, (float)d.dz };
			emit_quad(s, &pos, &st, normal, atlas, info.tiles[f], sky, blk, d.order);
		}
		dye_to(s, bx.undyed ? NO_DYE : dye);
	}
}

// Surface height of a fluid cell: full under more fluid, else by its level
// (a source sits a little below the block top).
fn float MeshInput.fluid_height(&self, int x, int y, int z, bool* covered)
{
	BlockId id = self.sample_near(x, y, z);
	BlockId above = self.sample_near(x, y + 1, z);
	*covered = above < self.blocks.len && self.blocks[above].fluid;
	return *covered ? 1.0f : math::max(0.125f, (float)self.blocks[id].fluid_level / 8.0f * 0.875f);
}

// The height of each top corner of the fluid cell (x, y, z), [cx + 2 * cz]:
// the average surface of the fluid cells sharing that corner, or a full block
// if fluid pours into any of them from above. Neighbouring cells compute the
// same corner the same way, so their surfaces meet without steps or gaps.
fn void MeshInput.fluid_tops(&self, int x, int y, int z, float[4]* tops)
{
	for (int cz = 0; cz < 2; cz++)
	{
		for (int cx = 0; cx < 2; cx++)
		{
			float sum = 0.0f;
			int n = 0;
			bool full = false;
			for (int dz = cz - 1; dz <= cz && !full; dz++)
			{
				for (int dx = cx - 1; dx <= cx && !full; dx++)
				{
					BlockId id = self.sample_near(x + dx, y, z + dz);
					if (id >= self.blocks.len || !self.blocks[id].fluid) continue;
					bool covered;
					float h = self.fluid_height(x + dx, y, z + dz, &covered);
					full = covered;
					sum += h;
					n++;
				}
			}
			(*tops)[cx + 2 * cz] = full ? 1.0f : sum / (float)math::max(1, n);
		}
	}
}

// neighbours: Face order; null means "outside the world" (above: air, below: solid).
// light: per-face light is taken from the cell in front of each face; null = full light.
// hood: the 3x3x3 chunk neighbourhood (neighbourhood_index), or null; fluid
// surfaces use its diagonal chunks so their corners match across chunk corners.
fn void mesh_chunk(ChunkData* chunk, ChunkData*[FACE_COUNT] neighbours, ChunkData*[27]* hood, BlockInfo[] blocks,
	AtlasLayout* atlas, LightRegion* light, MeshOptions* options, Surface* opaque_out, Surface* cutout_out, Surface* fluid)
{
	if (!chunk.mixed && chunk.uniform_value == AIR) return;
	sz count = blocks.len;
	MeshInput in = { .chunk = chunk, .neighbours = neighbours, .hood = hood, .blocks = blocks };
	// Find any opaque block id to stand in for "solid ground" below the world.
	for (sz i = 1; i < count; i++)
	{
		if (blocks[i].opaque)
		{
			in.solid_fallback = (BlockId)i;
			break;
		}
	}

	bool shadows = options.sun_shadows && light;
	int max_top = int::min;
	for (int i = 0; shadows && i < 9; i++)
	{
		if (!options.tops || !(*options.tops)[i])
		{
			max_top = int::max;
			break;
		}
		foreach (t : *(*options.tops)[i]) max_top = math::max(max_top, (int)t);
	}

	for (int y = 0; y < CHUNK; y++)
	{
		for (int z = 0; z < CHUNK; z++)
		{
			for (int x = 0; x < CHUNK; x++)
			{
				BlockId id = chunk.get(x, y, z);
				if (id == AIR || id >= count) continue;
				BlockInfo* info = &blocks[id];
				Surface* opaque = info.cutout ? cutout_out : opaque_out;
				Dye dye = chunk.dyes.is_empty() || info.fluid ? NO_DYE : chunk.dye(x, y, z);
				if (dye != NO_DYE) dye_to(opaque, NO_DYE); // (the blocks before)
				if (info.shape != CUBE)
				{
					sz first = opaque.vertex_count();
					emit_shape(opaque, &in, info, id, dye, x, y, z, atlas, light);
					if (shadows && !sun_reaches(light, options, max_top, x, y, z))
					{
						set_alpha(opaque, first, 0.75f); // shaped blocks: one shadow for the whole block
					}
					if (dye != NO_DYE) dye_to(opaque, dye); // (crossed plants)
					continue;
				}
				float[4] tops;
				bool tops_done = false;
				for (int f = 0; f < FACE_COUNT; f++)
				{
					FaceDef* d = &faces[f];
					BlockId nid = in.sample(x + d.dx, y + d.dy, z + d.dz);
					BlockInfo* n = nid < count ? &blocks[nid] : null;
					bool n_opaque = n && n.opaque;
					// Hidden faces first (most are): no light to work out for them.
					// Fluids: one connected surface, with faces only towards what
					// isn't fluid (and can be seen through). Opaque faces are hidden
					// by opaque neighbours; see-through blocks (e.g. glass) also hide
					// faces against the same block.
					if (info.fluid ? (nid != AIR && (!n || n.fluid || n.opaque)) : (n_opaque || (!info.opaque && nid == id)))
					{
						continue;
					}
					CornerLight cl;
					int fx = x + d.dx; // the cell in front of the face
					int fy = y + d.dy;
					int fz = z + d.dz;
					for (int c = 0; c < 4; c++)
					{
						if (!light)
						{
							cl.sky[c] = 1.0f;
							cl.block[c] = 0.0f;
							cl.ao[c] = 1.0f;
						}
						else if (options.smooth_lighting && !info.fluid)
						{
							corner_light(light, fx, fy, fz, d.ua, d.va, d.cu[c] ? 1 : -1, d.cv[c] ? 1 : -1,
								&cl.sky[c], &cl.block[c], &cl.ao[c]);
						}
						else
						{
							cl.sky[c] = (float)light.sky_at(fx, fy, fz) / (float)LIGHT_MAX;
							cl.block[c] = (float)light.block_at(fx, fy, fz) / (float)LIGHT_MAX;
							cl.ao[c] = 1.0f;
						}
					}
					bool toward_sun = (float)d.dx * options.sun_x + (float)d.dy * options.sun_y > 0.0f;
					if (info.fluid)
					{
						// Each top corner at the height shared by the fluid cells around it.
						if (!tops_done)
						{
							in.fluid_tops(x, y, z, &tops);
							tops_done = true;
						}
						emit_face(fluid, d, (float)x, (float)y, (float)z, atlas, info.tiles[f], &cl, 0, &tops);
						if (shadows && toward_sun && !sun_reaches(light, options, max_top, fx, fy, fz))
						{
							set_alpha(fluid, fluid.vertex_count() - 4, 0.75f);
						}
					}
					else
					{
						if (info.emit > 0)
						{
							for (int c = 0; c < 4; c++) // glowing blocks are lit by themselves
							{
								cl.block[c] = (float)info.emit / (float)LIGHT_MAX;
								cl.ao[c] = 1.0f;
							}
						}
						int rot = info.random_rotation && d.dy != 0
							? block_rotation(options.origin_x + x, options.origin_y + y, options.origin_z + z)
							: 0;
						emit_face(opaque, d, (float)x, (float)y, (float)z, atlas, info.tiles[f], &cl, rot);
						// Faces turned away from the sun get none anyway (chunk.hlsl).
						bool shadowed = shadows && toward_sun && !sun_reaches(light, options, max_top, fx, fy, fz);
						if (info.tint || shadowed)
						{
							set_alpha(opaque, opaque.vertex_count() - 4,
								1.0f - (info.tint ? 0.5f : 0.0f) - (shadowed ? 0.25f : 0.0f));
						}
					}
				}
				if (dye != NO_DYE) dye_to(opaque, dye);
			}
		}
	}
	dye_to(opaque_out, NO_DYE);
	dye_to(cutout_out, NO_DYE);
	dye_to(fluid, NO_DYE);
}
