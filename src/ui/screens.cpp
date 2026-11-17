#include "ui/screens.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace gc {

namespace {

const ui::Color SLOT_BG = ui::rgba(255, 255, 255, 12);
const ui::Color SLOT_HOVER = ui::rgba(255, 255, 255, 40);
const ui::Color SLOT_BORDER = ui::rgba(255, 255, 255, 40);
const ui::Color SLOT_HIGHLIGHT = ui::rgba(255, 255, 255, 235);

void count_text(ui::Gui &gui, ui::DrawList &dl, ui::Vec2 corner, int count) {
	const std::string n = std::to_string(count);
	const ui::Vec2 size = gui.text_size(n);
	dl.text({corner.x - size.x - 3, corner.y - size.y - 1}, ui::rgba(0, 0, 0), n, gui.theme.font);
	dl.text({corner.x - size.x - 4, corner.y - size.y - 2}, ui::rgba(255, 255, 255), n, gui.theme.font);
}

// Dims the world behind a screen.
void dim_background(ui::Gui &gui) {
	gui.background().fill({0, 0, gui.input().width, gui.input().height}, ui::rgba(0, 0, 0, 140));
}

ui::WindowOptions centred(ui::Gui &gui) {
	ui::WindowOptions o;
	o.pos = {gui.input().width * 0.5f, gui.input().height * 0.5f};
	o.pivot = {0.5f, 0.5f};
	o.alpha = 0.92f;
	return o;
}

} // namespace

void UiIcons::draw(ui::DrawList &dl, const std::string &id, ui::Rect r) const {
	dl.image(r, table->icon(id));
}

bool slot_button(ui::Gui &gui, const char *id, const Stack &stack, const UiIcons &icons, bool highlight,
		bool show_count, float size) {
	gui.push_id(id);
	const bool clicked = gui.invisible_button("slot", {size, size});
	const bool hovered = gui.item_hovered();
	const ui::Rect r = gui.item_rect();
	ui::DrawList &dl = gui.draw_list();
	dl.fill(r, hovered ? SLOT_HOVER : SLOT_BG, 4);
	dl.stroke(r, highlight ? SLOT_HIGHLIGHT : SLOT_BORDER, 4, highlight ? 2.0f : 1.0f);
	if (!stack.empty()) {
		const float inset = std::floor(size * 0.14f);
		icons.draw(dl, stack.id, {r.x0 + inset, r.y0 + inset, r.x1 - inset, r.y1 - inset});
		if (show_count && stack.count > 1) {
			count_text(gui, dl, {r.x1, r.y1}, stack.count);
		}
		if (hovered) {
			gui.tooltip(display_name(*icons.content, stack.id));
		}
	}
	gui.pop_id();
	return clicked;
}

bool inventory_screen(ui::Gui &gui, Game &game, const UiIcons &icons) {
	dim_background(gui);
	Inventory &inv = game.inventory();
	const bool creative = game.creative();
	// Armour slots and crafting only if the mods have any (the base game has neither).
	const bool armour = std::any_of(icons.content->items.begin(), icons.content->items.end(),
			[](const auto &e) { return e.second.contains("armor"); });
	const bool crafting = !creative && !game.hand_recipes().empty();
	gui.begin_window("Inventory", centred(gui));
	gui.push_spacing({4, 4});

	// Left: the backpack (rows of 9), armour, the hotbar row.
	gui.begin_group();
	gui.text("Inventory");
	// (Through the game: a client's host does each click to its copy too.)
	auto click = [&](int i) { game.inventory_action(Data::array({"click", i})); };
	for (int i = Inventory::HOTBAR; i < Inventory::SIZE; i++) {
		if ((i - Inventory::HOTBAR) % Inventory::HOTBAR != 0) {
			gui.same_line();
		}
		if (slot_button(gui, std::to_string(i).c_str(), inv.slots[i], icons, false, !creative)) {
			click(i);
		}
	}
	if (!creative && armour) {
		gui.space(6);
		gui.text("Armour");
		for (int k = 0; k < 4; k++) {
			if (k) {
				gui.same_line();
			}
			const std::string id = std::string("armor_") + Inventory::ARMOR_SLOTS[k];
			if (slot_button(gui, id.c_str(), inv.armor[k], icons, false, false)) {
				game.inventory_action(Data::array({"armor", k}));
			}
			if (gui.item_hovered() && inv.armor[k].empty()) {
				gui.tooltip(display_name(*icons.content, Inventory::ARMOR_SLOTS[k]));
			}
		}
	}
	gui.space(6);
	gui.text("Hotbar");
	for (int i = 0; i < Inventory::HOTBAR; i++) {
		if (i) {
			gui.same_line();
		}
		if (slot_button(gui, std::to_string(i).c_str(), inv.slots[i], icons, i == game.selected(), !creative)) {
			click(i);
		}
	}
	if (!creative && !crafting) {
		gui.text_dim("E or Esc to close");
	}
	gui.end_group();

	// Right: crafting (survival, if there is any) or every block and item (creative).
	if (creative || crafting) {
		gui.same_line(-1, 24);
		gui.begin_group();
	}
	if (creative) {
		gui.text("All blocks and items");
		gui.begin_child("palette", {9 * 44 + 24, 380}, false);
		std::vector<std::string> ids;
		for (const std::string &id : icons.content->block_ids()) {
			auto it = icons.content->blocks.find(id);
			if (it != icons.content->blocks.end() && !it->second.contains("fluid") && id != "core:air" &&
					!it->second.contains("variant_of") && !it->second.value("hidden", false)) {
				ids.push_back(id);
			}
		}
		for (const auto &[id, def] : icons.content->items) {
			ids.push_back(id);
		}
		for (size_t k = 0; k < ids.size(); k++) {
			if (k % 9) {
				gui.same_line();
			}
			if (slot_button(gui, ids[k].c_str(), {ids[k], 1}, icons, false, false, 40)) {
				game.inventory_action(Data::array({"pick", ids[k]}));
			}
		}
		gui.end_child();
		gui.text_dim("Click one, then a slot. E or Esc to close");
	} else if (crafting) {
		gui.text("Crafting");
		gui.begin_child("recipes", {340, 380}, false);
		for (const Recipe &r : game.hand_recipes()) {
			gui.push_id(r.id);
			gui.dummy({32, 32});
			icons.draw(gui.draw_list(), r.output, gui.item_rect());
			gui.same_line();
			gui.begin_group();
			gui.text(std::to_string(r.count) + " x " + display_name(*icons.content, r.output));
			std::string parts;
			for (const auto &[item, n] : r.ingredients) {
				parts += (parts.empty() ? "" : ", ") + std::to_string(n) + " " + display_name(*icons.content, item);
			}
			gui.text_dim(parts);
			gui.end_group();
			gui.same_line(270);
			gui.begin_disabled(!game.can_craft(r));
			if (gui.button("Craft")) {
				game.inventory_action(Data::array({"craft", r.id}));
			}
			gui.end_disabled();
			gui.pop_id();
		}
		gui.end_child();
		gui.text_dim("E or Esc to close");
	}
	if (creative || crafting) {
		gui.end_group();
	}
	gui.pop_spacing();
	gui.end_window();
	return true;
}

namespace {

// The player's backpack and hotbar rows, for a container screen.
void inventory_rows(ui::Gui &gui, Game &game, const UiIcons &icons) {
	Inventory &inv = game.inventory();
	gui.text("Inventory");
	for (int i = Inventory::HOTBAR; i < Inventory::SIZE; i++) {
		if ((i - Inventory::HOTBAR) % Inventory::HOTBAR != 0) {
			gui.same_line();
		}
		if (slot_button(gui, ("inv" + std::to_string(i)).c_str(), inv.slots[i], icons, false, true)) {
			game.inventory_action(Data::array({"click", i}));
		}
	}
	gui.space(4);
	for (int i = 0; i < Inventory::HOTBAR; i++) {
		if (i) {
			gui.same_line();
		}
		if (slot_button(gui, ("inv" + std::to_string(i)).c_str(), inv.slots[i], icons, i == game.selected(), true)) {
			game.inventory_action(Data::array({"click", i}));
		}
	}
}

void bar(ui::Gui &gui, float fraction, ui::Vec2 size, ui::Color color) {
	gui.dummy(size);
	const ui::Rect r = gui.item_rect();
	ui::DrawList &dl = gui.draw_list();
	dl.fill(r, ui::rgba(0, 0, 0, 160), 3);
	dl.fill({r.x0, r.y0, r.x0 + size.x * std::clamp(fraction, 0.0f, 1.0f), r.y1}, color, 3);
}

} // namespace

bool container_screen(ui::Gui &gui, Game &game, const BlockPos &pos, const UiIcons &icons) {
	BlockEntity *e = game.block_entities().at(pos);
	if (!e) {
		return false; // it went away
	}
	dim_background(gui);
	gui.begin_window("Container", centred(gui));
	gui.push_spacing({4, 4});
	auto click = [&](int slot) { game.inventory_action(Data::array({"container", pos.x, pos.y, pos.z, slot})); };
	if (e->type == "furnace") {
		gui.text("Furnace");
		float burn, progress;
		BlockEntities::furnace_status(*e, burn, progress);
		gui.begin_group();
		gui.text_dim("Input");
		if (slot_button(gui, "in", e->slots[BlockEntities::FURNACE_INPUT], icons, false, true)) {
			click(BlockEntities::FURNACE_INPUT);
		}
		bar(gui, burn, {48, 6}, ui::rgba(255, 140, 40)); // the flame: fuel left
		gui.text_dim("Fuel");
		if (slot_button(gui, "fuel", e->slots[BlockEntities::FURNACE_FUEL], icons, false, true)) {
			click(BlockEntities::FURNACE_FUEL);
		}
		gui.end_group();
		gui.same_line(-1, 16);
		gui.begin_group();
		gui.space(34);
		bar(gui, progress, {80, 10}, ui::rgba(255, 255, 255, 230)); // smelting
		gui.end_group();
		gui.same_line(-1, 16);
		gui.begin_group();
		gui.text_dim("Output");
		if (slot_button(gui, "out", e->slots[BlockEntities::FURNACE_OUTPUT], icons, false, true)) {
			click(BlockEntities::FURNACE_OUTPUT);
		}
		gui.end_group();
	} else {
		gui.text("Chest");
		for (int i = 0; i < int(e->slots.size()); i++) {
			if (i % 9) {
				gui.same_line();
			}
			if (slot_button(gui, ("c" + std::to_string(i)).c_str(), e->slots[i], icons, false, true)) {
				click(i);
			}
		}
	}
	gui.space(8);
	inventory_rows(gui, game, icons);
	gui.text_dim("E or Esc to close");
	gui.pop_spacing();
	gui.end_window();
	return true;
}

void draw_held(ui::Gui &gui, const Stack &held, const UiIcons &icons) {
	if (held.empty()) {
		return;
	}
	const ui::Vec2 m = gui.input().mouse;
	ui::DrawList &dl = gui.foreground();
	icons.draw(dl, held.id, {m.x + 4, m.y + 4, m.x + 40, m.y + 40});
	if (held.count > 1) {
		count_text(gui, dl, {m.x + 40, m.y + 40}, held.count);
	}
}

} // namespace gc
