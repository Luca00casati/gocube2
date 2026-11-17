-- Crafting, smelting, a tool and armour, as a mod would add them (the base
-- game had these before; tests/test_survival.cpp plays with them).

registry.register_item("stick", {color = "#8a6a40"})
registry.register_item("iron_ingot", {color = "#d8d8d8"})
registry.register_item("wood_pickaxe", {color = "#a07a50", tool = "pickaxe", max_stack = 1, damage = 3})
registry.register_item("iron_helmet", {color = "#c8c8c8", armor = {slot = "head", points = 2}, max_stack = 1})
registry.register_block("kiln", {hardness = 3.5, color = "#6a6a6a", block_entity = "furnace"})

registry.register_recipe("log_to_planks", {
	type = "shapeless",
	ingredients = {["core:log"] = 1},
	output = {item = "core:planks", count = 4},
})
registry.register_recipe("planks_to_sticks", {
	type = "shaped",
	pattern = {"P", "P"},
	key = {P = "core:planks"},
	output = {item = "kit:stick", count = 4},
})
registry.register_recipe("wood_pickaxe", {
	type = "shaped",
	pattern = {"PPP", " S ", " S "},
	key = {P = "core:planks", S = "kit:stick"},
	output = {item = "kit:wood_pickaxe"},
})
registry.register_recipe("chest", {
	type = "shaped",
	pattern = {"PPP", "P P", "PPP"},
	key = {P = "core:planks"},
	output = {item = "core:chest"},
})
registry.register_recipe("iron_ingot", {type = "smelting", input = "core:iron_ore", output = {item = "kit:iron_ingot"}})

-- {{id, ingredients = {[item] = count}, output = item, count = n}}, by output.
local function hand_recipes()
	local out = {}
	for id, r in pairs(registry.recipes()) do
		local ingredients = {}
		local ok = true
		if r.type == "shaped" then
			for _, row in ipairs(r.pattern or {}) do
				for ch in row:gmatch(".") do
					if ch ~= " " then
						local item = (r.key or {})[ch] or ""
						if item == "" then ok = false end
						ingredients[item] = (ingredients[item] or 0) + 1
					end
				end
			end
		elseif r.type == "shapeless" then
			for item, count in pairs(r.ingredients or {}) do
				ingredients[item] = count
			end
		else
			ok = false
		end
		local output = r.output or {}
		if ok and next(ingredients) ~= nil and output.item then
			out[#out + 1] = {id = id, ingredients = ingredients, output = output.item, count = output.count or 1}
		end
	end
	table.sort(out, function(a, b)
		if a.output ~= b.output then return a.output < b.output end
		return a.id < b.id
	end)
	return out
end

game.set_rule("hand_recipes", hand_recipes)

game.set_rule("hand_recipes", hand_recipes)

game.set_rule("smelting", function(input)
	for _, r in pairs(registry.recipes()) do
		if r.type == "smelting" and r.input == input and r.output and r.output.item then
			return {item = r.output.item, count = r.output.count or 1}
		end
	end
	return nil
end)

local FUEL = {["core:coal"] = 40, ["kit:stick"] = 2.5}

game.set_rule("fuel_seconds", function(id)
	return FUEL[id] or 0
end)

