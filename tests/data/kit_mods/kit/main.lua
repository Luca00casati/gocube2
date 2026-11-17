-- Crafting, smelting, a tool, armour, farming and food, as a mod would add them (the base
-- game had these before; tests/test_survival.cpp plays with them).

registry.register_item("stick", {color = "#8a6a40"})
registry.register_item("iron_ingot", {color = "#d8d8d8"})
registry.register_item("wood_pickaxe", {color = "#a07a50", tool = "pickaxe", max_stack = 1, damage = 3})
registry.register_item("iron_helmet", {color = "#c8c8c8", armor = {slot = "head", points = 2}, max_stack = 1})
registry.register_block("kiln", {hardness = 3.5, color = "#6a6a6a", block_entity = "furnace"})

registry.register_recipe("log_to_planks", {
	type = "shapeless",
	ingredients = {["core:log"] = 1},
	output = {item = "core:planks", count = 4},
})
registry.register_recipe("planks_to_sticks", {
	type = "shaped",
	pattern = {"P", "P"},
	key = {P = "core:planks"},
	output = {item = "kit:stick", count = 4},
})
registry.register_recipe("wood_pickaxe", {
	type = "shaped",
	pattern = {"PPP", " S ", " S "},
	key = {P = "core:planks", S = "kit:stick"},
	output = {item = "kit:wood_pickaxe"},
})
registry.register_recipe("chest", {
	type = "shaped",
	pattern = {"PPP", "P P", "PPP"},
	key = {P = "core:planks"},
	output = {item = "core:chest"},
})
registry.register_recipe("iron_ingot", {type = "smelting", input = "core:iron_ore", output = {item = "kit:iron_ingot"}})

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

game.set_rule("hand_recipes", hand_recipes)

game.set_rule("smelting", function(input)
	for _, r in pairs(registry.recipes()) do
		if r.type == "smelting" and r.input == input and r.output and r.output.item then
			return {item = r.output.item, count = r.output.count or 1}
		end
	end
	return nil
end)

local FUEL = {["core:coal"] = 40, ["kit:stick"] = 2.5}

game.set_rule("fuel_seconds", function(id)
	return FUEL[id] or 0
end)

-- A hostile creature (the base game has none): at night it runs at the
-- nearest survival player and strikes; in daylight under open sky it's gone
-- in a puff of smoke. (Much as the base game's ninjas were.)
registry.register_creature("stalker", {
	model = "ninja",
	box = {0.6, 1.8, 0.6},
	gravity = 24.0,
	step_height = 1.0,
	turn_rate = 12.0,
	jump = 8.0,
	jump_when_blocked = true,
	light_height = 1.0,
})

local cooldown = {}

events.on("tick", function(dt)
	local players = game.players()
	local day = game.day()
	for _, id in ipairs(creatures.all()) do
		local c = creatures.get(id)
		if c.kind == "kit:stalker" then
			cooldown[id] = math.max(0, (cooldown[id] or 0) - dt)
			local target, best = nil, 24.0
			for _, p in ipairs(players) do
				if p.survival and p.alive then
					local dx, dz = p.x - c.x, p.z - c.z
					local d = math.sqrt(dx * dx + dz * dz)
					if d < best then target, best = p, d end
				end
			end
			if target then
				local tx, tz = target.x - c.x, target.z - c.z
				creatures.set(id, {heading = math.atan(-tx, -tz), speed = best > 1.2 and 3.6 or 0})
				if best < 1.5 and math.abs(target.y - c.y) < 1.6 and cooldown[id] <= 0 then
					cooldown[id] = 1.0
					creatures.set(id, {attack = true})
					local nx, nz = 0, 0
					if best > 1e-4 then nx, nz = tx / best, tz / best end
					game.damage_player(target.peer, 3, "mob", nx * 6.0, 4.0, nz * 6.0)
				end
			end
			if day >= 0.35 and not world.raycast(c.x, c.y + 1.9, c.z, 0, 1, 0, 96) then
				game.effect("smoke", c.x, c.y + 0.9, c.z)
				creatures.remove(id)
				cooldown[id] = nil
			end
		end
	end
end)

-- Farming and food, as a mod would add them (the base game has none): seeds
-- planted on dirt grow into a crop; bread heals.
registry.register_item("bread", {color = "#c08a40", food = 5})
registry.register_item("seeds", {color = "#c8b040", plants = "kit:sprout", plant_on = {"core:dirt"}})
registry.register_block("sprout", {shape = "cross", hardness = 0, color = "#5a9a30", plant_on = {"core:dirt"},
	drops = "kit:seeds", grows_into = "kit:crop", grow_seconds = 50})
registry.register_block("crop", {shape = "cross", hardness = 0, color = "#c8b040", plant_on = {"core:dirt"},
	drops = "kit:bread"})

game.set_rule("use_item", function(item, x, y, z, nx, ny, nz)
	local def = registry.get_item(item) or {}
	if not def.plants or ny ~= 1 or world.get_block(x, y + 1, z) ~= "core:air" then
		return nil
	end
	local ok = false
	for _, b in ipairs(def.plant_on or {}) do
		ok = ok or b == world.get_block(x, y, z)
	end
	if not ok then
		return nil
	end
	world.set_block(x, y + 1, z, def.plants)
	return {take = 1, placed = {x, y + 1, z}}
end)
