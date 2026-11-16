-- Code entry point of the base game. Other mods use the same shape: a main.lua
-- that registers content (and event handlers and rules) through `registry`,
-- `events` and `game`.

-- The content: blocks, items, recipes, what the world generator makes, sounds.
require("content.blocks")
require("content.items")
require("content.recipes")
require("content.world")
require("content.sounds")

registry.register_block("planks", {hardness = 2.0, flammable = true, color = "#b8945f",
	textures = {all = "textures/planks.png"}, sound = "core:wood", tool = "axe", fuel = 7.5})
registry.register_recipe("planks_to_sticks", {
	type = "shaped",
	pattern = {"P", "P"},
	key = {P = "core:planks"},
	output = {item = "core:stick", count = 4},
})

-- The rules of the game.
require("rules.mining")
require("rules.crafting")
require("rules.health")
require("rules.plants")
require("rules.farming")
require("rules.fluids")

-- What a new survival player starts with: a torch, in hand.
game.set_rule("starting_items", function()
	return {{id = "core:torch", count = 1}}
end)
