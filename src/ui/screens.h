#pragma once
// Game screens drawn with Dear ImGui: the inventory (E) with hand crafting,
// or in creative a palette of every block and item. As the Godot version's
// InventoryScreen: click a slot to pick its stack up, click another to put it
// down (same items stack, others swap).

#include "content/content.h"
#include "game/game.h"

#include <imgui.h>

namespace gc {

struct UiIcons {
	ImTextureID atlas = 0;      // the block atlas (plain view)
	const BlockTable *table = nullptr;
	const Content *content = nullptr;

	// Draws an item's icon in a rectangle.
	void draw(ImDrawList *dl, const std::string &id, ImVec2 a, ImVec2 b) const;
};

// A slot: its icon and count, a tooltip with the item's name. True when clicked.
bool slot_button(const char *id, const Stack &stack, const UiIcons &icons, bool highlight, bool show_count,
		float size = 48);

// Draws the inventory screen (an open ImGui frame). False when it asks to close.
bool inventory_screen(Game &game, const UiIcons &icons);

// The stack the mouse holds, drawn at the mouse (call last in the frame).
void draw_held(const Stack &held, const UiIcons &icons);

} // namespace gc
