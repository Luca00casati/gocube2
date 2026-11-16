#include "game/hand.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace gc {

namespace {

constexpr XMFLOAT3 REST = {0.36f, -0.34f, -0.52f};   // where the hand rests (camera space)
// The arm turns about the shoulder, off the bottom right of the screen, so the
// sleeve always stays behind the hand.
constexpr XMFLOAT3 SHOULDER = {0.52f, -0.62f, 0.02f};
constexpr float SWING_WIND = 0.25f, SWING_HIT = 0.5f;
const Hand::Pose SWING_REST = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
const Hand::Pose SWING_UP = {{0.1f, -0.05f, 0.0f}, {0.0f, 0.015f, 0.03f}, {0.35f, 0.0f, 0.0f}};
const Hand::Pose SWING_DOWN = {{0.12f, 0.32f, 0.1f}, {-0.02f, 0.06f, -0.06f}, {-0.8f, 0.0f, 0.0f}};

float ease(float x) {
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

Hand::Pose mix(const Hand::Pose &a, const Hand::Pose &b, float u) {
	Hand::Pose p;
	for (int k = 0; k < 3; k++) {
		p.rotation[k] = a.rotation[k] + (b.rotation[k] - a.rotation[k]) * u;
		p.push[k] = a.push[k] + (b.push[k] - a.push[k]) * u;
		p.tilt[k] = a.tilt[k] + (b.tilt[k] - a.tilt[k]) * u;
	}
	return p;
}

// A Godot node's transform: Euler angles in its YXZ order (Z turned first),
// then the position; as a DirectXMath (row vector) matrix.
XMMATRIX node(XMFLOAT3 position, const float euler[3], float scale = 1.0f) {
	return XMMatrixScaling(scale, scale, scale) * XMMatrixRotationZ(euler[2]) * XMMatrixRotationX(euler[0]) *
			XMMatrixRotationY(euler[1]) * XMMatrixTranslation(position.x, position.y, position.z);
}

// The hand shader's shade for a face of the model's own normal is applied
// on the GPU; here a box (Godot's BoxMesh) of `size` centred on its origin.
void add_box(std::vector<WorldVertex> &out, XMFLOAT3 size, const XMMATRIX &m, const float color[3]) {
	static const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (const auto &n : N) {
		// Two axes across the face.
		const XMFLOAT3 a = std::abs(n[1]) > 0.5f ? XMFLOAT3{1, 0, 0} : XMFLOAT3{0, 1, 0};
		const XMVECTOR nv = XMVectorSet(n[0], n[1], n[2], 0);
		const XMVECTOR av = XMLoadFloat3(&a);
		const XMVECTOR bv = XMVector3Cross(nv, av);
		XMFLOAT3 c[4];
		const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
		for (int i = 0; i < 4; i++) {
			const XMVECTOR p = (nv + av * su[i] + bv * sv[i]) * XMVectorSet(size.x / 2, size.y / 2, size.z / 2, 0);
			XMStoreFloat3(&c[i], XMVector3TransformCoord(p, m));
		}
		for (int i : {0, 1, 2, 0, 2, 3}) {
			WorldVertex v = {};
			v.pos[0] = c[i].x;
			v.pos[1] = c[i].y;
			v.pos[2] = c[i].z;
			std::copy(n, n + 3, v.normal);
			std::copy(color, color + 3, v.color);
			v.color[3] = 1;
			out.push_back(v);
		}
	}
}

// Each light level is 80% as bright as the one above; level 0 is dark.
float light_curve(float level01) {
	return level01 <= 0.0f ? 0.0f : std::pow(0.8f, 15.0f - level01 * 15.0f);
}

} // namespace

bool HandLights::update(const World &world, const std::vector<std::array<float, 4>> &lights, const float viewer[3]) {
	const int half = SIZE / 2;
	auto step = [](float v) { return int(std::lround(v / float(STEP))) * STEP; };
	const BlockPos origin{step(viewer[0]) - half, step(viewer[1]) - half, step(viewer[2]) - half};
	std::vector<int64_t> key = {int64_t(world.revision()), origin.x, origin.y, origin.z};
	for (const auto &l : lights) {
		key.insert(key.end(), {int64_t(std::floor(l[0])), int64_t(std::floor(l[1])), int64_t(std::floor(l[2])), int64_t(l[3])});
	}
	count_ = int(lights.size());
	if (key == key_) {
		return false;
	}
	key_ = std::move(key);
	origin_ = origin;
	volume_ = lights.empty() ? std::vector<uint8_t>() : world.held_light(origin, SIZE, lights);
	return true;
}

int HandLights::level_at(const float at[3]) const {
	if (volume_.empty()) {
		return 0;
	}
	const int x = int(std::floor(at[0])) - origin_.x, y = int(std::floor(at[1])) - origin_.y,
			  z = int(std::floor(at[2])) - origin_.z;
	if (x < 0 || y < 0 || z < 0 || x >= SIZE || y >= SIZE || z >= SIZE) {
		return 0;
	}
	return volume_[size_t((y * SIZE + z) * SIZE + x)];
}

void light_for(const World &world, const HandLights &hand_lights, const float sky_light[3], const float at[3],
		float least, float out[3]) {
	int sky, block;
	world.light_at(int(std::floor(at[0])), int(std::floor(at[1])), int(std::floor(at[2])), sky, block);
	const float s = light_curve(float(sky) / 15.0f);
	const float b = light_curve(float(std::max(block, hand_lights.level_at(at))) / 15.0f);
	static const float BLOCK_LIGHT_COLOR[3] = {1.0f, 0.85f, 0.62f}; // (used as is, as the Godot version's Color)
	for (int k = 0; k < 3; k++) {
		out[k] = std::max(std::max(sky_light[k] * s, BLOCK_LIGHT_COLOR[k] * b), least);
	}
}

void Hand::swing() {
	if (swing_ < 0) {
		swing_ = 0;
	} else {
		swing_next_ = true;
	}
}

Hand::Pose Hand::swing_pose(float t) {
	if (t < SWING_WIND) {
		return mix(SWING_REST, SWING_UP, ease(t / SWING_WIND));
	}
	if (t < SWING_HIT) {
		const float u = (t - SWING_WIND) / (SWING_HIT - SWING_WIND);
		return mix(SWING_UP, SWING_DOWN, u * u); // speeds up into the blow
	}
	return mix(SWING_DOWN, SWING_REST, ease((t - SWING_HIT) / (1.0f - SWING_HIT)));
}

void Hand::update(float dt, bool mining, bool walking) {
	// Keep swinging while mining, one whole swing after another.
	if (mining && swing_ < 0) {
		swing_ = 0;
	}
	pose_ = SWING_REST;
	if (swing_ >= 0) {
		swing_ += dt;
		if (swing_ >= SWING_TIME) {
			swing_ = swing_next_ ? 0.0f : -1.0f;
			swing_next_ = false;
		}
		if (swing_ >= 0) {
			pose_ = swing_pose(swing_ / SWING_TIME);
		}
	}
	// Walking bob, faded in and out (never snapping when you stop).
	bob_amount_ += ((walking ? 1.0f : 0.0f) - bob_amount_) * (1.0f - std::exp(-8.0f * dt));
	bob_ += dt * 9.0f * bob_amount_;
	bob_offset_[0] = std::cos(bob_ * 0.5f) * 0.012f * bob_amount_;
	bob_offset_[1] = -std::abs(std::sin(bob_ * 0.5f)) * 0.02f * bob_amount_;
	bob_offset_[2] = 0;
}

void Hand::build(ItemModels &models, const std::string &item, const float light[3], std::vector<WorldVertex> &arm,
		std::vector<WorldVertex> &held) const {
	const float zero[3] = {0, 0, 0};
	const XMMATRIX hand = node({SHOULDER.x + bob_offset_[0] + pose_.push[0], SHOULDER.y + bob_offset_[1] + pose_.push[1],
									   SHOULDER.z + bob_offset_[2] + pose_.push[2]},
			pose_.rotation);
	const XMMATRIX grip = node({REST.x - SHOULDER.x, REST.y - SHOULDER.y, REST.z - SHOULDER.z}, zero) * hand;
	const float arm_turn[3] = {0.15f, -0.12f, 0.0f};
	const XMMATRIX arm_m = node({0.0f, -0.02f, 0.12f}, arm_turn) * grip;
	const XMMATRIX fist_m = node({0, 0, -0.22f}, zero) * arm_m;
	// (Colours as they are: items.hlsl writes the result out as the Godot version's renderer does.)
	const float sleeve[3] = {0.34f * light[0], 0.46f * light[1], 0.72f * light[2]};
	const float skin[3] = {0.86f * light[0], 0.68f * light[1], 0.54f * light[2]};
	add_box(arm, {0.135f, 0.135f, 0.42f}, arm_m, sleeve); // wider than the hand: a cuff around the wrist
	add_box(arm, {0.1f, 0.1f, 0.12f}, fist_m, skin);
	if (item.empty()) {
		return;
	}
	const XMMATRIX tilt = node({0, 0, 0}, pose_.tilt) * grip;
	XMMATRIX item_m;
	if (models.is_block(item)) {
		const float turn[3] = {0.1f, 0.75f, 0.0f};
		item_m = node({-0.04f, 0.04f, -0.2f}, turn, 1.1f) * tilt;
	} else {
		const float turn[3] = {0.0f, -1.35f, 0.35f}; // held edge-on, tip up
		item_m = node({-0.02f, 0.1f, -0.2f}, turn, 0.85f) * tilt;
	}
	for (const ModelVertex &mv : models.model(item)) {
		WorldVertex v = {};
		XMFLOAT3 p;
		XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(mv.pos[0], mv.pos[1], mv.pos[2], 1), item_m));
		v.pos[0] = p.x;
		v.pos[1] = p.y;
		v.pos[2] = p.z;
		std::copy(mv.normal, mv.normal + 3, v.normal);
		std::copy(mv.uv, mv.uv + 2, v.uv);
		std::copy(light, light + 3, v.color);
		v.color[3] = 1;
		held.push_back(v);
	}
}

} // namespace gc
