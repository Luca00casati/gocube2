-- The base game's content (it was the Godot version's content/base.json).
-- Items: tools (kind, tier, speed, damage), food, armour, seeds.

registry.register_item("stick", {max_stack = 64, texture = "textures/stick.png", fuel = 2.5})

registry.register_item("coal", {texture = "textures/coal.png", fuel = 40})

registry.register_item("iron_ingot", {texture = "textures/iron_ingot.png"})

registry.register_item("gold_ingot", {texture = "textures/gold_ingot.png"})

registry.register_item("wood_pickaxe", {
	texture = "textures/wood_pickaxe.png",
	tool = "pickaxe",
	speed = 2.0,
	max_stack = 1,
	tier = 1,
	damage = 2,
})

registry.register_item("wood_axe", {
	texture = "textures/wood_axe.png",
	tool = "axe",
	speed = 2.0,
	max_stack = 1,
	tier = 1,
	damage = 3,
})

registry.register_item("stone_pickaxe", {
	texture = "textures/stone_pickaxe.png",
	tool = "pickaxe",
	speed = 4.0,
	max_stack = 1,
	tier = 2,
	damage = 3,
})

registry.register_item("stone_axe", {
	texture = "textures/stone_axe.png",
	tool = "axe",
	speed = 4.0,
	max_stack = 1,
	tier = 2,
	damage = 4,
})

registry.register_item("wood_hoe", {
	texture = "textures/wood_hoe.png",
	tool = "hoe",
	speed = 2.0,
	tier = 1,
	max_stack = 1,
	damage = 1,
})

registry.register_item("wood_sword", {texture = "textures/wood_sword.png", tool = "sword", tier = 1, max_stack = 1, damage = 4})

registry.register_item("stone_hoe", {
	texture = "textures/stone_hoe.png",
	tool = "hoe",
	speed = 4.0,
	tier = 2,
	max_stack = 1,
	damage = 2,
})

registry.register_item("stone_sword", {texture = "textures/stone_sword.png", tool = "sword", tier = 2, max_stack = 1, damage = 5})

registry.register_item("iron_pickaxe", {
	texture = "textures/iron_pickaxe.png",
	tool = "pickaxe",
	speed = 6.0,
	tier = 3,
	max_stack = 1,
	damage = 4,
})

registry.register_item("iron_axe", {
	texture = "textures/iron_axe.png",
	tool = "axe",
	speed = 6.0,
	tier = 3,
	max_stack = 1,
	damage = 5,
})

registry.register_item("iron_hoe", {
	texture = "textures/iron_hoe.png",
	tool = "hoe",
	speed = 6.0,
	tier = 3,
	max_stack = 1,
	damage = 3,
})

registry.register_item("iron_sword", {texture = "textures/iron_sword.png", tool = "sword", tier = 3, max_stack = 1, damage = 6})

registry.register_item("iron_helmet", {texture = "textures/iron_helmet.png", armor = {slot = "head", points = 2}, max_stack = 1})

registry.register_item("iron_chestplate", {texture = "textures/iron_chestplate.png", armor = {slot = "chest", points = 6}, max_stack = 1})

registry.register_item("iron_leggings", {texture = "textures/iron_leggings.png", armor = {slot = "legs", points = 5}, max_stack = 1})

registry.register_item("iron_boots", {texture = "textures/iron_boots.png", armor = {slot = "feet", points = 2}, max_stack = 1})

registry.register_item("seeds", {texture = "textures/seeds.png", plants = "core:wheat_0", plant_on = {"core:farmland"}})

registry.register_item("wheat", {texture = "textures/wheat.png"})

registry.register_item("bread", {texture = "textures/bread.png", food = 5})

registry.register_item("snowball", {max_stack = 16, texture = "textures/snowball.png"})
