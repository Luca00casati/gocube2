#include "ui/gui_d2d.h"

#include "log.h"
#include "ui/ui_font.h"

#include <algorithm>
#include <cmath>

namespace gc {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring wide(std::string_view s) {
	if (s.empty()) {
		return {};
	}
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(std::max(n, 0)), L'\0');
	if (n > 0) {
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	}
	return w;
}

D2D1_COLOR_F color(ui::Color c) {
	return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}

D2D1_RECT_F rect(const ui::Rect &r) {
	return {r.x0, r.y0, r.x1, r.y1};
}

// The Direct3D 11 state Direct2D could change, kept around its drawing.
struct D3DState {
	static constexpr UINT SLOTS = 8;
	ID3D11DeviceContext *ctx;
	UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
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

bool GuiD2D::init(ID3D11Device *device) {
	ComPtr<IDXGIDevice> dxgi;
	D2D1_FACTORY_OPTIONS options = {};
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
			FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
					reinterpret_cast<void **>(factory_.GetAddressOf()))) ||
			FAILED(factory_->CreateDevice(dxgi.Get(), &device_)) ||
			FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)) ||
			FAILED(dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_)) ||
			FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
					reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf())))) {
		return false;
	}
	dc_->SetDpi(96, 96); // (units are pixels)
	dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	if (!load_font()) {
		log("Couldn't load the UI's font: Segoe UI instead");
	}
	return true;
}

GuiD2D::~GuiD2D() {
	ComPtr<IDWriteFactory5> f5;
	if (font_loader_ && SUCCEEDED(dwrite_.As(&f5))) {
		font_.Reset();
		f5->UnregisterFontFileLoader(font_loader_.Get());
	}
}

bool GuiD2D::load_font() {
	// The font file, from the executable's resources.
	const HMODULE exe = GetModuleHandleW(nullptr);
	HRSRC res = FindResourceW(exe, MAKEINTRESOURCEW(GOCUBE_UI_FONT), MAKEINTRESOURCEW(10)); // (RT_RCDATA)
	HGLOBAL data = res ? LoadResource(exe, res) : nullptr;
	const void *bytes = data ? LockResource(data) : nullptr;
	const DWORD size = res ? SizeofResource(exe, res) : 0;
	ComPtr<IDWriteFactory5> f5;
	if (!bytes || size == 0 || FAILED(dwrite_.As(&f5))) {
		return false;
	}
	// A collection of just that font (the resource stays mapped: no copy).
	ComPtr<IDWriteInMemoryFontFileLoader> loader;
	if (FAILED(f5->CreateInMemoryFontFileLoader(&loader)) || FAILED(f5->RegisterFontFileLoader(loader.Get()))) {
		return false;
	}
	font_loader_ = loader;
	ComPtr<IDWriteFontFile> file;
	ComPtr<IDWriteFontSetBuilder1> builder;
	ComPtr<IDWriteFontSet> set;
	ComPtr<IDWriteFontCollection1> collection;
	ComPtr<IDWriteFontFamily1> family;
	ComPtr<IDWriteLocalizedStrings> names;
	UINT32 length = 0;
	if (FAILED(loader->CreateInMemoryFontFileReference(f5.Get(), bytes, size, f5.Get(), &file)) ||
			FAILED(f5->CreateFontSetBuilder(&builder)) || FAILED(builder->AddFontFile(file.Get())) ||
			FAILED(builder->CreateFontSet(&set)) || FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &collection)) ||
			collection->GetFontFamilyCount() == 0 || FAILED(collection->GetFontFamily(0, &family)) ||
			FAILED(family->GetFamilyNames(&names)) || FAILED(names->GetStringLength(0, &length))) {
		return false;
	}
	std::wstring name(size_t(length) + 1, wchar_t(0));
	if (FAILED(names->GetString(0, name.data(), length + 1))) {
		return false;
	}
	name.resize(length);
	font_ = collection;
	family_ = name;
	return true;
}

IDWriteTextFormat *GuiD2D::format(float size) {
	const int key = int(std::lround(size * 4));
	auto it = formats_.find(key);
	if (it != formats_.end()) {
		return it->second.Get();
	}
	ComPtr<IDWriteTextFormat> f;
	if (FAILED(dwrite_->CreateTextFormat(family_.c_str(), font_.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, float(key) / 4.0f, L"en-us", &f))) {
		return nullptr;
	}
	return (formats_[key] = f).Get();
}

GuiD2D::Layout *GuiD2D::layout(std::string_view text, float size, float wrap) {
	std::string key = std::to_string(int(std::lround(size * 4))) + "|" + std::to_string(int(wrap)) + "|";
	key += text;
	auto it = layouts_.find(key);
	if (it != layouts_.end()) {
		it->second.used = frame_;
		return &it->second;
	}
	IDWriteTextFormat *f = format(size);
	if (!f) {
		return nullptr;
	}
	const std::wstring w = wide(text);
	Layout l;
	if (FAILED(dwrite_->CreateTextLayout(w.c_str(), UINT32(w.size()), f, wrap > 0 ? wrap : 100000.0f, 100000.0f,
				&l.layout))) {
		return nullptr;
	}
	l.layout->SetWordWrapping(wrap > 0 ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
	DWRITE_TEXT_METRICS m;
	l.layout->GetMetrics(&m);
	l.size = {std::ceil(m.widthIncludingTrailingWhitespace), std::ceil(m.height)};
	l.used = frame_;
	return &(layouts_[key] = std::move(l));
}

ui::Vec2 GuiD2D::measure(std::string_view text, float size, float wrap) {
	const Layout *l = layout(text, size, wrap);
	return l ? l->size : ui::Vec2{float(text.size()) * size * 0.5f, size * 1.33f};
}

ID2D1Bitmap1 *GuiD2D::dyed_icon(int tile, uint32_t dye) {
	const uint64_t key = uint64_t(uint32_t(tile)) << 32 | dye;
	if (auto it = dyed_.find(key); it != dyed_.end()) {
		return it->second.Get();
	}
	if (dyed_.size() > 1024) {
		dyed_.clear(); // (a few hundred dyes on screen at most)
	}
	const std::vector<uint8_t> &src = tile_pixels_[size_t(tile)];
	std::vector<uint8_t> px(src.size());
	for (size_t i = 0; i + 3 < px.size(); i += 4) {
		uint8_t rgb[3] = {src[i], src[i + 1], src[i + 2]};
		dye_pixel(rgb, dye);
		const unsigned a = src[i + 3];
		for (int k = 0; k < 3; k++) {
			px[i + k] = uint8_t((rgb[k] * a + 127) / 255);
		}
		px[i + 3] = uint8_t(a);
	}
	ComPtr<ID2D1Bitmap1> bitmap;
	const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
			D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
	if (FAILED(dc_->CreateBitmap(D2D1::SizeU(Atlas::TILE, Atlas::TILE), px.data(), Atlas::TILE * 4, props, &bitmap))) {
		return nullptr;
	}
	return (dyed_[key] = bitmap).Get();
}

void GuiD2D::set_icons(const Atlas &atlas) {
	icons_.Reset();
	tiles_.clear();
	dyed_.clear();
	tile_pixels_ = atlas.tiles;
	if (!dc_ || atlas.width <= 0 || atlas.height <= 0) {
		return;
	}
	std::vector<uint8_t> px(atlas.rgba.size()); // (premultiplied, as Direct2D draws)
	for (size_t i = 0; i + 3 < px.size(); i += 4) {
		const unsigned a = atlas.rgba[i + 3];
		for (int k = 0; k < 3; k++) {
			px[i + k] = uint8_t((atlas.rgba[i + k] * a + 127) / 255);
		}
		px[i + 3] = uint8_t(a);
	}
	const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
			D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
	if (FAILED(dc_->CreateBitmap(D2D1::SizeU(UINT32(atlas.width), UINT32(atlas.height)), px.data(),
				UINT32(atlas.width * 4), props, &icons_))) {
		log("Couldn't make the item icons' bitmap");
		return;
	}
	for (size_t t = 0; t < atlas.tiles.size(); t++) {
		float uv[4];
		atlas.tile_uv(int(t), uv);
		tiles_.push_back({uv[0] * atlas.width, uv[1] * atlas.height, uv[2] * atlas.width, uv[3] * atlas.height});
	}
}

void GuiD2D::draw(IDXGISwapChain1 *swap_chain, ID3D11DeviceContext *context, const std::vector<ui::DrawCmd> &cmds) {
	frame_++;
	if (!dc_ || cmds.empty()) {
		return;
	}
	ComPtr<IDXGISurface> surface;
	ComPtr<ID2D1Bitmap1> target;
	const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
	if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&surface))) ||
			FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) {
		return;
	}
	{
		const D3DState kept(context);
		dc_->SetTarget(target.Get());
		dc_->BeginDraw();
		dc_->SetTransform(D2D1::Matrix3x2F::Identity());
		int clips = 0;
		for (const ui::DrawCmd &c : cmds) {
			switch (c.type) {
				case ui::DrawCmd::FILL:
					brush_->SetColor(color(c.color));
					if (c.rounding > 0) {
						dc_->FillRoundedRectangle(D2D1::RoundedRect(rect(c.rect), c.rounding, c.rounding), brush_.Get());
					} else {
						dc_->FillRectangle(rect(c.rect), brush_.Get());
					}
					break;
				case ui::DrawCmd::STROKE: {
					brush_->SetColor(color(c.color));
					const float h = c.thickness / 2;
					const D2D1_RECT_F r{c.rect.x0 + h, c.rect.y0 + h, c.rect.x1 - h, c.rect.y1 - h};
					if (c.rounding > 0) {
						dc_->DrawRoundedRectangle(D2D1::RoundedRect(r, c.rounding, c.rounding), brush_.Get(), c.thickness);
					} else {
						dc_->DrawRectangle(r, brush_.Get(), c.thickness);
					}
					break;
				}
				case ui::DrawCmd::GRADIENT: {
					const D2D1_GRADIENT_STOP stops[2] = {{0, color(c.color)}, {1, color(c.color2)}};
					ComPtr<ID2D1GradientStopCollection> collection;
					ComPtr<ID2D1LinearGradientBrush> g;
					const D2D1_POINT_2F end = c.vertical ? D2D1::Point2F(c.rect.x0, c.rect.y1) : D2D1::Point2F(c.rect.x1, c.rect.y0);
					if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, &collection)) &&
							SUCCEEDED(dc_->CreateLinearGradientBrush(
									D2D1::LinearGradientBrushProperties(D2D1::Point2F(c.rect.x0, c.rect.y0), end),
									collection.Get(), &g))) {
						dc_->FillRectangle(rect(c.rect), g.Get());
					}
					break;
				}
				case ui::DrawCmd::TEXT:
					if (Layout *l = layout(c.text, c.size, c.wrap)) {
						brush_->SetColor(color(c.color));
						dc_->DrawTextLayout(D2D1::Point2F(c.rect.x0, c.rect.y0), l->layout.Get(), brush_.Get());
					}
					break;
				case ui::DrawCmd::IMAGE:
					if (c.dye != 0 && c.image >= 0 && c.image < int(tile_pixels_.size()) &&
							tile_pixels_[size_t(c.image)].size() == size_t(Atlas::TILE * Atlas::TILE * 4)) {
						if (ID2D1Bitmap1 *b = dyed_icon(c.image, c.dye)) {
							dc_->DrawBitmap(b, rect(c.rect), c.color.a / 255.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
						}
					} else if (icons_ && c.image >= 0 && c.image < int(tiles_.size())) {
						dc_->DrawBitmap(icons_.Get(), rect(c.rect), c.color.a / 255.0f,
								D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &tiles_[size_t(c.image)]);
					}
					break;
				case ui::DrawCmd::CLIP:
					dc_->PushAxisAlignedClip(rect(c.rect), D2D1_ANTIALIAS_MODE_ALIASED);
					clips++;
					break;
				case ui::DrawCmd::UNCLIP:
					if (clips > 0) {
						dc_->PopAxisAlignedClip();
						clips--;
					}
					break;
			}
		}
		while (clips-- > 0) {
			dc_->PopAxisAlignedClip();
		}
		dc_->EndDraw();
		dc_->SetTarget(nullptr);
	}
	// Text layouts not used for a while go.
	if (frame_ % 120 == 0) {
		for (auto it = layouts_.begin(); it != layouts_.end();) {
			it = frame_ - it->second.used > 120 ? layouts_.erase(it) : std::next(it);
		}
	}
}

} // namespace gc
