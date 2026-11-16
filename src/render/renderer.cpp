#include "render/renderer.h"

#include "log.h"

#include "blit_ps.h"
#include "blit_vs.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <png.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace gc {

namespace {

bool failed(HRESULT hr, const char *what) {
	if (FAILED(hr)) {
		log("%s failed (HRESULT 0x%08lx)", what, static_cast<unsigned long>(hr));
		return true;
	}
	return false;
}

std::string narrow(const wchar_t *w) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	}
	return s;
}

} // namespace

Renderer::~Renderer() {
	shutdown();
}

bool Renderer::init(HWND hwnd, int width, int height, const RendererOptions &options) {
	options_ = options;
	if (!create_device()) {
		return false;
	}

	// The factory that made the device, so the swap chain lands on the same adapter.
	ComPtr<IDXGIDevice> dxgi_device;
	ComPtr<IDXGIAdapter> adapter;
	if (failed(device_.As(&dxgi_device), "IDXGIDevice") || failed(dxgi_device->GetAdapter(&adapter), "GetAdapter") ||
			failed(adapter->GetParent(IID_PPV_ARGS(&factory_)), "IDXGIFactory2")) {
		return false;
	}
	DXGI_ADAPTER_DESC desc;
	adapter->GetDesc(&desc);
	adapter_name_ = narrow(desc.Description);
	log("Adapter: %s (feature level %u_%u)", adapter_name_.c_str(), (device_->GetFeatureLevel() >> 12) & 0xfu,
			(device_->GetFeatureLevel() >> 8) & 0xfu);

	ComPtr<IDXGIFactory5> factory5;
	BOOL tearing = FALSE;
	tearing_ = SUCCEEDED(factory_.As(&factory5)) &&
			SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) &&
			tearing;

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = static_cast<UINT>(width);
	sd.Height = static_cast<UINT>(height);
	sd.Format = BACK_BUFFER_FORMAT;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = BUFFERS;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	if (failed(factory_->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_chain_),
				"CreateSwapChainForHwnd")) {
		return false;
	}
	factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); // fullscreen comes later, our way
	width_ = width;
	height_ = height;
	return create_targets() && create_blit();
}

bool Renderer::create_blit() {
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	return !failed(device_->CreateVertexShader(g_blit_vs, sizeof(g_blit_vs), nullptr, &blit_vs_), "blit_vs") &&
			!failed(device_->CreatePixelShader(g_blit_ps, sizeof(g_blit_ps), nullptr, &blit_ps_), "blit_ps") &&
			!failed(device_->CreateSamplerState(&sd, &bilinear_), "CreateSamplerState(bilinear)");
}

void Renderer::set_render_scale(float scale) {
	scale = std::clamp(scale, 0.25f, 1.0f);
	if (scale == render_scale_) {
		return;
	}
	render_scale_ = scale;
	if (swap_chain_) {
		context_->OMSetRenderTargets(0, nullptr, nullptr);
		target_.Reset();
		world_target_.Reset();
		depth_.Reset();
		create_targets();
	}
}

bool Renderer::create_device() {
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	if (options_.debug) {
		flags |= D3D11_CREATE_DEVICE_DEBUG;
	}
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	const D3D_DRIVER_TYPE type = options_.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
	auto create = [&](UINT f) {
		return D3D11CreateDevice(nullptr, type, nullptr, f, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
				&device_, nullptr, &context_);
	};
	HRESULT hr = create(flags);
	if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && options_.debug) {
		log("Direct3D 11 debug layer not available (install Windows' Graphics Tools); running without it");
		options_.debug = false;
		hr = create(flags & ~D3D11_CREATE_DEVICE_DEBUG);
	}
	if (FAILED(hr)) {
		log("No graphics adapter supports Direct3D 11 at feature level 11_0 (HRESULT 0x%08lx; try --warp)",
				static_cast<unsigned long>(hr));
		return false;
	}
	if (options_.debug) {
		device_.As(&info_queue_);
	}
	return true;
}

bool Renderer::create_targets() {
	ComPtr<ID3D11Texture2D> back_buffer;
	if (failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "GetBuffer") ||
			failed(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &target_), "CreateRenderTargetView")) {
		return false;
	}
	D3D11_RENDER_TARGET_VIEW_DESC rd = {};
	rd.Format = WORLD_VIEW_FORMAT;
	rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (failed(device_->CreateRenderTargetView(back_buffer.Get(), &rd, &world_target_), "CreateRenderTargetView(sRGB)")) {
		return false;
	}
	// Render scale: the world's own smaller target.
	scaled_target_.Reset();
	scaled_view_.Reset();
	scaled_width_ = width_;
	scaled_height_ = height_;
	if (render_scale_ < 0.999f) {
		scaled_width_ = std::max(1, int(float(width_) * render_scale_ + 0.5f));
		scaled_height_ = std::max(1, int(float(height_) * render_scale_ + 0.5f));
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = static_cast<UINT>(scaled_width_);
		td.Height = static_cast<UINT>(scaled_height_);
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = WORLD_VIEW_FORMAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> scaled;
		if (failed(device_->CreateTexture2D(&td, nullptr, &scaled), "CreateTexture2D(scaled)") ||
				failed(device_->CreateRenderTargetView(scaled.Get(), nullptr, &scaled_target_), "scaled target") ||
				failed(device_->CreateShaderResourceView(scaled.Get(), nullptr, &scaled_view_), "scaled view")) {
			scaled_target_.Reset();
			return false;
		}
	}
	D3D11_TEXTURE2D_DESC dd = {};
	dd.Width = static_cast<UINT>(scaled_width_);
	dd.Height = static_cast<UINT>(scaled_height_);
	dd.MipLevels = 1;
	dd.ArraySize = 1;
	dd.Format = DEPTH_FORMAT;
	dd.SampleDesc.Count = 1;
	dd.Usage = D3D11_USAGE_DEFAULT;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ComPtr<ID3D11Texture2D> depth;
	return !failed(device_->CreateTexture2D(&dd, nullptr, &depth), "CreateTexture2D(depth)") &&
			!failed(device_->CreateDepthStencilView(depth.Get(), nullptr, &depth_), "CreateDepthStencilView");
}

bool Renderer::init_imgui() {
	imgui_ = ImGui_ImplDX11_Init(device_.Get(), context_.Get());
	return imgui_;
}

void Renderer::resize(int width, int height) {
	if (!swap_chain_ || width <= 0 || height <= 0) {
		return;
	}
	context_->OMSetRenderTargets(0, nullptr, nullptr);
	target_.Reset();
	world_target_.Reset();
	depth_.Reset();
	DXGI_SWAP_CHAIN_DESC1 sd;
	swap_chain_->GetDesc1(&sd);
	if (failed(swap_chain_->ResizeBuffers(BUFFERS, static_cast<UINT>(width), static_cast<UINT>(height), sd.Format,
					   sd.Flags),
				"ResizeBuffers")) {
		return;
	}
	width_ = width;
	height_ = height;
	create_targets();
}

void Renderer::begin_frame(const float clear_linear[4]) {
	// The flip model unbinds the back buffer at every Present, so bind it each frame.
	ID3D11RenderTargetView *world = scaled() ? scaled_target_.Get() : world_target_.Get();
	context_->OMSetRenderTargets(1, &world, depth_.Get());
	context_->ClearRenderTargetView(world, clear_linear);
	context_->ClearDepthStencilView(depth_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
	D3D11_VIEWPORT vp = {0, 0, float(scaled_width_), float(scaled_height_), 0.0f, 1.0f};
	context_->RSSetViewports(1, &vp);
}

bool Renderer::end_frame(const char *screenshot_png) {
	if (scaled()) {
		// Stretch the world over the window (through sRGB views: filtered in linear light).
		ID3D11RenderTargetView *full[] = {world_target_.Get()};
		context_->OMSetRenderTargets(1, full, nullptr);
		D3D11_VIEWPORT vp = {0, 0, float(width_), float(height_), 0.0f, 1.0f};
		context_->RSSetViewports(1, &vp);
		context_->IASetInputLayout(nullptr);
		context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context_->VSSetShader(blit_vs_.Get(), nullptr, 0);
		context_->PSSetShader(blit_ps_.Get(), nullptr, 0);
		ID3D11ShaderResourceView *views[] = {scaled_view_.Get()};
		context_->PSSetShaderResources(0, 1, views);
		ID3D11SamplerState *samplers[] = {bilinear_.Get()};
		context_->PSSetSamplers(0, 1, samplers);
		context_->RSSetState(nullptr);
		context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context_->OMSetDepthStencilState(nullptr, 0);
		context_->Draw(3, 0);
		ID3D11ShaderResourceView *none[] = {nullptr}; // (it's a render target again next frame)
		context_->PSSetShaderResources(0, 1, none);
	}
	ID3D11RenderTargetView *targets[] = {target_.Get()};
	context_->OMSetRenderTargets(1, targets, nullptr);
	if (imgui_) {
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}
	if (screenshot_png && !save_png(screenshot_png)) {
		log("Couldn't save the screenshot %s", screenshot_png);
	}

	const UINT flags = !options_.vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
	const HRESULT hr = swap_chain_->Present(options_.vsync ? 1 : 0, flags);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		log("Graphics device lost (reason 0x%08lx)", static_cast<unsigned long>(device_->GetDeviceRemovedReason()));
		return false;
	}
	return !failed(hr, "Present");
}

bool Renderer::save_png(const char *path) {
	ComPtr<ID3D11Texture2D> back_buffer;
	if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
		return false;
	}
	D3D11_TEXTURE2D_DESC d;
	back_buffer->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;
	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging))) {
		return false;
	}
	context_->CopyResource(staging.Get(), back_buffer.Get());
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
		return false;
	}
	std::vector<uint8_t> rgb(size_t(d.Width) * d.Height * 3);
	for (UINT y = 0; y < d.Height; y++) {
		const uint8_t *row = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
		for (UINT x = 0; x < d.Width; x++) {
			std::copy(row + x * 4, row + x * 4 + 3, &rgb[(size_t(y) * d.Width + x) * 3]);
		}
	}
	context_->Unmap(staging.Get(), 0);
	// (The path is UTF-8: opened through its wide form.)
	const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
	std::wstring wide(n > 0 ? size_t(n) : 1, L'\0');
	if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), n)) {
		return false;
	}
	FILE *f = nullptr;
	if (_wfopen_s(&f, wide.c_str(), L"wb") != 0 || !f) {
		return false;
	}
	png_image image = {};
	image.version = PNG_IMAGE_VERSION;
	image.width = d.Width;
	image.height = d.Height;
	image.format = PNG_FORMAT_RGB;
	const bool ok = png_image_write_to_stdio(&image, f, 0, rgb.data(), png_int_32(d.Width * 3), nullptr) != 0;
	png_image_free(&image);
	return fclose(f) == 0 && ok;
}

int Renderer::debug_messages() {
	if (!info_queue_) {
		return debug_messages_;
	}
	const UINT64 n = info_queue_->GetNumStoredMessages();
	for (UINT64 m = 0; m < n; m++) {
		SIZE_T size = 0;
		info_queue_->GetMessage(m, nullptr, &size);
		std::vector<char> buf(size);
		auto *msg = reinterpret_cast<D3D11_MESSAGE *>(buf.data());
		if (FAILED(info_queue_->GetMessage(m, msg, &size))) {
			continue;
		}
		if (msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) { // corruption, error, warning
			log("D3D11: %.*s", static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
			debug_messages_++;
		}
	}
	info_queue_->ClearStoredMessages();
	return debug_messages_;
}

void Renderer::shutdown() {
	if (imgui_) {
		ImGui_ImplDX11_Shutdown();
		imgui_ = false;
	}
	if (context_) {
		context_->ClearState();
		context_->Flush();
	}
}

} // namespace gc
