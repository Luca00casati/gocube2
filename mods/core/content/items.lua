-- The base game's content (it was the Godot version's content/base.json).
-- Items: what blocks drop that isn't a block (coal, snowballs). (No food,
-- farming, tools, weapons or armour:
-- everything is mined by hand, and creatures are fought bare-handed.)

registry.register_item("coal", {texture = "textures/coal.ppm"})

registry.register_item("snowball", {max_stack = 16, texture = "textures/snowball.ppm"})
