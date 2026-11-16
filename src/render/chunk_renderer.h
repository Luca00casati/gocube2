#pragma once
// Draws the world's chunk meshes: one vertex and index buffer per surface
// (opaque, cutout, fluid) per chunk, the block atlas, and the terrain shaders
// (shaders/chunk.hlsl). Chunks outside the view frustum are skipped.

#include "content/content.h"
#include "world/world.h"

#include <d3d11_1.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gc {

using Microsoft::WRL::ComPtr;

struct Camera {
	DirectX::XMFLOAT3 position = {0, 0, 0};
	float yaw = 0;   // radians; 0 looks towards -z, positive turns left
	float pitch = 0; // radians; positive looks up
	float fov_y = DirectX::XMConvertToRadians(75.0f); // (Godot's Camera3D default)
	float near_z = 0.05f, far_z = 1000.0f;

	DirectX::XMFLOAT3 forward() const; // unit view direction
	DirectX::XMMATRIX view() const;
	DirectX::XMMATRIX projection(float aspect) const;
};

struct SkyParams {
	float daylight = 1.0f;
	float sun_amount = 1.0f;
	DirectX::XMFLOAT3 sun_dir = {0.36f, 0.86f, 0.36f}; // towards the sun
	DirectX::XMFLOAT3 sky_tint = {1, 1, 1};
	DirectX::XMFLOAT3 fog_color = {0.62f, 0.78f, 0.95f}; // linear
	float fog_start = 58, fog_end = 122; // depth fog (see chunk.hlsl), by 3D distance
	float time = 0;
	// The sky (linear colours): gradient, sun, moon, stars, clouds.
	DirectX::XMFLOAT3 sky_top = {0.08f, 0.24f, 0.71f}, sky_horizon = {0.42f, 0.59f, 0.83f};
	DirectX::XMFLOAT3 sky_ground = {0.42f, 0.59f, 0.83f};
	float day = 1, dusk = 0, sky_angle = 1.57f;
	float cloud_offset[2] = {0, 0};
};

struct ChunkRenderStats {
	int chunks = 0;     // with a mesh on the GPU
	int drawn = 0;      // drawn this frame (in the frustum)
	int64_t triangles = 0;
};

class ChunkRenderer {
public:
	static constexpr int MAX_OUTLINE_BOXES = 16;
	bool init(ID3D11Device *device, const Atlas &atlas);
	void upload(ID3D11Device *device, const ChunkMesh &mesh); // replaces the chunk's mesh
	void remove(const ChunkPos &pos);
	// Draws into the bound render target and depth buffer.
	void draw(ID3D11DeviceContext *context, const Camera &camera, float aspect, const SkyParams &sky);
	// First of all: the sky behind everything.
	void draw_sky(ID3D11DeviceContext *context, const Camera &camera, float aspect, const SkyParams &sky);
	// Dropped items and the first-person hand (shaders/items.hlsl): vertices
	// placed already, `view_proj` takes them to the screen; their colour is the
	// light (times the texture if `textured`, else it's the whole colour).
	// Fog by distance from `eye`, as the terrain's. `cull_back`: faces wound
	// the other way aren't drawn (the held item, as the Godot version's hand
	// shader does: its extruded items are wound inside out, so it shows their
	// far side); otherwise both sides (drops).
	void draw_items(ID3D11DeviceContext *context, const std::vector<WorldVertex> &vertices,
			const DirectX::XMMATRIX &view_proj, const DirectX::XMFLOAT3 &eye, const SkyParams &sky, bool textured,
			bool cull_back = false);
	// Clears the depth buffer (the hand is drawn over everything).
	void clear_depth(ID3D11DeviceContext *context);
	// Torches held in hand (HandLights): their light levels, a cube of
	// HAND_LIGHT_SIZE blocks from `origin` ((y * size + z) * size + x), uploaded
	// when `changed`; none while `volume` is empty.
	static constexpr int HAND_LIGHT_SIZE = 48;
	void set_hand_light(ID3D11DeviceContext *context, const std::vector<uint8_t> &volume, const BlockPos &origin,
			int count, bool changed);
	// The block atlas for Dear ImGui (a plain view: ImGui draws without sRGB).
	ID3D11ShaderResourceView *ui_atlas() const { return atlas_.Get(); }
	// After draw(): the edges of these boxes (world coordinates), just outside
	// them so the lines don't fight with the faces.
	void draw_outline(ID3D11DeviceContext *context, const std::vector<gocube::Box> &boxes);
	const ChunkRenderStats &stats() const { return stats_; }

private:
	struct GpuSurface {
		ComPtr<ID3D11Buffer> vertices, indices;
		UINT index_count = 0;
	};
	struct GpuChunk {
		ChunkPos pos;
		GpuSurface surfaces[3]; // opaque, cutout, fluid
	};
	static int64_t key(const ChunkPos &p);
	static bool make_surface(ID3D11Device *device, const gocube::Surface &s, GpuSurface &out);

	std::unordered_map<int64_t, GpuChunk> chunks_;
	ComPtr<ID3D11VertexShader> vs_;
	ComPtr<ID3D11PixelShader> ps_opaque_, ps_cutout_, ps_fluid_;
	ComPtr<ID3D11InputLayout> layout_;
	ComPtr<ID3D11Buffer> frame_cb_, draw_cb_;
	ComPtr<ID3D11ShaderResourceView> atlas_; // plain (not sRGB) view: see init()
	ComPtr<ID3D11Buffer> dynamic_vertices_;
	UINT dynamic_capacity_ = 0;
	ComPtr<ID3D11SamplerState> sampler_;
	ComPtr<ID3D11RasterizerState> cull_back_, cull_none_;
	ComPtr<ID3D11DepthStencilState> depth_write_, depth_test_only_;
	ComPtr<ID3D11BlendState> alpha_blend_;
	ComPtr<ID3D11VertexShader> line_vs_;
	ComPtr<ID3D11PixelShader> line_ps_;
	ComPtr<ID3D11InputLayout> line_layout_;
	ComPtr<ID3D11Buffer> line_vertices_, line_cb_;
	ComPtr<ID3D11DepthStencilState> depth_less_equal_, depth_off_;
	ComPtr<ID3D11VertexShader> sky_vs_;
	ComPtr<ID3D11PixelShader> sky_ps_;
	ComPtr<ID3D11Buffer> sky_cb_;
	ComPtr<ID3D11VertexShader> items_vs_;
	ComPtr<ID3D11PixelShader> items_ps_;
	ComPtr<ID3D11Buffer> items_cb_;
	ComPtr<ID3D11Texture3D> hand_light_tex_;
	ComPtr<ID3D11ShaderResourceView> hand_light_srv_;
	DirectX::XMFLOAT3 hand_light_origin_ = {0, 0, 0};
	int hand_light_count_ = 0;
	bool upload_dynamic(ID3D11DeviceContext *context, const std::vector<WorldVertex> &vertices);
	ChunkRenderStats stats_;
};

} // namespace gc
