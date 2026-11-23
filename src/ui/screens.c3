// Game screens (ui/gui): the inventory (E) with hand crafting, or in
// creative a palette of every block: click a slot to pick its stack up,
// click another to put it down (same blocks stack, others swap); chests.
module gocube::screens;
import gocube::gui, gocube::game, gocube::game::inventory, gocube::game::rules, gocube::game::block_entities, gocube::content;
import gocube::world, gocube::data, gocube::util::vec;
import std::math;

const Color SLOT_BG @private = { 255, 255, 255, 12 };
const Color SLOT_HOVER @private = { 255, 255, 255, 40 };
const Color SLOT_BORDER @private = { 255, 255, 255, 40 };
const Color SLOT_HIGHLIGHT @private = { 255, 255, 255, 235 };

struct UiIcons
{
	BlockTable* table;
	Content* content;
}

// Draws a block's icon in a rectangle.
fn void UiIcons.draw(&self, DrawList* dl, String id, Area r) => dl.image(r, self.table.icon(id));

fn void count_text(Gui* g, DrawList* dl, Pt corner, int count) @private => @pool()
{
	String n = string::tformat("%d", count);
	Pt size = g.text_size(n);
	dl.text({ corner.x - size.x - 3, corner.y - size.y - 1 }, gui::rgba(0, 0, 0), n, g.theme.font);
	dl.text({ corner.x - size.x - 4, corner.y - size.y - 2 }, gui::rgba(255, 255, 255), n, g.theme.font);
}

// Dims the world behind a screen.
fn void dim_background(Gui* g) @private => g.background.fill({ 0, 0, g.in.width, g.in.height }, gui::rgba(0, 0, 0, 140));

fn WindowOptions centred(Gui* g) @private => { .pos = { g.in.width * 0.5f, g.in.height * 0.5f }, .pivot = { 0.5f, 0.5f }, .alpha = 0.92f };

// A slot: its icon and count, a tooltip with the block's name. True when clicked.
fn bool slot_button(Gui* g, String id, Stack stack, UiIcons* icons, bool highlight, bool show_count, float size = 48) => @pool()
{
	g.push_id(id);
	defer g.pop_id();
	bool clicked = g.invisible_button("slot", { size, size });
	bool hovered = g.item_hovered();
	Area r = g.item_rect();
	DrawList* dl = g.draw_list();
	dl.fill(r, hovered ? SLOT_HOVER : SLOT_BG, 4);
	dl.stroke(r, highlight ? SLOT_HIGHLIGHT : SLOT_BORDER, 4, highlight ? 2.0f : 1.0f);
	if (!stack.is_empty())
	{
		float inset = math::floor(size * 0.14f);
		icons.draw(dl, stack.id, { r.x0 + inset, r.y0 + inset, r.x1 - inset, r.y1 - inset });
		if (show_count && stack.count > 1) count_text(g, dl, { r.x1, r.y1 }, stack.count);
		if (hovered) g.tooltip(icons.content.display_name(tmem, stack.id));
	}
	return clicked;
}

fn String num(int i) @private => string::tformat("%d", i);

// Draws the inventory screen. False when it asks to close.
fn bool inventory_screen(Gui* g, Game* game, UiIcons* icons) => @pool()
{
	dim_background(g);
	Inventory* inv = &game.inventory;
	bool creative = game.creative;
	bool crafting = !creative && game.hand_recipes().len > 0;
	g.begin_window("Inventory", centred(g));
	g.push_spacing({ 4, 4 });
	// Left: the backpack (rows of 9), the hotbar row.
	g.begin_group();
	g.text("Inventory");
	// (Through the game: a client's host does each click to its copy too.)
	for (int i = inventory::HOTBAR; i < inventory::SIZE; i++)
	{
		if ((i - inventory::HOTBAR) % inventory::HOTBAR != 0) g.same_line();
		if (slot_button(g, num(i), inv.slots[i], icons, false, !creative)) game.inventory_action(game::action("click", { i }));
	}
	g.space(6);
	g.text("Hotbar");
	for (int i = 0; i < inventory::HOTBAR; i++)
	{
		if (i) g.same_line();
		if (slot_button(g, num(i), inv.slots[i], icons, i == game.selected, !creative)) game.inventory_action(game::action("click", { i }));
	}
	if (!creative && !crafting) g.text_dim("E or Esc to close");
	g.end_group();
	// Right: crafting (survival) or every block (creative).
	if (creative || crafting)
	{
		g.same_line(-1, 24);
		g.begin_group();
	}
	if (creative)
	{
		g.text("All blocks");
		g.begin_child("palette", { 9 * 44 + 24, 380 }, false);
		Vec{String} ids;
		defer ids.free();
		foreach (id : icons.content.block_ids())
		{
			Data* def = icons.content.blocks.get(id);
			if (def && !def.has("fluid") && id != "core:air" && !def.has("variant_of") && !def.flag("hidden")) ids.push(id);
		}
		foreach (k, id : ids)
		{
			if (k % 9) g.same_line();
			if (slot_button(g, id, inventory::stack(id, 1), icons, false, false, 40))
			{
				Data pick = game::action("pick");
				pick.push_string(id);
				game.inventory_action(pick);
			}
		}
		g.end_child();
		g.text_dim("Click one, then a slot. E or Esc to close");
	}
	else if (crafting)
	{
		g.text("Crafting");
		g.begin_child("recipes", { 340, 380 }, false);
		foreach (&r : game.hand_recipes())
		{
			g.push_id(r.id);
			g.dummy({ 32, 32 });
			icons.draw(g.draw_list(), r.output, g.item_rect());
			g.same_line();
			g.begin_group();
			g.text(string::tformat("%d x %s", r.count, icons.content.display_name(tmem, r.output)));
			DString parts;
			parts.tinit();
			for (int k = 0; k < r.ingredients; k++)
			{
				parts.appendf("%s%d %s", k ? ", " : "", r.ingredient_counts[k], icons.content.display_name(tmem, r.ingredient_ids[k]));
			}
			g.text_dim(parts.str_view());
			g.end_group();
			g.same_line(270);
			g.begin_disabled(!game.can_craft(inv, r));
			if (g.button("Craft"))
			{
				Data craft = game::action("craft");
				craft.push_string(r.id);
				game.inventory_action(craft);
			}
			g.end_disabled();
			g.pop_id();
		}
		g.end_child();
		g.text_dim("E or Esc to close");
	}
	if (creative || crafting) g.end_group();
	g.pop_spacing();
	g.end_window();
	return true;
}

// The player's backpack and hotbar rows, for a container screen.
fn void inventory_rows(Gui* g, Game* game, UiIcons* icons) @private => @pool()
{
	Inventory* inv = &game.inventory;
	g.text("Inventory");
	for (int i = inventory::HOTBAR; i < inventory::SIZE; i++)
	{
		if ((i - inventory::HOTBAR) % inventory::HOTBAR != 0) g.same_line();
		if (slot_button(g, string::tformat("inv%d", i), inv.slots[i], icons, false, true)) game.inventory_action(game::action("click", { i }));
	}
	g.space(4);
	for (int i = 0; i < inventory::HOTBAR; i++)
	{
		if (i) g.same_line();
		if (slot_button(g, string::tformat("inv%d", i), inv.slots[i], icons, i == game.selected, true)) game.inventory_action(game::action("click", { i }));
	}
}

// A chest at `pos` and the player's inventory. False when it asks
// to close (also when the container is gone).
fn bool container_screen(Gui* g, Game* game, BlockPos pos, UiIcons* icons) => @pool()
{
	BlockEntity* e = game.entities.at(pos);
	if (!e) return false; // it went away
	dim_background(g);
	g.begin_window("Container", centred(g));
	g.push_spacing({ 4, 4 });
	g.text("Chest");
	foreach (i, s : e.slots)
	{
		if (i % 9) g.same_line();
		if (slot_button(g, string::tformat("c%d", i), s, icons, false, true)) game.inventory_action(game::action("container", { pos.x, pos.y, pos.z, (long)i }));
	}
	g.space(8);
	inventory_rows(g, game, icons);
	g.text_dim("E or Esc to close");
	g.pop_spacing();
	g.end_window();
	return true;
}

// The stack the mouse holds, drawn at the mouse (call last in the frame).
fn void draw_held(Gui* g, Stack held, UiIcons* icons)
{
	if (held.is_empty()) return;
	Pt m = g.in.mouse;
	icons.draw(&g.foreground, held.id, { m.x + 4, m.y + 4, m.x + 40, m.y + 40 });
	if (held.count > 1) count_text(g, &g.foreground, { m.x + 40, m.y + 40 }, held.count);
}
