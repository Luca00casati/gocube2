-- Farming. (The Godot version's Player._place, for hoes and seeds.)
--
-- A hoe tills grass or dirt with air above into farmland, now and then
-- turning something up: the block's "till_drops" ({{item, count, chance}, ...};
-- grass: seeds). Seeds (an item with "plants": the crop's first block, and
-- "plant_on": the blocks it grows on) are planted on top of one of those; the
-- plants rule then grows the crop.
--
-- The "use_item" rule: right click with `item` on the block at x, y, z, on its
-- side nx, ny, nz. nil: nothing done here (the game goes on to open a door or
-- place the item); else a table: `take` = how many to take from the hand (in
-- survival), `placed` = {x, y, z} of a block put down, if any.

local TILLABLE = {["core:grass"] = true, ["core:dirt"] = true}

local function contains(list, value)
	for _, v in ipairs(list or {}) do
		if v == value then return true end
	end
	return false
end

game.set_rule("use_item", function(item, x, y, z, nx, ny, nz)
	local def = registry.get_item(item) or {}
	local target = world.get_block(x, y, z)
	if def.tool == "hoe" then
		if not TILLABLE[target] or world.get_block(x, y + 1, z) ~= "core:air" then
			return nil
		end
		local found = {}
		for _, e in ipairs((registry.get_block(target) or {}).till_drops or {}) do
			if game.random() < e[3] then
				for _ = 1, e[2] do
					found[#found + 1] = e[1]
				end
			end
		end
		world.set_block(x, y, z, "core:farmland")
		for _, drop in ipairs(found) do
			world.drop_item(x, y + 1, z, drop, 1) -- (on top of the farmland)
		end
		return {placed = {x, y, z}}
	end
	if def.plants then
		if not contains(def.plant_on, target) or ny ~= 1 or world.get_block(x, y + 1, z) ~= "core:air" then
			return nil
		end
		world.set_block(x, y + 1, z, def.plants)
		return {take = 1, placed = {x, y + 1, z}}
	end
	return nil
end)
