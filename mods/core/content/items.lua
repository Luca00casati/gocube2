-- The base game's content (it was the Godot version's content/base.json).
-- Items: food, seeds, and what blocks drop. (No tools, weapons or armour:
-- everything is mined by hand, and creatures are fought bare-handed.)

registry.register_item("coal", {texture = "textures/coal.ppm"})

registry.register_item("seeds", {texture = "textures/seeds.ppm", plants = "core:wheat_0", plant_on = {"core:farmland"}})

registry.register_item("wheat", {texture = "textures/wheat.ppm"})

registry.register_item("bread", {texture = "textures/bread.ppm", food = 5})

registry.register_item("snowball", {max_stack = 16, texture = "textures/snowball.ppm"})
