-- Mining: how long a block takes to break, and what breaking it gives.
-- (No tools: everything is mined by hand.)

local MINE_SECONDS_PER_HARDNESS = 0.375
local MIN_MINE_SECONDS = 0.1

-- Hardness x 0.375 s, whatever is held.
game.set_rule("mine_seconds", function(block, tool)
	local def = registry.get_block(block) or {}
	return math.max(MIN_MINE_SECONDS, (def.hardness or 1.0) * MINE_SECONDS_PER_HARDNESS)
end)

-- Its "drops" ("" = nothing), else the block itself (a variant: its base
-- block). Plus its "extra_drops" ({{item, count, chance}, ...}), rolled now.
game.set_rule("drops", function(block, tool)
	local def = registry.get_block(block) or {}
	local out = {}
	local main = def.drops or def.variant_of or block
	if main ~= "" then
		out[#out + 1] = main
	end
	for _, e in ipairs(def.extra_drops or {}) do
		if game.random() < e[3] then
			for _ = 1, e[2] do
				out[#out + 1] = e[1]
			end
		end
	end
	return out
end)
