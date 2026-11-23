// The base game's rules (formerly set by the core mod's code): mining
// time, what breaking a block drops, fall and void damage, the hand recipe
// and what a new player starts with.
module gocube::game::rules;
import gocube::data, gocube::util::vec, gocube::util::intern;
import std::math;

const double MINE_SECONDS_PER_HARDNESS = 0.375;
const double MIN_MINE_SECONDS = 0.1;
const double SAFE_FALL = 3.0; // blocks you can fall without damage

// Mining: hardness x 0.375 s, whatever is held (no tools). `def`: the block's (null: unknown).
fn float mine_seconds(Data* def) => (float)math::max(MIN_MINE_SECONDS, (def ? def.num("hardness", 1.0) : 1.0) * MINE_SECONDS_PER_HARDNESS);

// What breaking `block` gives (one each, interned): its "drops" (another
// block; "" = nothing), else the block itself (a variant: its base block).
fn void drops(Data* def, String block, Vec{String}* out)
{
	String main = block;
	if (def)
	{
		main = def.str("variant_of", block);
		if (Data* d = def.get("drops")) main = d.is_string() ? d.s : main;
	}
	if (main.len) out.push(intern::intern(main));
}

// Health a fall of that many blocks takes.
fn int fall_damage(double blocks) => blocks <= SAFE_FALL ? 0 : (int)math::floor(blocks - SAFE_FALL);

// Below the world's bottom: damage every half second.
const int VOID_DAMAGE = 4;

struct Recipe
{
	String id;
	String[2] ingredient_ids; // (the base game's recipes have one ingredient each)
	int[2] ingredient_counts;
	int ingredients;
	String output;
	int count;
}

// The only crafting: planks from logs.
const Recipe[1] HAND_RECIPES = {
	{ .id = "planks", .ingredient_ids = { "core:log", "" }, .ingredient_counts = { 1, 0 }, .ingredients = 1, .output = "core:planks", .count = 4 },
};
