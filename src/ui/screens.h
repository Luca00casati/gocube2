#pragma once
// Game screens (ui/gui.h): the inventory (E) with hand crafting, or in
// creative a palette of every block and item: click a slot to pick its
// stack up, click another to put it down (same items stack, others swap).

#include "content/content.h"
#include "game/game.h"
#include "ui/gui.h"

namespace gc {

struct UiIcons {
	const BlockTable *table = nullptr;
	const Content *content = nullptr;

	// Draws an item's icon in a rectangle.
	void draw(ui::DrawList &dl, const std::string &id, ui::Rect r) const;
};

// A slot: its icon and count, a tooltip with the item's name. True when clicked.
bool slot_button(ui::Gui &gui, const char *id, const Stack &stack, const UiIcons &icons, bool highlight,
		bool show_count, float size = 48);

// Draws the inventory screen. False when it asks to close.
bool inventory_screen(ui::Gui &gui, Game &game, const UiIcons &icons);

// A chest or furnace at `pos` and the player's inventory. False when it asks
// to close (also when the container is gone).
bool container_screen(ui::Gui &gui, Game &game, const BlockPos &pos, const UiIcons &icons);

// The stack the mouse holds, drawn at the mouse (call last in the frame).
void draw_held(ui::Gui &gui, const Stack &held, const UiIcons &icons);

} // namespace gc
