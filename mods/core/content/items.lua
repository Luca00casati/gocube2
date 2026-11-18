-- The base game's content.
-- Items: what blocks drop that isn't a block (coal, snowballs). (No food,
-- farming, tools, weapons or armour:
-- everything is mined by hand, and creatures are fought bare-handed.)

registry.register_item("coal", {texture = "textures/coal.png"})

registry.register_item("snowball", {max_stack = 16, texture = "textures/snowball.png"})

-- Gems, from the gem ores: thrown into the dye vat, they make its dye
-- ("dye": the colour each adds; see the dye vat in the README).
registry.register_item("white_gem", {texture = "textures/white_gem.png", dye = "#ffffff"})
registry.register_item("red_gem", {texture = "textures/red_gem.png", dye = "#ff0000"})
registry.register_item("green_gem", {texture = "textures/green_gem.png", dye = "#00ff00"})
registry.register_item("blue_gem", {texture = "textures/blue_gem.png", dye = "#0000ff"})
registry.register_item("black_gem", {texture = "textures/black_gem.png", dye = "#000000"})
