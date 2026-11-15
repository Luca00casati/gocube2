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
	float fov_y = DirectX::XMConvertToRadians(70.0f);
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
	float fog_start = 80, fog_end = 120;
	float time = 0;
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
	ComPtr<ID3D11ShaderResourceView> atlas_;
	ComPtr<ID3D11SamplerState> sampler_;
	ComPtr<ID3D11RasterizerState> cull_back_, cull_none_;
	ComPtr<ID3D11DepthStencilState> depth_write_, depth_test_only_;
	ComPtr<ID3D11BlendState> alpha_blend_;
	ComPtr<ID3D11VertexShader> line_vs_;
	ComPtr<ID3D11PixelShader> line_ps_;
	ComPtr<ID3D11InputLayout> line_layout_;
	ComPtr<ID3D11Buffer> line_vertices_, line_cb_;
	ComPtr<ID3D11DepthStencilState> depth_less_equal_;
	ChunkRenderStats stats_;
};

} // namespace gc
