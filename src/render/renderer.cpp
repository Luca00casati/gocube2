#include "render/renderer.h"

#include "log.h"
#include "platform/utf8.h"

#include "blit_ps.h"
#include "blit_vs.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
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
		td.Format = BACK_BUFFER_FORMAT;
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

void Renderer::resize(int width, int height) {
	if (!swap_chain_ || width <= 0 || height <= 0) {
		return;
	}
	context_->OMSetRenderTargets(0, nullptr, nullptr);
	target_.Reset();
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

// --- GPU times --------------------------------------------------------------------

void Renderer::gpu_stamp(const char *name) {
	GpuFrame &f = gpu_[gpu_frame_];
	const size_t i = f.names.size();
	if (i >= size_t(GPU_MARKS)) {
		return;
	}
	if (i >= f.stamps.size()) {
		D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP, 0};
		ComPtr<ID3D11Query> q;
		if (FAILED(device_->CreateQuery(&qd, &q))) {
			return;
		}
		f.stamps.push_back(q);
	}
	context_->End(f.stamps[i].Get());
	f.names.push_back(name);
}

void Renderer::gpu_mark(const char *name) {
	if (gpu_open_) {
		gpu_stamp(name);
	}
}

// A frame's times, if the GPU is done with them (else they're dropped: the
// queries are needed again).
void Renderer::gpu_collect(GpuFrame &f) {
	if (!f.pending) {
		return;
	}
	f.pending = false;
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
	if (context_->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint ||
			dj.Frequency == 0) {
		return;
	}
	std::vector<UINT64> t(f.names.size());
	for (size_t i = 0; i < t.size(); i++) {
		if (context_->GetData(f.stamps[i].Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
			return;
		}
	}
	gpu_times_.clear();
	for (size_t i = 0; i + 1 < t.size(); i++) {
		gpu_times_.push_back({f.names[i], double(t[i + 1] - t[i]) * 1000.0 / double(dj.Frequency)});
	}
	gpu_times_.push_back({"frame", double(t.back() - t.front()) * 1000.0 / double(dj.Frequency)});
}

void Renderer::begin_frame(const float clear_linear[4], bool clear_color) {
	// GPU times: the oldest frame's are read, its queries start this one.
	gpu_frame_ = (gpu_frame_ + 1) % GPU_FRAMES;
	GpuFrame &gf = gpu_[gpu_frame_];
	gpu_collect(gf);
	gf.names.clear();
	if (!gf.disjoint) {
		D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		device_->CreateQuery(&qd, &gf.disjoint);
	}
	gpu_open_ = gf.disjoint != nullptr;
	if (gpu_open_) {
		context_->Begin(gf.disjoint.Get());
		gpu_stamp("clear");
	}
	// The flip model unbinds the back buffer at every Present, so bind it each frame.
	ID3D11RenderTargetView *world = scaled() ? scaled_target_.Get() : target_.Get();
	context_->OMSetRenderTargets(1, &world, depth_.Get());
	//
	float clear[4];
	for (int k = 0; k < 3; k++) {
		clear[k] = std::max(1.055f * std::pow(std::max(clear_linear[k], 0.0f), 0.416666667f) - 0.055f, 0.0f);
	}
	clear[3] = clear_linear[3];
	if (clear_color) {
		context_->ClearRenderTargetView(world, clear);
	}
	context_->ClearDepthStencilView(depth_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
	D3D11_VIEWPORT vp = {0, 0, float(scaled_width_), float(scaled_height_), 0.0f, 1.0f};
	context_->RSSetViewports(1, &vp);
}

bool Renderer::end_frame(const std::function<void()> &overlay, const char *screenshot_png) {
	gpu_mark("scaling, UI");
	if (scaled()) {
		// Stretch the world over the window.
		ID3D11RenderTargetView *full[] = {target_.Get()};
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
	if (overlay) {
		overlay();
	}
	if (gpu_open_) {
		gpu_stamp("end");
		context_->End(gpu_[gpu_frame_].disjoint.Get());
		gpu_[gpu_frame_].pending = true;
		gpu_open_ = false;
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
	std::vector<uint8_t> bgr(size_t(d.Width) * d.Height * 3);
	for (UINT y = 0; y < d.Height; y++) {
		const uint8_t *row = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
		for (UINT x = 0; x < d.Width; x++) {
			uint8_t *o = &bgr[(size_t(y) * d.Width + x) * 3];
			o[0] = row[x * 4 + 2];
			o[1] = row[x * 4 + 1];
			o[2] = row[x * 4 + 0];
		}
	}
	context_->Unmap(staging.Get(), 0);
	// (The path is UTF-8: opened through its wide form.)
	const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
	std::wstring wide(n > 0 ? size_t(n) : 1, L'\0');
	if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), n)) {
		return false;
	}
	// Written by Windows' own PNG encoder (WIC).
	const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool ok = false;
	{
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapEncoder> encoder;
		ComPtr<IWICBitmapFrameEncode> frame;
		WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
		ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
				SUCCEEDED(factory->CreateStream(&stream)) &&
				SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) &&
				SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
				SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
				SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
				SUCCEEDED(frame->SetSize(d.Width, d.Height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
				format == GUID_WICPixelFormat24bppBGR &&
				SUCCEEDED(frame->WritePixels(d.Height, d.Width * 3, UINT(bgr.size()), bgr.data())) &&
				SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
	}
	if (SUCCEEDED(co)) {
		CoUninitialize();
	}
	return ok;
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
	if (context_) {
		context_->ClearState();
		context_->Flush();
	}
}

} // namespace gc
