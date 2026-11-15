#include "render/renderer.h"

#include "log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

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
	return create_render_target();
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

bool Renderer::create_render_target() {
	ComPtr<ID3D11Texture2D> back_buffer;
	if (failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "GetBuffer") ||
			failed(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &target_), "CreateRenderTargetView")) {
		return false;
	}
	return true;
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
	DXGI_SWAP_CHAIN_DESC1 sd;
	swap_chain_->GetDesc1(&sd);
	if (failed(swap_chain_->ResizeBuffers(BUFFERS, static_cast<UINT>(width), static_cast<UINT>(height), sd.Format,
					   sd.Flags),
				"ResizeBuffers")) {
		return;
	}
	create_render_target();
}

bool Renderer::render_frame(const float clear_color[4]) {
	// The flip model unbinds the back buffer at every Present, so bind it each frame.
	ID3D11RenderTargetView *targets[] = {target_.Get()};
	context_->OMSetRenderTargets(1, targets, nullptr);
	context_->ClearRenderTargetView(target_.Get(), clear_color);
	if (imgui_) {
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	const UINT flags = !options_.vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
	const HRESULT hr = swap_chain_->Present(options_.vsync ? 1 : 0, flags);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		log("Graphics device lost (reason 0x%08lx)", static_cast<unsigned long>(device_->GetDeviceRemovedReason()));
		return false;
	}
	return !failed(hr, "Present");
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
