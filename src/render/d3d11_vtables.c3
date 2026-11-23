// The Direct3D 11 and DXGI interfaces the game uses, as vtables (generated
// from the Windows SDK's d3d11.h, dxgi.h and dxgi1_2.h: every slot in order;
// d3d11.c3 calls through the ones used, typed).
module gocube::d3d11;

struct DeviceVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* create_buffer;
	void* create_texture1_d;
	void* create_texture2_d;
	void* create_texture3_d;
	void* create_shader_resource_view;
	void* create_unordered_access_view;
	void* create_render_target_view;
	void* create_depth_stencil_view;
	void* create_input_layout;
	void* create_vertex_shader;
	void* create_geometry_shader;
	void* create_geometry_shader_with_stream_output;
	void* create_pixel_shader;
	void* create_hull_shader;
	void* create_domain_shader;
	void* create_compute_shader;
	void* create_class_linkage;
	void* create_blend_state;
	void* create_depth_stencil_state;
	void* create_rasterizer_state;
	void* create_sampler_state;
	void* create_query;
	void* create_predicate;
	void* create_counter;
	void* create_deferred_context;
	void* open_shared_resource;
	void* check_format_support;
	void* check_multisample_quality_levels;
	void* check_counter_info;
	void* check_counter;
	void* check_feature_support;
	void* get_private_data;
	void* set_private_data;
	void* set_private_data_interface;
	void* get_feature_level;
	void* get_creation_flags;
	void* get_device_removed_reason;
	void* get_immediate_context;
	void* set_exception_mode;
	void* get_exception_mode;
}

struct Device
{
	DeviceVtbl* vt;
}

struct DeviceContextVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* get_device;
	void* get_private_data;
	void* set_private_data;
	void* set_private_data_interface;
	void* vs_set_constant_buffers;
	void* ps_set_shader_resources;
	void* ps_set_shader;
	void* ps_set_samplers;
	void* vs_set_shader;
	void* draw_indexed;
	void* draw;
	void* map;
	void* unmap;
	void* ps_set_constant_buffers;
	void* ia_set_input_layout;
	void* ia_set_vertex_buffers;
	void* ia_set_index_buffer;
	void* draw_indexed_instanced;
	void* draw_instanced;
	void* gs_set_constant_buffers;
	void* gs_set_shader;
	void* ia_set_primitive_topology;
	void* vs_set_shader_resources;
	void* vs_set_samplers;
	void* begin;
	void* end;
	void* get_data;
	void* set_predication;
	void* gs_set_shader_resources;
	void* gs_set_samplers;
	void* om_set_render_targets;
	void* om_set_render_targets_and_unordered_access_views;
	void* om_set_blend_state;
	void* om_set_depth_stencil_state;
	void* so_set_targets;
	void* draw_auto;
	void* draw_indexed_instanced_indirect;
	void* draw_instanced_indirect;
	void* dispatch;
	void* dispatch_indirect;
	void* rs_set_state;
	void* rs_set_viewports;
	void* rs_set_scissor_rects;
	void* copy_subresource_region;
	void* copy_resource;
	void* update_subresource;
	void* copy_structure_count;
	void* clear_render_target_view;
	void* clear_unordered_access_view_uint;
	void* clear_unordered_access_view_float;
	void* clear_depth_stencil_view;
	void* generate_mips;
	void* set_resource_min_lod;
	void* get_resource_min_lod;
	void* resolve_subresource;
	void* execute_command_list;
	void* hs_set_shader_resources;
	void* hs_set_shader;
	void* hs_set_samplers;
	void* hs_set_constant_buffers;
	void* ds_set_shader_resources;
	void* ds_set_shader;
	void* ds_set_samplers;
	void* ds_set_constant_buffers;
	void* cs_set_shader_resources;
	void* cs_set_unordered_access_views;
	void* cs_set_shader;
	void* cs_set_samplers;
	void* cs_set_constant_buffers;
	void* vs_get_constant_buffers;
	void* ps_get_shader_resources;
	void* ps_get_shader;
	void* ps_get_samplers;
	void* vs_get_shader;
	void* ps_get_constant_buffers;
	void* ia_get_input_layout;
	void* ia_get_vertex_buffers;
	void* ia_get_index_buffer;
	void* gs_get_constant_buffers;
	void* gs_get_shader;
	void* ia_get_primitive_topology;
	void* vs_get_shader_resources;
	void* vs_get_samplers;
	void* get_predication;
	void* gs_get_shader_resources;
	void* gs_get_samplers;
	void* om_get_render_targets;
	void* om_get_render_targets_and_unordered_access_views;
	void* om_get_blend_state;
	void* om_get_depth_stencil_state;
	void* so_get_targets;
	void* rs_get_state;
	void* rs_get_viewports;
	void* rs_get_scissor_rects;
	void* hs_get_shader_resources;
	void* hs_get_shader;
	void* hs_get_samplers;
	void* hs_get_constant_buffers;
	void* ds_get_shader_resources;
	void* ds_get_shader;
	void* ds_get_samplers;
	void* ds_get_constant_buffers;
	void* cs_get_shader_resources;
	void* cs_get_unordered_access_views;
	void* cs_get_shader;
	void* cs_get_samplers;
	void* cs_get_constant_buffers;
	void* clear_state;
	void* flush;
	void* get_type;
	void* get_context_flags;
	void* finish_command_list;
}

struct DeviceContext
{
	DeviceContextVtbl* vt;
}

struct DxgiDeviceVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* set_private_data;
	void* set_private_data_interface;
	void* get_private_data;
	void* get_parent;
	void* get_adapter;
	void* create_surface;
	void* query_resource_residency;
	void* set_gpu_thread_priority;
	void* get_gpu_thread_priority;
}

struct DxgiDevice
{
	DxgiDeviceVtbl* vt;
}

struct DxgiAdapterVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* set_private_data;
	void* set_private_data_interface;
	void* get_private_data;
	void* get_parent;
	void* enum_outputs;
	void* get_desc;
	void* check_interface_support;
}

struct DxgiAdapter
{
	DxgiAdapterVtbl* vt;
}

struct DxgiFactoryVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* set_private_data;
	void* set_private_data_interface;
	void* get_private_data;
	void* get_parent;
	void* enum_adapters;
	void* make_window_association;
	void* get_window_association;
	void* create_swap_chain;
	void* create_software_adapter;
	void* enum_adapters1;
	void* is_current;
	void* is_windowed_stereo_enabled;
	void* create_swap_chain_for_hwnd;
	void* create_swap_chain_for_core_window;
	void* get_shared_resource_adapter_luid;
	void* register_stereo_status_window;
	void* register_stereo_status_event;
	void* unregister_stereo_status;
	void* register_occlusion_status_window;
	void* register_occlusion_status_event;
	void* unregister_occlusion_status;
	void* create_swap_chain_for_composition;
}

struct DxgiFactory
{
	DxgiFactoryVtbl* vt;
}

struct SwapChainVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* set_private_data;
	void* set_private_data_interface;
	void* get_private_data;
	void* get_parent;
	void* get_device;
	void* present;
	void* get_buffer;
	void* set_fullscreen_state;
	void* get_fullscreen_state;
	void* get_desc;
	void* resize_buffers;
	void* resize_target;
	void* get_containing_output;
	void* get_frame_statistics;
	void* get_last_present_count;
	void* get_desc1;
	void* get_fullscreen_desc;
	void* get_hwnd;
	void* get_core_window;
	void* present1;
	void* is_temporary_mono_supported;
	void* get_restrict_to_output;
	void* set_background_color;
	void* get_background_color;
	void* set_rotation;
	void* get_rotation;
}

struct SwapChain
{
	SwapChainVtbl* vt;
}
// Any COM object: what every interface starts with.
struct UnknownVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
}

struct Unknown
{
	UnknownVtbl* vt;
}
