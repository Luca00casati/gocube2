-- Flowing water and lava.
--
-- Only cells near a recent change are looked at (every TICK seconds), so a
-- still sea costs nothing. A fluid is a block with "fluid" (its name) and
-- "level": 8 = source, 7..1 = flowing, each block further losing one (or
-- "flow_step": lava 2).
-- Rules for a fluid cell:
--   - flowing fluid must be fed: from above (-> level 7) or by a sideways
--     neighbour one step higher; otherwise it shrinks and finally drains away
--   - it falls into air below (as level 7) before spreading sideways
--   - otherwise it spreads sideways into air, one step lower
--   - lava touching water hardens: a source into stone, flowing into
--     cobblestone; lava and water flowing into each other make cobblestone

local TICK = 0.25
local MAX_CELLS_PER_TICK = 1024
-- A TICK's cells are spread over game ticks, this many at a time: a call
-- must stay well within the mods' time limit (20 ms), even in a debug build
-- (a big flood then spreads more slowly; a few blocks of water at once).
local CELLS_PER_CALL = 64
local SIDES = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}
local AROUND = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}
local AIR = "core:air"

local level_of   -- block id -> level (fluids only)
local fluid_of   -- block id -> fluid name
local block_for  -- fluid name -> {[level] = block id}
local step_of    -- fluid name -> levels lost per block
local stone, cobblestone

local function learn_fluids()
	level_of, fluid_of, block_for, step_of = {}, {}, {}, {}
	for id, d in pairs(registry.all("blocks")) do
		if d.fluid then
			local level = math.max(1, math.min(8, math.floor(d.level or 8)))
			level_of[id] = level
			fluid_of[id] = d.fluid
			block_for[d.fluid] = block_for[d.fluid] or {}
			block_for[d.fluid][level] = id
			step_of[d.fluid] = math.max(1, math.floor(d.flow_step or 1))
		end
	end
	stone = registry.get_block("core:stone") and "core:stone" or AIR
	cobblestone = registry.get_block("core:cobblestone") and "core:cobblestone" or AIR
end

-- A cell as one integer (21 bits a coordinate, up to a million blocks out):
-- no string or table to collect for each cell.
local SPAN = 1 << 21
local HALF = SPAN >> 1
local function key(x, y, z) return ((x + HALF) * SPAN + (y + HALF)) * SPAN + (z + HALF) end
local function unkey(k)
	local z = k % SPAN - HALF
	k = k // SPAN
	return k // SPAN - HALF, k % SPAN - HALF, z
end

-- Cells to look at next, in the order they came.
local pending, pending_set = {}, {}
local function add(x, y, z)
	local k = key(x, y, z)
	if not pending_set[k] then
		pending_set[k] = true
		pending[#pending + 1] = k
	end
end

events.on("block_changed", function(x, y, z)
	add(x, y, z)
	for _, a in ipairs(AROUND) do
		add(x + a[1], y + a[2], z + a[3])
	end
end)

local function set_fluid(x, y, z, fluid, level)
	local there = fluid_of[world.get_block(x, y, z)]
	if there and there ~= fluid then
		world.set_block(x, y, z, cobblestone)
		return
	end
	local id = block_for[fluid][level]
	if id then
		world.set_block(x, y, z, id)
	end
end

local function update(x, y, z)
	local block = world.get_block(x, y, z)
	if block == nil or not level_of[block] then
		return -- (not loaded, or not a fluid)
	end
	local fluid, level = fluid_of[block], level_of[block]
	local step = step_of[fluid]
	if fluid == "lava" then
		for _, a in ipairs(AROUND) do
			if a[2] ~= -1 and fluid_of[world.get_block(x + a[1], y + a[2], z + a[3]) or AIR] == "water" then
				world.set_block(x, y, z, level == 8 and stone or cobblestone)
				return
			end
		end
	end
	if level < 8 then
		-- Flowing: is something still feeding it?
		local fed = 0
		if fluid_of[world.get_block(x, y + 1, z) or AIR] == fluid then
			fed = 7
		end
		for _, a in ipairs(SIDES) do
			local n = world.get_block(x + a[1], y, z + a[3])
			if n and fluid_of[n] == fluid then
				fed = math.max(fed, level_of[n] - step)
			end
		end
		if fed <= 0 then
			world.set_block(x, y, z, AIR) -- drains away
			return
		end
		if fed ~= level then
			world.set_block(x, y, z, block_for[fluid][fed] or block)
			return -- (changing looks at this cell and its neighbours again)
		end
	end
	-- Fall first.
	local under = world.get_block(x, y - 1, z)
	if under == nil then
		return -- (not loaded)
	end
	if under == AIR or (fluid_of[under] and fluid_of[under] ~= fluid) then
		set_fluid(x, y - 1, z, fluid, 7)
		return
	end
	if fluid_of[under] == fluid then
		if level_of[under] < 7 then
			set_fluid(x, y - 1, z, fluid, 7)
		end
		if level < 8 then
			return -- falling fluid lands on fluid: no sideways spread
		end
	end
	-- Then sideways, one step lower.
	local next_level = level - step
	if next_level < 1 then
		return
	end
	for _, a in ipairs(SIDES) do
		local px, pz = x + a[1], z + a[3]
		local n = world.get_block(px, y, pz)
		if n and (n == AIR or (fluid_of[n] == fluid and level_of[n] < next_level)
				or (fluid_of[n] and fluid_of[n] ~= fluid)) then
			set_fluid(px, y, pz, fluid, next_level)
		end
	end
end

local batch, head = {}, 1
local timer = 0

events.on("tick", function(dt)
	timer = timer + dt
	if head > #batch then
		if timer < TICK or #pending == 0 then
			return
		end
		timer = 0
		if not level_of then
			learn_fluids()
		end
		-- This tick's cells (at most MAX_CELLS_PER_TICK; the rest next time).
		batch, head = {}, 1
		local rest = {}
		pending_set = {}
		for i, p in ipairs(pending) do
			if i <= MAX_CELLS_PER_TICK then
				batch[#batch + 1] = p
			else
				rest[#rest + 1] = p
				pending_set[p] = true
			end
		end
		pending = rest
	end
	local stop = math.min(#batch, head + CELLS_PER_CALL - 1)
	for i = head, stop do
		update(unkey(batch[i]))
	end
	head = stop + 1
end)

-- How many cells wait to be looked at (for tests).
game.set_rule("fluids_pending", function()
	return #pending + (#batch - head + 1)
end)
