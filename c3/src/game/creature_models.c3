// How creatures look, built from boxes: each part a flat colour, or a block's
// face texture tiled once a metre (a Block Eater that ate a block looks made
// of it). Vertices in world space for shaders/creatures.hlsl, which shades
// each face by its own normal and the light where the creature is.
// (src/game/creature_models.*)
module gocube::game::creature_models;
import gocube::game::creatures, gocube::game::figure, gocube::content, gocube::content::skin, gocube::mat, gocube::maths;
import gocube::util::vec;
import std::math;

struct CreatureVertex
{
	float[3] pos;    // world
	float[3] local;  // in its box (the texture is tiled from it)
	float[3] normal; // its box's own (shading looks at it)
	float[4] color;  // rgb: the light times the part's colour (raw); a: the red flash
	float[4] tile;   // atlas rectangle of a block face, or all 0 (a flat colour)
	float[3] skin;   // a player's skin: u, v (0..1) and its slot (-1: none)
}

alias CreatureVertices = Vec{CreatureVertex};

// A part's look: a flat colour, or a block face (tiled), or a part of a
// player's skin (its side faces cut to rows [from, to)).
struct Look
{
	float[3] rgb;
	float[4] tile;
	float glow; // light-giving blocks never look darker than their light
	bool has_skin;
	SkinPart skin;
	int skin_slot, from, to;
}

fn Look colour(float r, float g, float b) => { .rgb = { r, g, b }, .skin_slot = -1, .to = -1 };

fn Look skin_look(SkinPart part, int slot, int from = 0, int to = -1) => { .rgb = { 1, 1, 1 }, .has_skin = true, .skin = part, .skin_slot = slot, .from = from, .to = to };

// A block's face (engine order: 0 a side, 2 the top, 3 the bottom).
fn Look block_face(Content* content, BlockTable* table, String block, int face)
{
	int index = content.block_index(block);
	if (index <= 0) return colour(0.53f, 0.53f, 0.53f);
	Look l = colour(1, 1, 1);
	table.atlas.tile_uv(table.blocks[index].tiles[face], &l.tile);
	l.glow = (float)content.blocks.get(block).num("light", 0) / 15.0f;
	return l;
}

struct Builder
{
	CreatureVertices* out;
	float[3] light;
	float flash;
}

const float[3][6] NORMALS = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
const int[6] QUAD = { 0, 1, 2, 0, 2, 3 };

// A box of `size` centred at `at` in the space `m` takes to the world.
fn void Builder.box(&self, Matrix m, float[3] size, float[3] at, Look look, float[3] euler = { 0, 0, 0 })
{
	Matrix w = mat::node(at, euler) * m;
	float[3] half = { size[0] / 2, size[1] / 2, size[2] / 2 };
	float[3] lit;
	for (int k = 0; k < 3; k++) lit[k] = math::max(self.light[k], look.glow) * look.rgb[k];
	foreach (face, n : NORMALS)
	{
		int axis = math::abs(n[0]) > 0.5f ? 0 : (math::abs(n[1]) > 0.5f ? 1 : 2);
		int axis_u = (axis + 1) % 3; // the two across the face
		int axis_v = (axis + 2) % 3;
		float[3][4] corner;
		float[4] su = { -1, 1, 1, -1 };
		float[4] sv = { -1, -1, 1, 1 };
		for (int i = 0; i < 4; i++)
		{
			for (int k = 0; k < 3; k++) corner[i][k] = n[k] * half[k];
			corner[i][axis_u] = su[i] * half[axis_u];
			corner[i][axis_v] = sv[i] * half[axis_v];
		}
		foreach (i : QUAD)
		{
			CreatureVertex v = { .skin = { 0, 0, -1 } };
			v.pos = w.point(corner[i]);
			v.local = corner[i];
			v.normal = n;
			v.color = { lit[0], lit[1], lit[2], self.flash };
			v.tile = look.tile;
			if (look.has_skin)
			{
				// Across the face as the layout lays it out (content/skin), a
				// hair inside it (no bleeding from the next part).
				float x = corner[i][0] / size[0];
				float y = corner[i][1] / size[1];
				float z = corner[i][2] / size[2];
				float s = 0.5f - x;
				float t = 0.5f - y;
				switch (face)
				{
					case 0: s = 0.5f - z;
					case 1: s = 0.5f + z;
					case 2:
					case 3: t = 0.5f - z;
					case 4: s = 0.5f + x;
					default:
				}
				float[4] rect;
				skin::skin_face(look.skin, (int)face, &rect, look.from, look.to);
				s = math::clamp(s, 0.002f, 0.998f);
				t = math::clamp(t, 0.002f, 0.998f);
				v.skin = { (rect[0] + s * rect[2]) / (float)skin::SKIN_SIZE, (rect[1] + t * rect[3]) / (float)skin::SKIN_SIZE, (float)look.skin_slot };
			}
			self.out.push(v);
		}
	}
}

// The Block Eater: a squat grazer, a cushion of moss on its back, or made of
// the block it ate.
fn void block_eater(Creature* c, Content* content, BlockTable* table, Matrix root, Builder* b)
{
	Look dark = colour(0.10f, 0.08f, 0.07f);
	Look horn = colour(0.82f, 0.78f, 0.66f);
	Look hide;
	Look belly;
	Look moss;
	Look moss_light;
	float w = 0.86f; // body width
	if (!c.variant.len || content.block_index(c.variant) <= 0)
	{
		hide = colour(0.45f, 0.36f, 0.28f);
		belly = colour(0.58f, 0.49f, 0.38f);
		moss = colour(0.30f, 0.48f, 0.22f);
		moss_light = colour(0.42f, 0.60f, 0.28f);
		w = 0.68f; // the regular one is slim
	}
	else
	{
		hide = block_face(content, table, c.variant, 0); // sides
		moss = block_face(content, table, c.variant, 2); // the back is the block's top
		moss_light = moss;
		belly = block_face(content, table, c.variant, 3); // the bottom
	}
	// Walking: legs swing, a little bob; a squash when hurt.
	float swing = c.walking ? math::sin(c.walk_phase) * 0.6f : 0.0f;
	float bob = c.walking ? math::abs(math::sin(c.walk_phase)) * 0.04f : 0.0f;
	float squash = c.hurt_time > 0 ? 1.0f - 0.15f * math::sin(c.hurt_time / 0.3f * maths::PI) : 1.0f;
	Matrix body = mat::node({ 0, bob, 0 }, { 0, 0, 0 }, { 1 / squash, squash, 1 / squash }) * root;
	// A rounded back built from stacked, narrowing slabs.
	b.box(body, { w, 0.42f, 1.18f }, { 0, 0.55f, 0 }, hide);
	b.box(body, { w - 0.08f, 0.1f, 1.0f }, { 0, 0.32f, 0 }, belly);
	b.box(body, { w + 0.04f, 0.14f, 1.12f }, { 0, 0.82f, 0 }, moss);
	b.box(body, { w - 0.2f, 0.1f, 0.86f }, { 0, 0.93f, 0.02f }, moss_light);
	b.box(body, { 0.34f, 0.07f, 0.4f }, { 0.08f, 1.0f, -0.06f }, moss);
	// Head at the front (-Z is forward), low and blunt.
	Matrix head = mat::node({ 0, 0.56f, -0.68f }) * body;
	b.box(head, { 0.48f, 0.4f, 0.36f }, { 0, 0, 0 }, hide);
	b.box(head, { 0.32f, 0.2f, 0.16f }, { 0, -0.08f, -0.24f }, belly);       // snout
	b.box(head, { 0.06f, 0.06f, 0.02f }, { -0.07f, -0.06f, -0.325f }, dark); // nostrils
	b.box(head, { 0.06f, 0.06f, 0.02f }, { 0.07f, -0.06f, -0.325f }, dark);
	b.box(head, { 0.07f, 0.08f, 0.02f }, { -0.15f, 0.06f, -0.185f }, dark);  // eyes
	b.box(head, { 0.07f, 0.08f, 0.02f }, { 0.15f, 0.06f, -0.185f }, dark);
	b.box(head, { 0.08f, 0.12f, 0.08f }, { -0.17f, 0.24f, 0.02f }, horn);    // horn nubs
	b.box(head, { 0.08f, 0.12f, 0.08f }, { 0.17f, 0.24f, 0.02f }, horn);
	b.box(head, { 0.36f, 0.06f, 0.2f }, { 0, 0.23f, 0.06f }, moss);          // tuft
	// Four stubby legs, pivoting at the hip.
	float lx = w / 2 - 0.14f;
	float[2][4] corners = { { -lx, -0.38f }, { lx, -0.38f }, { -lx, 0.38f }, { lx, 0.38f } };
	for (int i = 0; i < 4; i++)
	{
		Matrix hip = mat::node({ corners[i][0], 0.36f, corners[i][1] }, { swing * (i % 3 == 0 ? 1.0f : -1.0f), 0, 0 }) * body;
		b.box(hip, { 0.2f, 0.36f, 0.2f }, { 0, -0.18f, 0 }, hide);
	}
}

// A jointed figure of boxes (game/figure) in these colours.
struct FigureLooks
{
	Look skin, shirt, sleeve, hands, trousers, shoes;
}

fn Matrix joint(Figure* f, Joint j, float[3] at, Matrix parent) => mat::node(at, f.rot[j.ordinal]) * parent;

// Builds the figure: its head and torso joints (to dress it up), and where
// its right hand is (what it holds goes there).
fn void build_figure(Figure* f, FigureLooks* l, Matrix root, Builder* b, Matrix* head_out, Matrix* torso_out, Matrix* hand_out)
{
	Matrix body = mat::node({ 0, 0, 0 }, { maths::PI / 2 * f.lie, 0, 0 }) * root; // (asleep: on its back)
	Matrix hips = joint(f, HIPS, { 0, f.hips_y, 0 }, body);
	Matrix torso = joint(f, TORSO_J, { 0, 0, 0 }, hips);
	b.box(torso, { 0.5f, figure::TORSO, 0.27f }, { 0, figure::TORSO / 2, 0 }, l.shirt);
	Matrix head = joint(f, HEAD, { 0, figure::TORSO + 0.02f, 0 }, torso);
	b.box(head, { 0.44f, 0.44f, 0.44f }, { 0, 0.22f, 0 }, l.skin);
	for (int i = 0; i < 2; i++)
	{
		float side = i == 0 ? -1.0f : 1.0f;
		Matrix arm = joint(f, i == 0 ? Joint.ARM_L : Joint.ARM_R, { 0.335f * side, figure::TORSO - 0.06f, 0 }, torso);
		b.box(arm, { 0.17f, figure::UPPER_ARM + 0.06f, 0.17f }, { 0, -figure::UPPER_ARM / 2 + 0.03f, 0 }, l.sleeve);
		Matrix elbow = joint(f, i == 0 ? Joint.ELBOW_L : Joint.ELBOW_R, { 0, -figure::UPPER_ARM, 0 }, arm);
		b.box(elbow, { 0.155f, figure::FOREARM - 0.08f, 0.155f }, { 0, -(figure::FOREARM - 0.08f) / 2, 0 }, l.sleeve);
		b.box(elbow, { 0.15f, 0.1f, 0.15f }, { 0, -figure::FOREARM + 0.05f, 0 }, l.hands);
		if (i == 1 && hand_out) *hand_out = mat::node({ 0, -figure::FOREARM + 0.05f, 0 }) * elbow;
		Matrix leg = joint(f, i == 0 ? Joint.LEG_L : Joint.LEG_R, { 0.125f * side, 0, 0 }, hips);
		b.box(leg, { 0.22f, figure::THIGH + 0.02f, 0.22f }, { 0, -figure::THIGH / 2, 0 }, l.trousers);
		Matrix knee = joint(f, i == 0 ? Joint.KNEE_L : Joint.KNEE_R, { 0, -figure::THIGH, 0 }, leg);
		b.box(knee, { 0.21f, figure::SHIN - 0.1f, 0.21f }, { 0, -(figure::SHIN - 0.1f) / 2, 0 }, l.trousers);
		b.box(knee, { 0.22f, 0.1f, 0.28f }, { 0, -figure::SHIN + 0.05f, -0.03f }, l.shoes);
	}
	*head_out = head;
	*torso_out = torso;
}

// A person (model "figure"): the jointed figure in plain clothes.
fn void person(Creature* c, Matrix root, Builder* b)
{
	Look cloth = colour(0.25f, 0.27f, 0.32f);
	Look skin = colour(0.80f, 0.62f, 0.48f);
	FigureLooks looks = { skin, cloth, cloth, skin, colour(0.18f, 0.19f, 0.23f), colour(0.12f, 0.1f, 0.09f) };
	Matrix head;
	Matrix torso;
	build_figure(&c.figure, &looks, root, b, &head, &torso, null);
}

fn Look hsv(float h, float s, float v)
{
	h = (h % 1.0f) * 6.0f;
	int i = (int)h % 6;
	float f = h - math::floor(h);
	float p = v * (1 - s);
	float q = v * (1 - s * f);
	float t = v * (1 - s * (1 - f));
	float[3][6] rgb = { { v, t, p }, { q, v, p }, { p, v, t }, { p, q, v }, { t, p, v }, { v, p, q } };
	return colour(rgb[i][0], rgb[i][1], rgb[i][2]);
}

const float P = 1.8f / 32.0f; // a skin pixel

// A part's box (w x h x d pixels at `at`), and its outer layer around it.
fn void skin_part(Builder* b, Matrix m, int slot, SkinPart inner, SkinPart outer, float h, float[3] at, int from = 0, int to = -1, float grow = 0.5f)
{
	float[3] size = { (float)inner.w * P, h * P, (float)inner.d * P };
	b.box(m, size, at, skin_look(inner, slot, from, to));
	b.box(m, { size[0] + grow * P, size[1] + grow * P, size[2] + grow * P }, at, skin_look(outer, slot, from, to));
}

// A player in its skin: the layout's figure (content/skin), 32 pixels to
// 1.8 m, jointed as Figure is (an arm or leg is two halves of 6 pixels).
// Returns where its right hand is.
fn Matrix skinned_figure(Figure* f, Matrix root, int slot, Builder* b)
{
	Matrix body = mat::node({ 0, 0, 0 }, { maths::PI / 2 * f.lie, 0, 0 }) * root; // (asleep: on its back)
	Matrix hips = joint(f, HIPS, { 0, f.hips_y * (12 * P / figure::HIP_Y), 0 }, body);
	Matrix torso = joint(f, TORSO_J, { 0, 0, 0 }, hips);
	skin_part(b, torso, slot, skin::BODY, skin::JACKET, 12, { 0, 6 * P, 0 });
	Matrix head = joint(f, HEAD, { 0, 12 * P, 0 }, torso);
	skin_part(b, head, slot, skin::HEAD, skin::HAT, 8, { 0, 4 * P, 0 }, 0, -1, 1.0f);
	Matrix hand = mat::identity();
	for (int i = 0; i < 2; i++)
	{
		bool right = i == 1;
		float side = right ? 1.0f : -1.0f; // (it faces -z: its right is +x)
		SkinPart arm = right ? skin::RIGHT_ARM : skin::LEFT_ARM;
		SkinPart sleeve = right ? skin::RIGHT_SLEEVE : skin::LEFT_SLEEVE;
		SkinPart leg = right ? skin::RIGHT_LEG : skin::LEFT_LEG;
		SkinPart pants = right ? skin::RIGHT_PANTS : skin::LEFT_PANTS;
		// The shoulder 2 pixels below the top; the upper arm from 2 above it to 4 below.
		Matrix shoulder = joint(f, right ? Joint.ARM_R : Joint.ARM_L, { 6 * P * side, 10 * P, 0 }, torso);
		skin_part(b, shoulder, slot, arm, sleeve, 6, { 0, -1 * P, 0 }, 0, 6);
		Matrix elbow = joint(f, right ? Joint.ELBOW_R : Joint.ELBOW_L, { 0, -4 * P, 0 }, shoulder);
		skin_part(b, elbow, slot, arm, sleeve, 6, { 0, -3 * P, 0 }, 6, 12);
		if (right) hand = mat::node({ 0, -5 * P, 0 }) * elbow;
		Matrix thigh = joint(f, right ? Joint.LEG_R : Joint.LEG_L, { 2 * P * side, 0, 0 }, hips);
		skin_part(b, thigh, slot, leg, pants, 6, { 0, -3 * P, 0 }, 0, 6);
		Matrix knee = joint(f, right ? Joint.KNEE_R : Joint.KNEE_L, { 0, -6 * P, 0 }, thigh);
		skin_part(b, knee, slot, leg, pants, 6, { 0, -3 * P, 0 }, 6, 12);
	}
	return hand;
}

// Another player: a figure posed by `f`, with its eyes at `eye`, facing
// `yaw`, lit by `light`, dressed in the skin in slot `skin` of the renderer's
// skins (-1: a plain figure coloured from its id). Returns where its right
// hand is (a held item's model goes there).
fn Matrix build_player(Figure* f, int id, float[3] eye, float yaw, float[3] light, CreatureVertices* out, int skin = -1)
{
	// Looks from its id (everyone sees the same person).
	uint h = (uint)id * 2654435761u;
	h ^= h >> 15;
	Look[4] skins = { colour(0.96f, 0.80f, 0.66f), colour(0.87f, 0.67f, 0.5f), colour(0.72f, 0.52f, 0.36f), colour(0.5f, 0.34f, 0.24f) };
	Look[4] hairs = { colour(0.25f, 0.16f, 0.1f), colour(0.1f, 0.08f, 0.07f), colour(0.75f, 0.55f, 0.25f), colour(0.55f, 0.24f, 0.12f) };
	Look shirt = hsv(((float)id * 0.618034f) % 1.0f, 0.6f, 0.85f);
	Look sleeve = shirt;
	foreach (&c : sleeve.rgb) *c *= 0.88f;
	Look tone = skins[h % 4];
	Look hair = hairs[(h / 7) % 4];
	Look trousers = hsv(0.6f + 0.08f * (float)((h / 13) % 3), 0.35f, 0.3f + 0.1f * (float)((h / 29) % 2));
	FigureLooks looks = { tone, shirt, sleeve, tone, trousers, colour(0.2f, 0.15f, 0.12f) };
	Builder b = { out, light, 0 };
	// The origin is the eye; asleep the head goes on the pillow, the body along the bed.
	float lie = f.lie;
	Matrix root = mat::node({ 0, -1.62f + (1.62f - 0.3f) * lie, -1.35f * lie }) * mat::node(eye, { 0, yaw, 0 });
	if (skin >= 0) return skinned_figure(f, root, skin, &b);
	Matrix head;
	Matrix torso;
	Matrix hand = mat::identity();
	build_figure(f, &looks, root, &b, &head, &torso, &hand);
	// Face and hair on the head (it faces -z; its box spans y 0..0.44).
	Look white = colour(0.95f, 0.95f, 0.95f);
	Look dark = colour(0.12f, 0.1f, 0.12f);
	Look mouth = colour(0.55f, 0.28f, 0.25f);
	b.box(head, { 0.46f, 0.12f, 0.46f }, { 0, 0.41f, 0 }, hair);    // top
	b.box(head, { 0.46f, 0.3f, 0.06f }, { 0, 0.29f, 0.205f }, hair); // back
	foreach (side : (float[2]){ -1.0f, 1.0f })
	{
		b.box(head, { 0.05f, 0.18f, 0.4f }, { 0.205f * side, 0.33f, 0.02f }, hair); // sides
		b.box(head, { 0.1f, 0.06f, 0.02f }, { 0.1f * side, 0.21f, -0.225f }, white);
		b.box(head, { 0.05f, 0.06f, 0.02f }, { 0.085f * side, 0.21f, -0.23f }, dark);
		b.box(head, { 0.11f, 0.03f, 0.02f }, { 0.1f * side, 0.27f, -0.225f }, hair); // brows
	}
	b.box(head, { 0.14f, 0.03f, 0.02f }, { 0, 0.09f, -0.225f }, mouth);
	b.box(torso, { 0.52f, 0.06f, 0.29f }, { 0, 0.03f, 0 }, colour(0.22f, 0.16f, 0.1f)); // belt
	return hand;
}

// A player's right arm in its skin (inner and outer layer), `pixel` metres a
// skin pixel, its length along the y of `m` (the arm's centre at the
// origin, the shoulder towards +y), lit by `light`: the first-person arm.
fn void build_skinned_arm(Matrix m, float pixel, float[3] light, int skin, CreatureVertices* out)
{
	Builder b = { out, light, 0 };
	float[3] size = { 4 * pixel, 12 * pixel, 4 * pixel };
	b.box(m, size, { 0, 0, 0 }, skin_look(skin::RIGHT_ARM, skin));
	b.box(m, { size[0] + 0.5f * pixel, size[1] + 0.5f * pixel, size[2] + 0.5f * pixel }, { 0, 0, 0 }, skin_look(skin::RIGHT_SLEEVE, skin));
}

// A plain cube (a puff of smoke): `size` across, centred at `at`, one colour
// on every side (raw, not lit).
fn void build_cube(float[3] at, float size, float[3] rgb, CreatureVertices* out)
{
	Builder b = { out, { 1, 1, 1 }, 0 };
	sz first = out.len();
	b.box(mat::translation(at[0], at[1], at[2]), { size, size, size }, { 0, 0, 0 }, colour(rgb[0], rgb[1], rgb[2]));
	for (sz i = first; i < out.len(); i++) out.items[i].normal = { 0, 1, 0 }; // unshaded: every face as bright as the top
}

// Appends the creature's boxes, posed for this moment (`alpha`: between its
// last two physics steps), lit by `light` (raw).
fn void build_creature(Creature* c, Content* content, BlockTable* table, float alpha, float[3] light, CreatureVertices* out)
{
	Builder b = { out, light, c.flash };
	float[3] at = { c.prev[0] + (c.pos[0] - c.prev[0]) * alpha, c.prev[1] + (c.pos[1] - c.prev[1]) * alpha, c.prev[2] + (c.pos[2] - c.prev[2]) * alpha };
	Matrix root = mat::node(at, { 0, maths::lerp_angle(c.prev_yaw, c.yaw, alpha), 0 });
	if (c.model == "block_eater")
	{
		block_eater(c, content, table, root, &b);
	}
	else if (c.model == "figure")
	{
		person(c, root, &b);
	}
}
