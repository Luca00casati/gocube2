// Draws the world's chunk meshes: one vertex and index buffer per surface
// (opaque, cutout, fluid) per chunk, the block atlas, and the terrain shaders
// (shaders/chunk.hlsl); the sky, outlines, dropped items and the hand,
// creatures. Chunks outside the view frustum are skipped.
// (src/render/chunk_renderer.*)
module gocube::chunk_renderer;
import gocube::d3d11, gocube::shaders, gocube::win, gocube::mat, gocube::log, gocube::world, gocube::voxel, gocube::content;
import gocube::content::skin, gocube::game::creature_models, gocube::util::vec, gocube::util::map;
import std::math, std::sort;

struct Camera
{
	float[3] position;
	float yaw;   // radians; 0 looks towards -z, positive turns left
	float pitch; // radians; positive looks up
	float fov_y;
	float near_z, far_z;
}

fn Camera camera() => { .fov_y = 75.0f * (float)math::PI / 180.0f, .near_z = 0.05f, .far_z = 1000.0f };

// Unit view direction.
fn float[3] Camera.forward(&self)
	=> { -math::sin(self.yaw) * math::cos(self.pitch), math::sin(self.pitch), -math::cos(self.yaw) * math::cos(self.pitch) };
fn Matrix Camera.view(&self) => mat::look_to_rh(self.position, self.forward(), { 0, 1, 0 });
fn Matrix Camera.projection(&self, float aspect) => mat::perspective_fov_rh(self.fov_y, aspect, self.near_z, self.far_z);

struct SkyParams
{
	float daylight;
	float sun_amount;
	float[3] sun_dir;   // towards the sun
	float[3] sky_tint;
	float[3] fog_color; // linear
	float fog_start, fog_end; // depth fog (see chunk.hlsl), by 3D distance
	float time;
	// The sky (linear colours): gradient, sun, moon, stars, clouds.
	float[3] sky_top, sky_horizon, sky_ground;
	float day, dusk, sky_angle;
	float[2] cloud_offset;
}

fn SkyParams sky_params() => { .daylight = 1, .sun_amount = 1, .sun_dir = { 0.36f, 0.86f, 0.36f }, .sky_tint = { 1, 1, 1 },
	.fog_color = { 0.62f, 0.78f, 0.95f }, .fog_start = 58, .fog_end = 122, .sky_top = { 0.08f, 0.24f, 0.71f },
	.sky_horizon = { 0.42f, 0.59f, 0.83f }, .sky_ground = { 0.42f, 0.59f, 0.83f }, .day = 1, .sky_angle = 1.57f };

struct ChunkRenderStats
{
	int chunks; // with a mesh on the GPU
	int drawn;  // drawn this frame (in the frustum)
	long triangles;
}

// Matches cbuffer Frame in chunk.hlsl (16-byte rows).
struct FrameConstants @private
{
	float[16] view_proj;
	float[3] camera_pos;
	float daylight;
	float[3] sun_dir;
	float sun_amount;
	float[3] sky_tint;
	float min_light;
	float[3] fog_color;
	float fog_start;
	float[3] block_light_color;
	float fog_end;
	float time;
	float[2] cloud_offset;
	float pad1;
	float[3] hand_light_origin;
	float hand_light_count;
}

// Matches cbuffer Sky in sky.hlsl.
struct SkyConstants @private
{
	float[16] inv_view_proj;
	float[3] top;
	float day;
	float[3] horizon;
	float dusk;
	float[3] ground;
	float sky_angle;
	float[3] sun_dir;
	float time;
	float[2] cloud_offset;
	float[2] pad;
}

// Matches cbuffer Items in items.hlsl.
struct ItemConstants @private
{
	float[16] view_proj;
	float[3] eye;
	float fog_start;
	float[3] fog_color;
	float fog_end;
	float use_texture;
	float[3] pad;
}

struct DrawConstants @private
{
	float[3] chunk_origin;
	float pad;
}

struct GpuSurface
{
	Buffer* vertices;
	Buffer* indices;
	uint index_count;
}

struct GpuChunk
{
	ChunkPos pos;
	GpuSurface[3] surfaces; // opaque, cutout, fluid
}

fn void GpuChunk.release(&self)
{
	foreach (&s : self.surfaces)
	{
		d3d11::release(s.vertices);
		d3d11::release(s.indices);
	}
	*self = {};
}

const int MAX_OUTLINE_BOXES = 16;
const int SKIN_SLOTS = 9; // (every other player, and us)
const int HAND_LIGHT_SIZE = 48;
const int NOISE_CELLS @private = 32; // (shaders/chunk.hlsl: NOISE_CELLS)
const int NOISE_TEXELS @private = 8;

struct ChunkRenderer
{
	Device* device;
	DeviceContext* context;
	Map{long, GpuChunk*} chunks;
	VertexShader* vs;
	PixelShader* ps_opaque;
	PixelShader* ps_cutout;
	PixelShader* ps_fluid;
	InputLayout* layout;
	Buffer* frame_cb;
	Buffer* draw_cb;
	ShaderResourceView* atlas; // plain (not sRGB) view
	Buffer* dynamic_vertices;
	uint dynamic_capacity;
	SamplerState* sampler;
	RasterizerState* cull_back;
	RasterizerState* cull_none;
	DepthStencilState* depth_write;
	DepthStencilState* depth_test_only;
	DepthStencilState* depth_less_equal;
	DepthStencilState* depth_off;
	BlendState* alpha_blend;
	VertexShader* line_vs;
	PixelShader* line_ps;
	InputLayout* line_layout;
	Buffer* line_vertices;
	Buffer* line_cb;
	VertexShader* sky_vs;
	PixelShader* sky_ps;
	Buffer* sky_cb;
	VertexShader* items_vs;
	PixelShader* items_ps;
	Buffer* items_cb;
	VertexShader* creatures_vs;
	PixelShader* creatures_ps;
	InputLayout* creatures_layout;
	Buffer* creature_vertices;
	uint creature_capacity;
	Texture3D* hand_light_tex;
	ShaderResourceView* hand_light_srv;
	ShaderResourceView* noise_srv; // smooth noise (cloud shadows, the grass's green)
	SamplerState* noise_sampler;
	Texture2D* skins_tex;
	ShaderResourceView* skins_srv;
	float[3] hand_light_origin;
	int hand_light_count;
	ChunkRenderStats stats;
	Vec{GpuChunk*} visible;
}

fn bool ok(Hresult hr, String what) @private
{
	if (hr >= 0) return true;
	log::log("%s failed (HRESULT 0x%08x)", what, (uint)hr);
	return false;
}


fn bool ChunkRenderer.init(&self, Device* device, DeviceContext* context, Atlas* atlas)
{
	self.device = device;
	self.context = context;
	if (!ok(device.create_vertex_shader(shaders::CHUNK_VS, &self.vs), "CreateVertexShader") ||
		!ok(device.create_pixel_shader(shaders::CHUNK_PS_OPAQUE, &self.ps_opaque), "ps_opaque") ||
		!ok(device.create_pixel_shader(shaders::CHUNK_PS_CUTOUT, &self.ps_cutout), "ps_cutout") ||
		!ok(device.create_pixel_shader(shaders::CHUNK_PS_FLUID, &self.ps_fluid), "ps_fluid")) return false;
	InputElementDesc[5] layout = {
		{ "POSITION", 0, d3d11::FORMAT_R32G32B32_FLOAT, 0, (uint)WorldVertex::members[0].offset, 0, 0 },
		{ "NORMAL", 0, d3d11::FORMAT_R32G32B32_FLOAT, 0, (uint)WorldVertex::members[1].offset, 0, 0 },
		{ "TEXCOORD", 0, d3d11::FORMAT_R32G32_FLOAT, 0, (uint)WorldVertex::members[2].offset, 0, 0 },
		{ "COLOR", 0, d3d11::FORMAT_R32G32B32A32_FLOAT, 0, (uint)WorldVertex::members[3].offset, 0, 0 },
		{ "COLOR", 1, d3d11::FORMAT_R32G32B32A32_FLOAT, 0, (uint)WorldVertex::members[4].offset, 0, 0 },
	};
	if (!ok(device.create_input_layout(&layout, shaders::CHUNK_VS, &self.layout), "CreateInputLayout")) return false;
	BufferDesc cb = { .usage = d3d11::USAGE_DYNAMIC, .bind_flags = d3d11::BIND_CONSTANT_BUFFER, .cpu_access_flags = d3d11::CPU_ACCESS_WRITE,
		.byte_width = FrameConstants::size };
	if (!ok(device.create_buffer(&cb, null, &self.frame_cb), "CreateBuffer(frame)")) return false;
	cb.byte_width = DrawConstants::size;
	if (!ok(device.create_buffer(&cb, null, &self.draw_cb), "CreateBuffer(draw)")) return false;

	// The atlas, with mipmaps made by the GPU (tiles repeat within their cells,
	// so mip levels don't mix neighbouring tiles). Read as it is, sRGB numbers
	// not decoded (see chunk.hlsl).
	Texture2DDesc td = { .width = (uint)atlas.width, .height = (uint)atlas.height, .mip_levels = 0, .array_size = 1, .format = d3d11::FORMAT_R8G8B8A8_UNORM,
		.sample_count = 1, .usage = d3d11::USAGE_DEFAULT, .bind_flags = d3d11::BIND_SHADER_RESOURCE | d3d11::BIND_RENDER_TARGET,
		.misc_flags = d3d11::RESOURCE_MISC_GENERATE_MIPS };
	Texture2D* tex;
	if (!ok(device.create_texture2d(&td, null, &tex), "CreateTexture2D(atlas)")) return false;
	defer d3d11::release(tex);
	if (!ok(device.create_shader_resource_view(tex, &self.atlas), "CreateShaderResourceView(atlas)")) return false;
	context.update_subresource(tex, 0, null, atlas.rgba.ptr, (uint)atlas.width * 4, 0);
	context.generate_mips(self.atlas);
	SamplerDesc sd = { .filter = d3d11::FILTER_MIN_MAG_POINT_MIP_LINEAR, // crisp pixels, smooth with distance
		.address_u = d3d11::TEXTURE_ADDRESS_CLAMP, .address_v = d3d11::TEXTURE_ADDRESS_CLAMP, .address_w = d3d11::TEXTURE_ADDRESS_CLAMP,
		.max_lod = d3d11::FLOAT32_MAX, .comparison_func = d3d11::COMPARISON_NEVER };
	if (!ok(device.create_sampler_state(&sd, &self.sampler), "CreateSamplerState")) return false;
	if (!self.make_noise()) return false;

	RasterizerDesc rd = { .fill_mode = d3d11::FILL_SOLID, .cull_mode = d3d11::CULL_BACK, .depth_clip_enable = 1 };
	device.create_rasterizer_state(&rd, &self.cull_back);
	rd.cull_mode = d3d11::CULL_NONE;
	device.create_rasterizer_state(&rd, &self.cull_none);
	DepthStencilDesc dd = { .depth_enable = 1, .depth_write_mask = d3d11::DEPTH_WRITE_MASK_ALL, .depth_func = d3d11::COMPARISON_LESS };
	device.create_depth_stencil_state(&dd, &self.depth_write);
	dd.depth_write_mask = d3d11::DEPTH_WRITE_MASK_ZERO;
	device.create_depth_stencil_state(&dd, &self.depth_test_only);
	dd.depth_func = d3d11::COMPARISON_LESS_EQUAL;
	device.create_depth_stencil_state(&dd, &self.depth_less_equal);
	dd.depth_enable = 0;
	device.create_depth_stencil_state(&dd, &self.depth_off);
	BlendDesc bd;
	bd.render_target[0] = { 1, d3d11::BLEND_SRC_ALPHA, d3d11::BLEND_INV_SRC_ALPHA, d3d11::BLEND_OP_ADD, d3d11::BLEND_ONE, d3d11::BLEND_INV_SRC_ALPHA,
		d3d11::BLEND_OP_ADD, d3d11::COLOR_WRITE_ENABLE_ALL };
	device.create_blend_state(&bd, &self.alpha_blend);

	// Outline lines.
	if (!ok(device.create_vertex_shader(shaders::LINES_VS, &self.line_vs), "lines_vs") ||
		!ok(device.create_pixel_shader(shaders::LINES_PS, &self.line_ps), "lines_ps")) return false;
	InputElementDesc[1] line_layout = { { "POSITION", 0, d3d11::FORMAT_R32G32B32_FLOAT, 0, 0, 0, 0 } };
	if (!ok(device.create_input_layout(&line_layout, shaders::LINES_VS, &self.line_layout), "line layout")) return false;
	BufferDesc lb = { .usage = d3d11::USAGE_DYNAMIC, .bind_flags = d3d11::BIND_VERTEX_BUFFER, .cpu_access_flags = d3d11::CPU_ACCESS_WRITE,
		.byte_width = MAX_OUTLINE_BOXES * 24 * 12 };
	cb.byte_width = 16;
	if (!ok(device.create_buffer(&lb, null, &self.line_vertices), "line buffer") || !ok(device.create_buffer(&cb, null, &self.line_cb), "line constants")) return false;

	// The sky.
	cb.byte_width = SkyConstants::size;
	if (!ok(device.create_vertex_shader(shaders::SKY_VS, &self.sky_vs), "sky_vs") || !ok(device.create_pixel_shader(shaders::SKY_PS, &self.sky_ps), "sky_ps") ||
		!ok(device.create_buffer(&cb, null, &self.sky_cb), "sky constants")) return false;

	// Held torches' light (HandLights): one level per block, written whenever it changes.
	Texture3DDesc hd = { .width = HAND_LIGHT_SIZE, .height = HAND_LIGHT_SIZE, .depth = HAND_LIGHT_SIZE, .mip_levels = 1, .format = d3d11::FORMAT_R8_UNORM,
		.usage = d3d11::USAGE_DEFAULT, .bind_flags = d3d11::BIND_SHADER_RESOURCE };
	char[] dark = mem::new_array(char, (sz)HAND_LIGHT_SIZE * HAND_LIGHT_SIZE * HAND_LIGHT_SIZE);
	defer free(dark.ptr);
	SubresourceData hdata = { dark.ptr, HAND_LIGHT_SIZE, HAND_LIGHT_SIZE * HAND_LIGHT_SIZE };
	if (!ok(device.create_texture3d(&hd, &hdata, &self.hand_light_tex), "CreateTexture3D(hand light)") ||
		!ok(device.create_shader_resource_view(self.hand_light_tex, &self.hand_light_srv), "hand light view")) return false;

	// Players' skins: a slice each (set_skin), see-through until set. Plain
	// RGBA, as the atlas is read (no sRGB conversion).
	Texture2DDesc skd = { .width = skin::SKIN_SIZE, .height = skin::SKIN_SIZE, .mip_levels = 1, .array_size = SKIN_SLOTS, .format = d3d11::FORMAT_R8G8B8A8_UNORM,
		.sample_count = 1, .usage = d3d11::USAGE_DEFAULT, .bind_flags = d3d11::BIND_SHADER_RESOURCE };
	char[] clear_skin = mem::new_array(char, skin::SKIN_BYTES);
	defer free(clear_skin.ptr);
	SubresourceData[SKIN_SLOTS] sdata;
	foreach (&s : sdata) *s = { clear_skin.ptr, skin::SKIN_SIZE * 4, 0 };
	if (!ok(device.create_texture2d(&skd, &sdata, &self.skins_tex), "CreateTexture2D(skins)") ||
		!ok(device.create_shader_resource_view(self.skins_tex, &self.skins_srv), "skins view")) return false;

	// Items and the hand (the terrain's vertex layout).
	cb.byte_width = ItemConstants::size;
	if (!ok(device.create_vertex_shader(shaders::ITEMS_VS, &self.items_vs), "items_vs") || !ok(device.create_pixel_shader(shaders::ITEMS_PS, &self.items_ps), "items_ps") ||
		!ok(device.create_buffer(&cb, null, &self.items_cb), "item constants")) return false;

	// Creatures.
	InputElementDesc[6] creature_layout = {
		{ "POSITION", 0, d3d11::FORMAT_R32G32B32_FLOAT, 0, (uint)CreatureVertex::members[0].offset, 0, 0 },
		{ "TEXCOORD", 0, d3d11::FORMAT_R32G32B32_FLOAT, 0, (uint)CreatureVertex::members[1].offset, 0, 0 },
		{ "NORMAL", 0, d3d11::FORMAT_R32G32B32_FLOAT, 0, (uint)CreatureVertex::members[2].offset, 0, 0 },
		{ "COLOR", 0, d3d11::FORMAT_R32G32B32A32_FLOAT, 0, (uint)CreatureVertex::members[3].offset, 0, 0 },
		{ "TEXCOORD", 1, d3d11::FORMAT_R32G32B32A32_FLOAT, 0, (uint)CreatureVertex::members[4].offset, 0, 0 },
		{ "TEXCOORD", 2, d3d11::FORMAT_R32G32B32_FLOAT, 0, (uint)CreatureVertex::members[5].offset, 0, 0 },
	};
	return ok(device.create_vertex_shader(shaders::CREATURES_VS, &self.creatures_vs), "creatures_vs") &&
		ok(device.create_pixel_shader(shaders::CREATURES_PS, &self.creatures_ps), "creatures_ps") &&
		ok(device.create_input_layout(&creature_layout, shaders::CREATURES_VS, &self.creatures_layout), "creature layout");
}

// Smooth noise for the terrain shader (cloud shadows, the grass's green
// drifting across the land), made once: random values on a lattice of
// NOISE_CELLS x NOISE_CELLS (wrapping round), smoothly in between,
// NOISE_TEXELS texels a cell; read filtered, tiling.
fn bool ChunkRenderer.make_noise(&self)
{
	const int SIZE = NOISE_CELLS * NOISE_TEXELS;
	float[NOISE_CELLS * NOISE_CELLS] lattice;
	uint state = 0x9e3779b9u;
	foreach (&v : lattice)
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		*v = (float)(state >> 8) / (float)(1u << 24);
	}
	char[] texels = mem::new_array(char, (sz)SIZE * SIZE);
	defer free(texels.ptr);
	for (int y = 0; y < SIZE; y++)
	{
		for (int x = 0; x < SIZE; x++)
		{
			int cx = x / NOISE_TEXELS;
			int cy = y / NOISE_TEXELS;
			float fx = smooth((float)(x % NOISE_TEXELS) / NOISE_TEXELS);
			float fy = smooth((float)(y % NOISE_TEXELS) / NOISE_TEXELS);
			float top = lattice_at(&lattice, cx, cy) + (lattice_at(&lattice, cx + 1, cy) - lattice_at(&lattice, cx, cy)) * fx;
			float bottom = lattice_at(&lattice, cx, cy + 1) + (lattice_at(&lattice, cx + 1, cy + 1) - lattice_at(&lattice, cx, cy + 1)) * fx;
			texels[(sz)y * SIZE + x] = (char)math::round((top + (bottom - top) * fy) * 255.0f);
		}
	}
	Texture2DDesc nd = { .width = SIZE, .height = SIZE, .mip_levels = 1, .array_size = 1, .format = d3d11::FORMAT_R8_UNORM, .sample_count = 1,
		.usage = d3d11::USAGE_IMMUTABLE, .bind_flags = d3d11::BIND_SHADER_RESOURCE };
	SubresourceData ndata = { texels.ptr, SIZE, 0 };
	SamplerDesc ns = { .filter = d3d11::FILTER_MIN_MAG_MIP_LINEAR, .address_u = d3d11::TEXTURE_ADDRESS_WRAP, .address_v = d3d11::TEXTURE_ADDRESS_WRAP,
		.address_w = d3d11::TEXTURE_ADDRESS_WRAP, .max_lod = d3d11::FLOAT32_MAX, .comparison_func = d3d11::COMPARISON_NEVER };
	Texture2D* noise;
	if (!ok(self.device.create_texture2d(&nd, &ndata, &noise), "CreateTexture2D(noise)")) return false;
	defer d3d11::release(noise);
	return ok(self.device.create_shader_resource_view(noise, &self.noise_srv), "noise view") &&
		ok(self.device.create_sampler_state(&ns, &self.noise_sampler), "noise sampler");
}

fn float smooth(float t) @private => t * t * (3.0f - 2.0f * t);
fn float lattice_at(float[NOISE_CELLS * NOISE_CELLS]* l, int x, int y) @private
	=> (*l)[((y + NOISE_CELLS) % NOISE_CELLS) * NOISE_CELLS + (x + NOISE_CELLS) % NOISE_CELLS];

fn void ChunkRenderer.free(&self)
{
	self.chunks.@each(; long k, GpuChunk* c)
	{
		c.release();
		free(c);
	};
	self.chunks.free();
	self.visible.free();
	void*[*] objects = { self.vs, self.ps_opaque, self.ps_cutout, self.ps_fluid, self.layout, self.frame_cb, self.draw_cb, self.atlas,
		self.dynamic_vertices, self.sampler, self.cull_back, self.cull_none, self.depth_write, self.depth_test_only, self.depth_less_equal,
		self.depth_off, self.alpha_blend, self.line_vs, self.line_ps, self.line_layout, self.line_vertices, self.line_cb, self.sky_vs,
		self.sky_ps, self.sky_cb, self.items_vs, self.items_ps, self.items_cb, self.creatures_vs, self.creatures_ps, self.creatures_layout,
		self.creature_vertices, self.hand_light_tex, self.hand_light_srv, self.noise_srv, self.noise_sampler, self.skins_tex, self.skins_srv };
	foreach (o : objects) d3d11::release(o);
	*self = {};
}

fn long key(ChunkPos p) @private => ((long)(p.x & 0x1FFFFF) << 42) | ((long)(p.y & 0x1FFFFF) << 21) | (long)(p.z & 0x1FFFFF);

// Writes `bytes` into a dynamic buffer (WRITE_DISCARD).
fn bool ChunkRenderer.write(&self, Buffer* b, void* bytes, usz n)
{
	MappedSubresource m;
	if (self.context.map(b, 0, d3d11::MAP_WRITE_DISCARD, &m) < 0) return false;
	mem::copy(m.data, bytes, (sz)n);
	self.context.unmap(b, 0);
	return true;
}

// A dynamic vertex buffer at least `bytes` big (grown to twice that).
fn bool ChunkRenderer.ensure(&self, Buffer** b, uint* capacity, uint bytes)
{
	if (bytes <= *capacity) return true;
	d3d11::release(*b);
	*b = null;
	*capacity = bytes * 2;
	BufferDesc bd = { .usage = d3d11::USAGE_DYNAMIC, .bind_flags = d3d11::BIND_VERTEX_BUFFER, .cpu_access_flags = d3d11::CPU_ACCESS_WRITE, .byte_width = *capacity };
	if (self.device.create_buffer(&bd, null, b) < 0)
	{
		*capacity = 0;
		return false;
	}
	return true;
}

fn ItemConstants item_constants(Matrix view_proj, float[3] eye, SkyParams* sky, bool textured) @private
	=> { .view_proj = view_proj.m, .eye = eye, .fog_start = sky.fog_start, .fog_end = sky.fog_end, .fog_color = sky.fog_color, .use_texture = textured ? 1.0f : 0.0f };

// Dropped items and the first-person hand (shaders/items.hlsl): vertices
// placed already, `view_proj` takes them to the screen; their colour is the
// light (times the texture if `textured`, else it's the whole colour). Fog
// by distance from `eye`, as the terrain's. `cull_back`: faces wound the
// other way aren't drawn (the held item); otherwise both sides (drops).
fn void ChunkRenderer.draw_items(&self, WorldVertex[] vertices, Matrix view_proj, float[3] eye, SkyParams* sky, bool textured, bool cull_back = false)
{
	if (!vertices.len) return;
	uint bytes = (uint)(vertices.len * WorldVertex::size);
	if (!self.ensure(&self.dynamic_vertices, &self.dynamic_capacity, bytes) || !self.write(self.dynamic_vertices, vertices.ptr, bytes)) return;
	ItemConstants k = item_constants(view_proj, eye, sky, textured);
	if (!self.write(self.items_cb, &k, ItemConstants::size)) return;
	DeviceContext* ctx = self.context;
	ctx.vs_set_constant_buffers(0, { self.items_cb });
	ctx.ps_set_constant_buffers(0, { self.items_cb });
	ctx.ia_set_input_layout(self.layout);
	ctx.ia_set_primitive_topology(d3d11::TOPOLOGY_TRIANGLELIST);
	ctx.ia_set_vertex_buffer(0, self.dynamic_vertices, WorldVertex::size);
	ctx.vs_set_shader(self.items_vs);
	ctx.ps_set_shader(self.items_ps);
	ctx.ps_set_shader_resources(0, { self.atlas });
	ctx.ps_set_samplers(0, { self.sampler });
	ctx.rs_set_state(cull_back ? self.cull_back : self.cull_none);
	ctx.om_set_depth_stencil_state(self.depth_write);
	ctx.om_set_blend_state(null);
	ctx.draw((uint)vertices.len, 0);
	ctx.rs_set_state(null);
	ctx.om_set_depth_stencil_state(null);
}

// Torches held in hand (HandLights): their light levels, a cube of
// HAND_LIGHT_SIZE blocks from `origin`, uploaded when `changed`; none while
// `volume` is empty.
fn void ChunkRenderer.set_hand_light(&self, char[] volume, BlockPos origin, int count, bool changed)
{
	self.hand_light_origin = { (float)origin.x, (float)origin.y, (float)origin.z };
	self.hand_light_count = volume.len ? count : 0;
	if (changed && volume.len == (sz)HAND_LIGHT_SIZE * HAND_LIGHT_SIZE * HAND_LIGHT_SIZE)
	{
		self.context.update_subresource(self.hand_light_tex, 0, null, volume.ptr, HAND_LIGHT_SIZE, HAND_LIGHT_SIZE * HAND_LIGHT_SIZE);
	}
}

// Creatures' boxes (game/creature_models; shaders/creatures.hlsl), in world space.
fn void ChunkRenderer.draw_creatures(&self, CreatureVertex[] vertices, Matrix view_proj, float[3] eye, SkyParams* sky)
{
	if (!vertices.len) return;
	uint bytes = (uint)(vertices.len * CreatureVertex::size);
	if (!self.ensure(&self.creature_vertices, &self.creature_capacity, bytes) || !self.write(self.creature_vertices, vertices.ptr, bytes)) return;
	ItemConstants k = item_constants(view_proj, eye, sky, false);
	if (!self.write(self.items_cb, &k, ItemConstants::size)) return;
	DeviceContext* ctx = self.context;
	ctx.vs_set_constant_buffers(0, { self.items_cb });
	ctx.ps_set_constant_buffers(0, { self.items_cb });
	ctx.ia_set_input_layout(self.creatures_layout);
	ctx.ia_set_primitive_topology(d3d11::TOPOLOGY_TRIANGLELIST);
	ctx.ia_set_vertex_buffer(0, self.creature_vertices, CreatureVertex::size);
	ctx.vs_set_shader(self.creatures_vs);
	ctx.ps_set_shader(self.creatures_ps);
	ctx.ps_set_shader_resources(0, { self.atlas, self.skins_srv });
	ctx.ps_set_samplers(0, { self.sampler });
	ctx.rs_set_state(self.cull_none);
	ctx.om_set_depth_stencil_state(self.depth_write);
	ctx.om_set_blend_state(null);
	ctx.draw((uint)vertices.len, 0);
	ctx.rs_set_state(null);
	ctx.om_set_depth_stencil_state(null);
}

// Players' skins (content/skin: SKIN_BYTES of RGBA) for creature vertices
// to show (CreatureVertex.skin's slot).
fn void ChunkRenderer.set_skin(&self, int slot, char[] rgba)
{
	if (!self.skins_tex || slot < 0 || slot >= SKIN_SLOTS || rgba.len != skin::SKIN_BYTES) return;
	self.context.update_subresource(self.skins_tex, (uint)slot, null, rgba.ptr, skin::SKIN_SIZE * 4, 0); // (subresource: mip 0 of slice `slot`)
}

// The sky where nothing is drawn yet (the depth buffer still clear): after
// the solid blocks, before what's see-through.
fn void ChunkRenderer.draw_sky(&self, Camera* camera, float aspect, SkyParams* sky)
{
	Matrix vp = camera.view() * camera.projection(aspect);
	SkyConstants k = { .inv_view_proj = vp.inverse().m, .top = sky.sky_top, .horizon = sky.sky_horizon, .ground = sky.sky_ground,
		.day = sky.day, .dusk = sky.dusk, .sky_angle = sky.sky_angle, .sun_dir = mat::normalize(sky.sun_dir), .time = sky.time,
		.cloud_offset = sky.cloud_offset };
	if (!self.write(self.sky_cb, &k, SkyConstants::size)) return;
	DeviceContext* ctx = self.context;
	ctx.ia_set_input_layout(null);
	ctx.ia_set_primitive_topology(d3d11::TOPOLOGY_TRIANGLELIST);
	ctx.vs_set_shader(self.sky_vs);
	ctx.ps_set_shader(self.sky_ps);
	ctx.ps_set_constant_buffers(0, { self.sky_cb });
	ctx.rs_set_state(self.cull_none);
	ctx.om_set_depth_stencil_state(self.depth_less_equal); // (at the far plane: only where the depth is still clear)
	ctx.om_set_blend_state(null);
	ctx.draw(3, 0);
	ctx.rs_set_state(null);
	ctx.om_set_depth_stencil_state(null);
}

// After draw(): the edges of these boxes (world coordinates), just outside
// them so the lines don't fight with the faces.
fn void ChunkRenderer.draw_outline(&self, voxel::Box[] boxes)
{
	if (!boxes.len) return;
	float[3][MAX_OUTLINE_BOXES * 24] v;
	int n = 0;
	for (sz i = 0; i < boxes.len && i < MAX_OUTLINE_BOXES; i++)
	{
		float g = 0.002f;
		float[3] mn = { boxes[i].mn[0] - g, boxes[i].mn[1] - g, boxes[i].mn[2] - g };
		float[3] mx = { boxes[i].mx[0] + g, boxes[i].mx[1] + g, boxes[i].mx[2] + g };
		for (int c = 0; c < 8; c++) // each edge once: from a corner along each axis it isn't at the end of
		{
			foreach (bit : (int[3]){ 1, 2, 4 })
			{
				if (c & bit) continue;
				int d = c | bit;
				v[n++] = { c & 1 ? mx[0] : mn[0], c & 2 ? mx[1] : mn[1], c & 4 ? mx[2] : mn[2] };
				v[n++] = { d & 1 ? mx[0] : mn[0], d & 2 ? mx[1] : mn[1], d & 4 ? mx[2] : mn[2] };
			}
		}
	}
	if (!self.write(self.line_vertices, &v, (usz)n * 12)) return;
	float[4] color = { 0, 0, 0, 0.8f };
	self.write(self.line_cb, &color, 16);
	DeviceContext* ctx = self.context;
	ctx.ia_set_vertex_buffer(0, self.line_vertices, 12);
	ctx.ia_set_input_layout(self.line_layout);
	ctx.ia_set_primitive_topology(d3d11::TOPOLOGY_LINELIST);
	ctx.vs_set_shader(self.line_vs);
	ctx.ps_set_shader(self.line_ps);
	ctx.vs_set_constant_buffers(0, { self.frame_cb, self.line_cb });
	ctx.ps_set_constant_buffers(0, { self.frame_cb, self.line_cb });
	ctx.om_set_blend_state(self.alpha_blend);
	ctx.om_set_depth_stencil_state(self.depth_less_equal);
	ctx.draw((uint)n, 0);
	ctx.om_set_blend_state(null);
	ctx.om_set_depth_stencil_state(null);
}

fn bool ChunkRenderer.make_surface(&self, Surface* s, GpuSurface* out)
{
	*out = {};
	if (s.is_empty()) return true;
	sz n = s.positions.len() / 3;
	WorldVertex[] vertices = mem::new_array(WorldVertex, n);
	defer free(vertices.ptr);
	for (sz i = 0; i < n; i++)
	{
		WorldVertex* v = &vertices[i];
		v.pos = { s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2] };
		v.normal = { s.normals[i * 3], s.normals[i * 3 + 1], s.normals[i * 3 + 2] };
		v.uv = { s.uvs[i * 2], s.uvs[i * 2 + 1] };
		v.color = { s.colors[i * 4], s.colors[i * 4 + 1], s.colors[i * 4 + 2], s.colors[i * 4 + 3] };
		v.set_dye(i < s.dyes.len() ? s.dyes[i] : voxel::NO_DYE);
	}
	BufferDesc bd = { .usage = d3d11::USAGE_IMMUTABLE, .bind_flags = d3d11::BIND_VERTEX_BUFFER, .byte_width = (uint)(n * WorldVertex::size) };
	SubresourceData data = { vertices.ptr, 0, 0 };
	if (self.device.create_buffer(&bd, &data, &out.vertices) < 0) return false;
	bd.bind_flags = d3d11::BIND_INDEX_BUFFER;
	bd.byte_width = (uint)(s.indices.len() * 4);
	data.data = s.indices.items;
	if (self.device.create_buffer(&bd, &data, &out.indices) < 0) return false;
	out.index_count = (uint)s.indices.len();
	return true;
}

// Replaces the chunk's mesh.
fn void ChunkRenderer.upload(&self, ChunkMesh* mesh)
{
	GpuChunk c = { .pos = mesh.pos };
	Surface*[3] surfaces = { &mesh.opaque, &mesh.cutout, &mesh.fluid };
	for (int i = 0; i < 3; i++)
	{
		if (!self.make_surface(surfaces[i], &c.surfaces[i]))
		{
			log::log("Couldn't upload chunk (%d, %d, %d)", mesh.pos.x, mesh.pos.y, mesh.pos.z);
			c.release();
			return;
		}
	}
	bool made;
	GpuChunk** slot = self.chunks.slot(key(mesh.pos), &made);
	if (made)
	{
		*slot = mem::new(GpuChunk);
	}
	else
	{
		(*slot).release();
	}
	**slot = c;
}

fn void ChunkRenderer.remove(&self, ChunkPos pos)
{
	long k = key(pos);
	if (GpuChunk** c = self.chunks.get_ref(k))
	{
		(*c).release();
		free(*c);
		self.chunks.remove(k);
	}
}

// A plane a*x + b*y + c*z + d (inside where >= 0).
alias Plane @private = float[<4>];

fn bool box_visible(Plane[6]* planes, float[3] mn, float[3] mx) @private
{
	foreach (p : *planes)
	{
		// The box corner furthest along the plane's normal.
		float x = p[0] >= 0 ? mx[0] : mn[0];
		float y = p[1] >= 0 ? mx[1] : mn[1];
		float z = p[2] >= 0 ? mx[2] : mn[2];
		if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
	}
	return true;
}

alias BeforeFluids = fn void(void* context);

fn void ChunkRenderer.bind(&self)
{
	DeviceContext* ctx = self.context;
	ctx.vs_set_constant_buffers(0, { self.frame_cb, self.draw_cb });
	ctx.ps_set_constant_buffers(0, { self.frame_cb, self.draw_cb });
	ctx.ia_set_input_layout(self.layout);
	ctx.ia_set_primitive_topology(d3d11::TOPOLOGY_TRIANGLELIST);
	ctx.vs_set_shader(self.vs);
	ctx.ps_set_shader_resources(0, { self.atlas, self.hand_light_srv, self.noise_srv });
	ctx.ps_set_samplers(0, { self.sampler, self.noise_sampler });
}

fn void ChunkRenderer.draw_surface(&self, GpuChunk* c, int surface)
{
	GpuSurface* s = &c.surfaces[surface];
	if (!s.index_count) return;
	DrawConstants d = { { (float)(c.pos.x * voxel::CHUNK), (float)(c.pos.y * voxel::CHUNK), (float)(c.pos.z * voxel::CHUNK) }, 0 };
	self.write(self.draw_cb, &d, DrawConstants::size);
	self.context.ia_set_vertex_buffer(0, s.vertices, WorldVertex::size);
	self.context.ia_set_index_buffer(s.indices, d3d11::FORMAT_R32_UINT);
	self.context.draw_indexed(s.index_count, 0, 0);
	self.stats.triangles += s.index_count / 3;
}

// Draws into the bound render target and depth buffer: the solid blocks
// (near chunks first: hidden pixels are skipped early), then calls
// `before_fluids` (the sky goes there: only where nothing was drawn), then
// water and lava blended over (far chunks first).
fn void ChunkRenderer.draw(&self, Camera* camera, float aspect, SkyParams* sky, BeforeFluids before_fluids = null, void* before_context = null)
{
	Matrix vp = camera.view() * camera.projection(aspect);
	FrameConstants f = { .view_proj = vp.m, .camera_pos = camera.position, .daylight = sky.daylight, .sun_dir = mat::normalize(sky.sun_dir),
		.sun_amount = sky.sun_amount, .sky_tint = sky.sky_tint, .min_light = 0.02f, .fog_color = sky.fog_color, .fog_start = sky.fog_start,
		.fog_end = sky.fog_end, .cloud_offset = sky.cloud_offset, .block_light_color = { 1.0f, 0.85f, 0.62f }, .time = sky.time,
		.hand_light_origin = self.hand_light_origin, .hand_light_count = (float)self.hand_light_count };
	self.write(self.frame_cb, &f, FrameConstants::size);
	// The frustum's planes, from the view-projection matrix's columns (row
	// vectors: clip = v * M).
	Plane[4] col;
	for (int j = 0; j < 4; j++) col[j] = { vp.m[j], vp.m[4 + j], vp.m[8 + j], vp.m[12 + j] };
	Plane[6] planes = { col[3] + col[0], col[3] - col[0], col[3] + col[1], col[3] - col[1], col[2], col[3] - col[2] };
	self.bind();
	self.stats = { .chunks = (int)self.chunks.len() };
	self.visible.clear();
	self.chunks.@each(; long k, GpuChunk* c)
	{
		float[3] mn = { (float)(c.pos.x * voxel::CHUNK), (float)(c.pos.y * voxel::CHUNK), (float)(c.pos.z * voxel::CHUNK) };
		float[3] mx = { mn[0] + voxel::CHUNK, mn[1] + voxel::CHUNK, mn[2] + voxel::CHUNK };
		if (box_visible(&planes, mn, mx)) self.visible.push(c);
	};
	self.stats.drawn = (int)self.visible.len();
	// Near first (what's behind is then skipped before shading).
	float[3] eye = camera.position;
	sort::quicksort(self.visible.array_view(), fn int(GpuChunk* a, GpuChunk* b, float[3]* eye)
	{
		float da = distance2(a, *eye);
		float db = distance2(b, *eye);
		return da < db ? -1 : da > db ? 1 : 0;
	}, &eye);
	// Opaque, then cutout (both write depth), then fluids blended on top.
	DeviceContext* ctx = self.context;
	ctx.rs_set_state(self.cull_back);
	ctx.om_set_depth_stencil_state(self.depth_write);
	ctx.om_set_blend_state(null);
	ctx.ps_set_shader(self.ps_opaque);
	foreach (c : self.visible) self.draw_surface(c, 0);
	ctx.rs_set_state(self.cull_none); // plants are single quads seen from both sides
	ctx.ps_set_shader(self.ps_cutout);
	foreach (c : self.visible) self.draw_surface(c, 1);
	if (before_fluids)
	{
		before_fluids(before_context);
		self.bind();
	}
	ctx.rs_set_state(self.cull_none);
	ctx.om_set_depth_stencil_state(self.depth_test_only);
	ctx.om_set_blend_state(self.alpha_blend);
	ctx.ps_set_shader(self.ps_fluid);
	for (sz i = self.visible.len(); i > 0; i--) self.draw_surface(self.visible[i - 1], 2); // (far first: blended over what's behind)
	ctx.om_set_blend_state(null);
	ctx.om_set_depth_stencil_state(null);
	ctx.rs_set_state(null);
}

fn float distance2(GpuChunk* c, float[3] eye) @private
{
	float dx = ((float)c.pos.x + 0.5f) * voxel::CHUNK - eye[0];
	float dy = ((float)c.pos.y + 0.5f) * voxel::CHUNK - eye[1];
	float dz = ((float)c.pos.z + 0.5f) * voxel::CHUNK - eye[2];
	return dx * dx + dy * dy + dz * dz;
}
