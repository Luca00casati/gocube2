-- Code entry point of the base game. Other mods use the same shape: a main.lua
-- that registers content (and event handlers and rules) through `registry`,
-- `events` and `game`.

-- The content: blocks, items, what the world generator makes, sounds, creatures.
local looks = require("content.looks")

require("content.blocks")
require("content.items")
require("content.world")
require("content.sounds")
require("content.creatures")

registry.register_block("planks", {hardness = 2.0, flammable = true, color = "#b8945f",
	textures = {all = looks.planks}, sound = "core:wood"})

-- The rules of the game.
require("rules.mining")
require("rules.health")
require("rules.plants")
require("rules.farming")
require("rules.fluids")
require("rules.creatures")

-- What a new survival player starts with: a torch, in hand.
game.set_rule("starting_items", function()
	return {{id = "core:torch", count = 1}}
end)
