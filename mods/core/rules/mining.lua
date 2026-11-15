-- Mining: how long a block takes to break, and what breaking it gives.
-- (The Godot version's Player.mine_seconds, drop_of and drops_of.)

local MINE_SECONDS_PER_HARDNESS = 0.75
local MIN_MINE_SECONDS = 0.1

local function item(id) return registry.get_item(id) or {} end
local function tool_kind(id) return item(id).tool or "" end
local function tool_tier(id) return item(id).tier or 0 end -- wood 1, stone 2, iron 3
local function tool_speed(id) return item(id).speed or 1.0 end

-- Whether `tool` is the block's tool, good enough for it.
local function right_tool(def, tool)
	return def.tool ~= nil and tool_kind(tool) == def.tool and tool_tier(tool) >= (def.min_tier or 1)
end

-- Hardness x 0.75 s, divided by the held tool's speed if it is the block's
-- tool; three times as long without it if the block requires one.
game.set_rule("mine_seconds", function(block, tool)
	local def = registry.get_block(block) or {}
	local seconds = math.max(MIN_MINE_SECONDS, (def.hardness or 1.0) * MINE_SECONDS_PER_HARDNESS)
	if def.tool then
		if right_tool(def, tool) then
			return math.max(MIN_MINE_SECONDS, seconds / tool_speed(tool))
		end
		if def.requires_tool then
			return seconds * 3.0
		end
	end
	return seconds
end)

-- Its "drops" ("" = nothing), else the block itself (a variant: its base
-- block); nothing if it requires a tool and the right one isn't held. Plus
-- its "extra_drops" ({{item, count, chance}, ...}), rolled now.
game.set_rule("drops", function(block, tool)
	local def = registry.get_block(block) or {}
	local out = {}
	local main = def.drops or def.variant_of or block
	if def.requires_tool and not right_tool(def, tool) then
		main = ""
	end
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
