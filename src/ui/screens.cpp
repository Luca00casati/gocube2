#include "ui/screens.h"

#include <algorithm>
#include <string>
#include <vector>

namespace gc {

namespace {

const ImU32 SLOT_BG = IM_COL32(30, 30, 34, 230);
const ImU32 SLOT_HOVER = IM_COL32(60, 60, 70, 240);
const ImU32 SLOT_BORDER = IM_COL32(255, 255, 255, 50);
const ImU32 SLOT_HIGHLIGHT = IM_COL32(255, 230, 120, 255);

void count_text(ImDrawList *dl, ImVec2 b, int count) {
	const std::string n = std::to_string(count);
	const ImVec2 size = ImGui::CalcTextSize(n.c_str());
	dl->AddText(ImVec2(b.x - size.x - 3, b.y - size.y - 1), IM_COL32(0, 0, 0, 255), n.c_str());
	dl->AddText(ImVec2(b.x - size.x - 4, b.y - size.y - 2), IM_COL32(255, 255, 255, 255), n.c_str());
}

// Dims the world behind a screen.
void dim_background() {
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
}

} // namespace

void UiIcons::draw(ImDrawList *dl, const std::string &id, ImVec2 a, ImVec2 b) const {
	float uv[4];
	table->atlas.tile_uv(table->icon(id), uv);
	dl->AddImage(atlas, a, b, ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
}

bool slot_button(const char *id, const Stack &stack, const UiIcons &icons, bool highlight, bool show_count, float size) {
	ImGui::PushID(id);
	const ImVec2 a = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::InvisibleButton("slot", ImVec2(size, size));
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 b(a.x + size, a.y + size);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, hovered ? SLOT_HOVER : SLOT_BG, 4.0f);
	dl->AddRect(a, b, highlight ? SLOT_HIGHLIGHT : SLOT_BORDER, 4.0f, 0, highlight ? 2.0f : 1.0f);
	if (!stack.empty()) {
		const float inset = size * 0.14f;
		icons.draw(dl, stack.id, ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset));
		if (show_count && stack.count > 1) {
			count_text(dl, b, stack.count);
		}
		if (hovered) {
			ImGui::SetTooltip("%s", display_name(*icons.content, stack.id).c_str());
		}
	}
	ImGui::PopID();
	return clicked;
}

bool inventory_screen(Game &game, const UiIcons &icons) {
	dim_background();
	Inventory &inv = game.inventory();
	const bool creative = game.creative();
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.92f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
	bool keep_open = true;
	ImGui::Begin("Inventory", nullptr, flags);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

	// Left: the backpack (rows of 9), armour, the hotbar row.
	ImGui::BeginGroup();
	ImGui::TextUnformatted("Inventory");
	// (Through the game: a client's host does each click to its copy too.)
	auto click = [&](int i) { game.inventory_action(Json::array({"click", i})); };
	for (int i = Inventory::HOTBAR; i < Inventory::SIZE; i++) {
		if ((i - Inventory::HOTBAR) % Inventory::HOTBAR != 0) {
			ImGui::SameLine();
		}
		if (slot_button(std::to_string(i).c_str(), inv.slots[i], icons, false, !creative)) {
			click(i);
		}
	}
	if (!creative) {
		ImGui::Dummy(ImVec2(0, 6));
		ImGui::TextUnformatted("Armour");
		for (int k = 0; k < 4; k++) {
			if (k) {
				ImGui::SameLine();
			}
			const std::string id = std::string("armor_") + Inventory::ARMOR_SLOTS[k];
			if (slot_button(id.c_str(), inv.armor[k], icons, false, false)) {
				game.inventory_action(Json::array({"armor", k}));
			}
			if (ImGui::IsItemHovered() && inv.armor[k].empty()) {
				ImGui::SetTooltip("%s", display_name(*icons.content, Inventory::ARMOR_SLOTS[k]).c_str());
			}
		}
	}
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::TextUnformatted("Hotbar");
	for (int i = 0; i < Inventory::HOTBAR; i++) {
		if (i) {
			ImGui::SameLine();
		}
		if (slot_button(std::to_string(i).c_str(), inv.slots[i], icons, i == game.selected(), !creative)) {
			click(i);
		}
	}
	ImGui::EndGroup();
	ImGui::SameLine(0, 24);

	// Right: crafting (survival) or every block and item (creative).
	ImGui::BeginGroup();
	if (creative) {
		ImGui::TextUnformatted("All blocks and items");
		ImGui::BeginChild("palette", ImVec2(9 * 44 + 24, 380), ImGuiChildFlags_None);
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
				ImGui::SameLine();
			}
			if (slot_button(ids[k].c_str(), {ids[k], 1}, icons, false, false, 40)) {
				game.inventory_action(Json::array({"pick", ids[k]}));
			}
		}
		ImGui::EndChild();
		ImGui::TextDisabled("Click one, then a slot. E or Esc to close");
	} else {
		ImGui::TextUnformatted("Crafting");
		ImGui::BeginChild("recipes", ImVec2(340, 380), ImGuiChildFlags_None);
		for (const Recipe &r : game.hand_recipes()) {
			ImGui::PushID(r.id.c_str());
			const ImVec2 a = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(32, 32));
			icons.draw(ImGui::GetWindowDrawList(), r.output, a, ImVec2(a.x + 32, a.y + 32));
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::Text("%d x %s", r.count, display_name(*icons.content, r.output).c_str());
			std::string parts;
			for (const auto &[item, n] : r.ingredients) {
				parts += (parts.empty() ? "" : ", ") + std::to_string(n) + " " + display_name(*icons.content, item);
			}
			ImGui::TextDisabled("%s", parts.c_str());
			ImGui::EndGroup();
			ImGui::SameLine(280);
			const bool can = game.can_craft(r);
			ImGui::BeginDisabled(!can);
			if (ImGui::Button("Craft")) {
				game.inventory_action(Json::array({"craft", r.id}));
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::TextDisabled("E or Esc to close");
	}
	ImGui::EndGroup();
	ImGui::PopStyleVar();
	ImGui::End();
	return keep_open;
}

namespace {

// The player's backpack and hotbar rows, for a container screen.
void inventory_rows(Game &game, const UiIcons &icons) {
	Inventory &inv = game.inventory();
	ImGui::TextUnformatted("Inventory");
	for (int i = Inventory::HOTBAR; i < Inventory::SIZE; i++) {
		if ((i - Inventory::HOTBAR) % Inventory::HOTBAR != 0) {
			ImGui::SameLine();
		}
		if (slot_button(("inv" + std::to_string(i)).c_str(), inv.slots[i], icons, false, true)) {
			game.inventory_action(Json::array({"click", i}));
		}
	}
	ImGui::Dummy(ImVec2(0, 4));
	for (int i = 0; i < Inventory::HOTBAR; i++) {
		if (i) {
			ImGui::SameLine();
		}
		if (slot_button(("inv" + std::to_string(i)).c_str(), inv.slots[i], icons, i == game.selected(), true)) {
			game.inventory_action(Json::array({"click", i}));
		}
	}
}

void bar(float fraction, ImVec2 size, ImU32 color) {
	const ImVec2 a = ImGui::GetCursorScreenPos();
	ImGui::Dummy(size);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), IM_COL32(0, 0, 0, 160), 3.0f);
	dl->AddRectFilled(a, ImVec2(a.x + size.x * std::clamp(fraction, 0.0f, 1.0f), a.y + size.y), color, 3.0f);
}

} // namespace

bool container_screen(Game &game, const BlockPos &pos, const UiIcons &icons) {
	BlockEntity *e = game.block_entities().at(pos);
	if (!e) {
		return false; // it went away
	}
	dim_background();
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.92f);
	ImGui::Begin("Container", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
	auto click = [&](int slot) { game.inventory_action(Json::array({"container", pos.x, pos.y, pos.z, slot})); };
	if (e->type == "furnace") {
		ImGui::TextUnformatted("Furnace");
		float burn, progress;
		BlockEntities::furnace_status(*e, burn, progress);
		ImGui::BeginGroup();
		ImGui::TextDisabled("Input");
		if (slot_button("in", e->slots[BlockEntities::FURNACE_INPUT], icons, false, true)) {
			click(BlockEntities::FURNACE_INPUT);
		}
		bar(burn, ImVec2(48, 6), IM_COL32(255, 140, 40, 255)); // the flame: fuel left
		ImGui::TextDisabled("Fuel");
		if (slot_button("fuel", e->slots[BlockEntities::FURNACE_FUEL], icons, false, true)) {
			click(BlockEntities::FURNACE_FUEL);
		}
		ImGui::EndGroup();
		ImGui::SameLine(0, 16);
		ImGui::BeginGroup();
		ImGui::Dummy(ImVec2(0, 34));
		bar(progress, ImVec2(80, 10), IM_COL32(255, 255, 255, 230)); // smelting
		ImGui::EndGroup();
		ImGui::SameLine(0, 16);
		ImGui::BeginGroup();
		ImGui::TextDisabled("Output");
		if (slot_button("out", e->slots[BlockEntities::FURNACE_OUTPUT], icons, false, true)) {
			click(BlockEntities::FURNACE_OUTPUT);
		}
		ImGui::EndGroup();
	} else {
		ImGui::TextUnformatted("Chest");
		for (int i = 0; i < int(e->slots.size()); i++) {
			if (i % 9) {
				ImGui::SameLine();
			}
			if (slot_button(("c" + std::to_string(i)).c_str(), e->slots[i], icons, false, true)) {
				click(i);
			}
		}
	}
	ImGui::Dummy(ImVec2(0, 8));
	inventory_rows(game, icons);
	ImGui::TextDisabled("E or Esc to close");
	ImGui::PopStyleVar();
	ImGui::End();
	return true;
}

void draw_held(const Stack &held, const UiIcons &icons) {
	if (held.empty()) {
		return;
	}
	const ImVec2 m = ImGui::GetIO().MousePos;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImVec2 a(m.x + 4, m.y + 4), b(m.x + 40, m.y + 40);
	icons.draw(dl, held.id, a, b);
	if (held.count > 1) {
		count_text(dl, b, held.count);
	}
}

} // namespace gc
