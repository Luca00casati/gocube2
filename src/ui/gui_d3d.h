#pragma once
// Draws the UI's commands (ui/gui.h) with Direct3D 11 over the back buffer,
// one shader for everything (shaders/gui.hlsl): rectangles (rounded, outlined,
// gradients) worked out per pixel, text from a glyph atlas, item icons from
// the block atlas (nearest-neighbour, as the world's pixels; dyed in the
// shader). Text is in Windows' own UI font (Segoe UI unless the system says
// otherwise), its glyphs rasterized by GDI (greyscale) and laid out here:
// word wrap, newlines, whole-pixel advances; no kerning and no shaping
// (complex scripts), no fallback fonts. The Direct3D state the world's
// renderers set is kept across it.

#include "content/content.h"
#include "ui/gui.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gc {

class GuiD3D {
public:
	GuiD3D() = default;
	GuiD3D(const GuiD3D &) = delete;
	GuiD3D &operator=(const GuiD3D &) = delete;
	~GuiD3D();

	bool init(ID3D11Device *device);
	// For Gui::Measure (GDI only: works before init too).
	ui::Vec2 measure(std::string_view text, float size, float wrap);
	// The block atlas: item icons are its tiles.
	void set_icons(const Atlas &atlas);
	// Draws over the render target bound, a width x height screen.
	void draw(ID3D11DeviceContext *context, int width, int height, const std::vector<ui::DrawCmd> &cmds);

	struct Vertex {
		float x, y, u, v;
		uint32_t color, dye; // RGBA8
		float rect[4];
		float params[4]; // rounding, thickness, mode, average brightness
	};

private:
	struct Glyph {
		float advance = 0;
		int w = 0, h = 0;   // the bitmap (0: nothing to draw)
		int ox = 0, oy = 0; // its top left from the pen, on the line's top
		int ax = -1, ay = 0; // where it is in the glyph atlas (-1: not there)
	};
	struct Font {
		void *gdi = nullptr; // HFONT
		int px = 0;
		float ascent = 0, line = 0;
		std::unordered_map<uint32_t, Glyph> glyphs;
	};
	struct Placed {
		uint32_t cp;
		float x, y;
	};
	struct Layout {
		int font = 0;
		ui::Vec2 size;
		std::vector<Placed> glyphs;
		int used = 0;
	};

	Font &font(float size);
	Glyph &glyph(Font &f, uint32_t cp);
	bool to_atlas(Font &f, uint32_t cp, Glyph &g);
	Layout *layout(std::string_view text, float size, float wrap);

	void quad(const ui::Rect &r, const float uv[4], uint32_t c0, uint32_t c1, bool vertical, uint32_t dye,
			const float rect[4], const float params[4]);
	void flush();

	void *dc_ = nullptr; // HDC: memory DC for GDI
	std::wstring family_ = L"Segoe UI";
	std::unordered_map<int, Font> fonts_; // by pixel size
	std::unordered_map<std::string, Layout> layouts_;
	int frame_ = 0;

	Microsoft::WRL::ComPtr<ID3D11Device> device_;
	ID3D11DeviceContext *ctx_ = nullptr; // (while drawing)
	Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> input_;
	Microsoft::WRL::ComPtr<ID3D11Buffer> vb_, cb_;
	Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> glyph_tex_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> glyph_view_, icon_view_;
	int shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0; // packing the glyph atlas
	std::vector<Vertex> verts_;

	std::vector<std::array<float, 4>> tiles_; // each icon's uv rectangle
	std::vector<float> tile_lum_;             // each tile's average brightness (-1: no pixels of its own)
};

} // namespace gc
