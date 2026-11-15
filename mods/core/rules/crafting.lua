-- Crafting: recipes as "ingredients -> output", crafted straight from the
-- inventory (no crafting grid), and smelting in a furnace.
-- (The Godot version's Crafting.)
--   "shaped":    {pattern = {"P", "P"}, key = {P = "core:planks"}, output = {...}}
--                (only the ingredient counts matter here, not the layout)
--   "shapeless": {ingredients = {["core:log"] = 1}, output = {item = "core:planks", count = 4}}
--   "smelting":  {input = "core:iron_ore", output = {item = "core:iron_ingot"}}: furnaces only

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

-- What smelting an item gives: {item = id, count = n}, or nil.
game.set_rule("smelting", function(input)
	for _, r in pairs(registry.recipes()) do
		if r.type == "smelting" and r.input == input and r.output and r.output.item then
			return {item = r.output.item, count = r.output.count or 1}
		end
	end
	return nil
end)

-- Seconds an item burns in a furnace ("fuel" on its item or block definition).
game.set_rule("fuel_seconds", function(id)
	local def = registry.get_item(id) or registry.get_block(id) or {}
	return def.fuel or 0
end)
