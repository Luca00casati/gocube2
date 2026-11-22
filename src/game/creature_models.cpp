#include "game/creature_models.h"

#include "content/skin.h"
#include "maths.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace DirectX;

namespace gc {

namespace {

// A part's look: a flat colour, or a block face (tiled).
struct Look {
	float rgb[3] = {1, 1, 1};
	float tile[4] = {0, 0, 0, 0};
	float glow = 0; // light-giving blocks never look darker than their light
	// Or a part of a player's skin (its side faces cut to rows [from, to)).
	const SkinPart *skin = nullptr;
	int skin_slot = -1, from = 0, to = -1;
};

Look skin_look(const SkinPart &part, int slot, int from = 0, int to = -1) {
	Look l;
	l.skin = &part;
	l.skin_slot = slot;
	l.from = from;
	l.to = to;
	return l;
}

Look colour(float r, float g, float b) {
	Look l;
	l.rgb[0] = r;
	l.rgb[1] = g;
	l.rgb[2] = b;
	return l;
}

// A block's face (engine order: 0 a side, 2 the top, 3 the bottom).
Look block_face(const Content &content, const BlockTable &table, const std::string &block, int face) {
	Look l;
	const int index = content.block_index(block);
	if (index <= 0) {
		return colour(0.53f, 0.53f, 0.53f);
	}
	table.atlas.tile_uv(table.blocks[size_t(index)].tiles[face], l.tile);
	l.glow = float(content.blocks.at(block).value("light", 0.0)) / 15.0f;
	return l;
}

// A node's transform: Euler angles (YXZ order: Z first), position, scale.
XMMATRIX node(XMFLOAT3 at, XMFLOAT3 euler = {0, 0, 0}, XMFLOAT3 scale = {1, 1, 1}) {
	return XMMatrixScaling(scale.x, scale.y, scale.z) * XMMatrixRotationZ(euler.z) * XMMatrixRotationX(euler.x) *
			XMMatrixRotationY(euler.y) * XMMatrixTranslation(at.x, at.y, at.z);
}

struct Builder {
	std::vector<CreatureVertex> &out;
	const float *light;
	float flash;

	// A box of `size` centred at `at` in the space `m` takes to the world.
	void box(const XMMATRIX &m, XMFLOAT3 size, XMFLOAT3 at, const Look &look, XMFLOAT3 euler = {0, 0, 0}) {
		const XMMATRIX w = node(at, euler) * m;
		static const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		const float half[3] = {size.x / 2, size.y / 2, size.z / 2};
		float lit[3];
		for (int k = 0; k < 3; k++) {
			lit[k] = std::max(light[k], look.glow) * look.rgb[k];
		}
		for (const auto &n : N) {
			const int axis = std::abs(n[0]) > 0.5f ? 0 : (std::abs(n[1]) > 0.5f ? 1 : 2);
			const int axis_u = (axis + 1) % 3, axis_v = (axis + 2) % 3; // the two across the face
			float corner[4][3];
			const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
			for (int i = 0; i < 4; i++) {
				for (int k = 0; k < 3; k++) {
					corner[i][k] = n[k] * half[k];
				}
				corner[i][axis_u] = su[i] * half[axis_u];
				corner[i][axis_v] = sv[i] * half[axis_v];
			}
			for (int i : {0, 1, 2, 0, 2, 3}) {
				CreatureVertex v;
				XMFLOAT3 p;
				XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(corner[i][0], corner[i][1], corner[i][2], 1), w));
				v.pos[0] = p.x;
				v.pos[1] = p.y;
				v.pos[2] = p.z;
				std::copy(corner[i], corner[i] + 3, v.local);
				std::copy(n, n + 3, v.normal);
				std::copy(lit, lit + 3, v.color);
				v.color[3] = flash;
				std::copy(look.tile, look.tile + 4, v.tile);
				if (look.skin) {
					// Across the face as the layout lays it out (content/skin.h),
					// a hair inside it (no bleeding from the next part).
					const int face = int(&n - N);
					const float x = corner[i][0] / size.x, y = corner[i][1] / size.y, z = corner[i][2] / size.z;
					float s = 0.5f - x, t = 0.5f - y;
					switch (face) {
						case 0: s = 0.5f - z; break;
						case 1: s = 0.5f + z; break;
						case 2:
						case 3: t = 0.5f - z; break;
						case 4: s = 0.5f + x; break;
						default: break;
					}
					float rect[4];
					skin_face(*look.skin, face, rect, look.from, look.to);
					s = std::clamp(s, 0.002f, 0.998f);
					t = std::clamp(t, 0.002f, 0.998f);
					v.skin[0] = (rect[0] + s * rect[2]) / float(SKIN_SIZE);
					v.skin[1] = (rect[1] + t * rect[3]) / float(SKIN_SIZE);
					v.skin[2] = float(look.skin_slot);
				}
				out.push_back(v);
			}
		}
	}
};

// The Block Eater: a squat grazer, a cushion of moss on its
// back, or made of the block it ate.
void block_eater(const Creature &c, const Content &content, const BlockTable &table, const XMMATRIX &root,
		Builder &b) {
	const Look dark = colour(0.10f, 0.08f, 0.07f), horn = colour(0.82f, 0.78f, 0.66f);
	Look hide, belly, moss, moss_light;
	float w = 0.86f; // body width
	if (c.variant.empty() || content.block_index(c.variant) <= 0) {
		hide = colour(0.45f, 0.36f, 0.28f);
		belly = colour(0.58f, 0.49f, 0.38f);
		moss = colour(0.30f, 0.48f, 0.22f);
		moss_light = colour(0.42f, 0.60f, 0.28f);
		w = 0.68f; // the regular one is slim
	} else {
		hide = block_face(content, table, c.variant, 0);  // sides
		moss = block_face(content, table, c.variant, 2);  // the back is the block's top
		moss_light = moss;
		belly = block_face(content, table, c.variant, 3); // the bottom
	}
	// Walking: legs swing, a little bob; a squash when hurt.
	const float swing = c.walking ? std::sin(c.walk_phase) * 0.6f : 0.0f;
	const float bob = c.walking ? std::abs(std::sin(c.walk_phase)) * 0.04f : 0.0f;
	const float squash = c.hurt_time > 0 ? 1.0f - 0.15f * std::sin(c.hurt_time / 0.3f * PI) : 1.0f;
	const XMMATRIX body = node({0, bob, 0}, {0, 0, 0}, {1 / squash, squash, 1 / squash}) * root;
	// A rounded back built from stacked, narrowing slabs.
	b.box(body, {w, 0.42f, 1.18f}, {0, 0.55f, 0}, hide);
	b.box(body, {w - 0.08f, 0.1f, 1.0f}, {0, 0.32f, 0}, belly);
	b.box(body, {w + 0.04f, 0.14f, 1.12f}, {0, 0.82f, 0}, moss);
	b.box(body, {w - 0.2f, 0.1f, 0.86f}, {0, 0.93f, 0.02f}, moss_light);
	b.box(body, {0.34f, 0.07f, 0.4f}, {0.08f, 1.0f, -0.06f}, moss);
	// Head at the front (-Z is forward), low and blunt.
	const XMMATRIX head = node({0, 0.56f, -0.68f}) * body;
	b.box(head, {0.48f, 0.4f, 0.36f}, {0, 0, 0}, hide);
	b.box(head, {0.32f, 0.2f, 0.16f}, {0, -0.08f, -0.24f}, belly);       // snout
	b.box(head, {0.06f, 0.06f, 0.02f}, {-0.07f, -0.06f, -0.325f}, dark); // nostrils
	b.box(head, {0.06f, 0.06f, 0.02f}, {0.07f, -0.06f, -0.325f}, dark);
	b.box(head, {0.07f, 0.08f, 0.02f}, {-0.15f, 0.06f, -0.185f}, dark);  // eyes
	b.box(head, {0.07f, 0.08f, 0.02f}, {0.15f, 0.06f, -0.185f}, dark);
	b.box(head, {0.08f, 0.12f, 0.08f}, {-0.17f, 0.24f, 0.02f}, horn);    // horn nubs
	b.box(head, {0.08f, 0.12f, 0.08f}, {0.17f, 0.24f, 0.02f}, horn);
	b.box(head, {0.36f, 0.06f, 0.2f}, {0, 0.23f, 0.06f}, moss);          // tuft
	// Four stubby legs, pivoting at the hip.
	const float lx = w / 2 - 0.14f;
	const float corners[4][2] = {{-lx, -0.38f}, {lx, -0.38f}, {-lx, 0.38f}, {lx, 0.38f}};
	for (int i = 0; i < 4; i++) {
		const XMMATRIX hip = node({corners[i][0], 0.36f, corners[i][1]}, {swing * (i % 3 == 0 ? 1.0f : -1.0f), 0, 0}) * body;
		b.box(hip, {0.2f, 0.36f, 0.2f}, {0, -0.18f, 0}, hide);
	}
}

// A jointed figure of boxes (game/figure.h) in these colours.
struct FigureLooks {
	Look skin, shirt, sleeve, hands, trousers, shoes;
};

// Builds the figure; returns its head and torso joints (to dress it up), and
// where its right hand is (`hand_out`, if given: what it holds goes there).
void figure(const Figure &f, const FigureLooks &l, const XMMATRIX &root, Builder &b, XMMATRIX &head_out,
		XMMATRIX &torso_out, XMMATRIX *hand_out = nullptr) {
	auto joint = [&](Figure::Joint j, XMFLOAT3 at, const XMMATRIX &parent) {
		return node(at, {f.rot[j][0], f.rot[j][1], f.rot[j][2]}) * parent;
	};
	const XMMATRIX body = node({0, 0, 0}, {PI / 2 * f.lie, 0, 0}) * root; // (asleep: on its back)
	const XMMATRIX hips = joint(Figure::HIPS, {0, f.hips_y, 0}, body);
	const XMMATRIX torso = joint(Figure::TORSO_J, {0, 0, 0}, hips);
	b.box(torso, {0.5f, Figure::TORSO, 0.27f}, {0, Figure::TORSO / 2, 0}, l.shirt);
	const XMMATRIX head = joint(Figure::HEAD, {0, Figure::TORSO + 0.02f, 0}, torso);
	b.box(head, {0.44f, 0.44f, 0.44f}, {0, 0.22f, 0}, l.skin);
	for (int i = 0; i < 2; i++) {
		const float side = i == 0 ? -1.0f : 1.0f;
		const XMMATRIX arm = joint(i == 0 ? Figure::ARM_L : Figure::ARM_R, {0.335f * side, Figure::TORSO - 0.06f, 0}, torso);
		b.box(arm, {0.17f, Figure::UPPER_ARM + 0.06f, 0.17f}, {0, -Figure::UPPER_ARM / 2 + 0.03f, 0}, l.sleeve);
		const XMMATRIX elbow = joint(i == 0 ? Figure::ELBOW_L : Figure::ELBOW_R, {0, -Figure::UPPER_ARM, 0}, arm);
		b.box(elbow, {0.155f, Figure::FOREARM - 0.08f, 0.155f}, {0, -(Figure::FOREARM - 0.08f) / 2, 0}, l.sleeve);
		b.box(elbow, {0.15f, 0.1f, 0.15f}, {0, -Figure::FOREARM + 0.05f, 0}, l.hands);
		if (i == 1 && hand_out) {
			*hand_out = node({0, -Figure::FOREARM + 0.05f, 0}) * elbow;
		}
		const XMMATRIX leg = joint(i == 0 ? Figure::LEG_L : Figure::LEG_R, {0.125f * side, 0, 0}, hips);
		b.box(leg, {0.22f, Figure::THIGH + 0.02f, 0.22f}, {0, -Figure::THIGH / 2, 0}, l.trousers);
		const XMMATRIX knee = joint(i == 0 ? Figure::KNEE_L : Figure::KNEE_R, {0, -Figure::THIGH, 0}, leg);
		b.box(knee, {0.21f, Figure::SHIN - 0.1f, 0.21f}, {0, -(Figure::SHIN - 0.1f) / 2, 0}, l.trousers);
		b.box(knee, {0.22f, 0.1f, 0.28f}, {0, -Figure::SHIN + 0.05f, -0.03f}, l.shoes);
	}
	head_out = head;
	torso_out = torso;
}

// A person (model "figure"): the jointed figure in plain clothes.
void person(const Creature &c, const XMMATRIX &root, Builder &b) {
	const Look cloth = colour(0.25f, 0.27f, 0.32f), skin = colour(0.80f, 0.62f, 0.48f);
	const FigureLooks looks{skin, cloth, cloth, skin, colour(0.18f, 0.19f, 0.23f), colour(0.12f, 0.1f, 0.09f)};
	XMMATRIX head, torso;
	figure(c.figure, looks, root, b, head, torso);
}

Look hsv(float h, float s, float v) {
	h = std::fmod(h, 1.0f) * 6.0f;
	const int i = int(h) % 6;
	const float f = h - std::floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	const float rgb[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
	return colour(rgb[i][0], rgb[i][1], rgb[i][2]);
}

// A player in its skin: the layout's figure (content/skin.h), 32 pixels to
// 1.8 m, jointed as Figure is (an arm or leg is two halves of 6 pixels).
// Returns where its right hand is.
XMMATRIX skinned_figure(const Figure &f, const XMMATRIX &root, int slot, Builder &b) {
	using namespace skin_parts;
	constexpr float P = 1.8f / 32.0f; // a skin pixel
	constexpr float OUTER_HEAD = 1.0f; // the outer layer: this much bigger (pixels; the head's, else 0.5)
	auto joint = [&](Figure::Joint j, XMFLOAT3 at, const XMMATRIX &parent) {
		return node(at, {f.rot[j][0], f.rot[j][1], f.rot[j][2]}) * parent;
	};
	// A part's box (w x h x d pixels at `at`), and its outer layer around it.
	auto part = [&](const XMMATRIX &m, const SkinPart &inner, const SkinPart &outer, float h, XMFLOAT3 at,
				int from = 0, int to = -1, float grow = 0.5f) {
		const XMFLOAT3 size{float(inner.w) * P, h * P, float(inner.d) * P};
		b.box(m, size, at, skin_look(inner, slot, from, to));
		b.box(m, {size.x + grow * P, size.y + grow * P, size.z + grow * P}, at, skin_look(outer, slot, from, to));
	};
	const XMMATRIX body = node({0, 0, 0}, {PI / 2 * f.lie, 0, 0}) * root; // (asleep: on its back)
	const XMMATRIX hips = joint(Figure::HIPS, {0, f.hips_y * (12 * P / Figure::HIP_Y), 0}, body);
	const XMMATRIX torso = joint(Figure::TORSO_J, {0, 0, 0}, hips);
	part(torso, BODY, JACKET, 12, {0, 6 * P, 0});
	const XMMATRIX head = joint(Figure::HEAD, {0, 12 * P, 0}, torso);
	part(head, HEAD, HAT, 8, {0, 4 * P, 0}, 0, -1, OUTER_HEAD);
	XMMATRIX hand = XMMatrixIdentity();
	for (int i = 0; i < 2; i++) {
		const bool right = i == 1;
		const float side = right ? 1.0f : -1.0f; // (it faces -z: its right is +x)
		const SkinPart &arm = right ? RIGHT_ARM : LEFT_ARM, &sleeve = right ? RIGHT_SLEEVE : LEFT_SLEEVE;
		const SkinPart &leg = right ? RIGHT_LEG : LEFT_LEG, &pants = right ? RIGHT_PANTS : LEFT_PANTS;
		// The shoulder 2 pixels below the top; the upper arm from 2 above it to 4 below.
		const XMMATRIX shoulder = joint(right ? Figure::ARM_R : Figure::ARM_L, {6 * P * side, 10 * P, 0}, torso);
		part(shoulder, arm, sleeve, 6, {0, -1 * P, 0}, 0, 6);
		const XMMATRIX elbow = joint(right ? Figure::ELBOW_R : Figure::ELBOW_L, {0, -4 * P, 0}, shoulder);
		part(elbow, arm, sleeve, 6, {0, -3 * P, 0}, 6, 12);
		if (right) {
			hand = node({0, -5 * P, 0}) * elbow;
		}
		const XMMATRIX thigh = joint(right ? Figure::LEG_R : Figure::LEG_L, {2 * P * side, 0, 0}, hips);
		part(thigh, leg, pants, 6, {0, -3 * P, 0}, 0, 6);
		const XMMATRIX knee = joint(right ? Figure::KNEE_R : Figure::KNEE_L, {0, -6 * P, 0}, thigh);
		part(knee, leg, pants, 6, {0, -3 * P, 0}, 6, 12);
	}
	return hand;
}

} // namespace

void build_player(const Figure &f, int id, const float eye[3], float yaw, const float light[3],
		std::vector<CreatureVertex> &out, float hand[16], int skin) {
	// Looks from its id (everyone sees the same person).
	uint32_t h = uint32_t(id) * 2654435761u;
	h ^= h >> 15;
	const Look skins[4] = {colour(0.96f, 0.80f, 0.66f), colour(0.87f, 0.67f, 0.5f), colour(0.72f, 0.52f, 0.36f),
			colour(0.5f, 0.34f, 0.24f)};
	const Look hairs[4] = {colour(0.25f, 0.16f, 0.1f), colour(0.1f, 0.08f, 0.07f), colour(0.75f, 0.55f, 0.25f),
			colour(0.55f, 0.24f, 0.12f)};
	const Look shirt = hsv(std::fmod(float(id) * 0.618034f, 1.0f), 0.6f, 0.85f);
	Look sleeve = shirt;
	for (float &c : sleeve.rgb) {
		c *= 0.88f;
	}
	const Look tone = skins[h % 4], hair = hairs[(h / 7) % 4];
	const Look trousers = hsv(0.6f + 0.08f * float((h / 13) % 3), 0.35f, 0.3f + 0.1f * float((h / 29) % 2));
	const FigureLooks looks{tone, shirt, sleeve, tone, trousers, colour(0.2f, 0.15f, 0.12f)};
	Builder b{out, light, 0};
	// The origin is the eye; asleep the head goes on the pillow, the body along the bed.
	const float lie = f.lie;
	const XMMATRIX root = node({0, -1.62f + (1.62f - 0.3f) * lie, -1.35f * lie}) * node({eye[0], eye[1], eye[2]}, {0, yaw, 0});
	if (skin >= 0) {
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, skinned_figure(f, root, skin, b));
		std::copy(&m._11, &m._11 + 16, hand);
		return;
	}
	XMMATRIX head, torso, hand_m = XMMatrixIdentity();
	figure(f, looks, root, b, head, torso, &hand_m);
	// Face and hair on the head (it faces -z; its box spans y 0..0.44).
	const Look white = colour(0.95f, 0.95f, 0.95f), dark = colour(0.12f, 0.1f, 0.12f), mouth = colour(0.55f, 0.28f, 0.25f);
	b.box(head, {0.46f, 0.12f, 0.46f}, {0, 0.41f, 0}, hair);    // top
	b.box(head, {0.46f, 0.3f, 0.06f}, {0, 0.29f, 0.205f}, hair); // back
	for (const float side : {-1.0f, 1.0f}) {
		b.box(head, {0.05f, 0.18f, 0.4f}, {0.205f * side, 0.33f, 0.02f}, hair); // sides
		b.box(head, {0.1f, 0.06f, 0.02f}, {0.1f * side, 0.21f, -0.225f}, white);
		b.box(head, {0.05f, 0.06f, 0.02f}, {0.085f * side, 0.21f, -0.23f}, dark);
		b.box(head, {0.11f, 0.03f, 0.02f}, {0.1f * side, 0.27f, -0.225f}, hair); // brows
	}
	b.box(head, {0.14f, 0.03f, 0.02f}, {0, 0.09f, -0.225f}, mouth);
	b.box(torso, {0.52f, 0.06f, 0.29f}, {0, 0.03f, 0}, colour(0.22f, 0.16f, 0.1f)); // belt
	XMFLOAT4X4 m;
	XMStoreFloat4x4(&m, hand_m);
	std::copy(&m._11, &m._11 + 16, hand);
}

void build_skinned_arm(const float m[16], float pixel, const float light[3], int skin, std::vector<CreatureVertex> &out) {
	using namespace skin_parts;
	Builder b{out, light, 0};
	const XMMATRIX at = XMMATRIX(m);
	const XMFLOAT3 size{4 * pixel, 12 * pixel, 4 * pixel};
	b.box(at, size, {0, 0, 0}, skin_look(RIGHT_ARM, skin));
	b.box(at, {size.x + 0.5f * pixel, size.y + 0.5f * pixel, size.z + 0.5f * pixel}, {0, 0, 0}, skin_look(RIGHT_SLEEVE, skin));
}

void build_cube(const float at[3], float size, const float rgb[3], std::vector<CreatureVertex> &out) {
	const float white[3] = {1, 1, 1};
	Builder b{out, white, 0};
	Look look = colour(rgb[0], rgb[1], rgb[2]);
	const size_t first = out.size();
	b.box(XMMatrixTranslation(at[0], at[1], at[2]), {size, size, size}, {0, 0, 0}, look);
	for (size_t i = first; i < out.size(); i++) { // unshaded: every face as bright as the top
		out[i].normal[0] = out[i].normal[2] = 0;
		out[i].normal[1] = 1;
	}
}

void build_creature(const Creature &c, const Content &content, const BlockTable &table, float alpha,
		const float light[3], std::vector<CreatureVertex> &out) {
	Builder b{out, light, c.flash};
	const float at[3] = {c.prev[0] + (c.pos[0] - c.prev[0]) * alpha, c.prev[1] + (c.pos[1] - c.prev[1]) * alpha,
			c.prev[2] + (c.pos[2] - c.prev[2]) * alpha};
	const XMMATRIX root = node({at[0], at[1], at[2]}, {0, lerp_angle(c.prev_yaw, c.yaw, alpha), 0});
	if (c.model == "block_eater") {
		block_eater(c, content, table, root, b);
	} else if (c.model == "figure") {
		person(c, root, b);
	}
}

} // namespace gc
