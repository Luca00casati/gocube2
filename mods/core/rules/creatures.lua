-- Creatures' behaviour (the Godot version's MobManager and BlockEater).
-- The game moves their bodies (gravity, steps, turning towards `heading` at
-- `speed`); this decides where they go, spawns and despawns them, and handles
-- hits.
--
-- Block Eaters come out by day on grass open to the sky, 20-48 blocks from a
-- player (at most MAX_EATERS), and go when no player is within
-- DESPAWN_DISTANCE. They graze (idle), wander, turn away from water and
-- drops, and run off when hit. The regular one (mossy) now and then eats the
-- block in front of it and takes its form; one made of a block drops it when
-- defeated.

local MAX_EATERS = 12
local SPAWN_EVERY = 2.0
local SPAWN_MIN, SPAWN_MAX = 20.0, 48.0
local DESPAWN_DISTANCE = 80.0
local REGULAR_CHANCE = 0.75 -- the rest are made of a random block
local TOP_Y = 200

local EATER = "core:block_eater"
local EATER_HEALTH = 8
local WALK_SPEED, FLEE_SPEED = 1.2, 3.4
local EAT_CHANCE = 0.02 -- each time a regular one starts grazing

local state = {} -- id -> {state, time, health, flee_x, flee_z}
local spawn_timer = 0

local function rand_range(a, b) return a + (b - a) * game.random() end

-- Every block a Block Eater can be made of, with how often ("eater_weight",
-- default 1; 0 none): solid blocks that aren't fluids.
local variants, weights, total_weight
local function learn_variants()
	variants, weights, total_weight = {}, {}, 0
	local ids = {}
	for id, def in pairs(registry.all("blocks")) do
		if id ~= "core:air" and def.solid ~= false and not def.fluid then
			ids[#ids + 1] = id
		end
	end
	table.sort(ids)
	for _, id in ipairs(ids) do
		local w = registry.get_block(id).eater_weight or 1.0
		if w > 0 then
			variants[#variants + 1] = id
			weights[id] = w
			total_weight = total_weight + w
		end
	end
end

local function random_variant()
	if #variants == 0 or game.random() < REGULAR_CHANCE then
		return ""
	end
	local r = game.random() * total_weight
	for _, id in ipairs(variants) do
		r = r - weights[id]
		if r < 0 then return id end
	end
	return variants[#variants]
end

local function block_sound(block, kind, x, y, z, db)
	local def = registry.get_block(block)
	if def and def.sound then
		game.play_sound(def.sound, kind, x, y, z, db)
	end
end

local function is_fluid(id) return id ~= nil and (registry.get_block(id) or {}).fluid ~= nil end

-- Water at the next step, or a fall of more than 3 blocks.
local function danger_ahead(c, heading)
	local ax = math.floor(c.x - math.sin(heading) * 0.9)
	local az = math.floor(c.z - math.cos(heading) * 0.9)
	local ay = math.floor(c.y)
	for dy = 1, -3, -1 do
		local b = world.get_block(ax, ay + dy, az)
		if is_fluid(b) then return true end
		if b ~= nil and b ~= "core:air" then return false end
	end
	return true
end

-- Eat the block in front of its feet: it goes, and the Block Eater takes its
-- form. Only blocks that have a form; never storage (the contents would spill).
local function try_eat(id, c)
	local x = math.floor(c.x - math.sin(c.yaw) * 0.9)
	local y = math.floor(c.y - 0.5)
	local z = math.floor(c.z - math.cos(c.yaw) * 0.9)
	local block = world.get_block(x, y, z)
	if block == nil or not weights[block] or (registry.get_block(block) or {}).block_entity then
		return
	end
	if world.set_block(x, y, z, "core:air") then
		creatures.set(id, {variant = block})
		block_sound(block, "break", x + 0.5, y + 0.5, z + 0.5)
	end
end

local function pick_state(id, s, c)
	if s.state == "flee" or game.random() < 0.45 then
		s.state = "idle" -- graze a while
		s.time = rand_range(2.0, 5.0)
		if c.variant == "" and game.random() < EAT_CHANCE then
			try_eat(id, c)
		end
	else
		s.state = "wander"
		s.time = rand_range(2.5, 6.0)
		s.heading = (s.heading or c.heading) + rand_range(-1.6, 1.6)
	end
end

local function eater(id, c, dt)
	local s = state[id]
	s.time = s.time - dt
	if s.time <= 0 then
		pick_state(id, s, c)
	end
	local speed = 0
	if s.state == "wander" then
		speed = WALK_SPEED
		if c.hit_wall then -- blocked: try another way
			s.heading = s.heading + math.pi * 0.5 + game.random() * math.pi
		end
	elseif s.state == "flee" then
		speed = FLEE_SPEED
		local ax, az = c.x - s.flee_x, c.z - s.flee_z
		if ax * ax + az * az > 0.01 then
			s.heading = math.atan(-ax, -az)
		end
	end
	if speed > 0 and danger_ahead(c, s.heading) then
		s.heading = s.heading + math.pi * (0.5 + game.random()) -- water or a drop ahead: turn away
	end
	creatures.set(id, {heading = s.heading, speed = speed})
end

local function spawn_eater(players)
	local p = players[math.floor(game.random() * #players) + 1]
	local angle = game.random() * 2 * math.pi
	local dist = rand_range(SPAWN_MIN, SPAWN_MAX)
	local x = math.floor(p.x + math.cos(angle) * dist)
	local z = math.floor(p.z + math.sin(angle) * dist)
	local hit = world.raycast(x + 0.5, TOP_Y, z + 0.5, 0, -1, 0, 400)
	if not hit or hit.id ~= "core:grass" then
		return -- only on grass, open to the sky
	end
	local fy = hit.y + 1
	if world.box_free(x + 0.5, fy, z + 0.5, 0.9, 0.95, 0.9) then
		local id = creatures.spawn(EATER, x + 0.5, fy, z + 0.5, random_variant())
		if id then
			state[id] = {state = "idle", time = 0, health = EATER_HEALTH, heading = game.random() * 2 * math.pi}
		end
	end
end

local function despawn_far(players)
	for _, id in ipairs(creatures.all()) do
		local c = creatures.get(id)
		local near = false
		for _, p in ipairs(players) do
			local dx, dy, dz = p.x - c.x, p.y - c.y, p.z - c.z
			if dx * dx + dy * dy + dz * dz < DESPAWN_DISTANCE * DESPAWN_DISTANCE then
				near = true
			end
		end
		if not near or c.y < -200 then
			creatures.remove(id)
			state[id] = nil
		end
	end
end

local function count(kind)
	local n = 0
	for _, id in ipairs(creatures.all()) do
		if creatures.get(id).kind == kind then n = n + 1 end
	end
	return n
end

events.on("tick", function(dt)
	if not variants then
		learn_variants()
	end
	for _, id in ipairs(creatures.all()) do
		local c = creatures.get(id)
		if c.kind == EATER then
			state[id] = state[id] or {state = "idle", time = 0, health = EATER_HEALTH, heading = c.heading}
			eater(id, c, dt)
		end
	end
	spawn_timer = spawn_timer + dt
	if spawn_timer < SPAWN_EVERY then
		return
	end
	spawn_timer = 0
	local players = game.players()
	despawn_far(players)
	if #players > 0 and game.day() > 0.3 and count(EATER) < MAX_EATERS then
		spawn_eater(players)
	end
end)

-- Hit by a player: hurt, knocked back, runs off; defeated, it drops what it's made of.
events.on("creature_hit", function(id, damage, fx, fy, fz)
	local c = creatures.get(id)
	if not c or c.kind ~= EATER then
		return
	end
	local s = state[id] or {health = EATER_HEALTH}
	state[id] = s
	if c.variant == "" then
		game.play_sound("core:effects", "hurt", c.x, c.y, c.z, -4.0)
	else
		block_sound(c.variant, "break", c.x, c.y, c.z)
	end
	s.health = s.health - damage
	s.state, s.time = "flee", 3.0
	s.flee_x, s.flee_z = fx, fz
	local px, pz = c.x - fx, c.z - fz
	local len = math.sqrt(px * px + pz * pz)
	if len > 1e-4 then px, pz = px / len, pz / len else px, pz = 0, 0 end
	creatures.set(id, {hurt = true, push = {px * 4.0, 5.0, pz * 4.0}})
	if s.health <= 0 then
		if c.variant ~= "" then
			world.drop_item(math.floor(c.x), math.floor(c.y + 0.5), math.floor(c.z), c.variant, 1)
		end
		creatures.remove(id)
		state[id] = nil
	end
end)
