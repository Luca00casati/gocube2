-- How the base game's blocks look: faces painted from a colour and noise (no
-- image files), shared by the blocks that look alike. A block's `textures`
-- face is one of these tables, or an image ("textures/x.ppm") for the few
-- with a shape to draw (saplings, torches, doors, beds, chests).
--
-- color: the base colour; noise: how much lighter or darker each pixel gets,
-- in `shades` steps; blotch: how much that follows larger patches. On top:
-- cells (stones), stripes, rings (a log's end), band (a grassy top), spots
-- (ore). See src/content/image.h.

local L = {}

L.stone = {color = "#75787e", noise = 0.3, shades = 5, blotch = 0.35}
L.cobblestone = {color = "#72757b", noise = 0.28, shades = 4,
	cells = {size = 5, edge = "#4e5156"}}
L.dirt = {color = "#7d583b", noise = 0.24, shades = 4, blotch = 0.25}
L.grass_top = {color = "#58923a", noise = 0.28, shades = 5, blotch = 0.3}
L.grass_side = {color = "#7d583b", noise = 0.24, shades = 4, blotch = 0.25,
	band = {color = "#58923a", depth = 3, ragged = 2}}
L.snow = {color = "#f0f5fa", noise = 0.05, shades = 3, blotch = 0.5}
L.sand = {color = "#d4c18a", noise = 0.12, shades = 5, blotch = 0.3}
L.sandstone = {color = "#cbb47a", noise = 0.08, shades = 3,
	stripes = {color = "#b39c63", across = true, every = 5, broken = 0.3}}
L.gravel = {color = "#4f3524", noise = 0.12, shades = 3,
	cells = {size = 3, edge = "#4f3524",
		colors = {"#7a7d83", "#686b71", "#8b8e93", "#785237", "#8b6142", "#c9b57c"}}}
L.ice = {color = "#9cc0e6", noise = 0.08, shades = 3, blotch = 0.7, blotch_size = 6,
	spots = {color = "#c3dcf3", count = 4, size = 3, shape = "dash"}}
L.lava = {colors = {"#c8380c", "#ec6418", "#ec6418", "#fb9a2a", "#ffd25a"}, blotch = 0.85, blotch_size = 6}

local function ore(color, light, dark, count, size)
	return {color = L.stone.color, noise = L.stone.noise, shades = L.stone.shades, blotch = L.stone.blotch,
		spots = {color = color, light = light, dark = dark, count = count, size = size}}
end
L.coal_ore = ore("#2a2a2f", "#4a4a52", "#1a1a1d", 6, 6)
L.iron_ore = ore("#b48a6c", "#e2c2a2", "#7d5c48", 5, 6)
L.gold_ore = ore("#dcae26", "#fde877", "#9c7210", 5, 5)
L.crystal_ore = ore("#38bcc6", "#b2f6f3", "#18707c", 4, 5)

L.log_side = {color = "#4a3624", noise = 0.15, shades = 3,
	stripes = {color = "#33251a", every = 4, wobble = 1, broken = 0.2}}
L.log_top = {color = "#a07246", noise = 0.08, shades = 3,
	rings = {color = "#7f5833", every = 3, border = "#59422c", border_width = 1.5}}

L.leaves = {color = "#2f5e24", noise = 0.3, shades = 4, blotch = 0.2}

L.planks = {color = "#93683d", noise = 0.1, shades = 3, blotch = 0.5,
	stripes = {
		{color = "#b88653", across = true, every = 4},
		{color = "#76522f", across = true, every = 4, offset = 3, seams = "#6b4a2b"},
	}}

L.cactus_side = {color = "#43832e", noise = 0.08, shades = 3,
	stripes = {color = "#346b25", every = 4},
	spots = {color = "#e6deb4", count = 10, size = 1}}
L.cactus_top = {color = "#43832e", noise = 0.1, shades = 3,
	rings = {color = "#367026", every = 3}}

return L
