#pragma once
// Draws the UI's commands (ui/gui.h) with Direct2D over the back buffer, on
// the game's Direct3D 11 device, and measures text with DirectWrite (the
// font built into the executable, ui/ui_font.h; Segoe UI if it can't be
// loaded). Item icons come from the block atlas (nearest-neighbour, as the
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
	~GuiD2D();
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
	bool load_font(); // the built-in one, into font_ and family_
	Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
	Microsoft::WRL::ComPtr<IDWriteInMemoryFontFileLoader> font_loader_;
	Microsoft::WRL::ComPtr<IDWriteFontCollection1> font_;
	std::wstring family_ = L"Segoe UI";
	std::unordered_map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_; // by size x 4
	std::unordered_map<std::string, Layout> layouts_;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> icons_;
	std::vector<D2D1_RECT_F> tiles_; // each tile's rectangle in it
	int frame_ = 0;
};

} // namespace gc
