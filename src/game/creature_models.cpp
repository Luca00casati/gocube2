#include "game/creature_models.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace DirectX;

namespace gc {

namespace {

constexpr float PI = 3.14159265f;

// A part's look: a flat colour, or a block face (tiled).
struct Look {
	float rgb[3] = {1, 1, 1};
	float tile[4] = {0, 0, 0, 0};
	float glow = 0; // light-giving blocks never look darker than their light
};

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

// A Godot node's transform: Euler angles (YXZ order: Z first), position, scale.
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
				out.push_back(v);
			}
		}
	}
};

float lerp_angle(float from, float to, float w) {
	float d = std::fmod(to - from, 2 * PI);
	if (d > PI) {
		d -= 2 * PI;
	} else if (d < -PI) {
		d += 2 * PI;
	}
	return from + d * w;
}

// The Godot version's BlockEater: a squat grazer, a cushion of moss on its
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

// The Godot version's Figure: a jointed figure of boxes in these colours.
struct FigureLooks {
	Look skin, shirt, sleeve, hands, trousers, shoes;
};

// Builds the figure; returns its head and torso joints (to dress it up).
void figure(const Figure &f, const FigureLooks &l, const XMMATRIX &root, Builder &b, XMMATRIX &head_out,
		XMMATRIX &torso_out) {
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
		const XMMATRIX leg = joint(i == 0 ? Figure::LEG_L : Figure::LEG_R, {0.125f * side, 0, 0}, hips);
		b.box(leg, {0.22f, Figure::THIGH + 0.02f, 0.22f}, {0, -Figure::THIGH / 2, 0}, l.trousers);
		const XMMATRIX knee = joint(i == 0 ? Figure::KNEE_L : Figure::KNEE_R, {0, -Figure::THIGH, 0}, leg);
		b.box(knee, {0.21f, Figure::SHIN - 0.1f, 0.21f}, {0, -(Figure::SHIN - 0.1f) / 2, 0}, l.trousers);
		b.box(knee, {0.22f, 0.1f, 0.28f}, {0, -Figure::SHIN + 0.05f, -0.03f}, l.shoes);
	}
	head_out = head;
	torso_out = torso;
}

// The Godot version's Ninja: a figure in dark cloth with a hood, a red sash
// and a headband whose tails flutter behind it, a sword on its back.
void ninja(const Creature &c, const XMMATRIX &root, Builder &b) {
	const Look cloth = colour(0.11f, 0.12f, 0.16f);
	FigureLooks looks{cloth, cloth, cloth, colour(0.07f, 0.07f, 0.1f), cloth, colour(0.06f, 0.06f, 0.08f)};
	XMMATRIX head, torso;
	figure(c.figure, looks, root, b, head, torso);
	const Look skin = colour(0.80f, 0.62f, 0.48f), eyes = colour(0.05f, 0.05f, 0.06f);
	const Look sash = colour(0.66f, 0.10f, 0.12f), steel = colour(0.62f, 0.65f, 0.7f);
	b.box(head, {0.36f, 0.1f, 0.02f}, {0, 0.25f, -0.222f}, skin); // eye slit
	for (const float side : {-1.0f, 1.0f}) {
		b.box(head, {0.06f, 0.05f, 0.02f}, {0.08f * side, 0.25f, -0.232f}, eyes);
	}
	b.box(head, {0.46f, 0.06f, 0.46f}, {0, 0.35f, 0}, sash); // headband
	const XMMATRIX tails = node({0.08f, 0.35f, 0.23f}, {c.tails[0], c.tails[1], c.tails[2]}) * head;
	b.box(tails, {0.05f, 0.05f, 0.3f}, {0.03f, 0, 0.15f}, sash);
	b.box(tails, {0.05f, 0.05f, 0.24f}, {-0.04f, -0.03f, 0.12f}, sash, {0, -0.25f, 0});
	b.box(torso, {0.52f, 0.1f, 0.29f}, {0, 0.06f, 0}, sash);          // sash
	b.box(torso, {0.1f, 0.26f, 0.04f}, {0.15f, -0.08f, -0.15f}, sash); // its ends
	b.box(torso, {0.06f, 0.62f, 0.06f}, {-0.12f, 0.34f, 0.17f}, steel, {0, 0, 0.7f}); // sword on the back
}

} // namespace

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
	} else if (c.model == "ninja") {
		ninja(c, root, b);
	}
}

} // namespace gc
