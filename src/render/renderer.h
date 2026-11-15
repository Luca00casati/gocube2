#pragma once
// Direct3D 11: device, flip-model swap chain, Dear ImGui.
//
// Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell)
// graphics such as the HD 4600, where every Direct3D 12 draw hangs the GPU,
// while their Direct3D 11 drivers are mature. Feature level 11_0 is the minimum.

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

	Renderer() = default;
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	~Renderer();

	bool init(HWND hwnd, int width, int height, const RendererOptions &options);
	bool init_imgui(); // after ImGui_ImplWin32_Init
	void resize(int width, int height);
	// Clears the back buffer, draws the ImGui draw data and presents.
	// False if the device was lost.
	bool render_frame(const float clear_color[4]);
	void shutdown();

	const std::string &adapter_name() const { return adapter_name_; }
	// Errors and warnings the debug layer reported so far (each is also logged).
	int debug_messages();

private:
	bool create_device();
	bool create_render_target();

	RendererOptions options_;
	std::string adapter_name_;
	ComPtr<IDXGIFactory2> factory_;
	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> context_;
	ComPtr<ID3D11InfoQueue> info_queue_;
	ComPtr<IDXGISwapChain1> swap_chain_;
	ComPtr<ID3D11RenderTargetView> target_;
	bool tearing_ = false;
	bool imgui_ = false;
	int debug_messages_ = 0;
};

} // namespace gc
