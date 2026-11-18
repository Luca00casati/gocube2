#pragma once
// Direct3D 11: device, flip-model swap chain, depth buffer; the UI drawn over it.
//
// Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell)
// graphics such as the HD 4600, where every Direct3D 12 draw hangs the GPU,
// while their Direct3D 11 drivers are mature. Feature level 11_0 is the minimum.
//
// The world is drawn into the plain back buffer: its shaders work out linear
// light and write it back through the sRGB curve (shaders/color.hlsli), so
// see-through surfaces blend in sRGB numbers; the UI's
// colours (Direct2D, ui/gui_d2d.h) are already what the screen shows. With a render scale below
// 1 the world is drawn into a smaller target first, then stretched over the
// window (bilinear), under the full-size UI.

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

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
	void resize(int width, int height);

	// The world's resolution as a part of the window's (0.25..1; the settings
	// offer 1, 0.85, 0.75 and 0.5).
	void set_render_scale(float scale);
	void set_vsync(bool on) { options_.vsync = on; } // (from the next frame)
	float render_scale() const { return render_scale_; }

	// Clears the world's target (to a linear colour; not if `clear_color` is
	// false: the sky will cover what nothing else does) and the depth buffer,
	// and binds them for drawing the world.
	void begin_frame(const float clear_linear[4], bool clear_color = true);
	// Calls `overlay` with the back buffer bound (the UI draws there), saves a
	// screenshot if `screenshot_png` is given, presents. False if the device
	// was lost.
	bool end_frame(const std::function<void()> &overlay = {}, const char *screenshot_png = nullptr);
	void shutdown();

	ID3D11Device *device() const { return device_.Get(); }
	ID3D11DeviceContext *context() const { return context_.Get(); }
	IDXGISwapChain1 *swap_chain() const { return swap_chain_.Get(); }
	int width() const { return width_; }
	int height() const { return height_; }
	const std::string &adapter_name() const { return adapter_name_; }
	// Errors and warnings the debug layer reported so far (each is also logged).
	int debug_messages();

	// The GPU's time for the parts of a frame (timestamp queries): from a mark
	// to the next one (or to the frame's end) is the mark's part. begin_frame
	// starts one ("clear"), end_frame marks the UI's. Read a few frames late
	// (never waiting for the GPU): the latest frame's times, (name, ms).
	void gpu_mark(const char *name);
	const std::vector<std::pair<std::string, double>> &gpu_times() const { return gpu_times_; }

private:
	struct GpuFrame {
		ComPtr<ID3D11Query> disjoint;
		std::vector<ComPtr<ID3D11Query>> stamps; // (reused)
		std::vector<std::string> names;          // the part each stamp starts; the last one ends the frame
		bool pending = false;
	};
	static constexpr int GPU_FRAMES = 4, GPU_MARKS = 16;
	GpuFrame gpu_[GPU_FRAMES];
	int gpu_frame_ = 0;
	bool gpu_open_ = false;
	std::vector<std::pair<std::string, double>> gpu_times_;
	void gpu_stamp(const char *name);
	void gpu_collect(GpuFrame &f);

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
	int debug_messages_ = 0;
};

} // namespace gc
