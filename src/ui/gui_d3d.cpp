#include "ui/gui_d3d.h"

#include "log.h"

// Compiled from shaders/gui.hlsl at build time (by fxc: see nob.c).
#include "gui_ps.h"
#include "gui_vs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace gc {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int GLYPH_ATLAS = 1024; // pixels square (R8: 1 MB)
constexpr size_t BATCH = 6 * 4096; // vertices per draw call at most
constexpr float FILL = 0, STROKE = 1, GLYPH = 2, ICON = 3; // shaders/gui.hlsl's modes

uint32_t rgba8(ui::Color c) {
	return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | uint32_t(c.a) << 24;
}

// The next code point of UTF-8 `s` from `i` (moved past it); U+FFFD for bad bytes.
uint32_t next_code_point(std::string_view s, size_t &i) {
	const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
	const unsigned char b = byte(i++);
	if (b < 0x80) {
		return b;
	}
	int more = b >= 0xf0 && b < 0xf8 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : -1;
	if (more < 0) {
		return 0xfffd;
	}
	uint32_t cp = b & (0x3f >> more);
	for (; more > 0; more--) {
		if (i >= s.size() || (byte(i) & 0xc0) != 0x80) {
			return 0xfffd;
		}
		cp = cp << 6 | (byte(i++) & 0x3f);
	}
	return cp;
}

const MAT2 IDENTITY = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

// The Direct3D 11 state the UI changes, kept around its drawing.
struct D3DState {
	static constexpr UINT SLOTS = 8;
	ID3D11DeviceContext *ctx;
	UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ComPtr<ID3D11RasterizerState> rs;
	ComPtr<ID3D11BlendState> blend;
	FLOAT blend_factor[4];
	UINT sample_mask;
	ComPtr<ID3D11DepthStencilState> ds;
	UINT stencil_ref;
	ComPtr<ID3D11InputLayout> layout;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	ID3D11Buffer *vb = nullptr;
	UINT vb_stride, vb_offset;
	ComPtr<ID3D11Buffer> ib;
	DXGI_FORMAT ib_format;
	UINT ib_offset;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ID3D11Buffer *vs_cb[SLOTS] = {}, *ps_cb[SLOTS] = {};
	ID3D11ShaderResourceView *ps_srv[SLOTS] = {}, *vs_srv[SLOTS] = {};
	ID3D11SamplerState *ps_samp[SLOTS] = {};

	explicit D3DState(ID3D11DeviceContext *c) : ctx(c) {
		ctx->RSGetViewports(&viewport_count, viewports);
		ctx->RSGetScissorRects(&scissor_count, scissors);
		ctx->RSGetState(&rs);
		ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
		ctx->OMGetDepthStencilState(&ds, &stencil_ref);
		ctx->IAGetInputLayout(&layout);
		ctx->IAGetPrimitiveTopology(&topology);
		ctx->IAGetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
		ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
		ctx->VSGetShader(&vs, nullptr, nullptr);
		ctx->PSGetShader(&ps, nullptr, nullptr);
		ctx->VSGetConstantBuffers(0, SLOTS, vs_cb);
		ctx->PSGetConstantBuffers(0, SLOTS, ps_cb);
		ctx->PSGetShaderResources(0, SLOTS, ps_srv);
		ctx->VSGetShaderResources(0, SLOTS, vs_srv);
		ctx->PSGetSamplers(0, SLOTS, ps_samp);
	}
	~D3DState() {
		ctx->RSSetViewports(viewport_count, viewports);
		ctx->RSSetScissorRects(scissor_count, scissors);
		ctx->RSSetState(rs.Get());
		ctx->OMSetBlendState(blend.Get(), blend_factor, sample_mask);
		ctx->OMSetDepthStencilState(ds.Get(), stencil_ref);
		ctx->IASetInputLayout(layout.Get());
		ctx->IASetPrimitiveTopology(topology);
		ctx->IASetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
		ctx->IASetIndexBuffer(ib.Get(), ib_format, ib_offset);
		ctx->VSSetShader(vs.Get(), nullptr, 0);
		ctx->PSSetShader(ps.Get(), nullptr, 0);
		ctx->VSSetConstantBuffers(0, SLOTS, vs_cb);
		ctx->PSSetConstantBuffers(0, SLOTS, ps_cb);
		ctx->PSSetShaderResources(0, SLOTS, ps_srv);
		ctx->VSSetShaderResources(0, SLOTS, vs_srv);
		ctx->PSSetSamplers(0, SLOTS, ps_samp);
		if (vb) {
			vb->Release();
		}
		for (UINT i = 0; i < SLOTS; i++) {
			for (IUnknown *p : {static_cast<IUnknown *>(vs_cb[i]), static_cast<IUnknown *>(ps_cb[i]),
						 static_cast<IUnknown *>(ps_srv[i]), static_cast<IUnknown *>(vs_srv[i]),
						 static_cast<IUnknown *>(ps_samp[i])}) {
				if (p) {
					p->Release();
				}
			}
		}
	}
};

} // namespace

GuiD3D::~GuiD3D() {
	for (auto &[px, f] : fonts_) {
		if (f.gdi) {
			DeleteObject(static_cast<HFONT>(f.gdi));
		}
	}
	if (dc_) {
		DeleteDC(static_cast<HDC>(dc_));
	}
}

bool GuiD3D::init(ID3D11Device *device) {
	device_ = device;
	const D3D11_INPUT_ELEMENT_DESC elements[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, dye), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, rect), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, params), D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	D3D11_BUFFER_DESC vbd = {};
	vbd.ByteWidth = UINT(BATCH * sizeof(Vertex));
	vbd.Usage = D3D11_USAGE_DYNAMIC;
	vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = 16;
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	D3D11_BLEND_DESC bd = {};
	D3D11_RENDER_TARGET_BLEND_DESC &rt = bd.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
	rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	rd.ScissorEnable = TRUE;
	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = FALSE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = td.Height = GLYPH_ATLAS;
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateVertexShader(g_gui_vs, sizeof(g_gui_vs), nullptr, &vs_)) ||
			FAILED(device->CreatePixelShader(g_gui_ps, sizeof(g_gui_ps), nullptr, &ps_)) ||
			FAILED(device->CreateInputLayout(elements, UINT(std::size(elements)), g_gui_vs, sizeof(g_gui_vs), &input_)) ||
			FAILED(device->CreateBuffer(&vbd, nullptr, &vb_)) || FAILED(device->CreateBuffer(&cbd, nullptr, &cb_)) ||
			FAILED(device->CreateBlendState(&bd, &blend_)) || FAILED(device->CreateRasterizerState(&rd, &raster_)) ||
			FAILED(device->CreateDepthStencilState(&dd, &depth_)) || FAILED(device->CreateSamplerState(&sd, &sampler_)) ||
			FAILED(device->CreateTexture2D(&td, nullptr, &glyph_tex_)) ||
			FAILED(device->CreateShaderResourceView(glyph_tex_.Get(), nullptr, &glyph_view_))) {
		return false;
	}
	verts_.reserve(BATCH);
	return true;
}

GuiD3D::Font &GuiD3D::font(float size) {
	const int px = std::max(1, int(std::lround(size)));
	auto it = fonts_.find(px);
	if (it != fonts_.end()) {
		return it->second;
	}
	if (!dc_) {
		dc_ = CreateCompatibleDC(nullptr);
		// Windows' own UI font (the one its dialogs use): Segoe UI unless the
		// system says otherwise.
		NONCLIENTMETRICSW metrics = {};
		metrics.cbSize = sizeof(metrics);
		if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) &&
				metrics.lfMessageFont.lfFaceName[0]) {
			family_ = metrics.lfMessageFont.lfFaceName;
		}
	}
	Font &f = fonts_[px];
	f.px = px;
	// (A negative height is the em size: what a font size means elsewhere.)
	f.gdi = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family_.c_str());
	TEXTMETRICW tm = {};
	if (dc_ && f.gdi) {
		SelectObject(static_cast<HDC>(dc_), static_cast<HFONT>(f.gdi));
		GetTextMetricsW(static_cast<HDC>(dc_), &tm);
	}
	f.ascent = tm.tmHeight > 0 ? float(tm.tmAscent) : std::round(px * 1.07f);
	f.line = tm.tmHeight > 0 ? float(tm.tmHeight + tm.tmExternalLeading) : std::round(px * 1.33f);
	return f;
}

GuiD3D::Glyph &GuiD3D::glyph(Font &f, uint32_t cp) {
	auto it = f.glyphs.find(cp);
	if (it != f.glyphs.end()) {
		return it->second;
	}
	Glyph &g = f.glyphs[cp];
	if (!dc_ || !f.gdi) {
		g.advance = std::round(f.px * 0.5f);
		return g;
	}
	const HDC dc = static_cast<HDC>(dc_);
	SelectObject(dc, static_cast<HFONT>(f.gdi));
	GLYPHMETRICS gm = {};
	// (GDI takes UTF-16 units: outside the basic plane, the replacement character.)
	const UINT ch = cp > 0xffff || (cp >= 0xd800 && cp < 0xe000) ? 0xfffd : UINT(cp);
	DWORD bytes = GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &IDENTITY);
	if (bytes == GDI_ERROR && cp != '?') {
		g = glyph(f, '?');
		g.ax = -1; // (its own place in the atlas, when drawn)
		return f.glyphs[cp] = g;
	}
	if (bytes == GDI_ERROR) {
		return g;
	}
	g.advance = float(gm.gmCellIncX);
	if (bytes > 0) { // (none for spaces)
		g.w = int(gm.gmBlackBoxX);
		g.h = int(gm.gmBlackBoxY);
		g.ox = int(gm.gmptGlyphOrigin.x);
		g.oy = int(f.ascent) - int(gm.gmptGlyphOrigin.y);
	}
	return g;
}

// Rasterizes the glyph into the atlas; false when the atlas is full.
bool GuiD3D::to_atlas(Font &f, uint32_t cp, Glyph &g) {
	if (g.w + 2 > GLYPH_ATLAS || g.h + 2 > GLYPH_ATLAS) {
		return true; // (never drawn)
	}
	if (shelf_x_ + g.w + 1 > GLYPH_ATLAS) {
		shelf_x_ = 0;
		shelf_y_ += shelf_h_;
		shelf_h_ = 0;
	}
	if (shelf_y_ + g.h + 1 > GLYPH_ATLAS) {
		return false;
	}
	const HDC dc = static_cast<HDC>(dc_);
	SelectObject(dc, static_cast<HFONT>(f.gdi));
	GLYPHMETRICS gm = {};
	const UINT ch = cp > 0xffff || (cp >= 0xd800 && cp < 0xe000) ? 0xfffd : UINT(cp);
	const UINT drawn = GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &IDENTITY) == GDI_ERROR ? '?' : ch;
	const DWORD bytes = GetGlyphOutlineW(dc, drawn, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &IDENTITY);
	if (bytes == GDI_ERROR || bytes == 0) {
		g.w = g.h = 0;
		return true;
	}
	std::vector<uint8_t> raw(bytes);
	GetGlyphOutlineW(dc, drawn, GGO_GRAY8_BITMAP, &gm, bytes, raw.data(), &IDENTITY);
	// Rows are DWORD-aligned, 65 levels (0..64).
	const int pitch = (g.w + 3) & ~3;
	std::vector<uint8_t> px(size_t(g.w) * size_t(g.h));
	for (int y = 0; y < g.h; y++) {
		for (int x = 0; x < g.w; x++) {
			const size_t at = size_t(y) * pitch + x;
			px[size_t(y) * g.w + x] = at < raw.size() ? uint8_t(std::min(255, raw[at] * 255 / 64)) : 0;
		}
	}
	g.ax = shelf_x_;
	g.ay = shelf_y_;
	const D3D11_BOX box = {UINT(g.ax), UINT(g.ay), 0, UINT(g.ax + g.w), UINT(g.ay + g.h), 1};
	ctx_->UpdateSubresource(glyph_tex_.Get(), 0, &box, px.data(), UINT(g.w), 0);
	shelf_x_ += g.w + 1;
	shelf_h_ = std::max(shelf_h_, g.h + 1);
	return true;
}

GuiD3D::Layout *GuiD3D::layout(std::string_view text, float size, float wrap) {
	std::string key = std::to_string(int(std::lround(size))) + "|" + std::to_string(int(wrap)) + "|";
	key += text;
	auto it = layouts_.find(key);
	if (it != layouts_.end()) {
		it->second.used = frame_;
		return &it->second;
	}
	Font &f = font(size);
	Layout l;
	l.font = f.px;
	l.used = frame_;
	std::vector<Placed> &out = l.glyphs;
	float pen = 0, widest = 0;
	int line = 0;
	size_t line_start = 0, space = SIZE_MAX; // space: the line's last space (a place to wrap)
	const auto new_line = [&](float width) {
		widest = std::max(widest, width);
		line++;
		pen = 0;
		line_start = out.size();
		space = SIZE_MAX;
	};
	for (size_t i = 0; i < text.size();) {
		const uint32_t cp = next_code_point(text, i);
		if (cp == '\n') {
			new_line(pen);
			continue;
		}
		if (cp == '\r') {
			continue;
		}
		const float advance = glyph(f, cp).advance;
		if (wrap > 0 && cp != ' ' && pen + advance > wrap && out.size() > line_start) {
			if (space != SIZE_MAX) {
				// The words after the line's last space go on the next line (the space stays, unseen).
				const size_t first = space + 1;
				const float from = first < out.size() ? out[first].x : pen, rest = pen - from;
				new_line(out[space].x);
				for (size_t k = first; k < out.size(); k++) {
					out[k].x -= from;
					out[k].y = float(line) * f.line;
				}
				line_start = first;
				pen = rest;
			}
			if (pen + advance > wrap && out.size() > line_start) {
				new_line(pen); // (a word longer than the line: broken anywhere)
			}
		}
		if (cp == ' ') {
			space = out.size();
		}
		out.push_back({cp, pen, float(line) * f.line});
		pen += advance;
	}
	widest = std::max(widest, pen);
	l.size = {std::ceil(widest), float(line + 1) * f.line};
	return &(layouts_[key] = std::move(l));
}

ui::Vec2 GuiD3D::measure(std::string_view text, float size, float wrap) {
	return layout(text, size, wrap)->size;
}

void GuiD3D::set_icons(const Atlas &atlas) {
	icon_view_.Reset();
	tiles_.clear();
	tile_lum_.clear();
	if (!device_ || atlas.width <= 0 || atlas.height <= 0 ||
			atlas.rgba.size() < size_t(atlas.width) * size_t(atlas.height) * 4) {
		return;
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = UINT(atlas.width);
	td.Height = UINT(atlas.height);
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const D3D11_SUBRESOURCE_DATA data = {atlas.rgba.data(), UINT(atlas.width * 4), 0};
	ComPtr<ID3D11Texture2D> tex;
	if (FAILED(device_->CreateTexture2D(&td, &data, &tex)) ||
			FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, &icon_view_))) {
		log("Couldn't make the item icons' texture");
		return;
	}
	for (size_t t = 0; t < atlas.tiles.size(); t++) {
		std::array<float, 4> uv;
		atlas.tile_uv(int(t), uv.data());
		tiles_.push_back(uv);
		// The tile's average brightness (its seen pixels), for dyeing.
		const std::vector<uint8_t> &src = atlas.tiles[t];
		float lum = -1;
		if (src.size() == size_t(Atlas::TILE * Atlas::TILE * 4)) {
			float sum = 0, seen = 0;
			for (size_t i = 0; i + 3 < src.size(); i += 4) {
				if (src[i + 3] > 0) {
					sum += pixel_lum(&src[i]);
					seen++;
				}
			}
			lum = seen > 0 ? sum / seen : 0.5f;
		}
		tile_lum_.push_back(lum);
	}
}

void GuiD3D::quad(const ui::Rect &r, const float uv[4], uint32_t c0, uint32_t c1, bool vertical, uint32_t dye,
		const float rect[4], const float params[4]) {
	if (verts_.size() + 6 > BATCH) {
		flush();
	}
	// Corners: top left, top right, bottom left, bottom right; c0 at the start, c1 at the end.
	const uint32_t tl = c0, tr = vertical ? c0 : c1, bl = vertical ? c1 : c0, br = c1;
	const auto v = [&](float x, float y, float u, float vv, uint32_t c) {
		Vertex out = {x, y, u, vv, c, dye, {rect[0], rect[1], rect[2], rect[3]}, {params[0], params[1], params[2], params[3]}};
		verts_.push_back(out);
	};
	v(r.x0, r.y0, uv[0], uv[1], tl);
	v(r.x1, r.y0, uv[2], uv[1], tr);
	v(r.x0, r.y1, uv[0], uv[3], bl);
	v(r.x1, r.y0, uv[2], uv[1], tr);
	v(r.x1, r.y1, uv[2], uv[3], br);
	v(r.x0, r.y1, uv[0], uv[3], bl);
}

void GuiD3D::flush() {
	if (verts_.empty()) {
		return;
	}
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(ctx_->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		std::memcpy(m.pData, verts_.data(), verts_.size() * sizeof(Vertex));
		ctx_->Unmap(vb_.Get(), 0);
		ctx_->Draw(UINT(verts_.size()), 0);
	}
	verts_.clear();
}

void GuiD3D::draw(ID3D11DeviceContext *context, int width, int height, const std::vector<ui::DrawCmd> &cmds) {
	frame_++;
	if (!vs_ || cmds.empty() || width <= 0 || height <= 0) {
		return;
	}
	{
		const D3DState kept(context);
		ctx_ = context;
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(context->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			const float to_clip[4] = {2.0f / float(width), 2.0f / float(height), 0, 0};
			std::memcpy(m.pData, to_clip, sizeof(to_clip));
			context->Unmap(cb_.Get(), 0);
		}
		const D3D11_VIEWPORT vp = {0, 0, float(width), float(height), 0, 1};
		context->RSSetViewports(1, &vp);
		context->RSSetState(raster_.Get());
		const float factor[4] = {};
		context->OMSetBlendState(blend_.Get(), factor, 0xffffffff);
		context->OMSetDepthStencilState(depth_.Get(), 0);
		context->IASetInputLayout(input_.Get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		const UINT stride = sizeof(Vertex), offset = 0;
		context->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->VSSetShader(vs_.Get(), nullptr, 0);
		context->PSSetShader(ps_.Get(), nullptr, 0);
		context->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
		ID3D11ShaderResourceView *views[] = {glyph_view_.Get(), icon_view_.Get()};
		context->PSSetShaderResources(0, 2, views);
		context->PSSetSamplers(0, 1, sampler_.GetAddressOf());

		// Clipping: the scissor rectangle, each clip inside the one before (pixels whose centres are in it).
		std::vector<ui::Rect> clips;
		const auto scissor = [&] {
			D3D11_RECT s = {0, 0, width, height};
			for (const ui::Rect &c : clips) {
				s.left = std::max(s.left, LONG(std::lround(c.x0)));
				s.top = std::max(s.top, LONG(std::lround(c.y0)));
				s.right = std::min(s.right, LONG(std::lround(c.x1)));
				s.bottom = std::min(s.bottom, LONG(std::lround(c.y1)));
			}
			s.right = std::max(s.right, s.left);
			s.bottom = std::max(s.bottom, s.top);
			context->RSSetScissorRects(1, &s);
		};
		scissor();

		const float no_uv[4] = {}, none[4] = {};
		for (const ui::DrawCmd &c : cmds) {
			const float rect[4] = {c.rect.x0, c.rect.y0, c.rect.x1, c.rect.y1};
			switch (c.type) {
				case ui::DrawCmd::FILL:
				case ui::DrawCmd::STROKE: {
					const bool fill = c.type == ui::DrawCmd::FILL;
					const float params[4] = {c.rounding, fill ? 0 : c.thickness, fill ? FILL : STROKE, 0};
					// (A pixel wider each way, for the smoothed edge.)
					const ui::Rect r{c.rect.x0 - 1, c.rect.y0 - 1, c.rect.x1 + 1, c.rect.y1 + 1};
					quad(r, no_uv, rgba8(c.color), rgba8(c.color), true, 0, rect, params);
					break;
				}
				case ui::DrawCmd::GRADIENT: {
					const float params[4] = {0, 0, FILL, 0};
					quad(c.rect, no_uv, rgba8(c.color), rgba8(c.color2), c.vertical, 0, rect, params);
					break;
				}
				case ui::DrawCmd::TEXT: {
					Layout *l = layout(c.text, c.size, c.wrap);
					Font &f = font(c.size);
					const float x0 = std::round(c.rect.x0), y0 = std::round(c.rect.y0);
					const float params[4] = {0, 0, GLYPH, 0};
					const uint32_t color = rgba8(c.color);
					for (const Placed &p : l->glyphs) {
						Glyph &g = glyph(f, p.cp);
						if (g.w <= 0 || g.h <= 0) {
							continue;
						}
						if (g.ax < 0 && !to_atlas(f, p.cp, g)) {
							// The atlas is full: draw what uses it, then start it again.
							flush();
							for (auto &[px, font] : fonts_) {
								for (auto &[cp, other] : font.glyphs) {
									other.ax = -1;
								}
							}
							shelf_x_ = shelf_y_ = shelf_h_ = 0;
							if (!to_atlas(f, p.cp, g)) {
								continue;
							}
						}
						if (g.ax < 0 || g.w <= 0) {
							continue;
						}
						const float x = x0 + p.x + float(g.ox), y = y0 + p.y + float(g.oy);
						const float s = 1.0f / GLYPH_ATLAS;
						const float uv[4] = {g.ax * s, g.ay * s, (g.ax + g.w) * s, (g.ay + g.h) * s};
						quad({x, y, x + float(g.w), y + float(g.h)}, uv, color, color, true, 0, none, params);
					}
					break;
				}
				case ui::DrawCmd::IMAGE: {
					if (!icon_view_ || c.image < 0 || c.image >= int(tiles_.size())) {
						break;
					}
					const float lum = tile_lum_[size_t(c.image)];
					const bool dyed = c.dye != 0 && lum >= 0;
					// The dye as RGBA8 (gocube::Dye is 0xFFRRGGBB); alpha 255 says dyed.
					const uint32_t dye = dyed ? (c.dye >> 16 & 255) | (c.dye & 0xff00) | (c.dye & 255) << 16 | 0xff000000u : 0;
					const float params[4] = {0, 0, ICON, lum};
					const uint32_t color = uint32_t(c.color.a) << 24 | 0xffffff;
					quad(c.rect, tiles_[size_t(c.image)].data(), color, color, true, dye, none, params);
					break;
				}
				case ui::DrawCmd::CLIP:
					flush();
					clips.push_back(c.rect);
					scissor();
					break;
				case ui::DrawCmd::UNCLIP:
					if (!clips.empty()) {
						flush();
						clips.pop_back();
						scissor();
					}
					break;
			}
		}
		flush();
		ctx_ = nullptr;
	}
	// Text layouts not used for a while go.
	if (frame_ % 120 == 0) {
		for (auto it = layouts_.begin(); it != layouts_.end();) {
			it = frame_ - it->second.used > 120 ? layouts_.erase(it) : std::next(it);
		}
	}
}

} // namespace gc
