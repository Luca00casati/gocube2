// Draws the UI's commands (ui/gui) with Direct3D 11 over the back buffer, one
// shader for everything (shaders/gui.hlsl): rectangles (rounded, outlined,
// gradients) worked out per pixel, text from a glyph atlas, item icons from
// the block atlas (nearest-neighbour, as the world's pixels). Text is in
// Segoe UI (Windows' UI font): GDI draws each glyph at SUPERSAMPLE times the
// size (greyscale), scaled down here, so it keeps its shape (GDI's hinting
// at small sizes snaps stems and widths to whole pixels: uneven, blotchy
// text); laid out here with fractional advances, each glyph on a whole pixel:
// word wrap, newlines; no kerning and no shaping (complex scripts), no
// fallback fonts.
// It sets all the Direct3D state it draws with (the world's renderers set
// theirs again each frame).
module gocube::gui_d3d;
import gocube::gui, gocube::d3d11, gocube::shaders, gocube::win, gocube::log, gocube::content, gocube::util::vec, gocube::util::map;
import std::math;

const int GLYPH_ATLAS = 1024; // pixels square (R8: 1 MB)
const int SUPERSAMPLE = 4;    // glyphs drawn this many times the size, then scaled down
const sz BATCH = 6 * 4096;    // vertices per draw call at most
// shaders/gui.hlsl's modes.
const float MODE_FILL @private = 0;
const float MODE_STROKE @private = 1;
const float MODE_GLYPH @private = 2;
const float MODE_ICON @private = 3;

struct GuiVertex
{
	float x, y, u, v;
	uint color; // RGBA8
	float[4] rect;
	float[4] params; // rounding, thickness, mode, 0
}

struct Glyph
{
	float advance;
	int w, h;   // the bitmap (0: nothing to draw)
	int ox, oy; // its top left from the pen, on the line's top
	int sx, sy; // where GDI's (big) bitmap starts in the first pixel, in big pixels
	int ax, ay; // where it is in the glyph atlas (-1: not there)
}

struct Font
{
	Handle gdi;
	int px;
	float ascent, line;
	Map{uint, Glyph} glyphs;
}

struct Placed
{
	uint cp;
	float x, y;
}

struct Layout
{
	int font;
	Pt size;
	Vec{Placed} glyphs;
	int used;
}

struct GuiD3D
{
	Handle dc; // memory DC for GDI
	Map{int, Font*} fonts;      // by pixel size
	Map{String, Layout*} layouts; // keys owned
	int frame;
	Device* device;
	DeviceContext* ctx;
	VertexShader* vs;
	PixelShader* ps;
	InputLayout* input;
	Buffer* vb;
	Buffer* cb;
	BlendState* blend;
	RasterizerState* raster;
	DepthStencilState* depth;
	SamplerState* sampler;
	Texture2D* glyph_tex;
	ShaderResourceView* glyph_view;
	ShaderResourceView* icon_view;
	int shelf_x, shelf_y, shelf_h; // packing the glyph atlas
	Vec{GuiVertex} verts;
	Vec{float[4]} tiles;  // each icon's uv rectangle
}

fn uint rgba8(Color c) @private => (uint)c.r | (uint)c.g << 8 | (uint)c.b << 16 | (uint)c.a << 24;

// The next code point of UTF-8 `s` from `*i` (moved past it); U+FFFD for bad bytes.
fn uint next_code_point(String s, sz* i) @private
{
	char b = s[(*i)++];
	if (b < 0x80) return b;
	int more = b >= 0xf0 && b < 0xf8 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : -1;
	if (more < 0) return 0xfffd;
	uint cp = b & (0x3f >> more);
	for (; more > 0; more--)
	{
		if (*i >= s.len || (s[*i] & 0xc0) != 0x80) return 0xfffd;
		cp = cp << 6 | (s[(*i)++] & 0x3f);
	}
	return cp;
}

fn bool GuiD3D.init(&self, Device* device, DeviceContext* context)
{
	self.device = device;
	self.ctx = context;
	InputElementDesc[5] elements = {
		{ "POSITION", 0, d3d11::FORMAT_R32G32_FLOAT, 0, 0, 0, 0 },
		{ "TEXCOORD", 0, d3d11::FORMAT_R32G32_FLOAT, 0, 8, 0, 0 },
		{ "COLOR", 0, d3d11::FORMAT_R8G8B8A8_UNORM, 0, 16, 0, 0 },
		{ "TEXCOORD", 1, d3d11::FORMAT_R32G32B32A32_FLOAT, 0, 20, 0, 0 },
		{ "TEXCOORD", 2, d3d11::FORMAT_R32G32B32A32_FLOAT, 0, 36, 0, 0 },
	};
	BufferDesc vbd = { .byte_width = (uint)(BATCH * GuiVertex::size), .usage = d3d11::USAGE_DYNAMIC, .bind_flags = d3d11::BIND_VERTEX_BUFFER,
		.cpu_access_flags = d3d11::CPU_ACCESS_WRITE };
	BufferDesc cbd = { .byte_width = 16, .usage = d3d11::USAGE_DYNAMIC, .bind_flags = d3d11::BIND_CONSTANT_BUFFER, .cpu_access_flags = d3d11::CPU_ACCESS_WRITE };
	BlendDesc bd;
	bd.render_target[0] = { 1, d3d11::BLEND_ONE, d3d11::BLEND_INV_SRC_ALPHA, d3d11::BLEND_OP_ADD, d3d11::BLEND_ONE, d3d11::BLEND_INV_SRC_ALPHA,
		d3d11::BLEND_OP_ADD, d3d11::COLOR_WRITE_ENABLE_ALL };
	RasterizerDesc rd = { .fill_mode = d3d11::FILL_SOLID, .cull_mode = d3d11::CULL_NONE, .depth_clip_enable = 1, .scissor_enable = 1 };
	DepthStencilDesc dd = { .depth_write_mask = d3d11::DEPTH_WRITE_MASK_ZERO, .depth_func = d3d11::COMPARISON_ALWAYS };
	SamplerDesc sd = { .filter = d3d11::FILTER_MIN_MAG_MIP_POINT, .address_u = d3d11::TEXTURE_ADDRESS_CLAMP, .address_v = d3d11::TEXTURE_ADDRESS_CLAMP,
		.address_w = d3d11::TEXTURE_ADDRESS_CLAMP, .max_lod = d3d11::FLOAT32_MAX };
	Texture2DDesc td = { .width = GLYPH_ATLAS, .height = GLYPH_ATLAS, .mip_levels = 1, .array_size = 1, .format = d3d11::FORMAT_R8_UNORM,
		.sample_count = 1, .usage = d3d11::USAGE_DEFAULT, .bind_flags = d3d11::BIND_SHADER_RESOURCE };
	return device.create_vertex_shader(shaders::code(GUI_VS), &self.vs) >= 0 && device.create_pixel_shader(shaders::code(GUI_PS), &self.ps) >= 0 &&
		device.create_input_layout(&elements, shaders::code(GUI_VS), &self.input) >= 0 && device.create_buffer(&vbd, null, &self.vb) >= 0 &&
		device.create_buffer(&cbd, null, &self.cb) >= 0 && device.create_blend_state(&bd, &self.blend) >= 0 &&
		device.create_rasterizer_state(&rd, &self.raster) >= 0 && device.create_depth_stencil_state(&dd, &self.depth) >= 0 &&
		device.create_sampler_state(&sd, &self.sampler) >= 0 && device.create_texture2d(&td, null, &self.glyph_tex) >= 0 &&
		device.create_shader_resource_view(self.glyph_tex, &self.glyph_view) >= 0;
}

fn void GuiD3D.free(&self)
{
	self.fonts.@each(; int px, Font* f)
	{
		if (f.gdi) win::delete_object(f.gdi);
		f.glyphs.free();
		free(f);
	};
	self.fonts.free();
	self.layouts.@each(; String k, Layout* l)
	{
		free(k.ptr);
		l.glyphs.free();
		free(l);
	};
	self.layouts.free();
	if (self.dc) win::delete_dc(self.dc);
	void*[*] objects = { self.vs, self.ps, self.input, self.vb, self.cb, self.blend, self.raster, self.depth, self.sampler, self.glyph_tex,
		self.glyph_view, self.icon_view };
	foreach (o : objects) d3d11::release(o);
	self.verts.free();
	self.tiles.free();
	*self = {};
}

fn Font* GuiD3D.font(&self, float size)
{
	int px = math::max(1, (int)math::round(size));
	if (Font* f = self.fonts.get_or(px, null)) return f;
	if (!self.dc) self.dc = win::create_compatible_dc(null);
	Font* f = mem::new(Font);
	self.fonts[px] = f;
	f.px = px;
	// (A negative height is the em size: what a font size means elsewhere.)
	f.gdi = win::create_font(-px * SUPERSAMPLE, 0, 0, 0, win::FW_NORMAL, 0, 0, 0, win::DEFAULT_CHARSET, win::OUT_TT_PRECIS, win::CLIP_DEFAULT_PRECIS,
		win::ANTIALIASED_QUALITY, 0, @wstring("Segoe UI"));
	TextMetric tm;
	if (self.dc && f.gdi)
	{
		win::select_object(self.dc, f.gdi);
		win::get_text_metrics(self.dc, &tm);
	}
	f.ascent = math::round(tm.height > 0 ? (float)tm.ascent / SUPERSAMPLE : px * 1.07f);
	f.line = math::round(tm.height > 0 ? (float)(tm.height + tm.external_leading) / SUPERSAMPLE : px * 1.33f);
	return f;
}

fn int floor_div(int v, int d) @private => v >= 0 ? v / d : -((-v + d - 1) / d);

fn uint gdi_char(uint cp) @private => cp > 0xffff || (cp >= 0xd800 && cp < 0xe000) ? 0xfffd : cp; // (GDI takes UTF-16 units)

// The glyph's metrics (made the first time; the pointer is good until the
// next new glyph of this font).
fn Glyph* GuiD3D.glyph(&self, Font* f, uint cp)
{
	if (Glyph* g = f.glyphs.get_ref(cp)) return g;
	Glyph g = { .ax = -1 };
	if (!self.dc || !f.gdi)
	{
		g.advance = math::round(f.px * 0.5f);
	}
	else
	{
		win::select_object(self.dc, f.gdi);
		GlyphMetrics gm;
		Dword bytes = win::get_glyph_outline(self.dc, gdi_char(cp), win::GGO_GRAY8_BITMAP, &gm, 0, null, &&win::MAT2_IDENTITY);
		if (bytes == win::GDI_ERROR && cp != '?')
		{
			g = *self.glyph(f, '?');
			g.ax = -1; // (its own place in the atlas, when drawn)
		}
		else if (bytes != win::GDI_ERROR)
		{
			g.advance = (float)gm.cell_inc_x / SUPERSAMPLE;
			if (bytes > 0) // (none for spaces)
			{
				// The big bitmap's corner, from the pen on the line's top (the
				// baseline on a whole pixel), split into pixels and the rest.
				int left = gm.glyph_origin.x;
				int top = (int)f.ascent * SUPERSAMPLE - gm.glyph_origin.y;
				g.ox = floor_div(left, SUPERSAMPLE);
				g.oy = floor_div(top, SUPERSAMPLE);
				g.sx = left - g.ox * SUPERSAMPLE;
				g.sy = top - g.oy * SUPERSAMPLE;
				g.w = (g.sx + (int)gm.black_box_x + SUPERSAMPLE - 1) / SUPERSAMPLE;
				g.h = (g.sy + (int)gm.black_box_y + SUPERSAMPLE - 1) / SUPERSAMPLE;
			}
		}
	}
	f.glyphs[cp] = g;
	return f.glyphs.get_ref(cp);
}

// Rasterizes the glyph into the atlas; false when the atlas is full.
fn bool GuiD3D.to_atlas(&self, Font* f, uint cp, Glyph* g)
{
	if (g.w + 2 > GLYPH_ATLAS || g.h + 2 > GLYPH_ATLAS) return true; // (never drawn)
	if (self.shelf_x + g.w + 1 > GLYPH_ATLAS)
	{
		self.shelf_x = 0;
		self.shelf_y += self.shelf_h;
		self.shelf_h = 0;
	}
	if (self.shelf_y + g.h + 1 > GLYPH_ATLAS) return false;
	win::select_object(self.dc, f.gdi);
	GlyphMetrics gm;
	uint ch = gdi_char(cp);
	uint drawn = win::get_glyph_outline(self.dc, ch, win::GGO_GRAY8_BITMAP, &gm, 0, null, &&win::MAT2_IDENTITY) == win::GDI_ERROR ? '?' : ch;
	Dword bytes = win::get_glyph_outline(self.dc, drawn, win::GGO_GRAY8_BITMAP, &gm, 0, null, &&win::MAT2_IDENTITY);
	if (bytes == win::GDI_ERROR || bytes == 0)
	{
		g.w = g.h = 0;
		return true;
	}
	char[] raw = mem::new_array(char, bytes);
	defer free(raw.ptr);
	win::get_glyph_outline(self.dc, drawn, win::GGO_GRAY8_BITMAP, &gm, bytes, raw.ptr, &&win::MAT2_IDENTITY);
	// Rows are DWORD-aligned, 65 levels (0..64); each pixel here is the
	// average of SUPERSAMPLE x SUPERSAMPLE of them.
	int bw = (int)gm.black_box_x;
	int bh = (int)gm.black_box_y;
	int pitch = (bw + 3) & ~3;
	char[] px = mem::new_array(char, (sz)g.w * g.h);
	defer free(px.ptr);
	for (int y = 0; y < g.h; y++)
	{
		for (int x = 0; x < g.w; x++)
		{
			int sum = 0;
			for (int j = 0; j < SUPERSAMPLE; j++)
			{
				int by = y * SUPERSAMPLE + j - g.sy;
				if (by < 0 || by >= bh) continue;
				for (int i = 0; i < SUPERSAMPLE; i++)
				{
					int bx = x * SUPERSAMPLE + i - g.sx;
					sz at = (sz)by * pitch + bx;
					if (bx >= 0 && bx < bw && at < raw.len) sum += raw[at];
				}
			}
			px[(sz)y * g.w + x] = (char)math::min(255, sum * 255 / (64 * SUPERSAMPLE * SUPERSAMPLE));
		}
	}
	g.ax = self.shelf_x;
	g.ay = self.shelf_y;
	Box box = { (uint)g.ax, (uint)g.ay, 0, (uint)(g.ax + g.w), (uint)(g.ay + g.h), 1 };
	self.ctx.update_subresource(self.glyph_tex, 0, &box, px.ptr, (uint)g.w, 0);
	self.shelf_x += g.w + 1;
	self.shelf_h = math::max(self.shelf_h, g.h + 1);
	return true;
}

fn Layout* GuiD3D.layout(&self, String text, float size, float wrap) => @pool()
{
	String key = string::tformat("%d|%d|%s", (int)math::round(size), (int)wrap, text);
	if (Layout* l = self.layouts.get_or(key, null))
	{
		l.used = self.frame;
		return l;
	}
	Font* f = self.font(size);
	Layout* l = mem::new(Layout);
	l.font = f.px;
	l.used = self.frame;
	Vec{Placed}* out = &l.glyphs;
	float pen = 0;
	float widest = 0;
	int line = 0;
	sz line_start = 0;
	sz space = sz::max; // the line's last space (a place to wrap)
	for (sz i = 0; i < text.len;)
	{
		uint cp = next_code_point(text, &i);
		if (cp == '\n')
		{
			widest = math::max(widest, pen);
			line++;
			pen = 0;
			line_start = out.len();
			space = sz::max;
			continue;
		}
		if (cp == '\r') continue;
		float advance = self.glyph(f, cp).advance;
		if (wrap > 0 && cp != ' ' && pen + advance > wrap && out.len() > line_start)
		{
			if (space != sz::max)
			{
				// The words after the line's last space go on the next line (the space stays, unseen).
				sz first = space + 1;
				float from = first < out.len() ? out.items[first].x : pen;
				float rest = pen - from;
				widest = math::max(widest, out.items[space].x);
				line++;
				for (sz k = first; k < out.len(); k++)
				{
					out.items[k].x -= from;
					out.items[k].y = (float)line * f.line;
				}
				line_start = first;
				space = sz::max;
				pen = rest;
			}
			if (pen + advance > wrap && out.len() > line_start)
			{
				widest = math::max(widest, pen); // (a word longer than the line: broken anywhere)
				line++;
				pen = 0;
				line_start = out.len();
				space = sz::max;
			}
		}
		if (cp == ' ') space = out.len();
		out.push({ cp, pen, (float)line * f.line });
		pen += advance;
	}
	widest = math::max(widest, pen);
	l.size = { math::ceil(widest), (float)(line + 1) * f.line };
	self.layouts[key.copy(mem)] = l;
	return l;
}

// For Gui's measure (GDI only: works before init too).
fn Pt measure(void* self, String text, float size, float wrap) => ((GuiD3D*)self).layout(text, size, wrap).size;

// The block atlas: item icons are its tiles.
fn void GuiD3D.set_icons(&self, Atlas* atlas)
{
	d3d11::release(self.icon_view);
	self.icon_view = null;
	self.tiles.clear();
	if (!self.device || atlas.width <= 0 || atlas.height <= 0 || atlas.rgba.len < (sz)atlas.width * atlas.height * 4) return;
	Texture2DDesc td = { .width = (uint)atlas.width, .height = (uint)atlas.height, .mip_levels = 1, .array_size = 1, .format = d3d11::FORMAT_R8G8B8A8_UNORM,
		.sample_count = 1, .usage = d3d11::USAGE_IMMUTABLE, .bind_flags = d3d11::BIND_SHADER_RESOURCE };
	SubresourceData data = { atlas.rgba.ptr, (uint)atlas.width * 4, 0 };
	Texture2D* tex;
	if (self.device.create_texture2d(&td, &data, &tex) < 0)
	{
		log::log("Couldn't make the item icons' texture");
		return;
	}
	defer d3d11::release(tex);
	if (self.device.create_shader_resource_view(tex, &self.icon_view) < 0)
	{
		log::log("Couldn't make the item icons' texture");
		return;
	}
	foreach (t, src : atlas.tiles)
	{
		float[4] uv;
		atlas.tile_uv((int)t, &uv);
		self.tiles.push(uv);
	}
}

fn void GuiD3D.quad(&self, Area r, float[4] uv, uint c0, uint c1, bool vertical, float[4] rect, float[4] params)
{
	if (self.verts.len() + 6 > BATCH) self.flush();
	// Corners: top left, top right, bottom left, bottom right; c0 at the start, c1 at the end.
	uint tl = c0;
	uint tr = vertical ? c0 : c1;
	uint bl = vertical ? c1 : c0;
	uint br = c1;
	self.verts.push({ r.x0, r.y0, uv[0], uv[1], tl, rect, params });
	self.verts.push({ r.x1, r.y0, uv[2], uv[1], tr, rect, params });
	self.verts.push({ r.x0, r.y1, uv[0], uv[3], bl, rect, params });
	self.verts.push({ r.x1, r.y0, uv[2], uv[1], tr, rect, params });
	self.verts.push({ r.x1, r.y1, uv[2], uv[3], br, rect, params });
	self.verts.push({ r.x0, r.y1, uv[0], uv[3], bl, rect, params });
}

fn void GuiD3D.flush(&self)
{
	if (self.verts.is_empty()) return;
	MappedSubresource m;
	if (self.ctx.map(self.vb, 0, d3d11::MAP_WRITE_DISCARD, &m) >= 0)
	{
		mem::copy(m.data, self.verts.items, (sz)(self.verts.len() * GuiVertex::size));
		self.ctx.unmap(self.vb, 0);
		self.ctx.draw((uint)self.verts.len(), 0);
	}
	self.verts.clear();
}

fn void GuiD3D.scissor(&self, int width, int height, Area[] clips)
{
	d3d11::Rect s = { 0, 0, width, height };
	foreach (c : clips)
	{
		s.left = math::max(s.left, (int)math::round(c.x0));
		s.top = math::max(s.top, (int)math::round(c.y0));
		s.right = math::min(s.right, (int)math::round(c.x1));
		s.bottom = math::min(s.bottom, (int)math::round(c.y1));
	}
	s.right = math::max(s.right, s.left);
	s.bottom = math::max(s.bottom, s.top);
	self.ctx.rs_set_scissor_rect(s);
}

// Draws over the render target bound, a width x height screen.
fn void GuiD3D.draw(&self, int width, int height, DrawCmd[] cmds)
{
	self.frame++;
	if (!self.vs || !cmds.len || width <= 0 || height <= 0) return;
	DeviceContext* ctx = self.ctx;
	float[4] to_clip = { 2.0f / (float)width, 2.0f / (float)height, 0, 0 };
	MappedSubresource m;
	if (ctx.map(self.cb, 0, d3d11::MAP_WRITE_DISCARD, &m) >= 0)
	{
		mem::copy(m.data, &to_clip, 16);
		ctx.unmap(self.cb, 0);
	}
	ctx.rs_set_viewport({ 0, 0, (float)width, (float)height, 0, 1 });
	ctx.rs_set_state(self.raster);
	ctx.om_set_blend_state(self.blend);
	ctx.om_set_depth_stencil_state(self.depth);
	ctx.ia_set_input_layout(self.input);
	ctx.ia_set_primitive_topology(d3d11::TOPOLOGY_TRIANGLELIST);
	ctx.ia_set_vertex_buffer(0, self.vb, GuiVertex::size);
	ctx.ia_set_index_buffer(null, d3d11::FORMAT_UNKNOWN);
	ctx.vs_set_shader(self.vs);
	ctx.ps_set_shader(self.ps);
	ctx.vs_set_constant_buffers(0, { self.cb });
	ctx.ps_set_shader_resources(0, { self.glyph_view, self.icon_view });
	ctx.ps_set_samplers(0, { self.sampler });
	// Clipping: the scissor rectangle, each clip inside the one before (pixels whose centres are in it).
	Vec{Area} clips;
	defer clips.free();
	self.scissor(width, height, {});
	float[4] none;
	foreach (&c : cmds)
	{
		float[4] rect = { c.rect.x0, c.rect.y0, c.rect.x1, c.rect.y1 };
		switch (c.type)
		{
			case FILL:
			case STROKE:
				bool fill = c.type == FILL;
				// (A pixel wider each way, for the smoothed edge.)
				self.quad({ c.rect.x0 - 1, c.rect.y0 - 1, c.rect.x1 + 1, c.rect.y1 + 1 }, none, rgba8(c.color), rgba8(c.color), true, rect,
					{ c.rounding, fill ? 0 : c.thickness, fill ? MODE_FILL : MODE_STROKE, 0 });
			case GRADIENT:
				self.quad(c.rect, none, rgba8(c.color), rgba8(c.color2), c.vertical, rect, { 0, 0, MODE_FILL, 0 });
			case TEXT:
				Layout* l = self.layout(c.text, c.size, c.wrap);
				Font* f = self.font(c.size);
				float x0 = math::round(c.rect.x0);
				float y0 = math::round(c.rect.y0);
				uint color = rgba8(c.color);
				foreach (p : l.glyphs)
				{
					Glyph* g = self.glyph(f, p.cp);
					if (g.w <= 0 || g.h <= 0) continue;
					if (g.ax < 0 && !self.to_atlas(f, p.cp, g))
					{
						// The atlas is full: draw what uses it, then start it again.
						self.flush();
						self.fonts.@each(; int px, Font* font)
						{
							font.glyphs.@each_ref(; uint cp, Glyph* other) { other.ax = -1; };
						};
						self.shelf_x = self.shelf_y = self.shelf_h = 0;
						if (!self.to_atlas(f, p.cp, g)) continue;
					}
					if (g.ax < 0 || g.w <= 0) continue;
					float x = math::round(x0 + p.x) + (float)g.ox; // (on a whole pixel: sampled nearest)
					float y = y0 + p.y + (float)g.oy;
					float s = 1.0f / GLYPH_ATLAS;
					self.quad({ x, y, x + (float)g.w, y + (float)g.h }, { (float)g.ax * s, (float)g.ay * s, (float)(g.ax + g.w) * s, (float)(g.ay + g.h) * s }, color, color, true,
						none, { 0, 0, MODE_GLYPH, 0 });
				}
			case IMAGE:
				if (!self.icon_view || c.image < 0 || c.image >= self.tiles.len()) break;
				uint color = (uint)c.color.a << 24 | 0xffffff;
				self.quad(c.rect, self.tiles[c.image], color, color, true, none, { 0, 0, MODE_ICON, 0 });
			case CLIP:
				self.flush();
				clips.push(c.rect);
				self.scissor(width, height, clips.array_view());
			case UNCLIP:
				if (!clips.is_empty())
				{
					self.flush();
					clips.pop();
					self.scissor(width, height, clips.array_view());
				}
		}
	}
	self.flush();
	ctx.ps_set_shader_resources(0, { null, null });
	// Text layouts not used for a while go.
	if (self.frame % 120 == 0)
	{
		Vec{String} gone;
		defer gone.free();
		self.layouts.@each(; String k, Layout* l) { if (self.frame - l.used > 120) gone.push(k); };
		foreach (k : gone)
		{
			Layout* l = self.layouts.get_or(k, null);
			self.layouts.remove(k);
			l.glyphs.free();
			free(l);
			free(k.ptr);
		}
	}
}
