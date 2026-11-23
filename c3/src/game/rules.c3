// The base game's rules (what the core mod's code set: src/core_mod): mining
// time, what breaking a block drops, fall and void damage, food, the two
// hand recipes, the dye vat's palette and what a new player starts with.
module gocube::game::rules;
import gocube::data, gocube::content, gocube::voxel, gocube::util::vec, gocube::util::rand, gocube::util::intern;
import std::math;

const double MINE_SECONDS_PER_HARDNESS = 0.375;
const double MIN_MINE_SECONDS = 0.1;
const double SAFE_FALL = 3.0; // blocks you can fall without damage

// Mining: hardness x 0.375 s, whatever is held (no tools). `def`: the block's (null: unknown).
fn float mine_seconds(Data* def) => (float)math::max(MIN_MINE_SECONDS, (def ? def.num("hardness", 1.0) : 1.0) * MINE_SECONDS_PER_HARDNESS);

// What breaking `block` gives (one each, interned): its "drops" ("" =
// nothing), else the block itself (a variant: its base block); plus its
// "extra_drops" ([[item, count, chance], ...]), rolled now.
fn void drops(Data* def, String block, Rng* random, Vec{String}* out)
{
	String main = block;
	if (def)
	{
		main = def.str("variant_of", block);
		if (Data* d = def.get("drops")) main = d.is_string() ? d.s : main;
	}
	if (main.len) out.push(intern::intern(main));
	Data* extra = def ? def.get("extra_drops") : null;
	if (!extra) return;
	foreach (&e : extra.items())
	{
		if (e.is_array() && e.len() >= 3 && e.at(0).is_string() && random.unit() < e.at(2).as_double())
		{
			for (int i = 0; i < e.at(1).as_int(); i++) out.push(intern::intern(e.at(0).s));
		}
	}
}

// Health a fall of that many blocks takes.
fn int fall_damage(double blocks) => blocks <= SAFE_FALL ? 0 : (int)math::floor(blocks - SAFE_FALL);

// Below the world's bottom: damage every half second.
const int VOID_DAMAGE = 4;

// Health an item gives when eaten (0: it isn't food): its "food".
fn int food(Content* content, String item)
{
	Data* def = content.items.get(item);
	return def ? (int)def.num("food", 0) : 0;
}

struct Recipe
{
	String id;
	String[2] ingredient_ids; // (the base game's recipes have one ingredient each)
	int[2] ingredient_counts;
	int ingredients;
	String output;
	int count;
}

// The only crafting: planks from logs, and the dye vat from planks.
const Recipe[2] HAND_RECIPES = {
	{ .id = "planks", .ingredient_ids = { "core:log", "" }, .ingredient_counts = { 1, 0 }, .ingredients = 1, .output = "core:planks", .count = 4 },
	{ .id = "dye_vat", .ingredient_ids = { "core:planks", "" }, .ingredient_counts = { 6, 0 }, .ingredients = 1, .output = "core:dye_vat", .count = 1 },
};

// The dye vat's colours: whatever the gems mix to, the nearest of these
// natural tones (so no mix is garish).
const Dye[*] DYE_PALETTE = {
	// whites, greys, black
	0xFFecebe6, 0xFFb9b8b2, 0xFF807f7b, 0xFF4a4a48, 0xFF232325,
	// reds, pinks
	0xFFb8392f, 0xFF9c3b2e, 0xFF6e2a2a, 0xFFd98c8c, 0xFFc26a7a, 0xFFe8b8b0,
	// oranges, yellows, creams
	0xFFcc7a2e, 0xFFa8552d, 0xFFc9a23a, 0xFFe0c872, 0xFFece0b8,
	// browns
	0xFFb08a62, 0xFF6b4a2f, 0xFF4a3221,
	// greens
	0xFF9cba4a, 0xFF7a7f35, 0xFF4e8a3e, 0xFF5f7a3a, 0xFF2f4f33, 0xFF8fa58a, 0xFF3f7f78,
	// blues
	0xFF4f9cb0, 0xFF7fa8cf, 0xFF3d62a8, 0xFF5b6f86, 0xFF263a63,
	// purples
	0xFF9b8bbf, 0xFF6d4a8f, 0xFFa04a86, 0xFF5e3352,
};

// What a new survival player starts with: a torch, in hand.
const String STARTING_ITEM = "core:torch";
const int STARTING_COUNT = 1;
