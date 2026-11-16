#include "render/chunk_renderer.h"

#include "log.h"

// Compiled from shaders/chunk.hlsl at build time (see CMakeLists.txt).
#include "chunk_ps_cutout.h"
#include "chunk_ps_fluid.h"
#include "chunk_ps_opaque.h"
#include "chunk_vs.h"
#include "items_ps.h"
#include "items_vs.h"
#include "lines_ps.h"
#include "lines_vs.h"
#include "sky_ps.h"
#include "sky_vs.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace DirectX;

namespace gc {

namespace {

// Matches cbuffer Frame in chunk.hlsl (16-byte rows).
struct FrameConstants {
	XMFLOAT4X4 view_proj;
	XMFLOAT3 camera_pos;
	float daylight;
	XMFLOAT3 sun_dir;
	float sun_amount;
	XMFLOAT3 sky_tint;
	float min_light;
	XMFLOAT3 fog_color;
	float fog_start;
	XMFLOAT3 block_light_color;
	float fog_end;
	float time;
	float cloud_offset[2];
	float pad1;
	XMFLOAT3 hand_light_origin;
	float hand_light_count;
};
static_assert(sizeof(FrameConstants) % 16 == 0);

// Matches cbuffer Sky in sky.hlsl.
struct SkyConstants {
	XMFLOAT4X4 inv_view_proj;
	XMFLOAT3 top;
	float day;
	XMFLOAT3 horizon;
	float dusk;
	XMFLOAT3 ground;
	float sky_angle;
	XMFLOAT3 sun_dir;
	float time;
	float cloud_offset[2];
	float pad[2];
};
static_assert(sizeof(SkyConstants) % 16 == 0);

// Matches cbuffer Items in items.hlsl.
struct ItemConstants {
	XMFLOAT4X4 view_proj;
	XMFLOAT3 eye;
	float fog_start;
	XMFLOAT3 fog_color;
	float fog_end;
	float use_texture;
	float pad[3];
};
static_assert(sizeof(ItemConstants) % 16 == 0);

struct DrawConstants {
	XMFLOAT3 chunk_origin;
	float pad;
};

using Vertex = WorldVertex;

// A plane a*x + b*y + c*z + d (inside where >= 0), from the view-projection
// matrix's columns (row-vector convention: clip = v * M).
struct Plane {
	float a, b, c, d;
};

void frustum_planes(const XMFLOAT4X4 &m, Plane out[6]) {
	auto col = [&](int j) { return Plane{m.m[0][j], m.m[1][j], m.m[2][j], m.m[3][j]}; };
	const Plane c0 = col(0), c1 = col(1), c2 = col(2), c3 = col(3);
	auto add = [](Plane p, Plane q, float s) { return Plane{p.a + s * q.a, p.b + s * q.b, p.c + s * q.c, p.d + s * q.d}; };
	out[0] = add(c3, c0, 1);  // left
	out[1] = add(c3, c0, -1); // right
	out[2] = add(c3, c1, 1);  // bottom
	out[3] = add(c3, c1, -1); // top
	out[4] = c2;              // near (Direct3D: 0 <= z)
	out[5] = add(c3, c2, -1); // far
}

bool box_visible(const Plane planes[6], const float mn[3], const float mx[3]) {
	for (int i = 0; i < 6; i++) {
		const Plane &p = planes[i];
		// The box corner furthest along the plane's normal.
		const float x = p.a >= 0 ? mx[0] : mn[0], y = p.b >= 0 ? mx[1] : mn[1], z = p.c >= 0 ? mx[2] : mn[2];
		if (p.a * x + p.b * y + p.c * z + p.d < 0) {
			return false;
		}
	}
	return true;
}

// sRGB -> linear, for colours given as they look.
float linear(float c) {
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

// --- Camera --------------------------------------------------------------------

XMFLOAT3 Camera::forward() const {
	return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

XMMATRIX Camera::view() const {
	const XMFLOAT3 f = forward();
	return XMMatrixLookToRH(XMLoadFloat3(&position), XMLoadFloat3(&f), XMVectorSet(0, 1, 0, 0));
}

XMMATRIX Camera::projection(float aspect) const {
	return XMMatrixPerspectiveFovRH(fov_y, aspect, near_z, far_z);
}

// --- ChunkRenderer ---------------------------------------------------------------

int64_t ChunkRenderer::key(const ChunkPos &p) {
	return (static_cast<int64_t>(p.x & 0x1FFFFF) << 42) | (static_cast<int64_t>(p.y & 0x1FFFFF) << 21) |
			static_cast<int64_t>(p.z & 0x1FFFFF);
}

bool ChunkRenderer::init(ID3D11Device *device, const Atlas &atlas) {
	auto ok = [](HRESULT hr, const char *what) {
		if (FAILED(hr)) {
			log("%s failed (HRESULT 0x%08lx)", what, static_cast<unsigned long>(hr));
			return false;
		}
		return true;
	};
	if (!ok(device->CreateVertexShader(g_chunk_vs, sizeof(g_chunk_vs), nullptr, &vs_), "CreateVertexShader") ||
			!ok(device->CreatePixelShader(g_chunk_ps_opaque, sizeof(g_chunk_ps_opaque), nullptr, &ps_opaque_), "ps_opaque") ||
			!ok(device->CreatePixelShader(g_chunk_ps_cutout, sizeof(g_chunk_ps_cutout), nullptr, &ps_cutout_), "ps_cutout") ||
			!ok(device->CreatePixelShader(g_chunk_ps_fluid, sizeof(g_chunk_ps_fluid), nullptr, &ps_fluid_), "ps_fluid")) {
		return false;
	}
	const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	if (!ok(device->CreateInputLayout(layout, 4, g_chunk_vs, sizeof(g_chunk_vs), &layout_), "CreateInputLayout")) {
		return false;
	}

	D3D11_BUFFER_DESC cb = {};
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cb.ByteWidth = sizeof(FrameConstants);
	if (!ok(device->CreateBuffer(&cb, nullptr, &frame_cb_), "CreateBuffer(frame)")) {
		return false;
	}
	cb.ByteWidth = sizeof(DrawConstants);
	if (!ok(device->CreateBuffer(&cb, nullptr, &draw_cb_), "CreateBuffer(draw)")) {
		return false;
	}

	// The atlas, with mipmaps made by the GPU (tiles repeat within their cells,
	// so mip levels don't mix neighbouring tiles). Read as it is, sRGB numbers
	// not decoded: by the world's shaders as the Godot version's renderer does
	// (see chunk.hlsl), and by ImGui.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = UINT(atlas.width);
	td.Height = UINT(atlas.height);
	td.MipLevels = 0; // the full chain
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	ComPtr<ID3D11Texture2D> tex;
	if (!ok(device->CreateTexture2D(&td, nullptr, &tex), "CreateTexture2D(atlas)") ||
			!ok(device->CreateShaderResourceView(tex.Get(), nullptr, &atlas_), "CreateShaderResourceView(atlas)")) {
		return false;
	}
	ComPtr<ID3D11DeviceContext> ctx;
	device->GetImmediateContext(&ctx);
	ctx->UpdateSubresource(tex.Get(), 0, nullptr, atlas.rgba.data(), UINT(atlas.width) * 4, 0);
	// (Mipmaps averaged in sRGB, as the Godot version's Image.generate_mipmaps does.)
	ctx->GenerateMips(atlas_.Get());

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR; // crisp pixels, smooth with distance
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	if (!ok(device->CreateSamplerState(&sd, &sampler_), "CreateSamplerState")) {
		return false;
	}

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_BACK;
	rd.DepthClipEnable = TRUE;
	device->CreateRasterizerState(&rd, &cull_back_);
	rd.CullMode = D3D11_CULL_NONE;
	device->CreateRasterizerState(&rd, &cull_none_);

	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = TRUE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dd.DepthFunc = D3D11_COMPARISON_LESS;
	device->CreateDepthStencilState(&dd, &depth_write_);
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	device->CreateDepthStencilState(&dd, &depth_test_only_);

	D3D11_BLEND_DESC bd = {};
	auto &rt = bd.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
	rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ONE;
	rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	device->CreateBlendState(&bd, &alpha_blend_);

	// Outline lines.
	if (!ok(device->CreateVertexShader(g_lines_vs, sizeof(g_lines_vs), nullptr, &line_vs_), "lines_vs") ||
			!ok(device->CreatePixelShader(g_lines_ps, sizeof(g_lines_ps), nullptr, &line_ps_), "lines_ps")) {
		return false;
	}
	const D3D11_INPUT_ELEMENT_DESC line_layout[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
	if (!ok(device->CreateInputLayout(line_layout, 1, g_lines_vs, sizeof(g_lines_vs), &line_layout_), "line layout")) {
		return false;
	}
	D3D11_BUFFER_DESC lb = {};
	lb.Usage = D3D11_USAGE_DYNAMIC;
	lb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	lb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	lb.ByteWidth = MAX_OUTLINE_BOXES * 24 * sizeof(XMFLOAT3);
	cb.ByteWidth = sizeof(XMFLOAT4);
	if (!ok(device->CreateBuffer(&lb, nullptr, &line_vertices_), "line buffer") ||
			!ok(device->CreateBuffer(&cb, nullptr, &line_cb_), "line constants")) {
		return false;
	}
	dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	device->CreateDepthStencilState(&dd, &depth_less_equal_);
	dd.DepthEnable = FALSE;
	device->CreateDepthStencilState(&dd, &depth_off_);

	// The sky.
	cb.ByteWidth = sizeof(SkyConstants);
	if (!ok(device->CreateVertexShader(g_sky_vs, sizeof(g_sky_vs), nullptr, &sky_vs_), "sky_vs") ||
			!ok(device->CreatePixelShader(g_sky_ps, sizeof(g_sky_ps), nullptr, &sky_ps_), "sky_ps") ||
			!ok(device->CreateBuffer(&cb, nullptr, &sky_cb_), "sky constants")) {
		return false;
	}

	// Held torches' light (HandLights): one level per block, written whenever it changes.
	D3D11_TEXTURE3D_DESC hd = {};
	hd.Width = hd.Height = hd.Depth = HAND_LIGHT_SIZE;
	hd.MipLevels = 1;
	hd.Format = DXGI_FORMAT_R8_UNORM;
	hd.Usage = D3D11_USAGE_DEFAULT;
	hd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const std::vector<uint8_t> dark(size_t(HAND_LIGHT_SIZE) * HAND_LIGHT_SIZE * HAND_LIGHT_SIZE, 0);
	const D3D11_SUBRESOURCE_DATA hdata = {dark.data(), HAND_LIGHT_SIZE, HAND_LIGHT_SIZE * HAND_LIGHT_SIZE};
	if (!ok(device->CreateTexture3D(&hd, &hdata, &hand_light_tex_), "CreateTexture3D(hand light)") ||
			!ok(device->CreateShaderResourceView(hand_light_tex_.Get(), nullptr, &hand_light_srv_), "hand light view")) {
		return false;
	}

	// Items and the hand (the terrain's vertex layout).
	cb.ByteWidth = sizeof(ItemConstants);
	if (!ok(device->CreateVertexShader(g_items_vs, sizeof(g_items_vs), nullptr, &items_vs_), "items_vs") ||
			!ok(device->CreatePixelShader(g_items_ps, sizeof(g_items_ps), nullptr, &items_ps_), "items_ps") ||
			!ok(device->CreateBuffer(&cb, nullptr, &items_cb_), "item constants")) {
		return false;
	}
	return true;
}

bool ChunkRenderer::upload_dynamic(ID3D11DeviceContext *ctx, const std::vector<WorldVertex> &vertices) {
	if (vertices.size() > dynamic_capacity_) {
		ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		dynamic_capacity_ = UINT(vertices.size() * 2);
		D3D11_BUFFER_DESC bd = {};
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bd.ByteWidth = dynamic_capacity_ * sizeof(WorldVertex);
		dynamic_vertices_.Reset();
		if (FAILED(device->CreateBuffer(&bd, nullptr, &dynamic_vertices_))) {
			dynamic_capacity_ = 0;
			return false;
		}
	}
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(dynamic_vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return false;
	}
	std::memcpy(m.pData, vertices.data(), vertices.size() * sizeof(WorldVertex));
	ctx->Unmap(dynamic_vertices_.Get(), 0);
	return true;
}

void ChunkRenderer::draw_items(ID3D11DeviceContext *ctx, const std::vector<WorldVertex> &vertices,
		const XMMATRIX &view_proj, const XMFLOAT3 &eye, const SkyParams &sky, bool textured) {
	if (vertices.empty() || !upload_dynamic(ctx, vertices)) {
		return;
	}
	ItemConstants k = {};
	XMStoreFloat4x4(&k.view_proj, view_proj); // (HLSL reads it transposed: mul(view_proj, v) == v * vp)
	k.eye = eye;
	k.fog_start = sky.fog_start;
	k.fog_end = sky.fog_end;
	k.fog_color = sky.fog_color;
	k.use_texture = textured ? 1.0f : 0.0f;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(items_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return;
	}
	std::memcpy(m.pData, &k, sizeof(k));
	ctx->Unmap(items_cb_.Get(), 0);
	ID3D11Buffer *cbs[] = {items_cb_.Get()};
	ctx->VSSetConstantBuffers(0, 1, cbs);
	ctx->PSSetConstantBuffers(0, 1, cbs);
	ctx->IASetInputLayout(layout_.Get());
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const UINT stride = sizeof(WorldVertex), offset = 0;
	ID3D11Buffer *vb = dynamic_vertices_.Get();
	ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
	ctx->VSSetShader(items_vs_.Get(), nullptr, 0);
	ctx->PSSetShader(items_ps_.Get(), nullptr, 0);
	ID3D11ShaderResourceView *srvs[] = {atlas_.Get()};
	ctx->PSSetShaderResources(0, 1, srvs);
	ID3D11SamplerState *samplers[] = {sampler_.Get()};
	ctx->PSSetSamplers(0, 1, samplers);
	ctx->RSSetState(cull_none_.Get());
	ctx->OMSetDepthStencilState(depth_write_.Get(), 0);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->Draw(UINT(vertices.size()), 0);
	ctx->RSSetState(nullptr);
	ctx->OMSetDepthStencilState(nullptr, 0);
}

void ChunkRenderer::set_hand_light(ID3D11DeviceContext *ctx, const std::vector<uint8_t> &volume, const BlockPos &origin,
		int count, bool changed) {
	hand_light_origin_ = {float(origin.x), float(origin.y), float(origin.z)};
	hand_light_count_ = volume.empty() ? 0 : count;
	const size_t size = size_t(HAND_LIGHT_SIZE) * HAND_LIGHT_SIZE * HAND_LIGHT_SIZE;
	if (changed && volume.size() == size) {
		ctx->UpdateSubresource(hand_light_tex_.Get(), 0, nullptr, volume.data(), HAND_LIGHT_SIZE,
				HAND_LIGHT_SIZE * HAND_LIGHT_SIZE);
	}
}

void ChunkRenderer::clear_depth(ID3D11DeviceContext *ctx) {
	ComPtr<ID3D11DepthStencilView> dsv;
	ctx->OMGetRenderTargets(0, nullptr, &dsv);
	if (dsv) {
		ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
	}
}

void ChunkRenderer::draw_sky(ID3D11DeviceContext *ctx, const Camera &camera, float aspect, const SkyParams &sky) {
	SkyConstants k = {};
	const XMMATRIX vp = camera.view() * camera.projection(aspect);
	XMStoreFloat4x4(&k.inv_view_proj, XMMatrixInverse(nullptr, vp));
	k.top = sky.sky_top;
	k.horizon = sky.sky_horizon;
	k.ground = sky.sky_ground;
	k.day = sky.day;
	k.dusk = sky.dusk;
	k.sky_angle = sky.sky_angle;
	XMStoreFloat3(&k.sun_dir, XMVector3Normalize(XMLoadFloat3(&sky.sun_dir)));
	k.time = sky.time;
	k.cloud_offset[0] = sky.cloud_offset[0];
	k.cloud_offset[1] = sky.cloud_offset[1];
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(sky_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return;
	}
	std::memcpy(m.pData, &k, sizeof(k));
	ctx->Unmap(sky_cb_.Get(), 0);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(sky_vs_.Get(), nullptr, 0);
	ctx->PSSetShader(sky_ps_.Get(), nullptr, 0);
	ID3D11Buffer *cbs[] = {sky_cb_.Get()};
	ctx->PSSetConstantBuffers(0, 1, cbs);
	ctx->RSSetState(cull_none_.Get());
	ctx->OMSetDepthStencilState(depth_off_.Get(), 0);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->Draw(3, 0);
	ctx->RSSetState(nullptr);
	ctx->OMSetDepthStencilState(nullptr, 0);
}

void ChunkRenderer::draw_outline(ID3D11DeviceContext *ctx, const std::vector<gocube::Box> &boxes) {
	if (boxes.empty()) {
		return;
	}
	std::vector<XMFLOAT3> v;
	for (size_t i = 0; i < boxes.size() && i < MAX_OUTLINE_BOXES; i++) {
		const float g = 0.002f;
		const float mn[3] = {boxes[i].mn[0] - g, boxes[i].mn[1] - g, boxes[i].mn[2] - g};
		const float mx[3] = {boxes[i].mx[0] + g, boxes[i].mx[1] + g, boxes[i].mx[2] + g};
		auto corner = [&](int c) { return XMFLOAT3{c & 1 ? mx[0] : mn[0], c & 2 ? mx[1] : mn[1], c & 4 ? mx[2] : mn[2]}; };
		for (int c = 0; c < 8; c++) { // each edge once: from a corner along each axis it isn't at the end of
			for (int bit : {1, 2, 4}) {
				if (!(c & bit)) {
					v.push_back(corner(c));
					v.push_back(corner(c | bit));
				}
			}
		}
	}
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(line_vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return;
	}
	std::memcpy(m.pData, v.data(), v.size() * sizeof(XMFLOAT3));
	ctx->Unmap(line_vertices_.Get(), 0);
	const XMFLOAT4 color = {0, 0, 0, 0.8f};
	if (SUCCEEDED(ctx->Map(line_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		std::memcpy(m.pData, &color, sizeof(color));
		ctx->Unmap(line_cb_.Get(), 0);
	}
	const UINT stride = sizeof(XMFLOAT3), offset = 0;
	ID3D11Buffer *vb = line_vertices_.Get();
	ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
	ctx->IASetInputLayout(line_layout_.Get());
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
	ctx->VSSetShader(line_vs_.Get(), nullptr, 0);
	ctx->PSSetShader(line_ps_.Get(), nullptr, 0);
	ID3D11Buffer *cbs[] = {frame_cb_.Get(), line_cb_.Get()};
	ctx->VSSetConstantBuffers(0, 2, cbs);
	ctx->PSSetConstantBuffers(0, 2, cbs);
	ctx->OMSetBlendState(alpha_blend_.Get(), nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(depth_less_equal_.Get(), 0);
	ctx->Draw(UINT(v.size()), 0);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
}

bool ChunkRenderer::make_surface(ID3D11Device *device, const gocube::Surface &s, GpuSurface &out) {
	out = {};
	if (s.empty()) {
		return true;
	}
	const size_t n = s.positions.size() / 3;
	std::vector<Vertex> vertices(n);
	for (size_t i = 0; i < n; i++) {
		Vertex &v = vertices[i];
		std::memcpy(v.pos, &s.positions[i * 3], sizeof(v.pos));
		std::memcpy(v.normal, &s.normals[i * 3], sizeof(v.normal));
		std::memcpy(v.uv, &s.uvs[i * 2], sizeof(v.uv));
		std::memcpy(v.color, &s.colors[i * 4], sizeof(v.color));
	}
	D3D11_BUFFER_DESC bd = {};
	bd.Usage = D3D11_USAGE_IMMUTABLE;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.ByteWidth = UINT(vertices.size() * sizeof(Vertex));
	D3D11_SUBRESOURCE_DATA data = {vertices.data()};
	if (FAILED(device->CreateBuffer(&bd, &data, &out.vertices))) {
		return false;
	}
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	bd.ByteWidth = UINT(s.indices.size() * sizeof(int32_t));
	data.pSysMem = s.indices.data();
	if (FAILED(device->CreateBuffer(&bd, &data, &out.indices))) {
		return false;
	}
	out.index_count = UINT(s.indices.size());
	return true;
}

void ChunkRenderer::upload(ID3D11Device *device, const ChunkMesh &mesh) {
	GpuChunk c;
	c.pos = mesh.pos;
	const gocube::Surface *surfaces[3] = {&mesh.opaque, &mesh.cutout, &mesh.fluid};
	for (int i = 0; i < 3; i++) {
		if (!make_surface(device, *surfaces[i], c.surfaces[i])) {
			log("Couldn't upload chunk (%d, %d, %d)", mesh.pos.x, mesh.pos.y, mesh.pos.z);
			return;
		}
	}
	chunks_[key(mesh.pos)] = std::move(c);
}

void ChunkRenderer::remove(const ChunkPos &pos) {
	chunks_.erase(key(pos));
}

void ChunkRenderer::draw(ID3D11DeviceContext *ctx, const Camera &camera, float aspect, const SkyParams &sky) {
	const XMMATRIX vp = camera.view() * camera.projection(aspect);
	FrameConstants f = {};
	XMStoreFloat4x4(&f.view_proj, vp); // (HLSL reads it transposed: mul(view_proj, v) == v * vp)
	f.camera_pos = camera.position;
	f.daylight = sky.daylight;
	XMStoreFloat3(&f.sun_dir, XMVector3Normalize(XMLoadFloat3(&sky.sun_dir)));
	f.sun_amount = sky.sun_amount;
	f.sky_tint = sky.sky_tint;
	f.min_light = 0.02f;
	f.fog_color = sky.fog_color;
	f.fog_start = sky.fog_start;
	f.fog_end = sky.fog_end;
	f.cloud_offset[0] = sky.cloud_offset[0];
	f.cloud_offset[1] = sky.cloud_offset[1];
	f.block_light_color = {1.0f, 0.85f, 0.62f}; // the Godot version's, as it is (raw: see chunk.hlsl)
	f.time = sky.time;
	f.hand_light_origin = hand_light_origin_;
	f.hand_light_count = float(hand_light_count_);
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(ctx->Map(frame_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		std::memcpy(m.pData, &f, sizeof(f));
		ctx->Unmap(frame_cb_.Get(), 0);
	}

	Plane planes[6];
	XMFLOAT4X4 vpm;
	XMStoreFloat4x4(&vpm, vp);
	frustum_planes(vpm, planes);

	ID3D11Buffer *cbs[] = {frame_cb_.Get(), draw_cb_.Get()};
	ctx->VSSetConstantBuffers(0, 2, cbs);
	ctx->PSSetConstantBuffers(0, 2, cbs);
	ctx->IASetInputLayout(layout_.Get());
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(vs_.Get(), nullptr, 0);
	ID3D11ShaderResourceView *srvs[] = {atlas_.Get(), hand_light_srv_.Get()};
	ctx->PSSetShaderResources(0, 2, srvs);
	ID3D11SamplerState *samplers[] = {sampler_.Get()};
	ctx->PSSetSamplers(0, 1, samplers);

	stats_ = {};
	stats_.chunks = int(chunks_.size());
	std::vector<const GpuChunk *> visible;
	for (const auto &[k, c] : chunks_) {
		const float mn[3] = {float(c.pos.x * gocube::CHUNK), float(c.pos.y * gocube::CHUNK), float(c.pos.z * gocube::CHUNK)};
		const float mx[3] = {mn[0] + gocube::CHUNK, mn[1] + gocube::CHUNK, mn[2] + gocube::CHUNK};
		if (box_visible(planes, mn, mx)) {
			visible.push_back(&c);
		}
	}
	stats_.drawn = int(visible.size());

	auto draw_surface = [&](const GpuChunk &c, int surface) {
		const GpuSurface &s = c.surfaces[surface];
		if (!s.index_count) {
			return;
		}
		DrawConstants d = {{float(c.pos.x * gocube::CHUNK), float(c.pos.y * gocube::CHUNK), float(c.pos.z * gocube::CHUNK)}, 0};
		D3D11_MAPPED_SUBRESOURCE dm;
		if (SUCCEEDED(ctx->Map(draw_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
			std::memcpy(dm.pData, &d, sizeof(d));
			ctx->Unmap(draw_cb_.Get(), 0);
		}
		const UINT stride = sizeof(Vertex), offset = 0;
		ID3D11Buffer *vb = s.vertices.Get();
		ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		ctx->IASetIndexBuffer(s.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
		ctx->DrawIndexed(s.index_count, 0, 0);
		stats_.triangles += s.index_count / 3;
	};

	// Opaque, then cutout (both write depth), then fluids blended on top.
	ctx->RSSetState(cull_back_.Get());
	ctx->OMSetDepthStencilState(depth_write_.Get(), 0);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->PSSetShader(ps_opaque_.Get(), nullptr, 0);
	for (const GpuChunk *c : visible) {
		draw_surface(*c, 0);
	}
	ctx->RSSetState(cull_none_.Get()); // plants are single quads seen from both sides
	ctx->PSSetShader(ps_cutout_.Get(), nullptr, 0);
	for (const GpuChunk *c : visible) {
		draw_surface(*c, 1);
	}
	ctx->OMSetDepthStencilState(depth_test_only_.Get(), 0);
	ctx->OMSetBlendState(alpha_blend_.Get(), nullptr, 0xffffffff);
	ctx->PSSetShader(ps_fluid_.Get(), nullptr, 0);
	for (const GpuChunk *c : visible) {
		draw_surface(*c, 2);
	}
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->RSSetState(nullptr);
}

} // namespace gc
