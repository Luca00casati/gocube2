-- Farming. (The Godot version's Player._place, for seeds.)
--
-- Seeds (an item with "plants": the crop's first block, and "plant_on": the
-- blocks it grows on) are planted on top of one of those; the plants rule
-- then grows the crop.
--
-- The "use_item" rule: right click with `item` on the block at x, y, z, on its
-- side nx, ny, nz. nil: nothing done here (the game goes on to open a door or
-- place the item); else a table: `take` = how many to take from the hand (in
-- survival), `placed` = {x, y, z} of a block put down, if any.

local function contains(list, value)
	for _, v in ipairs(list or {}) do
		if v == value then return true end
	end
	return false
end

game.set_rule("use_item", function(item, x, y, z, nx, ny, nz)
	local def = registry.get_item(item) or {}
	local target = world.get_block(x, y, z)
	if def.plants then
		if not contains(def.plant_on, target) or ny ~= 1 or world.get_block(x, y + 1, z) ~= "core:air" then
			return nil
		end
		world.set_block(x, y + 1, z, def.plants)
		return {take = 1, placed = {x, y + 1, z}}
	end
	return nil
end)
