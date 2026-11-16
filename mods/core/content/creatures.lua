-- The base game's creatures: how big they are and how they move. What they do
-- is rules/creatures.lua; how they look, the game's models ("model").

-- A squat, slow grazer (the Godot version's BlockEater).
registry.register_creature("block_eater", {
	model = "block_eater",
	box = {0.9, 0.95, 0.9},
	gravity = 24.0,
	step_height = 1.0, -- hops up 1-block steps
	turn_rate = 8.0,
	light_height = 0.5,
})

-- Comes out at night, runs at the nearest player and strikes (the Godot version's Ninja).
registry.register_creature("ninja", {
	model = "ninja",
	box = {0.6, 1.8, 0.6},
	gravity = 24.0,
	step_height = 1.0,
	turn_rate = 12.0,
	jump = 8.0,
	jump_when_blocked = true, -- leaps over whatever is in the way
	light_height = 1.0,
})
