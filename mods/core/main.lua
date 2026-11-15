-- Code entry point of the base game. Other mods use the same shape: a main.lua
-- that registers content (and event handlers) through `registry` and `events`,
-- plus optional JSON content files.

-- Content that needs logic can be registered from code instead of JSON.
registry.register_block("planks", {hardness = 2.0, flammable = true, color = "#b8945f",
	textures = {all = "textures/planks.png"}, sound = "core:wood", tool = "axe", fuel = 7.5})
registry.register_recipe("planks_to_sticks", {
	type = "shaped",
	pattern = {"P", "P"},
	key = {P = "core:planks"},
	output = {item = "core:stick", count = 4},
})
