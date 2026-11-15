#pragma once
// Direct3D 11: device, flip-model swap chain, depth buffer, Dear ImGui.
//
// Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell)
// graphics such as the HD 4600, where every Direct3D 12 draw hangs the GPU,
// while their Direct3D 11 drivers are mature. Feature level 11_0 is the minimum.
//
// The world is drawn through an sRGB view of the back buffer (textures are
// sRGB, lighting is linear, as in the Godot version); Dear ImGui through a
// plain view (its colours are already what the screen shows).

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace gc {

using Microsoft::WRL::ComPtr;

struct RendererOptions {
	bool debug = false; // Direct3D 11 debug layer (needs Windows' Graphics Tools)
	bool warp = false; // software adapter: runs without a GPU (tests, CI)
	bool vsync = true;
};

class Renderer {
public:
	static constexpr UINT BUFFERS = 2; // swap chain buffers (the flip model needs at least 2)
	static constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
	static constexpr DXGI_FORMAT WORLD_VIEW_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

	Renderer() = default;
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	~Renderer();

	bool init(HWND hwnd, int width, int height, const RendererOptions &options);
	bool init_imgui(); // after ImGui_ImplWin32_Init
	void resize(int width, int height);

	// Clears the back buffer (to a linear colour) and the depth buffer, and
	// binds them for drawing the world.
	void begin_frame(const float clear_linear[4]);
	// Draws the ImGui draw data, saves a screenshot if `screenshot_png` is
	// given, presents. False if the device was lost.
	bool end_frame(const char *screenshot_png = nullptr);
	void shutdown();

	ID3D11Device *device() const { return device_.Get(); }
	ID3D11DeviceContext *context() const { return context_.Get(); }
	int width() const { return width_; }
	int height() const { return height_; }
	const std::string &adapter_name() const { return adapter_name_; }
	// Errors and warnings the debug layer reported so far (each is also logged).
	int debug_messages();

private:
	bool create_device();
	bool create_targets();
	bool save_png(const char *path);

	RendererOptions options_;
	std::string adapter_name_;
	int width_ = 0, height_ = 0;
	ComPtr<IDXGIFactory2> factory_;
	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> context_;
	ComPtr<ID3D11InfoQueue> info_queue_;
	ComPtr<IDXGISwapChain1> swap_chain_;
	ComPtr<ID3D11RenderTargetView> target_;       // plain (ImGui)
	ComPtr<ID3D11RenderTargetView> world_target_; // sRGB (the world)
	ComPtr<ID3D11DepthStencilView> depth_;
	bool tearing_ = false;
	bool imgui_ = false;
	int debug_messages_ = 0;
};

} // namespace gc
