-- What the world generator makes: ore veins, trees, and the biome.
-- A biome's "trees" is a list of {id, percent} pairs: its order reaches the
-- generator, and Lua tables keyed by id have none.

registry.register_ore("coal", {block = "core:coal_ore", min_y = -90, max_y = 40, veins_per_chunk = 18, size = {6, 14}})

registry.register_ore("iron", {block = "core:iron_ore", min_y = -96, max_y = 8, veins_per_chunk = 12, size = {4, 9}})

registry.register_ore("gold", {block = "core:gold_ore", min_y = -96, max_y = -28, veins_per_chunk = 4, size = {3, 7}})

registry.register_ore("crystal", {block = "core:crystal_ore", min_y = -96, max_y = -56, veins_per_chunk = 2, size = {2, 5}})

registry.register_tree("oak", {log = "core:log", leaves = "core:leaves", trunk = {4, 6}, style = "oak"})

registry.register_tree("big_oak", {log = "core:log", leaves = "core:leaves", trunk = {6, 9}, style = "big_oak"})

registry.register_tree("cactus", {log = "core:cactus", leaves = "", trunk = {1, 3}, style = "cactus"})

registry.register_biome("vanilla", {
	temperature = 0.0,
	humidity = -0.1,
	height = 14,
	base = 2,
	surface = "core:grass",
	filler = "core:dirt",
	trees = {{"core:oak", 7}, {"core:big_oak", 1}},
	mountains = 34,
	rock = "core:stone",
	rock_above = 46,
	cover = "core:snow",
	cover_above = 56,
})
