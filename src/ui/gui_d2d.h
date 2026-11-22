#pragma once
// Draws the UI's commands (ui/gui.h) with Direct2D over the back buffer, on
// the game's Direct3D 11 device, and measures text with DirectWrite (in
// Windows' own UI font, Segoe UI). Item icons come from the block atlas (nearest-neighbour, as the
// world's pixels). The Direct3D state the world's renderers set is kept
// across it.

#include "content/content.h"
#include "ui/gui.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gc {

class GuiD2D {
public:
	bool init(ID3D11Device *device);
	// For Gui::Measure.
	ui::Vec2 measure(std::string_view text, float size, float wrap);
	// The block atlas: item icons are its tiles.
	void set_icons(const Atlas &atlas);
	// Draws over the swap chain's back buffer.
	void draw(IDXGISwapChain1 *swap_chain, ID3D11DeviceContext *context, const std::vector<ui::DrawCmd> &cmds);

private:
	struct Layout {
		Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
		ui::Vec2 size;
		int used = 0;
	};
	Layout *layout(std::string_view text, float size, float wrap);
	IDWriteTextFormat *format(float size);

	Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
	Microsoft::WRL::ComPtr<ID2D1Device> device_;
	Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
	Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
	std::wstring family_ = L"Segoe UI"; // (Windows' UI font: init)
	std::unordered_map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_; // by size x 4
	std::unordered_map<std::string, Layout> layouts_;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> icons_;
	std::vector<D2D1_RECT_F> tiles_; // each tile's rectangle in it
	// Dyed blocks' icons, made when first drawn: by tile and dye.
	ID2D1Bitmap1 *dyed_icon(int tile, uint32_t dye);
	std::vector<std::vector<uint8_t>> tile_pixels_; // RGBA, Atlas::TILE square
	std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> dyed_;
	int frame_ = 0;
};

} // namespace gc
