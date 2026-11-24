// Survival, continued: the day, beds, plants, trees, fluids, sounds, the
// void, game modes.
module gocube::test::survival2_test;
import gocube::test::play, gocube::content, gocube::data, gocube::game, gocube::game::inventory, gocube::game::player;
import gocube::game::day_cycle, gocube::world, gocube::util::vec, gocube::clock;
import std::io, std::math;

fn bool near(float a, float b, float eps = 1e-3) => math::abs(a - b) <= eps;

fn void day_sun_daylight_clock() @test
{
	DayCycle d = day_cycle::new();
	d.time = 0.5; // noon
	assert(near(d.sun_height(), 1.0f) && near(d.day_amount(), 1.0f) && near(d.daylight(), 1.0f));
	assert(near(d.sun_dir()[1], 1.0f));
	d.time = 0.0; // midnight
	assert(near(d.day_amount(), 0.0f) && near(d.daylight(), day_cycle::NIGHT_LIGHT));
	d.time = 0.25; // sunrise: in the east (+x)
	assert(near(d.sun_dir()[0], 1.0f) && near(d.dusk(), 1.0f));
	@pool()
	{
		assert(day_cycle::clock_text(0.75, false) == "6:00 PM");
		assert(day_cycle::clock_text(0.0, false) == "12:00 AM");
		assert(day_cycle::clock_text(0.5, true) == "12:00");
	};
	d.time = 0.9;
	d.day_length = 100;
	d.update(20); // at night (its half passes in NIGHT_SHARE of the day): past midnight
	assert(math::abs(d.time - (0.9 + 20.0 * 0.5 / (100.0 * day_cycle::NIGHT_SHARE) - 1.0)) < 1e-6);
	d.time = 0.3;
	d.update(10); // by day, slower
	assert(math::abs(d.time - (0.3 + 10.0 * 0.5 / (100.0 * day_cycle::DAY_SHARE))) < 1e-6);
}

fn void bed_reported_every_time() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.set(1, 40, 0, "core:bed");
	p.set(1, 40, -1, "core:bed_n_head");
	p.player.position = { 1.5f, 41.0f, 2.5f };
	p.player.pitch = -math::atan2(1.62f + 0.45f, 2.0f);
	for (int i = 0; i < 2; i++)
	{
		p.game.update(0.016f, { .captured = true, .use_pressed = true });
		assert(p.game.take_bed_used());
		assert(!p.game.take_bed_used());
	}
}

// Game ticks (20 a second) for this long.
fn void run_ticks(Play* p, float seconds)
{
	for (int i = 0; i < (int)(seconds * 20); i++) p.game.tick(0.05);
}

fn int drops_of_item(Play* p, String item)
{
	int n = 0;
	foreach (d : p.game.drops.drops) n += d.item == item ? d.count : 0;
	return n;
}

fn void sand_falls_unsupported_pop_off() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.set(1, 45, 1, "core:sand"); // 5 above the floor (y 40)
	p.set(0, 41, 1, "core:ladder_w"); // against the west wall (x -1)
	run_ticks(p, 1.0f);
	assert(p.block(1, 45, 1) == "core:air");
	assert(p.block(1, 40, 1) == "core:sand");
	assert(p.block(0, 41, 1) == "core:ladder_w"); // still held by the wall
	p.set(-1, 41, 1, "core:air"); // the wall behind it goes
	run_ticks(p, 0.5f);
	assert(p.block(0, 41, 1) == "core:air");
	assert(drops_of_item(p, "core:ladder") == 1);
}

fn void leaves_decay_without_log() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.set(1, 40, 1, "core:log");
	for (int x = 0; x <= 2; x++) p.set(x, 41, 1, "core:leaves");
	p.set(1, 42, 1, "core:leaves_placed"); // placed by a player: stays
	run_ticks(p, 4.0f);
	assert(p.block(0, 41, 1) == "core:leaves"); // the log holds them
	p.set(1, 40, 1, "core:air");
	run_ticks(p, 9.0f); // (the leaf above the log first, then its neighbours: up to 3 s each)
	for (int x = 0; x <= 2; x++) assert(p.block(x, 41, 1) == "core:air");
	assert(p.block(1, 42, 1) == "core:leaves_placed");
}

fn void tree_grows_where_room() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	for (int y = 43; y <= 46; y++) // (open the pool upwards: trees are tall)
	{
		for (int x = -1; x <= 3; x++)
		{
			for (int z = -1; z <= 3; z++) p.set(x, y, z, "core:air");
		}
	}
	p.set(1, 40, 1, "core:grass");
	assert(p.game.grow_tree({ 1, 41, 1 }, "core:oak"));
	assert(p.block(1, 41, 1) == "core:log");
	assert(p.block(1, 42, 1) == "core:log");
	assert(!p.game.grow_tree({ 1, 41, 1 }, "core:nonexistent"));
}

fn void no_farming_small_plants() @test
{
	Play* p = play::new();
	defer p.free();
	foreach (gone : (String[*]){ "core:farmland", "core:tall_grass", "core:oak_sapling", "core:stone_slab" }) assert(!p.content.blocks.has(gone));
	assert(p.content.trees.has("core:oak"));
	foreach (&e : p.content.blocks.entries) assert(!e.def.has("grows_tree"));
	assert(p.content.blocks.get("core:grass").has("sprouts_trees"));
}

fn void water_spreads_and_drains() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.set(0, 40, 0, "core:water");
	run_ticks(p, 3.0f);
	assert(p.block(0, 40, 0) == "core:water");
	assert(p.block(1, 40, 0) == "core:water_7");
	assert(p.block(2, 40, 0) == "core:water_6");
	assert(p.block(1, 40, 1) == "core:water_6");
	assert(p.block(2, 40, 2) == "core:water_4", "%s", p.block(2, 40, 2));
	assert(p.block(0, 41, 0) == "core:air");
	p.set(0, 40, 0, "core:air"); // the source goes: it all drains
	run_ticks(p, 10.0f);
	for (int x = 0; x <= 2; x++)
	{
		for (int z = 0; z <= 2; z++) assert(p.block(x, 40, z) == "core:air");
	}
	assert(p.game.fluids_pending() == 0);
}

fn void lava_touching_water_turns_to_stone() @test
{
	Play* p = play::new();
	defer p.free();
	p.pool("core:air");
	p.set(0, 40, 0, "core:lava");
	p.set(1, 40, 0, "core:water");
	run_ticks(p, 1.0f);
	assert(p.block(0, 40, 0) == "core:stone");
}

fn void big_flood_doesnt_stall() @test
{
	Play* p = play::new();
	defer p.free();
	// A 40 x 40 sheet of unfed flowing water over a pit: all of it changes at once.
	for (int x = -20; x < 20; x++)
	{
		for (int z = -20; z < 20; z++)
		{
			p.set(x, 45, z, "core:water_3");
			for (int y = 40; y <= 44; y++) p.set(x, y, z, "core:air");
		}
	}
	double worst = 0;
	for (int i = 0; i < 20 * 30; i++)
	{
		double t0 = clock::now();
		p.game.tick(0.05);
		worst = math::max(worst, (clock::now() - t0) * 1000);
	}
	assert(worst < 50.0, "worst tick: %f ms", worst); // (unoptimized: a frame or so, even after a flood of changes)
}

fn int count_sounds(SoundEvent[] sounds, String set, String kind)
{
	int n = 0;
	foreach (e : sounds) n += e.set == set && e.kind == kind ? 1 : 0;
	return n;
}

fn void sounds_of_blocks_steps_pickups_hurts() @test
{
	Play* p = play::new();
	defer p.free();
	Vec{SoundEvent} sounds = p.game.take_sounds();
	sounds.free();
	// Breaking stone by hand: the stone's own break (made from it), at the block.
	p.set(2, 60, 0, "core:stone");
	p.player.position = { 0.5f, 59.0f, 0.5f };
	p.player.flying = true;
	p.game.break_block({ 2, 60, 0 });
	sounds = p.game.take_sounds();
	assert(count_sounds(sounds.array_view(), "block:core:stone", "break") == 1);
	assert(near(sounds[0].at[0], 2.5f));
	sounds.free();
	// Hurt: the effects' hurt.
	p.game.damage(2);
	sounds = p.game.take_sounds();
	assert(count_sounds(sounds.array_view(), "core:effects", "hurt") == 1);
	sounds.free();
	// Walking 10 blocks on grass: a footstep every 1.7 m.
	p.pool("core:air");
	for (int x = -1; x <= 3; x++)
	{
		for (int z = -12; z <= 3; z++)
		{
			p.set(x, 39, z, "core:grass");
			for (int y = 40; y <= 43; y++) p.set(x, y, z, "core:air");
		}
	}
	p.player.flying = false;
	p.player.position = { 1.5f, 40.0f, 1.5f };
	p.player.yaw = 0; // towards -z
	for (int i = 0; i < 30; i++) // settle on the floor
	{
		p.player.step({}, &p.world, &p.rules);
		p.game.after_move(player::STEP);
	}
	sounds = p.game.take_sounds();
	sounds.free();
	float start = p.player.position[2];
	while (start - p.player.position[2] < 10.0f)
	{
		p.player.step({ .forward = 1 }, &p.world, &p.rules);
		p.game.after_move(player::STEP);
	}
	sounds = p.game.take_sounds();
	int steps = count_sounds(sounds.array_view(), "block:core:grass", "step");
	sounds.free();
	assert(steps >= 5 && steps <= 6, "%d steps", steps);
	// Picking up a drop.
	p.game.drop_item("core:dirt", 1, { p.player.position[0], p.player.position[1] + 0.5f, p.player.position[2] });
	for (int i = 0; i < 120; i++) p.game.physics_step(1.0f / 60.0f);
	sounds = p.game.take_sounds();
	assert(count_sounds(sounds.array_view(), "core:effects", "pickup") == 1);
	sounds.free();
}

// Grass all around the origin at y 70, open to the sky; the player on it.
fn void open_field(Play* p)
{
	for (int x = -50; x <= 50; x++)
	{
		for (int z = -50; z <= 50; z++)
		{
			if (!p.world.is_loaded(x, 70, z)) continue;
			p.world.set_block({ x, 70, z }, p.id("core:grass"));
			for (int y = 71; y <= 74; y++) p.world.set_block({ x, y, z }, 0);
		}
	}
	p.player.position = { 0.5f, 71.0f, 0.5f };
	p.player.flying = true;
}

fn void live_for(Play* p, float seconds)
{
	for (int i = 0; i < (int)(seconds * 20); i++)
	{
		for (int k = 0; k < 3; k++) p.game.physics_step(1.0f / 60.0f);
		p.game.tick(0.05);
	}
}

fn int count_field(Play* p, int y, String id)
{
	int n = 0;
	for (int x = -50; x <= 50; x++)
	{
		for (int z = -50; z <= 50; z++) n += p.world.is_loaded(x, y, z) && p.block(x, y, z) == id ? 1 : 0;
	}
	return n;
}

fn void trees_sprout_on_grass() @test
{
	Play* p = play::new();
	defer p.free();
	open_field(p);
	assert(count_field(p, 71, "core:log") == 0);
	live_for(p, 240); // (a tree tries every 20 s)
	assert(count_field(p, 71, "core:log") > 0);
	// Not close to the player (at the origin).
	for (int x = -24; x <= 24; x++)
	{
		for (int z = -24; z <= 24; z++)
		{
			if (math::hypot(x + 0.5f, z + 0.5f) < 22.0f) assert(p.block(x, 71, z) != "core:log");
		}
	}
}

fn void the_void_hurts_not_in_creative() @test
{
	Play* p = play::new();
	defer p.free();
	p.player.position = { 0.5f, (float)world::BOTTOM_Y - 2.0f, 0.5f };
	p.live(0.05f);
	assert(p.game.health == game::MAX_HEALTH - 4); // at once
	p.live(0.5f);
	assert(p.game.health == game::MAX_HEALTH - 8);
	p.live(2.0f);
	assert(p.game.health == 0); // (gone)
	Play* creative = play::new(play::setup(true));
	defer creative.free();
	creative.player.position = { 0.5f, (float)world::BOTTOM_Y - 2.0f, 0.5f };
	creative.live(1.0f);
	assert(creative.game.health == game::MAX_HEALTH);
}

fn void nothing_hurts_at_night() @test
{
	Play* p = play::new();
	defer p.free();
	open_field(p);
	p.game.day = 0.0f;
	live_for(p, 30.0f);
	assert(p.game.health == game::MAX_HEALTH);
}

fn void game_modes_one_inventory() @test
{
	Play* p = play::new(); // (survival)
	defer p.free();
	p.game.inventory.slots[0] = inventory::stack("core:planks", 7);
	p.game.set_creative(true);
	assert(p.game.creative && p.game.inventory.count_of("core:planks") == 7); // (as it was)
	p.game.set_creative(false);
	assert(p.game.inventory.count_of("core:planks") == 7 && p.game.health == game::MAX_HEALTH);
	// A new creative player: nothing in hand; the palette gives one of anything.
	Play* c = play::new(play::setup(true));
	defer c.free();
	foreach (s : c.game.inventory.slots) assert(s.is_empty());
	Data pick = game::action("pick");
	pick.push_string("core:planks");
	c.game.inventory_action(pick);
	assert(c.game.inventory.cursor.id == "core:planks" && c.game.inventory.cursor.count == 1);
	c.game.inventory_action(game::action("click", { 0 }));
	assert(c.game.inventory.slots[0].id == "core:planks");
	// It picks up what lies around.
	c.player.position[1] = 80.0f;
	c.player.flying = true;
	c.game.drop_item("core:cobblestone", 3, { c.player.position[0], c.player.position[1] + 0.5f, c.player.position[2] });
	for (int i = 0; i < 120; i++) c.game.physics_step(1.0f / 60.0f);
	assert(c.game.inventory.count_of("core:cobblestone") == 3);
	// Saved and loaded as it is.
	Play* again = play::new(play::setup(true));
	defer again.free();
	Data saved = c.game.save_player();
	defer saved.free();
	again.game.load_player(&saved);
	assert(again.game.inventory.count_of("core:cobblestone") == 3 && again.game.inventory.count_of("core:planks") == 1);
}
