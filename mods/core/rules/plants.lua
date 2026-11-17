-- Growing crops and blocks that need support. (The Godot version's PlantSim.)
--
-- Falling: blocks with "falls" (sand, gravel) drop while there's air or a
-- fluid under them.
-- Support: plants ("plant_on") need one of those blocks below; blocks with
-- "needs_support" need a solid block below or behind them (torches, ladders,
-- doors); unsupported blocks pop off and drop their item.
-- Leaf decay: leaves more than LEAF_REACH steps (through leaves) from a log
-- fall after a short random delay, now and then dropping a sapling; placed
-- leaves are a "_placed" variant that never decays.
-- Growth: a block with "grows_into" turns into that block after about
-- "grow_seconds"; a sapling ("grows_tree": a tree id, or {[tree id] = weight})
-- grows into that tree, trying again later when there's no room. What's
-- growing is saved with the world (world.json's "plants").

local TICK = 1.0
local LEAF_REACH = 4
local SAPLING_CHANCE = 0.05
-- At most this many cells (and leaves) are looked at a game tick, the rest
-- later: a call must stay well within the mods' time limit (20 ms) after many
-- blocks change at once.
local MAX_CHECKS = 2048
local MAX_LEAF_CHECKS = 128
local AROUND = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}

local growing = {}   -- cell (key) -> seconds left
local check = {}     -- cells whose support (or falling) to check
local leaf_check = {} -- leaves to test for a nearby log
local decaying = {}  -- leaves -> seconds until they fall
local timer = 0

-- A cell as one integer (21 bits a coordinate, up to a million blocks out):
-- no string or table to collect for each cell looked at.
local SPAN = 1 << 21
local HALF = SPAN >> 1
local function key(x, y, z) return ((x + HALF) * SPAN + (y + HALF)) * SPAN + (z + HALF) end
local function unkey(k)
	local z = k % SPAN - HALF
	k = k // SPAN
	return k // SPAN - HALF, k % SPAN - HALF, z
end

-- Block definitions, looked up once each (they don't change while playing).
local defs = {}
local function def(id)
	if id == nil then return {} end
	local d = defs[id]
	if d == nil then
		d = registry.get_block(id) or false
		defs[id] = d
	end
	return d or {}
end

-- Every tree's log.
local logs
local function is_log(id)
	if not logs then
		logs = {}
		for _, t in pairs(registry.all("trees")) do
			if t.log then logs[t.log] = true end
		end
	end
	return logs[id] == true
end

-- Up to n keys taken out of a set.
local function take(set, n)
	local out = {}
	for k in pairs(set) do
		if #out >= n then break end
		out[#out + 1] = k
		set[k] = nil -- (clearing a field while going through is allowed)
	end
	return out
end

local function is_leaves(id) return id ~= nil and def(id).leaves == true end
local function is_fluid(id) return id ~= nil and def(id).fluid ~= nil end

events.on("block_changed", function(x, y, z, id)
	local d = def(id)
	local k = key(x, y, z)
	if d.grows_into or d.grows_tree then
		growing[k] = (d.grow_seconds or 60.0) * (0.6 + 0.8 * game.random())
	else
		growing[k] = nil
	end
	check[k] = true
	for _, a in ipairs(AROUND) do
		local nx, ny, nz = x + a[1], y + a[2], z + a[3]
		check[key(nx, ny, nz)] = true
		if is_leaves(world.get_block(nx, ny, nz)) then
			leaf_check[key(nx, ny, nz)] = true
		end
	end
end)

-- Whether a log is within LEAF_REACH steps, going through leaves.
local function near_log(x, y, z)
	local seen = {[key(x, y, z)] = 0}
	local queue = {{x, y, z}}
	local head = 1
	while queue[head] do
		local p = queue[head]
		head = head + 1
		local dist = seen[key(p[1], p[2], p[3])]
		for _, a in ipairs(AROUND) do
			local nx, ny, nz = p[1] + a[1], p[2] + a[2], p[3] + a[3]
			local k = key(nx, ny, nz)
			if seen[k] == nil then
				local b = world.get_block(nx, ny, nz)
				if is_log(b) then return true end
				if dist + 1 < LEAF_REACH and is_leaves(b) then
					seen[k] = dist + 1
					queue[#queue + 1] = {nx, ny, nz}
				end
			end
		end
	end
	return false
end

-- A tree id from "grows_tree": the id itself, or picked by weight.
local function pick_tree(grows)
	if type(grows) ~= "table" then return grows end
	local ids, total = {}, 0
	for id, w in pairs(grows) do
		ids[#ids + 1] = id
		total = total + w
	end
	table.sort(ids)
	local roll = game.random() * total
	for _, id in ipairs(ids) do
		roll = roll - grows[id]
		if roll <= 0 then return id end
	end
	return ids[#ids]
end

events.on("tick", function(dt)
	-- Falling and support.
	for _, k in ipairs(take(check, MAX_CHECKS)) do
		local x, y, z = unkey(k)
		local id = world.get_block(x, y, z)
		local d = def(id)
		if d.falls then
			local below = world.get_block(x, y - 1, z)
			if below == "core:air" or is_fluid(below) then
				world.set_block(x, y, z, "core:air")
				world.set_block(x, y - 1, z, id) -- (checked again next tick)
			end
		elseif id ~= nil and id ~= "core:air" and not world.supported(x, y, z, id) then
			local drop = d.drops or d.variant_of or id
			world.set_block(x, y, z, "core:air") -- (its neighbours get checked in turn)
			local upper_door = d.shape == "door" and d.state and d.state.half == "upper"
			if drop ~= "" and not upper_door then
				world.drop_item(x, y, z, drop, 1)
			end
		end
	end
	-- Leaves far from any log start to decay.
	for _, k in ipairs(take(leaf_check, MAX_LEAF_CHECKS)) do
		local x, y, z = unkey(k)
		if is_leaves(world.get_block(x, y, z)) and not decaying[k] and not near_log(x, y, z) then
			decaying[k] = 0.3 + 2.7 * game.random()
		end
	end
	for k, left in pairs(decaying) do
		left = left - dt
		if left > 0 then
			decaying[k] = left
		else
			decaying[k] = nil
			local x, y, z = unkey(k)
			local leaf = world.get_block(x, y, z)
			if is_leaves(leaf) then
				local sapling = def(leaf).sapling or ""
				world.set_block(x, y, z, "core:air")
				if sapling ~= "" and game.random() < SAPLING_CHANCE then
					world.drop_item(x, y, z, sapling, 1)
				end
			end
		end
	end
	-- Growth, once a second.
	timer = timer + dt
	if timer < TICK then return end
	for k, left in pairs(growing) do
		left = left - timer
		if left > 0 then
			growing[k] = left
		else
			growing[k] = nil
			local x, y, z = unkey(k)
			local d = def(world.get_block(x, y, z))
			if d.grows_into then
				world.set_block(x, y, z, d.grows_into) -- (tracks the next stage)
			elseif d.grows_tree and not world.grow_tree(x, y, z, pick_tree(d.grows_tree)) then
				growing[k] = (d.grow_seconds or 60.0) * 0.5 -- no room yet: later
			end
		end
	end
	timer = 0
end)

-- world.json's "plants": [[x, y, z, seconds left], ...], as the Godot version.
game.set_rule("save_plants", function()
	local out = {}
	for k, left in pairs(growing) do
		local x, y, z = unkey(k)
		out[#out + 1] = {x, y, z, left}
	end
	table.sort(out, function(a, b)
		if a[1] ~= b[1] then return a[1] < b[1] end
		if a[2] ~= b[2] then return a[2] < b[2] end
		return a[3] < b[3]
	end)
	return out
end)

game.set_rule("load_plants", function(data)
	for _, e in ipairs(data) do
		if type(e) == "table" and #e == 4 then
			growing[key(math.floor(e[1]), math.floor(e[2]), math.floor(e[3]))] = e[4]
		end
	end
end)
