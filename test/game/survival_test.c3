// Survival: the inventory, the base game's mining and drop rules, breaking
// with drops that get picked up, placing that uses up blocks, starting items,
// containers, health.
module gocube::test::survival_test;
import gocube::test::play, gocube::content, gocube::data, gocube::game, gocube::game::inventory, gocube::game::rules;
import gocube::game::player, gocube::game::block_entities, gocube::world, gocube::util::vec, gocube::util::rand;
import std::io, std::math;

fn bool same(Stack s, String id, int count) => s.id == id && s.count == count;

fn void inventory_stacks() @test
{
	Content content;
	defer content.free();
	DString why;
	defer why.free();
	assert(content.load(play::CONTENT_DIR, &why));
	Inventory inv = inventory::new(&content);
	assert(inv.add("core:dirt", 100) == 0); // 64 + 36
	assert(same(inv.slots[0], "core:dirt", 64));
	assert(same(inv.slots[1], "core:dirt", 36));
	assert(inv.count_of("core:dirt") == 100);
	assert(!inv.remove("core:dirt", 101));
	assert(inv.remove("core:dirt", 40)); // from the back first
	assert(inv.slots[0].count == 60);
	assert(inv.slots[1].is_empty());
	inv.click(0); // pick up the 60 dirt
	assert(same(inv.cursor, "core:dirt", 60));
	inv.click(5); // put it down elsewhere
	assert(same(inv.slots[5], "core:dirt", 60));
	assert(inv.add("core:stone", 64 * 36) > 0); // full
	assert(!inv.can_fit("core:glass"));
}

fn void inventory_save_format() @test
{
	Content content;
	defer content.free();
	DString why;
	defer why.free();
	assert(content.load(play::CONTENT_DIR, &why));
	Data saved = data::array();
	defer saved.free();
	Data planks = data::array();
	planks.push_string("core:planks");
	planks.push_long(5);
	saved.push(planks);
	saved.push(data::boolean(false));
	Data gone = data::array();
	gone.push_string("gone:item"); // no longer in the game: dropped
	gone.push_long(3);
	saved.push(gone);
	for (int i = 3; i < 36; i++) saved.push(data::boolean(false));
	Inventory inv = inventory::new(&content);
	inv.deserialize(&saved);
	assert(same(inv.slots[0], "core:planks", 5));
	assert(inv.slots[2].is_empty());
	Data again = inv.serialize();
	defer again.free();
	assert(again.len() == 36);
	assert(again.at(0).at(0).as_string() == "core:planks" && again.at(0).at(1).as_long() == 5);
	assert(again.at(2).is_boolean() && !again.at(2).b);
}

fn void rules_mining_and_drops() @test
{
	Play* p = play::new();
	defer p.free();
	assert(math::abs(p.game.mine_seconds_of("core:stone") - 1.5f * 0.375f) < 1e-5); // hardness x 0.375 s
	assert(math::abs(p.game.mine_seconds_of("core:dirt") - 0.5f * 0.375f) < 1e-5);
	assert(math::abs(p.game.mine_seconds_of("core:snow") - 0.1f) < 1e-5); // the least
	String[2][*] cases = {
		{ "core:stone", "core:cobblestone" },
		{ "core:sandstone", "core:sandstone" }, { "core:grass", "core:dirt" }, { "core:log", "core:log" },
		{ "core:wood_door_n_upper", "core:wood_door" }, { "core:leaves", "core:leaves" }, { "core:glass", "" } };
	foreach (c : cases)
	{
		Vec{String} out;
		defer out.free();
		rules::drops(p.content.blocks.get(c[0]), c[0], &out);
		if (c[1].len)
		{
			assert(out.len() == 1 && out[0] == c[1], "%s drops %s", c[0], out.len() ? out[0] : "nothing");
		}
		else
		{
			assert(out.is_empty(), "%s drops something", c[0]);
		}
	}
}

fn void breaking_drops_items_picked_up() @test
{
	Play* p = play::new();
	defer p.free();
	assert(p.game.inventory.slots[0].is_empty()); // (a new player starts with nothing)
	// A dirt block next to the player, broken by hand.
	BlockPos at = { 2, 60, 0 };
	p.set(2, 60, 0, "core:dirt");
	p.player.position = { 0.5f, 59.0f, 0.5f };
	p.player.flying = true;
	p.game.break_block(at);
	assert(p.block(2, 60, 0) == "core:air");
	assert(p.game.drops.drops.len() == 1);
	assert(p.game.drops.drops[0].item == "core:dirt");
	for (int i = 0; i < 120; i++) p.game.physics_step(1.0f / 60.0f); // two seconds: it pops out, then drifts in
	assert(p.game.drops.drops.is_empty());
	assert(p.game.inventory.count_of("core:dirt") == 1);
}

fn void placing_uses_blocks_up() @test
{
	foreach (creative : (bool[2]){ false, true })
	{
		Play* p = play::new(play::setup(creative));
		defer p.free();
		p.game.inventory.slots[0] = inventory::stack("core:planks", 2);
		// Stand on a stone floor and place against it, looking down ahead.
		int h = 40;
		for (int x = -1; x <= 1; x++)
		{
			for (int z = -3; z <= 1; z++)
			{
				p.set(x, h, z, "core:stone");
				for (int y = h + 1; y <= h + 3; y++) p.set(x, y, z, "core:air");
			}
		}
		p.player.position = { 0.5f, (float)(h + 1), 0.5f };
		p.player.yaw = 0;
		p.player.pitch = -math::atan2(1.62f, 2.0f);
		p.game.update(0.016f, { .captured = true, .use_pressed = true });
		assert(p.block(0, h + 1, -2) == "core:planks", "creative %s: %s", creative, p.block(0, h + 1, -2));
		assert(p.game.inventory.slots[0].count == (creative ? 2 : 1));
		// Break it: survival mines over time and drops it, creative breaks at once.
		p.game.update(0.016f, { .captured = true, .break_held = true, .break_pressed = true });
		for (int i = 0; i < 200 && p.block(0, h + 1, -2) != "core:air"; i++) p.game.update(0.016f, { .captured = true, .break_held = true });
		assert(p.block(0, h + 1, -2) == "core:air");
		assert(p.game.drops.drops.len() == (creative ? 0 : 1));
	}
}

fn void inventory_saved_in_world() @test
{
	Play* p = play::new();
	defer p.free();
	p.game.inventory.slots[4] = inventory::stack("core:cobblestone", 7);
	p.game.update(0.016f, { .captured = true, .number_key = 5 });
	Data saved = p.game.save_player();
	defer saved.free();
	assert(saved.get("selected").as_long() == 4);
	assert(saved.get("inventory").at(4).at(0).as_string() == "core:cobblestone");
	Play* q = play::new();
	defer q.free();
	q.game.load_player(&saved);
	assert(q.game.selected == 4);
	assert(same(q.game.inventory.slots[4], "core:cobblestone", 7));
	assert(q.game.inventory.slots[0].is_empty());
}

fn void crafting_planks() @test
{
	Play* p = play::new();
	defer p.free();
	assert(p.game.hand_recipes().len == 1);
	assert(p.game.hand_recipes()[0].output == "core:planks");
	Inventory* inv = &p.game.inventory;
	Recipe* planks = &p.game.hand_recipes()[0];
	assert(!p.game.craft(inv, planks)); // no logs yet
	inv.add("core:log", 2);
	assert(p.game.craft(inv, planks));
	assert(p.game.craft(inv, planks));
	assert(inv.count_of("core:planks") == 8);
	// A plain block holds nothing; a chest holds things.
	BlockPos at = { 3, 60, 3 };
	p.set(3, 60, 3, "core:stone");
	assert(!p.game.entities.at(at));
	p.set(3, 60, 3, "core:chest");
	assert(p.game.entities.at(at));
}

fn void chest_holds_saves_spills() @test
{
	Play* p = play::new();
	defer p.free();
	BlockPos at = { 3, 60, 3 };
	p.set(3, 60, 3, "core:chest");
	BlockEntities* be = &p.game.entities;
	assert(be.at(at));
	assert(be.at(at).type == "chest");
	assert(be.at(at).slots.len() == 27);
	assert(be.click(at, 0, inventory::stack("core:cobblestone", 5)).is_empty()); // put in
	assert(be.click(at, 0, inventory::stack("core:cobblestone", 3)).is_empty()); // stacked
	assert(same(be.at(at).slots[0], "core:cobblestone", 8));
	assert(be.click(at, 4, inventory::stack("core:dirt", 2)).is_empty());
	Data saved = be.serialize();
	defer saved.free();
	assert(saved.get("3,60,3").str("type") == "chest");
	assert(saved.get("3,60,3").get("slots").at(0).at(1).as_long() == 8);
	assert(saved.get("3,60,3").get("slots").at(1).is_boolean());
	BlockEntities copy = { .content = &p.content };
	defer copy.free();
	copy.deserialize(&saved);
	assert(copy.at(at) && same(copy.at(at).slots[4], "core:dirt", 2));
	p.game.break_block(at);
	assert(!be.at(at));
	int cobble;
	int dirt;
	int chests;
	foreach (d : p.game.drops.drops)
	{
		cobble += d.item == "core:cobblestone" ? d.count : 0;
		dirt += d.item == "core:dirt" ? d.count : 0;
		chests += d.item == "core:chest" ? d.count : 0;
	}
	assert(cobble == 8 && dirt == 2 && chests == 1, "%d %d %d", cobble, dirt, chests);
}

fn void container_opens_sneak_places() @test
{
	Play* p = play::new();
	defer p.free();
	int h = 40;
	for (int z = -3; z <= 1; z++)
	{
		p.set(0, h, z, "core:stone");
		for (int y = h + 1; y <= h + 3; y++) p.set(0, y, z, "core:air");
	}
	p.set(0, h + 1, -2, "core:chest");
	p.player.position = { 0.5f, (float)(h + 1), 0.5f };
	p.player.yaw = 0;
	p.player.pitch = -0.42f; // at the chest's front face (just under its top edge)
	p.game.update(0.016f, { .captured = true, .use_pressed = true });
	BlockPos opened;
	assert(p.game.take_opened(&opened));
	assert(opened == (BlockPos){ 0, h + 1, -2 });
	p.game.inventory.slots[0] = inventory::stack("core:dirt", 1);
	p.game.update(0.016f, { .captured = true, .use_pressed = true, .sneak = true });
	assert(!p.game.take_opened(&opened));
	assert(p.block(0, h + 1, -1) == "core:dirt"); // placed against it
}

fn void falls_hurt_healing_comes_back() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.player.position = { 1.5f, 40.0f + 10.5f, 1.5f }; // 10.5 blocks above the floor (y 40): floor(7.5) = 7
	p.live(0.05f);
	p.live(3);
	assert(p.player.on_floor);
	assert(p.game.health == game::MAX_HEALTH - 7, "%d", p.game.health);
	p.live(4.0f + 2.5f * 3 + 0.1f); // 4 s, then a point every 2.5 s
	assert(p.game.health == game::MAX_HEALTH - 4);
}

fn void drowning_and_lava() @test
{
	Play* water = play::new();
	defer water.free();
	water.pool("core:water");
	water.player.position = { 1.5f, 40.0f, 1.5f };
	water.live(9);
	assert(water.game.health == game::MAX_HEALTH);
	assert(water.game.breath < 2);
	water.live(4); // 3 s of no breath: 2 a second
	assert(water.game.health < game::MAX_HEALTH - 2);

	Play* lava = play::new();
	defer lava.free();
	lava.pool("core:lava");
	lava.player.position = { 1.5f, 40.0f, 1.5f };
	lava.live(1.1f);
	assert(lava.game.health < game::MAX_HEALTH - 5);
}

fn void dying_and_respawning() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.game.inventory.slots[3] = inventory::stack("core:cobblestone", 9);
	p.game.world_spawn = { 1.5f, 40.0f, 1.5f };
	p.game.has_world_spawn = true;
	// A bed on the pool's floor, set as the respawn point by right-clicking it.
	p.set(1, 40, 0, "core:bed"); // foot (facing north: head at z - 1)
	p.set(1, 40, -1, "core:bed_n_head");
	p.player.position = { 1.5f, 41.0f, 2.5f };
	p.player.yaw = 0;
	p.player.pitch = -math::atan2(1.62f + 0.45f, 2.0f); // at the bed's foot
	p.game.update(0.016f, { .captured = true, .use_pressed = true });
	assert(p.game.notice() == "Respawn point set");
	p.game.damage(100);
	assert(p.game.dead && p.game.frozen());
	p.game.respawn();
	assert(!p.game.dead);
	assert(p.game.health == game::MAX_HEALTH);
	assert(math::abs(p.player.position[2] + 0.5f) < 1e-4); // on the bed's head
	assert(same(p.game.inventory.slots[3], "core:cobblestone", 9));
	// The bed gone: back to the world spawn.
	p.set(1, 40, -1, "core:air");
	p.game.damage(100);
	p.game.respawn();
	assert(math::abs(p.player.position[0] - 1.5f) < 1e-4 && math::abs(p.player.position[2] - 1.5f) < 1e-4);
	assert(p.game.notice().contains("bed was missing"));
}

fn void new_player_on_dry_ground() @test
{
	Play* p = play::new();
	defer p.free();
	p.player.position = { 0.5f, 100.0f, 0.5f };
	p.game.spawn_on_ground();
	assert(p.game.frozen());
	p.game.tick(0.016);
	assert(!p.game.frozen());
	assert(p.game.has_world_spawn);
	int x = (int)math::floor(p.player.position[0]);
	int y = (int)math::floor(p.player.position[1]);
	int z = (int)math::floor(p.player.position[2]);
	assert(p.block(x, y - 1, z) == "core:grass", "%s", p.block(x, y - 1, z)); // (on grass)
	assert(p.block(x, y, z) != "core:water");
}
