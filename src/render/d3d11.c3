// Direct3D 11 and DXGI, as much as the game uses: the descriptions as
// Windows lays them out (x64), the constants, and typed calls through the
// vtables (d3d11_vtables.c3, generated from the SDK headers). Objects are
// plain pointers; release() any of them.
module gocube::d3d11 @link("d3d11", "dxgi");
import gocube::win;


// Every other object: just a COM object to the calls that take it.
alias Buffer = Unknown;
alias Texture2D = Unknown;
alias Texture3D = Unknown;
alias Resource = Unknown;
alias ShaderResourceView = Unknown;
alias RenderTargetView = Unknown;
alias DepthStencilView = Unknown;
alias InputLayout = Unknown;
alias VertexShader = Unknown;
alias PixelShader = Unknown;
alias BlendState = Unknown;
alias DepthStencilState = Unknown;
alias RasterizerState = Unknown;
alias SamplerState = Unknown;
alias Query = Unknown;

struct Guid
{
	uint data1;
	ushort data2, data3;
	char[8] data4;
}

const Guid IID_DXGI_DEVICE = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
const Guid IID_DXGI_FACTORY2 = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
const Guid IID_TEXTURE2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

// --- Descriptions --------------------------------------------------------------------

struct BufferDesc
{
	uint byte_width, usage, bind_flags, cpu_access_flags, misc_flags, structure_byte_stride;
}

struct SubresourceData
{
	void* data;
	uint pitch, slice_pitch;
}

struct Texture2DDesc
{
	uint width, height, mip_levels, array_size, format, sample_count, sample_quality, usage, bind_flags, cpu_access_flags, misc_flags;
}

struct Texture3DDesc
{
	uint width, height, depth, mip_levels, format, usage, bind_flags, cpu_access_flags, misc_flags;
}

struct SamplerDesc
{
	uint filter, address_u, address_v, address_w;
	float mip_lod_bias;
	uint max_anisotropy, comparison_func;
	float[4] border_color;
	float min_lod, max_lod;
}

struct RasterizerDesc
{
	uint fill_mode, cull_mode;
	Bool front_counter_clockwise;
	int depth_bias;
	float depth_bias_clamp, slope_scaled_depth_bias;
	Bool depth_clip_enable, scissor_enable, multisample_enable, antialiased_line_enable;
}

struct RenderTargetBlendDesc
{
	Bool blend_enable;
	uint src_blend, dest_blend, blend_op, src_blend_alpha, dest_blend_alpha, blend_op_alpha;
	char write_mask;
}

struct BlendDesc
{
	Bool alpha_to_coverage_enable, independent_blend_enable;
	RenderTargetBlendDesc[8] render_target;
}

struct DepthStencilOpDesc
{
	uint fail_op, depth_fail_op, pass_op, func;
}

struct DepthStencilDesc
{
	Bool depth_enable;
	uint depth_write_mask, depth_func;
	Bool stencil_enable;
	char stencil_read_mask, stencil_write_mask;
	DepthStencilOpDesc front_face, back_face;
}

struct InputElementDesc
{
	ZString semantic_name;
	uint semantic_index, format, input_slot, aligned_byte_offset, input_slot_class, instance_data_step_rate;
}

struct Viewport
{
	float x, y, width, height, min_depth, max_depth;
}

struct Rect
{
	int left, top, right, bottom;
}

struct Box
{
	uint left, top, front, right, bottom, back;
}

struct MappedSubresource
{
	void* data;
	uint row_pitch, depth_pitch;
}

struct QueryDesc
{
	uint query, misc_flags;
}

struct TimestampDisjoint
{
	ulong frequency;
	Bool disjoint;
}

struct SwapChainDesc1
{
	uint width, height, format;
	Bool stereo;
	uint sample_count, sample_quality, buffer_usage, buffer_count, scaling, swap_effect, alpha_mode, flags;
}

struct AdapterDesc
{
	WChar[128] description;
	uint vendor_id, device_id, sub_sys_id, revision;
	usz dedicated_video_memory, dedicated_system_memory, shared_system_memory;
	uint luid_low;
	int luid_high;
}

// --- Constants ------------------------------------------------------------------------

const uint USAGE_DEFAULT = 0;
const uint USAGE_IMMUTABLE = 1;
const uint USAGE_DYNAMIC = 2;
const uint USAGE_STAGING = 3;
const uint BIND_VERTEX_BUFFER = 0x1;
const uint BIND_INDEX_BUFFER = 0x2;
const uint BIND_CONSTANT_BUFFER = 0x4;
const uint BIND_SHADER_RESOURCE = 0x8;
const uint BIND_RENDER_TARGET = 0x20;
const uint BIND_DEPTH_STENCIL = 0x40;
const uint CPU_ACCESS_WRITE = 0x10000;
const uint CPU_ACCESS_READ = 0x20000;
const uint RESOURCE_MISC_GENERATE_MIPS = 0x1;
const uint MAP_READ = 1;
const uint MAP_WRITE_DISCARD = 4;
const uint BLEND_ONE = 2;
const uint BLEND_SRC_ALPHA = 5;
const uint BLEND_INV_SRC_ALPHA = 6;
const uint BLEND_OP_ADD = 1;
const char COLOR_WRITE_ENABLE_ALL = 15;
const uint COMPARISON_NEVER = 1;
const uint COMPARISON_LESS = 2;
const uint COMPARISON_LESS_EQUAL = 4;
const uint COMPARISON_ALWAYS = 8;
const uint FILL_SOLID = 3;
const uint CULL_NONE = 1;
const uint CULL_BACK = 3;
const uint DEPTH_WRITE_MASK_ZERO = 0;
const uint DEPTH_WRITE_MASK_ALL = 1;
const uint FILTER_MIN_MAG_MIP_POINT = 0;
const uint FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1;
const uint FILTER_MIN_MAG_MIP_LINEAR = 0x15;
const uint TEXTURE_ADDRESS_WRAP = 1;
const uint TEXTURE_ADDRESS_CLAMP = 3;
const uint INPUT_PER_VERTEX_DATA = 0;
const uint TOPOLOGY_LINELIST = 2;
const uint TOPOLOGY_TRIANGLELIST = 4;
const uint QUERY_TIMESTAMP = 2;
const uint QUERY_TIMESTAMP_DISJOINT = 3;
const uint ASYNC_GETDATA_DONOTFLUSH = 1;
const uint CLEAR_DEPTH = 1;
const uint CREATE_DEVICE_DEBUG = 0x2;
const uint CREATE_DEVICE_BGRA_SUPPORT = 0x20;
const uint SDK_VERSION = 7;
const uint FEATURE_LEVEL_11_0 = 0xb000;
const uint FEATURE_LEVEL_11_1 = 0xb100;
const uint DRIVER_TYPE_HARDWARE = 1;
const uint DRIVER_TYPE_WARP = 5;
const float FLOAT32_MAX = 3.402823466e+38f;
const uint VIEWPORT_AND_SCISSOR_COUNT = 16;

const uint FORMAT_UNKNOWN = 0;
const uint FORMAT_R32G32B32A32_FLOAT = 2;
const uint FORMAT_R32G32B32_FLOAT = 6;
const uint FORMAT_R32G32_FLOAT = 16;
const uint FORMAT_R8G8B8A8_UNORM = 28;
const uint FORMAT_D32_FLOAT = 40;
const uint FORMAT_R32_UINT = 42;
const uint FORMAT_R8_UNORM = 61;

const uint USAGE_RENDER_TARGET_OUTPUT = 0x20;
const uint SWAP_EFFECT_FLIP_DISCARD = 4;
const uint MWA_NO_ALT_ENTER = 2;
const Hresult ERROR_DEVICE_REMOVED = (Hresult)0x887A0005;
const Hresult ERROR_DEVICE_RESET = (Hresult)0x887A0007;
const Hresult ERROR_SDK_COMPONENT_MISSING = (Hresult)0x887A002D;
const Hresult S_OK = 0;

extern fn Hresult create_device(void* adapter, uint driver_type, void* software, uint flags, uint* levels, uint level_count,
	uint sdk_version, Device** device, uint* level, DeviceContext** context) @cname("D3D11CreateDevice");

// --- Calls ------------------------------------------------------------------------------


alias Fn1 @private = fn uint(void*);
alias Fn2 @private = fn Hresult(void*, Guid*, void**);
alias Fn3 @private = fn Hresult(void*, InputElementDesc*, uint, void*, usz, void**);
alias Fn4 @private = fn Hresult(void*, void*, usz, void*, void**);
alias Fn5 @private = fn Hresult(void*);
alias Fn6 @private = fn void(void*, void*, void*, uint);
alias Fn7 @private = fn void(void*, uint, uint);
alias Fn8 @private = fn void(void*, uint, uint, int);
alias Fn9 @private = fn Hresult(void*, void*, uint, uint, uint, MappedSubresource*);
alias Fn10 @private = fn void(void*, void*, uint);
alias Fn11 @private = fn void(void*, void*);
alias Fn12 @private = fn void(void*, uint, uint, void**, uint*, uint*);
alias Fn13 @private = fn void(void*, void*, uint, uint);
alias Fn14 @private = fn void(void*, uint);
alias Fn15 @private = fn Hresult(void*, void*, void*, uint, uint);
alias Fn16 @private = fn void(void*, uint, void**, void*);
alias Fn17 @private = fn void(void*, void*, float*, uint);
alias Fn18 @private = fn void(void*, uint, Viewport*);
alias Fn19 @private = fn void(void*, uint, Rect*);
alias Fn20 @private = fn void(void*, void*, void*);
alias Fn21 @private = fn void(void*, void*, uint, Box*, void*, uint, uint);
alias Fn22 @private = fn void(void*, void*, float*);
alias Fn23 @private = fn void(void*, void*, uint, float, char);
alias Fn24 @private = fn void(void*);
alias Fn25 @private = fn Hresult(void*, void**);
alias Fn26 @private = fn Hresult(void*, AdapterDesc*);
alias Fn27 @private = fn Hresult(void*, void*, Hwnd, SwapChainDesc1*, void*, void*, void**);
alias Fn28 @private = fn Hresult(void*, Hwnd, uint);
alias Fn29 @private = fn Hresult(void*, uint, Guid*, void**);
alias Fn30 @private = fn Hresult(void*, uint, uint);
alias Fn31 @private = fn Hresult(void*, uint, uint, uint, uint, uint);
alias Fn32 @private = fn Hresult(void*, SwapChainDesc1*);

fn void release(void* object)
{
	if (!object) return;
	Unknown* u = object;
	((Fn1)u.vt.release)(u);
}

fn Hresult query_interface(void* object, Guid* iid, void** out)
{
	Unknown* u = object;
	return ((Fn2)u.vt.query_interface)(u, iid, out);
}

alias CreateFn @private = fn Hresult(void*, void*, void*, void**);

fn Hresult Device.create_buffer(&self, BufferDesc* desc, SubresourceData* data, Buffer** out)
	=> ((CreateFn)self.vt.create_buffer)(self, desc, data, (void**)out);
fn Hresult Device.create_texture2d(&self, Texture2DDesc* desc, SubresourceData* data, Texture2D** out)
	=> ((CreateFn)self.vt.create_texture2_d)(self, desc, data, (void**)out);
fn Hresult Device.create_texture3d(&self, Texture3DDesc* desc, SubresourceData* data, Texture3D** out)
	=> ((CreateFn)self.vt.create_texture3_d)(self, desc, data, (void**)out);
fn Hresult Device.create_shader_resource_view(&self, Resource* r, ShaderResourceView** out)
	=> ((CreateFn)self.vt.create_shader_resource_view)(self, r, null, (void**)out);
fn Hresult Device.create_render_target_view(&self, Resource* r, RenderTargetView** out)
	=> ((CreateFn)self.vt.create_render_target_view)(self, r, null, (void**)out);
fn Hresult Device.create_depth_stencil_view(&self, Resource* r, DepthStencilView** out)
	=> ((CreateFn)self.vt.create_depth_stencil_view)(self, r, null, (void**)out);

fn Hresult Device.create_input_layout(&self, InputElementDesc[] elements, char[] code, InputLayout** out)
	=> ((Fn3)self.vt.create_input_layout)(self, elements.ptr, (uint)elements.len, code.ptr, code.len, (void**)out);
fn Hresult Device.create_vertex_shader(&self, char[] code, VertexShader** out)
	=> ((Fn4)self.vt.create_vertex_shader)(self, code.ptr, code.len, null, (void**)out);
fn Hresult Device.create_pixel_shader(&self, char[] code, PixelShader** out)
	=> ((Fn4)self.vt.create_pixel_shader)(self, code.ptr, code.len, null, (void**)out);

alias CreateStateFn @private = fn Hresult(void*, void*, void**);
fn Hresult Device.create_blend_state(&self, BlendDesc* d, BlendState** out) => ((CreateStateFn)self.vt.create_blend_state)(self, d, (void**)out);
fn Hresult Device.create_depth_stencil_state(&self, DepthStencilDesc* d, DepthStencilState** out) => ((CreateStateFn)self.vt.create_depth_stencil_state)(self, d, (void**)out);
fn Hresult Device.create_rasterizer_state(&self, RasterizerDesc* d, RasterizerState** out) => ((CreateStateFn)self.vt.create_rasterizer_state)(self, d, (void**)out);
fn Hresult Device.create_sampler_state(&self, SamplerDesc* d, SamplerState** out) => ((CreateStateFn)self.vt.create_sampler_state)(self, d, (void**)out);
fn Hresult Device.create_query(&self, QueryDesc* d, Query** out) => ((CreateStateFn)self.vt.create_query)(self, d, (void**)out);
fn uint Device.get_feature_level(&self) => ((Fn1)self.vt.get_feature_level)(self);
fn Hresult Device.get_device_removed_reason(&self) => ((Fn5)self.vt.get_device_removed_reason)(self);

alias SetSlotsFn @private = fn void(void*, uint, uint, void**);
fn void DeviceContext.vs_set_constant_buffers(&self, uint start, void*[] b) => ((SetSlotsFn)self.vt.vs_set_constant_buffers)(self, start, (uint)b.len, b.ptr);
fn void DeviceContext.ps_set_constant_buffers(&self, uint start, void*[] b) => ((SetSlotsFn)self.vt.ps_set_constant_buffers)(self, start, (uint)b.len, b.ptr);
fn void DeviceContext.ps_set_shader_resources(&self, uint start, void*[] v) => ((SetSlotsFn)self.vt.ps_set_shader_resources)(self, start, (uint)v.len, v.ptr);
fn void DeviceContext.vs_set_shader_resources(&self, uint start, void*[] v) => ((SetSlotsFn)self.vt.vs_set_shader_resources)(self, start, (uint)v.len, v.ptr);
fn void DeviceContext.ps_set_samplers(&self, uint start, void*[] s) => ((SetSlotsFn)self.vt.ps_set_samplers)(self, start, (uint)s.len, s.ptr);
fn void DeviceContext.vs_set_shader(&self, VertexShader* s) => ((Fn6)self.vt.vs_set_shader)(self, s, null, 0);
fn void DeviceContext.ps_set_shader(&self, PixelShader* s) => ((Fn6)self.vt.ps_set_shader)(self, s, null, 0);
fn void DeviceContext.draw(&self, uint count, uint start) => ((Fn7)self.vt.draw)(self, count, start);
fn void DeviceContext.draw_indexed(&self, uint count, uint start, int base) => ((Fn8)self.vt.draw_indexed)(self, count, start, base);
fn Hresult DeviceContext.map(&self, Resource* r, uint sub, uint type, MappedSubresource* out)
	=> ((Fn9)self.vt.map)(self, r, sub, type, 0, out);
fn void DeviceContext.unmap(&self, Resource* r, uint sub) => ((Fn10)self.vt.unmap)(self, r, sub);
fn void DeviceContext.ia_set_input_layout(&self, InputLayout* l) => ((Fn11)self.vt.ia_set_input_layout)(self, l);
fn void DeviceContext.ia_set_vertex_buffer(&self, uint slot, Buffer* b, uint stride, uint offset = 0)
{
	void* bufs = b;
	((Fn12)self.vt.ia_set_vertex_buffers)(self, slot, 1, &bufs, &stride, &offset);
}
fn void DeviceContext.ia_set_index_buffer(&self, Buffer* b, uint format, uint offset = 0) => ((Fn13)self.vt.ia_set_index_buffer)(self, b, format, offset);
fn void DeviceContext.ia_set_primitive_topology(&self, uint t) => ((Fn14)self.vt.ia_set_primitive_topology)(self, t);
fn void DeviceContext.begin(&self, Query* q) => ((Fn11)self.vt.begin)(self, q);
fn void DeviceContext.end(&self, Query* q) => ((Fn11)self.vt.end)(self, q);
fn Hresult DeviceContext.get_data(&self, Query* q, void* data, uint size, uint flags) => ((Fn15)self.vt.get_data)(self, q, data, size, flags);
fn void DeviceContext.om_set_render_targets(&self, void*[] targets, DepthStencilView* depth)
	=> ((Fn16)self.vt.om_set_render_targets)(self, (uint)targets.len, targets.ptr, depth);
fn void DeviceContext.om_set_blend_state(&self, BlendState* s) => ((Fn17)self.vt.om_set_blend_state)(self, s, null, 0xFFFFFFFF);
fn void DeviceContext.om_set_depth_stencil_state(&self, DepthStencilState* s) => ((Fn10)self.vt.om_set_depth_stencil_state)(self, s, 0);
fn void DeviceContext.rs_set_state(&self, RasterizerState* s) => ((Fn11)self.vt.rs_set_state)(self, s);
fn void DeviceContext.rs_set_viewport(&self, Viewport v) => ((Fn18)self.vt.rs_set_viewports)(self, 1, &v);
fn void DeviceContext.rs_set_scissor_rect(&self, Rect r) => ((Fn19)self.vt.rs_set_scissor_rects)(self, 1, &r);
fn void DeviceContext.copy_resource(&self, Resource* to, Resource* from) => ((Fn20)self.vt.copy_resource)(self, to, from);
fn void DeviceContext.update_subresource(&self, Resource* r, uint sub, Box* box, void* data, uint row_pitch, uint depth_pitch)
	=> ((Fn21)self.vt.update_subresource)(self, r, sub, box, data, row_pitch, depth_pitch);
fn void DeviceContext.clear_render_target_view(&self, RenderTargetView* v, float[4] color) => ((Fn22)self.vt.clear_render_target_view)(self, v, &color);
fn void DeviceContext.clear_depth(&self, DepthStencilView* v, float depth) => ((Fn23)self.vt.clear_depth_stencil_view)(self, v, CLEAR_DEPTH, depth, 0);
fn void DeviceContext.generate_mips(&self, ShaderResourceView* v) => ((Fn11)self.vt.generate_mips)(self, v);
fn void DeviceContext.clear_state(&self) => ((Fn24)self.vt.clear_state)(self);
fn void DeviceContext.flush(&self) => ((Fn24)self.vt.flush)(self);

fn Hresult DxgiDevice.get_adapter(&self, DxgiAdapter** out) => ((Fn25)self.vt.get_adapter)(self, (void**)out);
fn Hresult DxgiAdapter.get_parent(&self, Guid* iid, void** out) => ((Fn2)self.vt.get_parent)(self, iid, out);
fn Hresult DxgiAdapter.get_desc(&self, AdapterDesc* out) => ((Fn26)self.vt.get_desc)(self, out);
fn Hresult DxgiFactory.create_swap_chain_for_hwnd(&self, Device* device, Hwnd hwnd, SwapChainDesc1* desc, SwapChain** out)
	=> ((Fn27)self.vt.create_swap_chain_for_hwnd)(self, device, hwnd, desc, null, null, (void**)out);
fn Hresult DxgiFactory.make_window_association(&self, Hwnd hwnd, uint flags) => ((Fn28)self.vt.make_window_association)(self, hwnd, flags);
fn Hresult SwapChain.get_buffer(&self, uint index, Texture2D** out) => ((Fn29)self.vt.get_buffer)(self, index, &&IID_TEXTURE2D, (void**)out);
fn Hresult SwapChain.present(&self, uint sync, uint flags) => ((Fn30)self.vt.present)(self, sync, flags);
fn Hresult SwapChain.resize_buffers(&self, uint count, uint width, uint height, uint format, uint flags)
	=> ((Fn31)self.vt.resize_buffers)(self, count, width, height, format, flags);
fn Hresult SwapChain.get_desc1(&self, SwapChainDesc1* out) => ((Fn32)self.vt.get_desc1)(self, out);

fn bool failed(Hresult hr) @inline => hr < 0;
