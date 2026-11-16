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
--
-- Ninjas come out at night (after a new world's first night, in survival)
-- on solid ground 20-40 blocks from a player (at most MAX_NINJAS), run at the
-- nearest survival player within SIGHT, leap over what's in the way and
-- strike when close; caught by the sun (by day, nothing above them) they
-- vanish in a puff of smoke.

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

local NINJA = "core:ninja"
local MAX_NINJAS = 6
local NINJA_NIGHT = 0.15 -- daylight below which ninjas come out
local NINJA_HEALTH = 12
local RUN_SPEED, WANDER_SPEED = 3.6, 1.4
local SIGHT, REACH = 24.0, 1.5
local NINJA_DAMAGE, ATTACK_EVERY = 3, 1.0

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

-- A ninja: runs at the nearest survival player in sight and strikes when
-- close; else wanders. Returns false when the sun caught it (it's gone).
local function ninja(id, c, dt, players, day)
	local s = state[id]
	s.cooldown = math.max(0, s.cooldown - dt)
	local target, best = nil, SIGHT
	for _, p in ipairs(players) do
		if p.survival and p.alive then
			local dx, dy, dz = p.x - c.x, p.y - c.y, p.z - c.z
			local d = math.sqrt(dx * dx + dy * dy + dz * dz)
			if d < best then target, best = p, d end
		end
	end
	local speed = 0
	if target then
		local tx, ty, tz = target.x - c.x, target.y - c.y, target.z - c.z
		s.heading = math.atan(-tx, -tz)
		local flat = math.sqrt(tx * tx + tz * tz)
		if flat > REACH * 0.8 then
			speed = RUN_SPEED
		end
		if flat < REACH and math.abs(ty) < 1.6 and s.cooldown <= 0 then
			s.cooldown = ATTACK_EVERY
			creatures.set(id, {attack = true})
			local nx, nz = 0, 0
			if flat > 1e-4 then nx, nz = tx / flat, tz / flat end
			game.damage_player(target.peer, NINJA_DAMAGE, "mob", nx * 6.0, 4.0, nz * 6.0)
		end
	else
		s.wander = s.wander - dt
		if s.wander <= 0 then
			s.wander = rand_range(2.0, 5.0)
			s.heading = s.heading + rand_range(-2.0, 2.0)
		end
		speed = math.fmod(s.wander, 2.0) > 0.8 and WANDER_SPEED or 0
	end
	creatures.set(id, {heading = s.heading, speed = speed})
	-- In daylight under open sky (nothing above its head): gone in a puff of smoke.
	if day >= 0.35 and not world.raycast(c.x, c.y + 1.9, c.z, 0, 1, 0, 96) then
		game.effect("smoke", c.x, c.y + 0.9, c.z)
		game.play_sound("core:effects", "splash", c.x, c.y + 0.9, c.z, -12.0)
		creatures.remove(id)
		state[id] = nil
		return false
	end
	return true
end

-- A ninja on solid ground (not leaves) 20-40 blocks from a player, with room to stand.
local function spawn_ninja(players)
	local p = players[math.floor(game.random() * #players) + 1]
	local angle = game.random() * 2 * math.pi
	local dist = rand_range(20.0, 40.0)
	local x = math.floor(p.x + math.cos(angle) * dist)
	local z = math.floor(p.z + math.sin(angle) * dist)
	local hit = world.raycast(x + 0.5, TOP_Y, z + 0.5, 0, -1, 0, 400)
	if not hit or (registry.get_block(hit.id) or {}).leaves then
		return
	end
	local fy = hit.y + 1
	if world.box_free(x + 0.5, fy, z + 0.5, 0.6, 1.8, 0.6) and not is_fluid(world.get_block(x, fy, z)) then
		local id = creatures.spawn(NINJA, x + 0.5, fy, z + 0.5)
		if id then
			state[id] = {health = NINJA_HEALTH, heading = game.random() * 2 * math.pi, cooldown = 0, wander = 0}
		end
	end
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
	local players = game.players()
	local day = game.day()
	for _, id in ipairs(creatures.all()) do
		local c = creatures.get(id)
		if c.kind == EATER then
			state[id] = state[id] or {state = "idle", time = 0, health = EATER_HEALTH, heading = c.heading}
			eater(id, c, dt)
		elseif c.kind == NINJA then
			state[id] = state[id] or {health = NINJA_HEALTH, heading = c.heading, cooldown = 0, wander = 0}
			ninja(id, c, dt, players, day)
		end
	end
	spawn_timer = spawn_timer + dt
	if spawn_timer < SPAWN_EVERY then
		return
	end
	spawn_timer = 0
	despawn_far(players)
	if #players == 0 then
		return
	end
	-- Ninjas only once a new world's first night is over, and only for survival players.
	local survival = false
	for _, p in ipairs(players) do survival = survival or p.survival end
	if day > 0.3 and count(EATER) < MAX_EATERS then
		spawn_eater(players)
	elseif survival and game.first_night() == "over" and day < NINJA_NIGHT and count(NINJA) < MAX_NINJAS then
		spawn_ninja(players)
	end
end)

-- A ninja hit: it flashes red and is knocked back; defeated, it's gone.
local function ninja_hit(id, c, damage, fx, fz)
	local s = state[id] or {health = NINJA_HEALTH, heading = c.heading, cooldown = 0, wander = 0}
	state[id] = s
	game.play_sound("core:effects", "hurt", c.x, c.y, c.z, -4.0)
	s.health = s.health - damage
	local px, pz = c.x - fx, c.z - fz
	local len = math.sqrt(px * px + pz * pz)
	if len > 1e-4 then px, pz = px / len, pz / len else px, pz = 0, 0 end
	creatures.set(id, {flash = true, push = {px * 5.0, 4.5, pz * 5.0}})
	if s.health <= 0 then
		creatures.remove(id)
		state[id] = nil
	end
end

-- Hit by a player: hurt, knocked back, runs off; defeated, it drops what it's made of.
events.on("creature_hit", function(id, damage, fx, fy, fz)
	local c = creatures.get(id)
	if c and c.kind == NINJA then
		ninja_hit(id, c, damage, fx, fz)
		return
	end
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
