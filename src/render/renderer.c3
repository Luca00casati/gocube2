// Direct3D 11: device, flip-model swap chain, depth buffer; the UI drawn over
// it.
//
// Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation
// (Haswell) graphics such as the HD 4600, where every Direct3D 12 draw hangs
// the GPU, while their Direct3D 11 drivers are mature. Feature level 11_0 is
// the minimum.
//
// The world is drawn into the plain back buffer: its shaders work out linear
// light and write it back through the sRGB curve (shaders/color.hlsli), so
// see-through surfaces blend in sRGB numbers; the UI's colours are already
// what the screen shows. With a render scale below 1 the world is drawn into
// a smaller target first, then stretched over the window (bilinear), under
// the full-size UI.
module gocube::renderer;
import gocube::d3d11, gocube::shaders, gocube::win, gocube::log, gocube::content::image, gocube::fs, gocube::util::vec;
import std::math;

struct RendererOptions
{
	bool debug; // Direct3D 11 debug layer (needs Windows' Graphics Tools)
	bool warp;  // software adapter: runs without a GPU (tests, CI)
	bool vsync;
}

const uint BUFFERS = 2; // swap chain buffers (the flip model needs at least 2)
const uint BACK_BUFFER_FORMAT = d3d11::FORMAT_R8G8B8A8_UNORM;
const uint DEPTH_FORMAT = d3d11::FORMAT_D32_FLOAT;
const int GPU_FRAMES = 4;
const int GPU_MARKS = 16;

struct GpuTime
{
	String name; // (a literal)
	double ms;
}

struct GpuFrame
{
	Query* disjoint;
	Query*[GPU_MARKS] stamps; // (reused)
	String[GPU_MARKS] names;  // the part each stamp starts; the last one ends the frame
	int count;
	bool pending;
}

struct Renderer
{
	RendererOptions options;
	String adapter_name; // (heap)
	int width, height;
	DxgiFactory* factory;
	Device* device;
	DeviceContext* context;
	SwapChain* swap_chain;
	RenderTargetView* target;
	DepthStencilView* depth;
	// Render scale below 1 or the retro filter: the world's own target, then
	// the last pass over it (the stretch, the filter: blit.hlsl).
	float render_scale;
	bool retro;
	Buffer* blit_cb;
	double blit_time;
	int scaled_width, scaled_height;
	RenderTargetView* scaled_target;
	ShaderResourceView* scaled_view;
	VertexShader* blit_vs;
	PixelShader* blit_ps;
	SamplerState* bilinear;
	GpuFrame[GPU_FRAMES] gpu;
	int gpu_frame;
	bool gpu_open;
	Vec{GpuTime} gpu_times;
}

fn bool failed(Hresult hr, String what) @private
{
	if (hr >= 0) return false;
	log::log("%s failed (HRESULT 0x%08x)", what, (uint)hr);
	return true;
}

fn bool Renderer.init(&self, Hwnd hwnd, int width, int height, RendererOptions options)
{
	*self = { .options = options, .render_scale = 1.0f };
	if (!self.create_device()) return false;
	// The factory that made the device, so the swap chain lands on the same adapter.
	DxgiDevice* dxgi_device;
	DxgiAdapter* adapter;
	if (failed(d3d11::query_interface(self.device, &&d3d11::IID_DXGI_DEVICE, (void**)&dxgi_device), "IDXGIDevice")) return false;
	defer d3d11::release(dxgi_device);
	if (failed(dxgi_device.get_adapter(&adapter), "GetAdapter")) return false;
	defer d3d11::release(adapter);
	if (failed(adapter.get_parent(&&d3d11::IID_DXGI_FACTORY2, (void**)&self.factory), "IDXGIFactory2")) return false;
	AdapterDesc desc;
	adapter.get_desc(&desc);
	self.adapter_name = win::utf8_of(mem, &desc.description);
	uint level = self.device.get_feature_level();
	log::log("Adapter: %s (feature level %d_%d)", self.adapter_name, (level >> 12) & 0xF, (level >> 8) & 0xF);
	SwapChainDesc1 sd = { .width = (uint)width, .height = (uint)height, .format = BACK_BUFFER_FORMAT, .sample_count = 1,
		.buffer_usage = d3d11::USAGE_RENDER_TARGET_OUTPUT, .buffer_count = BUFFERS, .swap_effect = d3d11::SWAP_EFFECT_FLIP_DISCARD };
	if (failed(self.factory.create_swap_chain_for_hwnd(self.device, hwnd, &sd, &self.swap_chain), "CreateSwapChainForHwnd")) return false;
	self.factory.make_window_association(hwnd, d3d11::MWA_NO_ALT_ENTER); // fullscreen comes later, our way
	self.width = width;
	self.height = height;
	return self.create_targets() && self.create_blit();
}

fn bool Renderer.create_device(&self)
{
	uint flags = d3d11::CREATE_DEVICE_BGRA_SUPPORT;
	if (self.options.debug) flags |= d3d11::CREATE_DEVICE_DEBUG;
	uint[2] levels = { d3d11::FEATURE_LEVEL_11_1, d3d11::FEATURE_LEVEL_11_0 };
	uint type = self.options.warp ? d3d11::DRIVER_TYPE_WARP : d3d11::DRIVER_TYPE_HARDWARE;
	Hresult hr = d3d11::create_device(null, type, null, flags, &levels, 2, d3d11::SDK_VERSION, &self.device, null, &self.context);
	if (hr == d3d11::ERROR_SDK_COMPONENT_MISSING && self.options.debug)
	{
		log::log("Direct3D 11 debug layer not available (install Windows' Graphics Tools); running without it");
		self.options.debug = false;
		hr = d3d11::create_device(null, type, null, flags & ~d3d11::CREATE_DEVICE_DEBUG, &levels, 2, d3d11::SDK_VERSION, &self.device, null, &self.context);
	}
	if (hr < 0)
	{
		log::log("No graphics adapter supports Direct3D 11 at feature level 11_0 (HRESULT 0x%08x; try --warp)", (uint)hr);
		return false;
	}
	return true;
}

fn bool Renderer.create_blit(&self)
{
	SamplerDesc sd = { .filter = d3d11::FILTER_MIN_MAG_MIP_LINEAR, .address_u = d3d11::TEXTURE_ADDRESS_CLAMP, .address_v = d3d11::TEXTURE_ADDRESS_CLAMP,
		.address_w = d3d11::TEXTURE_ADDRESS_CLAMP, .max_lod = d3d11::FLOAT32_MAX, .comparison_func = d3d11::COMPARISON_NEVER };
	BufferDesc cb = { .usage = d3d11::USAGE_DYNAMIC, .bind_flags = d3d11::BIND_CONSTANT_BUFFER, .cpu_access_flags = d3d11::CPU_ACCESS_WRITE,
		.byte_width = 16 };
	return !failed(self.device.create_vertex_shader(shaders::code(BLIT_VS), &self.blit_vs), "blit_vs") &&
		!failed(self.device.create_pixel_shader(shaders::code(BLIT_PS), &self.blit_ps), "blit_ps") &&
		!failed(self.device.create_sampler_state(&sd, &self.bilinear), "CreateSamplerState(bilinear)") &&
		!failed(self.device.create_buffer(&cb, null, &self.blit_cb), "CreateBuffer(blit)");
}

fn void Renderer.release_targets(&self)
{
	self.context.om_set_render_targets({}, null);
	d3d11::release(self.target);
	d3d11::release(self.depth);
	d3d11::release(self.scaled_target);
	d3d11::release(self.scaled_view);
	self.target = self.depth = self.scaled_target = self.scaled_view = null;
}

fn bool Renderer.create_targets(&self)
{
	Texture2D* back_buffer;
	if (failed(self.swap_chain.get_buffer(0, &back_buffer), "GetBuffer")) return false;
	defer d3d11::release(back_buffer);
	if (failed(self.device.create_render_target_view(back_buffer, &self.target), "CreateRenderTargetView")) return false;
	// Render scale or the filter: the world's own target (smaller, or the window's size).
	self.scaled_width = self.width;
	self.scaled_height = self.height;
	if (self.render_scale < 0.999f || self.retro)
	{
		float scale = self.render_scale < 0.999f ? self.render_scale : 1.0f;
		self.scaled_width = math::max(1, (int)((float)self.width * scale + 0.5f));
		self.scaled_height = math::max(1, (int)((float)self.height * scale + 0.5f));
		Texture2DDesc td = { .width = (uint)self.scaled_width, .height = (uint)self.scaled_height, .mip_levels = 1, .array_size = 1,
			.format = BACK_BUFFER_FORMAT, .sample_count = 1, .usage = d3d11::USAGE_DEFAULT, .bind_flags = d3d11::BIND_RENDER_TARGET | d3d11::BIND_SHADER_RESOURCE };
		Texture2D* scaled;
		if (failed(self.device.create_texture2d(&td, null, &scaled), "CreateTexture2D(scaled)")) return false;
		defer d3d11::release(scaled);
		if (failed(self.device.create_render_target_view(scaled, &self.scaled_target), "scaled target") ||
			failed(self.device.create_shader_resource_view(scaled, &self.scaled_view), "scaled view"))
		{
			d3d11::release(self.scaled_target);
			self.scaled_target = null;
			return false;
		}
	}
	Texture2DDesc dd = { .width = (uint)self.scaled_width, .height = (uint)self.scaled_height, .mip_levels = 1, .array_size = 1,
		.format = DEPTH_FORMAT, .sample_count = 1, .usage = d3d11::USAGE_DEFAULT, .bind_flags = d3d11::BIND_DEPTH_STENCIL };
	Texture2D* depth;
	if (failed(self.device.create_texture2d(&dd, null, &depth), "CreateTexture2D(depth)")) return false;
	defer d3d11::release(depth);
	return !failed(self.device.create_depth_stencil_view(depth, &self.depth), "CreateDepthStencilView");
}

fn bool Renderer.scaled(&self) => self.scaled_target != null;

// The world's resolution as a part of the window's (0.25..1).
fn void Renderer.set_render_scale(&self, float scale)
{
	scale = math::clamp(scale, 0.25f, 1.0f);
	if (scale == self.render_scale) return;
	self.render_scale = scale;
	if (self.swap_chain)
	{
		self.release_targets();
		self.create_targets();
	}
}

// The retro screen filter (blit.hlsl) on or off.
fn void Renderer.set_retro(&self, bool on)
{
	if (on == self.retro) return;
	self.retro = on;
	if (self.swap_chain)
	{
		self.release_targets();
		self.create_targets();
	}
}

fn void Renderer.resize(&self, int width, int height)
{
	if (!self.swap_chain || width <= 0 || height <= 0) return;
	self.release_targets();
	SwapChainDesc1 sd;
	self.swap_chain.get_desc1(&sd);
	if (failed(self.swap_chain.resize_buffers(BUFFERS, (uint)width, (uint)height, sd.format, sd.flags), "ResizeBuffers")) return;
	self.width = width;
	self.height = height;
	self.create_targets();
}

// --- GPU times ------------------------------------------------------------------------
// The GPU's time for the parts of a frame (timestamp queries): from a mark to
// the next one (or to the frame's end) is the mark's part. Read a few frames
// late (never waiting for the GPU).

fn void Renderer.gpu_stamp(&self, String name)
{
	GpuFrame* f = &self.gpu[self.gpu_frame];
	if (f.count >= GPU_MARKS) return;
	if (!f.stamps[f.count])
	{
		QueryDesc qd = { d3d11::QUERY_TIMESTAMP, 0 };
		if (self.device.create_query(&qd, &f.stamps[f.count]) < 0) return;
	}
	self.context.end(f.stamps[f.count]);
	f.names[f.count++] = name;
}

fn void Renderer.gpu_mark(&self, String name)
{
	if (self.gpu_open) self.gpu_stamp(name);
}

fn void Renderer.gpu_collect(&self, GpuFrame* f)
{
	if (!f.pending) return;
	f.pending = false;
	TimestampDisjoint dj;
	if (self.context.get_data(f.disjoint, &dj, TimestampDisjoint::size, d3d11::ASYNC_GETDATA_DONOTFLUSH) != d3d11::S_OK || dj.disjoint || dj.frequency == 0) return;
	ulong[GPU_MARKS] t;
	for (int i = 0; i < f.count; i++)
	{
		if (self.context.get_data(f.stamps[i], &t[i], ulong::size, d3d11::ASYNC_GETDATA_DONOTFLUSH) != d3d11::S_OK) return;
	}
	self.gpu_times.clear();
	if (f.count == 0) return;
	for (int i = 0; i + 1 < f.count; i++) self.gpu_times.push({ f.names[i], (double)(t[i + 1] - t[i]) * 1000.0 / (double)dj.frequency });
	self.gpu_times.push({ "frame", (double)(t[f.count - 1] - t[0]) * 1000.0 / (double)dj.frequency });
}

// Clears the world's target (to a linear colour; not if `clear_color` is
// false: the sky will cover what nothing else does) and the depth buffer, and
// binds them for drawing the world.
fn void Renderer.begin_frame(&self, float[4] clear_linear, bool clear_color = true)
{
	// GPU times: the oldest frame's are read, its queries start this one.
	self.gpu_frame = (self.gpu_frame + 1) % GPU_FRAMES;
	GpuFrame* gf = &self.gpu[self.gpu_frame];
	self.gpu_collect(gf);
	gf.count = 0;
	if (!gf.disjoint)
	{
		QueryDesc qd = { d3d11::QUERY_TIMESTAMP_DISJOINT, 0 };
		self.device.create_query(&qd, &gf.disjoint);
	}
	self.gpu_open = gf.disjoint != null;
	if (self.gpu_open)
	{
		self.context.begin(gf.disjoint);
		self.gpu_stamp("clear");
	}
	// The flip model unbinds the back buffer at every Present, so bind it each frame.
	RenderTargetView* world = self.scaled() ? self.scaled_target : self.target;
	self.context.om_set_render_targets({ world }, self.depth);
	float[4] clear;
	for (int k = 0; k < 3; k++) clear[k] = math::max(1.055f * math::pow(math::max(clear_linear[k], 0.0f), 0.416666667f) - 0.055f, 0.0f);
	clear[3] = clear_linear[3];
	if (clear_color) self.context.clear_render_target_view(world, clear);
	self.context.clear_depth(self.depth, 1.0f);
	self.context.rs_set_viewport({ 0, 0, (float)self.scaled_width, (float)self.scaled_height, 0, 1 });
}

alias Overlay = fn void(void* context);

// Calls `overlay` with the back buffer bound (the UI draws there), saves a
// screenshot if `screenshot_png` is given, presents. False if the device was
// lost.
fn bool Renderer.end_frame(&self, Overlay overlay = null, void* overlay_context = null, String screenshot_png = "")
{
	self.gpu_mark("scaling, UI");
	if (self.scaled())
	{
		// Stretch the world over the window, through the filter if it's on.
		self.blit_time += 1.0 / 60.0; // (the grain's frame: new every frame)
		float[4] k = { self.retro ? 1.0f : 0.0f, (float)(self.blit_time % 1000.0), (float)self.width, (float)self.height };
		MappedSubresource m;
		if (self.context.map(self.blit_cb, 0, d3d11::MAP_WRITE_DISCARD, &m) >= 0)
		{
			mem::copy(m.data, &k, 16);
			self.context.unmap(self.blit_cb, 0);
		}
		self.context.ps_set_constant_buffers(0, { self.blit_cb });
		self.context.om_set_render_targets({ self.target }, null);
		self.context.rs_set_viewport({ 0, 0, (float)self.width, (float)self.height, 0, 1 });
		self.context.ia_set_input_layout(null);
		self.context.ia_set_primitive_topology(d3d11::TOPOLOGY_TRIANGLELIST);
		self.context.vs_set_shader(self.blit_vs);
		self.context.ps_set_shader(self.blit_ps);
		self.context.ps_set_shader_resources(0, { self.scaled_view });
		self.context.ps_set_samplers(0, { self.bilinear });
		self.context.rs_set_state(null);
		self.context.om_set_blend_state(null);
		self.context.om_set_depth_stencil_state(null);
		self.context.draw(3, 0);
		self.context.ps_set_shader_resources(0, { null }); // (it's a render target again next frame)
	}
	self.context.om_set_render_targets({ self.target }, null);
	if (overlay) overlay(overlay_context);
	if (self.gpu_open)
	{
		self.gpu_stamp("end");
		self.context.end(self.gpu[self.gpu_frame].disjoint);
		self.gpu[self.gpu_frame].pending = true;
		self.gpu_open = false;
	}
	if (screenshot_png.len && !self.save_png(screenshot_png)) log::log("Couldn't save the screenshot %s", screenshot_png);
	Hresult hr = self.swap_chain.present(self.options.vsync ? 1 : 0, 0);
	if (hr == d3d11::ERROR_DEVICE_REMOVED || hr == d3d11::ERROR_DEVICE_RESET)
	{
		log::log("Graphics device lost (reason 0x%08x)", (uint)self.device.get_device_removed_reason());
		return false;
	}
	return !failed(hr, "Present");
}

// The back buffer as a PNG file.
fn bool Renderer.save_png(&self, String path)
{
	Texture2D* back_buffer;
	if (self.swap_chain.get_buffer(0, &back_buffer) < 0) return false;
	defer d3d11::release(back_buffer);
	Texture2DDesc d = { .width = (uint)self.width, .height = (uint)self.height, .mip_levels = 1, .array_size = 1, .format = BACK_BUFFER_FORMAT,
		.sample_count = 1, .usage = d3d11::USAGE_STAGING, .cpu_access_flags = d3d11::CPU_ACCESS_READ };
	Texture2D* staging;
	if (self.device.create_texture2d(&d, null, &staging) < 0) return false;
	defer d3d11::release(staging);
	self.context.copy_resource(staging, back_buffer);
	MappedSubresource m;
	if (self.context.map(staging, 0, d3d11::MAP_READ, &m) < 0) return false;
	char[] rgba = mem::alloc_array(char, (sz)d.width * d.height * 4);
	defer free(rgba.ptr);
	for (uint y = 0; y < d.height; y++)
	{
		char* row = (char*)m.data + (usz)y * m.row_pitch;
		rgba[(sz)y * d.width * 4 : (sz)d.width * 4] = row[: (sz)d.width * 4];
	}
	self.context.unmap(staging, 0);
	Vec{char} png;
	defer png.free();
	image::encode_png((int)d.width, (int)d.height, rgba, false, &png);
	return fs::save_atomically(path, png.array_view());
}

fn void Renderer.free(&self)
{
	if (self.context)
	{
		self.context.clear_state();
		self.context.flush();
	}
	foreach (&f : self.gpu)
	{
		d3d11::release(f.disjoint);
		foreach (q : f.stamps) d3d11::release(q);
	}
	self.gpu_times.free();
	d3d11::release(self.blit_vs);
	d3d11::release(self.blit_ps);
	d3d11::release(self.blit_cb);
	d3d11::release(self.bilinear);
	d3d11::release(self.target);
	d3d11::release(self.depth);
	d3d11::release(self.scaled_target);
	d3d11::release(self.scaled_view);
	d3d11::release(self.swap_chain);
	d3d11::release(self.factory);
	d3d11::release(self.context);
	d3d11::release(self.device);
	free(self.adapter_name.ptr);
	*self = {};
}
