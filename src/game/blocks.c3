// Block rules for playing: which blocks can be placed, which variant a
// placement makes (slabs, stairs, torches, ladders, doors, beds), the other
// half of a door or bed, opening doors, and what needs support.
module gocube::game::blocks;
import gocube::content, gocube::data, gocube::world, gocube::util::vec, std::math;

const Data EMPTY_DEF @private = {};

fn BlockPos facing_dir(String f) // towards -z, +x, +z, -x
{
	switch (f)
	{
		case "n": return { 0, 0, -1 };
		case "e": return { 1, 0, 0 };
		case "s": return { 0, 0, 1 };
		case "w": return { -1, 0, 0 };
		default: return { 0, -1, 0 }; // "floor"
	}
}

// The wall a block sits against when placed on a face with normal n.
fn String wall_facing(BlockPos n) @private
{
	if (n.z == 1) return "n";
	if (n.z == -1) return "s";
	if (n.x == 1) return "w";
	return "e";
}

fn String look_facing(float[3] look)
{
	if (math::abs(look[0]) > math::abs(look[2])) return look[0] > 0 ? "e" : "w";
	return look[2] > 0 ? "s" : "n";
}

struct Placed
{
	BlockPos pos;
	String id; // (temp)
}

alias Placement = Vec{Placed};

struct BlockRules
{
	Content* content;
	Vec{String} placeable_; // views of the content's ids
}

fn void BlockRules.init(&self, Content* content)
{
	self.content = content;
	self.placeable_.clear();
	foreach (id : content.block_ids()) // (sorted after air)
	{
		if (self.is_placeable(id)) self.placeable_.push(id);
	}
}

fn void BlockRules.free(&self) => self.placeable_.free();

// "" if unknown.
fn String BlockRules.id_of(&self, int block)
{
	String[] ids = self.content.block_ids();
	return block >= 0 && block < ids.len ? ids[block] : "";
}

fn int BlockRules.index_of(&self, String id) => self.content.block_index(id);

// A block's definition (a dyed block's: its block's); an empty one if unknown.
fn Data* BlockRules.def(&self, String id)
{
	Data* d = self.content.blocks.get(content::undyed(id));
	return d ? d : &EMPTY_DEF;
}

fn Data* BlockRules.def_of(&self, int block) => self.def(self.id_of(block));

// Blocks a player can place (not air, fluids, variants or hidden ones), by id.
fn String[] BlockRules.placeable(&self) => self.placeable_.array_view();

fn bool BlockRules.is_placeable(&self, String dyed_id)
{
	String id = content::undyed(dyed_id); // (dyed blocks too)
	Data* d = self.content.blocks.get(id);
	return d && !d.has("fluid") && id != "core:air" && !d.has("variant_of") && !d.flag("hidden");
}

// A variant's base block (or the block itself): a view of either.
fn String BlockRules.base_of(&self, String id) => self.def(id).str("variant_of", id);

fn bool BlockRules.replaceable(&self, int block) => self.def_of(block).flag("replaceable"); // placing into it replaces it (tall grass, flowers)
fn bool BlockRules.climbable(&self, int block) => self.def_of(block).flag("climbable");    // ladders
fn bool BlockRules.is_fluid(&self, int block) => self.def_of(block).has("fluid");

fn String variant(String base, String facing) @private => facing == "n" ? base : string::tformat("%s_%s", base, facing);

fn String door_id(String base, String facing, String half, bool open) @private
{
	if (facing == "n" && half == "lower" && !open) return base;
	return string::tformat("%s_%s_%s%s", base, facing, half, open ? "_open" : "");
}

// What placing `base` against the face `hit` makes: [(cell, block id)] (ids
// temp); doors and beds place two. `look`: the view direction; `hit_y`: the
// height of the point hit (slabs go in the half you aim at). Empty if it can't
// go there.
fn void BlockRules.placement(&self, String base, RayHit* hit, float[3] look, float hit_y, Placement* out)
{
	Data* d = self.def(base);
	BlockPos pos = hit.previous;
	BlockPos n = hit.normal;
	if (d.flag("leaves"))
	{
		out.push({ pos, string::tformat("%s_placed", base) }); // placed leaves never decay
		return;
	}
	String shape = d.str("shape", "cube");
	switch (shape)
	{
		case "slab":
			float frac = hit_y - math::floor(hit_y);
			bool top = n.y == -1 || (n.y == 0 && frac > 0.5f);
			out.push({ pos, top ? string::tformat("%s_top", base) : base });
		case "stairs":
			out.push({ pos, variant(base, look_facing(look)) });
		case "torch":
			if (n.y == 1)
			{
				out.push({ pos, base });
			}
			else if (n.y != -1)
			{
				out.push({ pos, string::tformat("%s_wall_%s", base, wall_facing(n)) });
			}
		case "ladder":
			if (n.y == 0) out.push({ pos, variant(base, wall_facing(n)) });
		case "door":
			String f = look_facing(look);
			out.push({ pos, door_id(base, f, "lower", false) });
			out.push({ pos + { 0, 1, 0 }, door_id(base, f, "upper", false) });
		case "bed":
			// The foot where it's placed, the head one block further the way you look.
			String f = look_facing(look);
			out.push({ pos, f == "n" ? base : string::tformat("%s_%s_foot", base, f) });
			out.push({ pos + facing_dir(f), string::tformat("%s_%s_head", base, f) });
		default:
			out.push({ pos, base });
	}
}

fn Data* BlockRules.state(&self, Data* d)
{
	Data* st = d.get("state");
	return st ? st : &EMPTY_DEF;
}

// The other half of a door or bed at `pos` (false if it has none).
fn bool BlockRules.other_part(&self, String id, BlockPos pos, BlockPos* out)
{
	Data* d = self.def(id);
	Data* st = self.state(d);
	String shape = d.str("shape");
	if (shape == "door")
	{
		*out = pos + (BlockPos){ 0, st.str("half") == "lower" ? 1 : -1, 0 };
		return true;
	}
	if (shape == "bed")
	{
		BlockPos f = facing_dir(st.str("facing"));
		*out = st.str("part") == "foot" ? pos + f : pos + (BlockPos){ -f.x, -f.y, -f.z };
		return true;
	}
	return false;
}

// The door at `pos` opened or closed (both halves; ids temp), or nothing if it isn't a door.
fn void BlockRules.toggle_door(&self, String id, BlockPos pos, Placement* out)
{
	Data* d = self.def(id);
	if (d.str("shape") != "door") return;
	String base = self.base_of(id);
	Data* st = self.state(d);
	bool open = !st.flag("open");
	String half = st.str("half");
	String facing = st.str("facing");
	String other_half = half == "lower" ? "upper" : "lower";
	BlockPos other = pos + (BlockPos){ 0, half == "lower" ? 1 : -1, 0 };
	out.push({ pos, door_id(base, facing, half, open) });
	out.push({ other, door_id(base, facing, other_half, open) });
}

fn bool BlockRules.solid_support(&self, World* world, BlockPos pos)
{
	String id = self.id_of(world.block_at(pos.x, pos.y, pos.z));
	Data* d = self.def(id);
	String shape = d.str("shape", "cube");
	return !d.is_empty() && d.flag("solid", true) && !d.has("fluid") && id != "core:air" &&
		(shape == "cube" || shape == "slab" || shape == "stairs");
}

// Whether `id` can stand at `pos`: plants on the right block, torches and
// ladders on something solid, doors on the floor.
fn bool BlockRules.supported(&self, World* world, String id, BlockPos pos)
{
	Data* d = self.def(id);
	String below = self.id_of(world.block_at(pos.x, pos.y - 1, pos.z));
	Data* plant_on = d.get("plant_on");
	if (plant_on && plant_on.is_array())
	{
		foreach (&b : plant_on.items())
		{
			if (b.is_string() && b.s == below) return true;
		}
		return false;
	}
	if (!d.flag("needs_support")) return true;
	Data* st = self.state(d);
	if (d.str("shape") == "door") // stands on the floor whichever way it faces
	{
		if (st.str("half") == "upper") return self.base_of(below) == self.base_of(id);
		return self.solid_support(world, pos + { 0, -1, 0 });
	}
	return self.solid_support(world, pos + facing_dir(st.str("facing", "floor")));
}
