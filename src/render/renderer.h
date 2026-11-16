#pragma once
// Direct3D 11: device, flip-model swap chain, depth buffer, Dear ImGui.
//
// Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell)
// graphics such as the HD 4600, where every Direct3D 12 draw hangs the GPU,
// while their Direct3D 11 drivers are mature. Feature level 11_0 is the minimum.
//
// The world is drawn into the plain back buffer: its shaders work out linear
// light and write it as the Godot version's renderer does (shaders/
// godot_color.hlsli), so see-through surfaces blend as there; Dear ImGui's
// colours are already what the screen shows. With a render scale below
// 1 the world is drawn into a smaller target first, then stretched over the
// window (bilinear), under the full-size UI: the Godot version's setting.

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
	static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

	Renderer() = default;
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	~Renderer();

	bool init(HWND hwnd, int width, int height, const RendererOptions &options);
	bool init_imgui(); // after ImGui_ImplWin32_Init
	void resize(int width, int height);

	// The world's resolution as a part of the window's (0.25..1; the Godot
	// version offers 1, 0.85, 0.75 and 0.5).
	void set_render_scale(float scale);
	float render_scale() const { return render_scale_; }

	// Clears the world's target (to a linear colour) and the depth buffer, and
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
	bool create_blit();
	bool save_png(const char *path);
	bool scaled() const { return scaled_target_ != nullptr; }

	RendererOptions options_;
	std::string adapter_name_;
	int width_ = 0, height_ = 0;
	ComPtr<IDXGIFactory2> factory_;
	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> context_;
	ComPtr<ID3D11InfoQueue> info_queue_;
	ComPtr<IDXGISwapChain1> swap_chain_;
	ComPtr<ID3D11RenderTargetView> target_;
	ComPtr<ID3D11DepthStencilView> depth_;
	// Render scale below 1: the world's smaller target, and the stretch.
	float render_scale_ = 1.0f;
	int scaled_width_ = 0, scaled_height_ = 0;
	ComPtr<ID3D11RenderTargetView> scaled_target_;
	ComPtr<ID3D11ShaderResourceView> scaled_view_;
	ComPtr<ID3D11VertexShader> blit_vs_;
	ComPtr<ID3D11PixelShader> blit_ps_;
	ComPtr<ID3D11SamplerState> bilinear_;
	bool tearing_ = false;
	bool imgui_ = false;
	int debug_messages_ = 0;
};

} // namespace gc
