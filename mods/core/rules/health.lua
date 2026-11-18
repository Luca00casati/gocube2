-- Health: how much a fall hurts, falling out of the world, and what food gives.
--

local SAFE_FALL = 3.0 -- blocks you can fall without damage

game.set_rule("fall_damage", function(blocks)
	if blocks <= SAFE_FALL then return 0 end
	return math.floor(blocks - SAFE_FALL)
end)

-- Below the world's bottom (`depth` blocks under it): damage every half second.
game.set_rule("void_damage", function(depth)
	return 4
end)

-- Health an item gives when eaten (0: it isn't food).
game.set_rule("food", function(id)
	local def = registry.get_item(id) or {}
	return def.food or 0
end)
