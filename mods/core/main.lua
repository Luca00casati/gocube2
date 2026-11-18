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
require("rules.fluids")
require("rules.creatures")

-- The only crafting: planks from logs, and the dye vat from planks.
game.set_rule("hand_recipes", function()
	return {
		{id = "planks", output = "core:planks", count = 4, ingredients = {["core:log"] = 1}},
		{id = "dye_vat", output = "core:dye_vat", count = 1, ingredients = {["core:planks"] = 6}},
	}
end)

-- The dye vat's colours: whatever the gems mix to, the nearest of these
-- natural tones (so no mix is garish).
game.set_rule("dye_palette", function()
	return {
		-- whites, greys, black
		"#ecebe6", "#b9b8b2", "#807f7b", "#4a4a48", "#232325",
		-- reds, pinks
		"#b8392f", "#9c3b2e", "#6e2a2a", "#d98c8c", "#c26a7a", "#e8b8b0",
		-- oranges, yellows, creams
		"#cc7a2e", "#a8552d", "#c9a23a", "#e0c872", "#ece0b8",
		-- browns
		"#b08a62", "#6b4a2f", "#4a3221",
		-- greens
		"#9cba4a", "#7a7f35", "#4e8a3e", "#5f7a3a", "#2f4f33", "#8fa58a", "#3f7f78",
		-- blues
		"#4f9cb0", "#7fa8cf", "#3d62a8", "#5b6f86", "#263a63",
		-- purples
		"#9b8bbf", "#6d4a8f", "#a04a86", "#5e3352",
	}
end)

-- What a new survival player starts with: a torch, in hand.
game.set_rule("starting_items", function()
	return {{id = "core:torch", count = 1}}
end)
