-- The base game's content.
-- Blocks: their look, hardness, what they drop,
-- light, and how plants and fluids behave (the rules in rules/ read these).

local looks = require("content.looks")

registry.register_block("air", {solid = false, transparent = true})

registry.register_block("stone", {
	hardness = 1.5,
	drops = "core:cobblestone",
	color = "#7f7f7f",
	textures = {all = looks.stone},
	sound = "core:stone",
	random_rotation = true,
})

registry.register_block("cobblestone", {
	hardness = 2.0,
	color = "#6a6a6a",
	textures = {all = looks.cobblestone},
	sound = "core:stone",
	random_rotation = true,
})

registry.register_block("dirt", {
	hardness = 0.5,
	color = "#8b5a2b",
	textures = {all = looks.dirt},
	sound = "core:grass",
	random_rotation = true,
})

registry.register_block("grass", {
	hardness = 0.6,
	tint = true,
	drops = "core:dirt",
	color = "#5fa83a",
	textures = {
		top = looks.grass_top,
		side = looks.grass_side,
		bottom = looks.dirt,
	},
	sound = "core:grass",
	random_rotation = true,
	-- Trees come up on it by themselves (rules/plants.lua): which, by weight.
	sprouts_trees = {["core:oak"] = 9, ["core:big_oak"] = 1},
})

registry.register_block("sand", {
	hardness = 0.5,
	color = "#d6c38a",
	textures = {all = looks.sand},
	sound = "core:sand",
	random_rotation = true,
	falls = true,
})

registry.register_block("gravel", {
	hardness = 0.6,
	color = "#7a6f66",
	textures = {all = looks.gravel},
	sound = "core:gravel",
	random_rotation = true,
	falls = true,
})

registry.register_block("log", {
	hardness = 2.0,
	flammable = true,
	color = "#59422c",
	textures = {
		top = looks.log_top,
		bottom = looks.log_top,
		side = looks.log_side,
	},
	sound = "core:wood",
})

registry.register_block("leaves", {
	tint = true,
	hardness = 0.2,
	flammable = true,
	color = "#356828",
	textures = {all = looks.leaves},
	sound = "core:grass",
	leaves = true,
})

registry.register_block("coal_ore", {
	hardness = 3.0,
	color = "#3a3a40",
	textures = {all = looks.coal_ore},
	sound = "core:stone",
	drops = "core:coal",
	random_rotation = true,
})

registry.register_block("iron_ore", {
	hardness = 3.0,
	color = "#9a8070",
	textures = {all = looks.iron_ore},
	sound = "core:stone",
	random_rotation = true,
})

registry.register_block("gold_ore", {
	hardness = 3.0,
	color = "#b8a050",
	textures = {all = looks.gold_ore},
	sound = "core:stone",
	random_rotation = true,
})

registry.register_block("crystal_ore", {
	hardness = 3.0,
	color = "#5aa8b0",
	textures = {all = looks.crystal_ore},
	sound = "core:stone",
	random_rotation = true,
})

registry.register_block("water", {solid = false, fluid = "water", color = "#3f76d6b3", level = 8})

registry.register_block("lamp", {
	hardness = 0.3,
	light = 14,
	color = "#f2c25a",
	textures = {all = "textures/lamp.ppm"},
	sound = "core:stone",
})

registry.register_block("water_1", {solid = false, fluid = "water", level = 1, flowing = true, color = "#3f76d6b3"})

registry.register_block("water_2", {solid = false, fluid = "water", level = 2, flowing = true, color = "#3f76d6b3"})

registry.register_block("water_3", {solid = false, fluid = "water", level = 3, flowing = true, color = "#3f76d6b3"})

registry.register_block("water_4", {solid = false, fluid = "water", level = 4, flowing = true, color = "#3f76d6b3"})

registry.register_block("water_5", {solid = false, fluid = "water", level = 5, flowing = true, color = "#3f76d6b3"})

registry.register_block("water_6", {solid = false, fluid = "water", level = 6, flowing = true, color = "#3f76d6b3"})

registry.register_block("water_7", {solid = false, fluid = "water", level = 7, flowing = true, color = "#3f76d6b3"})

registry.register_block("furnace", {
	hardness = 3.5,
	sound = "core:stone",
	color = "#6a6a6a",
	textures = {
		side = "textures/furnace_side.ppm",
		top = looks.stone,
		bottom = looks.stone,
	},
})

registry.register_block("chest", {
	hardness = 2.5,
	sound = "core:wood",
	color = "#9a7448",
	block_entity = "chest",
	textures = {
		side = "textures/chest_side.ppm",
		top = "textures/chest_top.ppm",
		bottom = looks.planks,
	},
})

registry.register_block("torch", {
	shape = "torch",
	light = 13,
	hardness = 0.1,
	sound = "core:wood",
	color = "#e0a040",
	textures = {all = "textures/torch.ppm"},
	needs_support = true,
})

registry.register_block("ladder", {
	shape = "ladder",
	climbable = true,
	hardness = 0.6,
	sound = "core:wood",
	color = "#93683d",
	textures = {all = "textures/ladder.ppm"},
	needs_support = true,
})

registry.register_block("glass", {
	transparent = true,
	hardness = 0.4,
	sound = "core:stone",
	color = "#cfe3ec",
	drops = "",
	textures = {all = "textures/glass.ppm"},
})

registry.register_block("wood_door", {
	shape = "door",
	hardness = 1.5,
	sound = "core:wood",
	color = "#93683d",
	textures = {all = "textures/door_lower.ppm"},
	textures_upper = {all = "textures/door_upper.ppm"},
	icon = "textures/door_upper.ppm",
	needs_support = true,
})

registry.register_block("planks_fence", {
	shape = "fence",
	hardness = 1.5,
	sound = "core:wood",
	color = "#a67748",
	textures = {all = looks.planks},
})

registry.register_block("stone_stairs", {
	textures = {all = looks.stone},
	color = "#7a7d83",
	hardness = 1.5,
	sound = "core:stone",
	shape = "stairs",
})

registry.register_block("cobblestone_stairs", {
	textures = {all = looks.cobblestone},
	color = "#6a6a6a",
	hardness = 1.5,
	sound = "core:stone",
	shape = "stairs",
})

registry.register_block("planks_stairs", {
	textures = {all = looks.planks},
	color = "#a67748",
	hardness = 1.0,
	sound = "core:wood",
	shape = "stairs",
})

registry.register_block("sandstone", {
	hardness = 1.0,
	sound = "core:stone",
	color = "#cdb57a",
	textures = {all = looks.sandstone},
	random_rotation = true,
})

registry.register_block("ice", {
	hardness = 0.5,
	sound = "core:stone",
	color = "#a9caeb",
	drops = "",
	textures = {all = looks.ice},
	random_rotation = true,
})

registry.register_block("cactus", {
	shape = "boxes",
	boxes = {{1, 0, 1, 15, 16, 15}},
	hardness = 0.4,
	sound = "core:grass",
	color = "#43832e",
	textures = {
		side = looks.cactus_side,
		top = looks.cactus_top,
		bottom = looks.cactus_top,
	},
	plant_on = {"core:sand", "core:cactus"},
})

registry.register_block("lava", {
	solid = false,
	fluid = "lava",
	level = 8,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_1", {
	solid = false,
	fluid = "lava",
	level = 1,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_2", {
	solid = false,
	fluid = "lava",
	level = 2,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_3", {
	solid = false,
	fluid = "lava",
	level = 3,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_4", {
	solid = false,
	fluid = "lava",
	level = 4,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_5", {
	solid = false,
	fluid = "lava",
	level = 5,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_6", {
	solid = false,
	fluid = "lava",
	level = 6,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("lava_7", {
	solid = false,
	fluid = "lava",
	level = 7,
	flowing = true,
	light = 15,
	color = "#e8641e",
	flow_step = 2,
	damage = 4,
	textures = {all = looks.lava},
})

registry.register_block("bed", {
	shape = "bed",
	hardness = 0.3,
	sound = "core:wood",
	color = "#b43a36",
	textures = {
		top = "textures/bed_foot_top.ppm",
		side = "textures/bed_side.ppm",
		bottom = looks.planks,
	},
	-- The head half: a pillow, the blanket running on into the foot's.
	textures_head = {
		top = "textures/bed_head_top.ppm",
		side = "textures/bed_side.ppm",
		bottom = looks.planks,
	},
	bed = true,
	flammable = true,
})

registry.register_block("snow", {
	shape = "boxes",
	boxes = {{0, 0, 0, 16, 1, 16}},
	hardness = 0.1,
	sound = "core:grass",
	color = "#f2f7fb",
	textures = {all = looks.snow},
	replaceable = true,
	needs_support = true,
	drops = "core:snowball",
})
