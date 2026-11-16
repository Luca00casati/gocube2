-- The base game's content (it was the Godot version's content/base.json).
-- Crafting and smelting recipes.

registry.register_recipe("stone_from_cobblestone", {type = "smelting", input = "core:cobblestone", output = {item = "core:stone", count = 1}})

registry.register_recipe("log_to_planks", {
	type = "shapeless",
	ingredients = {["core:log"] = 1},
	output = {item = "core:planks", count = 4},
})

registry.register_recipe("lamp", {
	type = "shapeless",
	ingredients = {["core:crystal_ore"] = 1, ["core:planks"] = 4},
	output = {item = "core:lamp", count = 1},
})

registry.register_recipe("wood_pickaxe", {
	type = "shapeless",
	ingredients = {["core:planks"] = 3, ["core:stick"] = 2},
	output = {item = "core:wood_pickaxe", count = 1},
})

registry.register_recipe("wood_axe", {
	type = "shapeless",
	ingredients = {["core:planks"] = 3, ["core:stick"] = 2},
	output = {item = "core:wood_axe", count = 1},
})

registry.register_recipe("stone_pickaxe", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 3, ["core:stick"] = 2},
	output = {item = "core:stone_pickaxe", count = 1},
})

registry.register_recipe("stone_axe", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 3, ["core:stick"] = 2},
	output = {item = "core:stone_axe", count = 1},
})

registry.register_recipe("furnace", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 8},
	output = {item = "core:furnace", count = 1},
})

registry.register_recipe("chest", {
	type = "shapeless",
	ingredients = {["core:planks"] = 8},
	output = {item = "core:chest", count = 1},
})

registry.register_recipe("iron_ingot", {type = "smelting", input = "core:iron_ore", output = {item = "core:iron_ingot", count = 1}})

registry.register_recipe("gold_ingot", {type = "smelting", input = "core:gold_ore", output = {item = "core:gold_ingot", count = 1}})

registry.register_recipe("stone_slab", {
	type = "shapeless",
	ingredients = {["core:stone"] = 3},
	output = {item = "core:stone_slab", count = 6},
})

registry.register_recipe("stone_stairs", {
	type = "shapeless",
	ingredients = {["core:stone"] = 6},
	output = {item = "core:stone_stairs", count = 4},
})

registry.register_recipe("cobblestone_slab", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 3},
	output = {item = "core:cobblestone_slab", count = 6},
})

registry.register_recipe("cobblestone_stairs", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 6},
	output = {item = "core:cobblestone_stairs", count = 4},
})

registry.register_recipe("planks_slab", {
	type = "shapeless",
	ingredients = {["core:planks"] = 3},
	output = {item = "core:planks_slab", count = 6},
})

registry.register_recipe("planks_stairs", {
	type = "shapeless",
	ingredients = {["core:planks"] = 6},
	output = {item = "core:planks_stairs", count = 4},
})

registry.register_recipe("torch", {
	type = "shapeless",
	ingredients = {["core:coal"] = 1, ["core:stick"] = 1},
	output = {item = "core:torch", count = 4},
})

registry.register_recipe("ladder", {
	type = "shapeless",
	ingredients = {["core:stick"] = 7},
	output = {item = "core:ladder", count = 3},
})

registry.register_recipe("wood_door", {
	type = "shapeless",
	ingredients = {["core:planks"] = 6},
	output = {item = "core:wood_door", count = 3},
})

registry.register_recipe("planks_fence", {
	type = "shapeless",
	ingredients = {["core:planks"] = 4, ["core:stick"] = 2},
	output = {item = "core:planks_fence", count = 3},
})

registry.register_recipe("glass", {type = "smelting", input = "core:sand", output = {item = "core:glass", count = 1}})

registry.register_recipe("wood_hoe", {
	type = "shapeless",
	ingredients = {["core:planks"] = 2, ["core:stick"] = 2},
	output = {item = "core:wood_hoe", count = 1},
})

registry.register_recipe("wood_sword", {
	type = "shapeless",
	ingredients = {["core:planks"] = 2, ["core:stick"] = 1},
	output = {item = "core:wood_sword", count = 1},
})

registry.register_recipe("stone_hoe", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 2, ["core:stick"] = 2},
	output = {item = "core:stone_hoe", count = 1},
})

registry.register_recipe("stone_sword", {
	type = "shapeless",
	ingredients = {["core:cobblestone"] = 2, ["core:stick"] = 1},
	output = {item = "core:stone_sword", count = 1},
})

registry.register_recipe("iron_pickaxe", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 3, ["core:stick"] = 2},
	output = {item = "core:iron_pickaxe", count = 1},
})

registry.register_recipe("iron_axe", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 3, ["core:stick"] = 2},
	output = {item = "core:iron_axe", count = 1},
})

registry.register_recipe("iron_hoe", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 2, ["core:stick"] = 2},
	output = {item = "core:iron_hoe", count = 1},
})

registry.register_recipe("iron_sword", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 2, ["core:stick"] = 1},
	output = {item = "core:iron_sword", count = 1},
})

registry.register_recipe("iron_helmet", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 5},
	output = {item = "core:iron_helmet", count = 1},
})

registry.register_recipe("iron_chestplate", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 8},
	output = {item = "core:iron_chestplate", count = 1},
})

registry.register_recipe("iron_leggings", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 7},
	output = {item = "core:iron_leggings", count = 1},
})

registry.register_recipe("iron_boots", {
	type = "shapeless",
	ingredients = {["core:iron_ingot"] = 4},
	output = {item = "core:iron_boots", count = 1},
})

registry.register_recipe("bread", {
	type = "shapeless",
	ingredients = {["core:wheat"] = 3},
	output = {item = "core:bread", count = 1},
})

registry.register_recipe("birch_to_planks", {
	type = "shapeless",
	ingredients = {["core:birch_log"] = 1},
	output = {item = "core:planks", count = 4},
})

registry.register_recipe("spruce_to_planks", {
	type = "shapeless",
	ingredients = {["core:spruce_log"] = 1},
	output = {item = "core:planks", count = 4},
})

registry.register_recipe("sandstone", {
	type = "shapeless",
	ingredients = {["core:sand"] = 4},
	output = {item = "core:sandstone", count = 1},
})

registry.register_recipe("bed", {
	type = "shapeless",
	ingredients = {["core:planks"] = 3, ["core:wheat"] = 3},
	output = {item = "core:bed", count = 1},
})

registry.register_recipe("snow", {
	type = "shapeless",
	ingredients = {["core:snowball"] = 2},
	output = {item = "core:snow", count = 1},
})
