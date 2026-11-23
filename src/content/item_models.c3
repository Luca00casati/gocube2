// The 3D model of a block as an item: a small cube for solid blocks (each
// face its own texture, shrunk to a slab's single box), a whole bed for beds,
// else the block's icon given depth (doors, ladders: each opaque
// pixel a little block, walls where it borders a clear one). Used for dropped
// blocks and the block in the first-person hand.
//
// Models are centred on the origin (a cube stands on the bottom of its 0.25
// cell: a slab's top is at 0), in metres, with atlas uvs; built once per item.
module gocube::content::item_models;
import gocube::content, gocube::data, gocube::voxel, gocube::util::vec, gocube::util::map, std::math;

alias Vec3 = float[<3>];

struct ModelVertex
{
	float[3] pos;
	float[3] normal; // the model's own (shading looks at it, not the turned one)
	float[2] uv;     // in the block atlas
}

alias Model = Vec{ModelVertex};

const float CUBE_SIZE = 0.25f;
const float ITEM_SIZE = 0.42f;       // an icon spans this many metres
const float ITEM_THICKNESS = 0.035f;

struct ItemModels
{
	Content* content;   // (must outlive this)
	BlockTable* table;
	Map{String, Model*} models; // keys owned
}

fn void ItemModels.free(&self)
{
	self.models.@each(; String k, Model* m)
	{
		free(k.ptr);
		m.free();
		free(m);
	};
	self.models.free();
}

fn Vec3 cross(Vec3 a, Vec3 b) @private => { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };

fn ModelVertex vertex(Vec3 p, Vec3 n, float u, float v) @private => { { p.x, p.y, p.z }, { n.x, n.y, n.z }, { u, v } };

// The single box (in 0..1 of the cell) a slab, a one-box "boxes" block or a
// bed fills; the whole cell for everything else.
fn void model_bounds(Data* def, float[3]* mn, float[3]* mx) @private
{
	*mn = { 0, 0, 0 };
	*mx = { 1, 1, 1 };
	String shape = def.str("shape", "cube");
	if (shape == "slab")
	{
		(*mx)[1] = 0.5f;
	}
	else if (shape == "bed")
	{
		(*mx)[1] = 9.0f / 16.0f;
	}
	else if (shape == "boxes")
	{
		Data* boxes = def.get("boxes");
		if (boxes && boxes.is_array() && boxes.len() == 1 && boxes.at(0).is_array() && boxes.at(0).len() >= 6)
		{
			for (int k = 0; k < 3; k++)
			{
				(*mn)[k] = (float)(boxes.at(0).at(k).as_double() / 16.0);
				(*mx)[k] = (float)(boxes.at(0).at((sz)k + 3).as_double() / 16.0);
			}
		}
	}
}

// Whether the model is a block's boxes (held differently from a flat icon).
fn bool ItemModels.is_block(&self, String item)
{
	int index = self.content.block_index(item);
	if (index <= 0) return false;
	Data* def = self.content.blocks.get(item);
	return self.table.blocks[index].solid || (def && def.str("shape") == "bed");
}

// Triangles (three vertices each), front faces counter-clockwise.
fn ModelVertex[] ItemModels.model(&self, String item) => @pool()
{
	if (try m = self.models.get(item)) return m.array_view();
	Model* out = mem::new(Model);
	int index = self.content.block_index(item);
	Data* def = index > 0 ? self.content.blocks.get(item) : null;
	if (def && def.str("shape") == "bed")
	{
		// A whole bed in one cell: the head half (pillow) towards -Z, the foot
		// half towards +Z, each squeezed to half its length; without the two
		// faces where they meet.
		float h = 9.0f / 16.0f;
		self.add_box(out, index, { 0, 0, 0.5f }, { 1, h, 1 }, 5);
		int head = self.content.block_index(string::tformat("%s_n_head", item));
		self.add_box(out, head > 0 ? head : index, { 0, 0, 0 }, { 1, h, 0.5f }, 4);
	}
	else if (index > 0 && self.table.blocks[index].solid)
	{
		float[3] mn;
		float[3] mx;
		model_bounds(def, &mn, &mx);
		self.add_box(out, index, mn, mx, -1);
	}
	else
	{
		self.add_extruded(out, self.table.icon(item));
	}
	self.models[item.copy(mem)] = out;
	return out.array_view();
}

// A CUBE_SIZE cube squeezed into the cell part mn..mx, each face its own
// texture; sides show the texture rows the block fills.
fn void ItemModels.add_box(&self, Model* out, int block, float[3] mn, float[3] mx, int skip)
{
	Vec3[6] normals = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	float[2][4] corners = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	float s = CUBE_SIZE;
	Vec3 half = { 0.5f, 0.5f, 0.5f };
	for (int f = 0; f < 6; f++)
	{
		if (f == skip) continue;
		Vec3 n = normals[f];
		// Two axes across the face: right, and up (v runs down), as seen from outside.
		Vec3 up = n.y > 0.5f ? (Vec3){ 0, 0, -1 } : (n.y < -0.5f ? (Vec3){ 0, 0, 1 } : (Vec3){ 0, 1, 0 });
		Vec3 right = cross(up, n);
		float[4] r;
		self.table.atlas.tile_uv(self.table.blocks[block].tiles[f], &r);
		ModelVertex[4] q;
		for (int c = 0; c < 4; c++)
		{
			float cx = corners[c][0];
			float cy = corners[c][1];
			Vec3 unit = n * 0.5f + right * (cx - 0.5f) - up * (cy - 0.5f) + half;
			Vec3 p = { mn[0] + unit.x * (mx[0] - mn[0]), mn[1] + unit.y * (mx[1] - mn[1]), mn[2] + unit.z * (mx[2] - mn[2]) };
			float u = cx;
			float v = math::abs(n.y) < 0.5f ? 1.0f - p.y : cy;
			q[c] = vertex((p - half) * s, n, r[0] + (r[2] - r[0]) * u, r[1] + (r[3] - r[1]) * v);
		}
		foreach (i : (int[6]){ 0, 1, 2, 0, 2, 3 }) out.push(q[i]);
	}
}

// The icon with depth: front and back faces plus side walls wherever an
// opaque pixel borders a clear one.
fn void ItemModels.add_extruded(&self, Model* out, int tile)
{
	if (tile < 0 || tile >= self.table.atlas.tiles.len()) return;
	char[] img = self.table.atlas.tiles[tile];
	int w = content::TILE;
	int h = content::TILE;
	float[4] r;
	self.table.atlas.tile_uv(tile, &r);
	float px = ITEM_SIZE / (float)math::max(w, h);
	float t = ITEM_THICKNESS / 2.0f;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			if (!solid_pixel(img, x, y, w, h)) continue;
			float x0 = ((float)x - (float)w / 2.0f) * px;
			float x1 = x0 + px;
			float y0 = ((float)h / 2.0f - (float)y) * px; // image rows go down
			float y1 = y0 - px;
			float cu = r[0] + (r[2] - r[0]) * ((float)x + 0.5f) / (float)w;
			float cv = r[1] + (r[3] - r[1]) * ((float)y + 0.5f) / (float)h;
			// Front (+Z) and back (-Z): the same pixel.
			float[4] fu = { r[0] + (r[2] - r[0]) * (float)x / (float)w, r[0] + (r[2] - r[0]) * (float)(x + 1) / (float)w,
				r[0] + (r[2] - r[0]) * (float)(x + 1) / (float)w, r[0] + (r[2] - r[0]) * (float)x / (float)w };
			float[4] fv = { r[1] + (r[3] - r[1]) * (float)y / (float)h, r[1] + (r[3] - r[1]) * (float)y / (float)h,
				r[1] + (r[3] - r[1]) * (float)(y + 1) / (float)h, r[1] + (r[3] - r[1]) * (float)(y + 1) / (float)h };
			Vec3[4] front = { { x0, y0, t }, { x1, y0, t }, { x1, y1, t }, { x0, y1, t } };
			foreach (i : (int[6]){ 0, 2, 1, 0, 3, 2 }) out.push(vertex(front[i], { 0, 0, 1 }, fu[i], fv[i]));
			Vec3[4] back = { { x0, y0, -t }, { x1, y0, -t }, { x1, y1, -t }, { x0, y1, -t } };
			foreach (i : (int[6]){ 0, 1, 2, 0, 2, 3 }) out.push(vertex(back[i], { 0, 0, -1 }, fu[i], fv[i]));
			// Walls where the neighbour is clear.
			if (!solid_pixel(img, x, y - 1, w, h)) quad(out, { x0, y0, -t }, { x0, y0, t }, { x1, y0, t }, { x1, y0, -t }, { 0, 1, 0 }, cu, cv);
			if (!solid_pixel(img, x, y + 1, w, h)) quad(out, { x0, y1, t }, { x0, y1, -t }, { x1, y1, -t }, { x1, y1, t }, { 0, -1, 0 }, cu, cv);
			if (!solid_pixel(img, x - 1, y, w, h)) quad(out, { x0, y0, -t }, { x0, y1, -t }, { x0, y1, t }, { x0, y0, t }, { -1, 0, 0 }, cu, cv);
			if (!solid_pixel(img, x + 1, y, w, h)) quad(out, { x1, y0, t }, { x1, y1, t }, { x1, y1, -t }, { x1, y0, -t }, { 1, 0, 0 }, cu, cv);
		}
	}
}

fn bool solid_pixel(char[] img, int x, int y, int w, int h) @private
	=> x >= 0 && y >= 0 && x < w && y < h && img[(y * w + x) * 4 + 3] >= 128;

fn void quad(Model* out, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, float u, float v) @private
{
	foreach (p : (Vec3[6]){ a, b, c, a, c, d }) out.push(vertex(p, n, u, v));
}
