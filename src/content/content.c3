// The game's content: blocks, items, recipes, ores, trees, biomes, sound sets
// and creatures, read from the game's content.json (definitions as data); numeric
// block ids; the engine's block table and texture atlas; the world
// generator's tables.
module gocube::content;
import gocube::data, gocube::data::json, gocube::content::image, gocube::voxel, gocube::util::vec, gocube::util::map;
import std::io, std::math, std::sort;

// The game's own ids are "core:..." (as saved worlds have them).
const String GAME = "core";

// One definition: its qualified id ("core:stone") and its fields (with
// "id" and "mod" added).
struct Entry
{
	String id;
	Data def;
}

// Definitions sorted by id (byte order), each found by id.
struct Table
{
	Vec{Entry} entries;
}

fn int compare_ids(String a, String b)
{
	sz n = math::min(a.len, b.len);
	for (sz i = 0; i < n; i++)
	{
		if (a[i] != b[i]) return (int)a[i] - (int)b[i];
	}
	return a.len < b.len ? -1 : (a.len > b.len ? 1 : 0);
}

// Where `id` is, or would go: (index, found).
fn sz Table.search(&self, String id, bool* found)
{
	sz lo = 0;
	sz hi = self.entries.len();
	while (lo < hi)
	{
		sz mid = (lo + hi) / 2;
		int c = compare_ids(self.entries[mid].id, id);
		if (c == 0)
		{
			*found = true;
			return mid;
		}
		if (c < 0)
		{
			lo = mid + 1;
		}
		else
		{
			hi = mid;
		}
	}
	*found = false;
	return lo;
}

fn Data* Table.get(&self, String id)
{
	bool found;
	sz i = self.search(id, &found);
	return found ? &self.entries.get_ref(i).def : null;
}

fn bool Table.has(&self, String id) => self.get(id) != null;
fn sz Table.len(&self) => self.entries.len();
fn Entry[] Table.all(&self) => self.entries.array_view();

// Sets (taking `def` over, replacing any definition with that id).
fn void Table.set(&self, String id, Data def)
{
	bool found;
	sz i = self.search(id, &found);
	if (found)
	{
		Entry* e = self.entries.get_ref(i);
		e.def.free();
		e.def = def;
		return;
	}
	self.entries.insert_at(i, { id.copy(mem), def });
}

fn void Table.free(&self)
{
	foreach (&e : self.entries)
	{
		free(e.id.ptr);
		e.def.free();
	}
	self.entries.free();
}

struct Content
{
	Table blocks, items, recipes, ores, trees, biomes, sound_sets, creatures;
	String dir; // the folder content.json is in: images and sounds are relative to it
	Vec{String} block_ids_;
	Map{String, int} block_index_;
}

// The table for a section name ("blocks", "items", ...), or null.
fn Table* Content.table(&self, String section)
{
	switch (section)
	{
		case "blocks": return &self.blocks;
		case "items": return &self.items;
		case "recipes": return &self.recipes;
		case "ores": return &self.ores;
		case "trees": return &self.trees;
		case "biomes": return &self.biomes;
		case "sound_sets": return &self.sound_sets;
		case "creatures": return &self.creatures;
		default: return null;
	}
}

// One entry: `id` gets "core:" in front unless it has a ":". (A copy of `def`.)
fn void Content.register(&self, Table* table, String id, Data* def) => @pool()
{
	String full = id.contains_char(':') ? id : string::tformat("%s:%s", GAME, id);
	String mod = full[: full.index_of_char(':')!!];
	Data entry = def.copy();
	entry.set_string("id", full);
	entry.set_string("mod", mod);
	table.set(full, entry);
}

// Reads `dir`/content.json: false and `error` if it can't be read or isn't
// a table of sections of tables.
fn bool Content.load(&self, String dir, DString* error) => @pool()
{
	if (!*error) error.init(mem);
	if (self.dir.ptr) free(self.dir.ptr);
	self.dir = dir.copy(mem);
	Data d;
	defer d.free();
	if (!json::read_file(string::tformat("%s/content.json", dir), &d, error)) return false;
	if (!d.is_object())
	{
		error.append("content.json must be a table of sections (blocks = {...}, items = {...})");
		return false;
	}
	foreach (&section : d.fields())
	{
		Table* table = self.table(section.key);
		if (!table || !section.value.is_object())
		{
			error.appendf("content.json has an unknown section \"%s\" (blocks, items, recipes, ores, trees, biomes, sound_sets, creatures)", section.key);
			return false;
		}
		foreach (&e : section.value.fields())
		{
			if (!e.value.is_object())
			{
				error.appendf("content.json's %s.%s isn't a table of named fields", section.key, e.key);
				return false;
			}
			self.register(table, e.key, &e.value);
		}
	}
	return true;
}

fn void Content.free(&self)
{
	foreach (t : (Table*[*]){ &self.blocks, &self.items, &self.recipes, &self.ores, &self.trees, &self.biomes, &self.sound_sets, &self.creatures })
	{
		t.free();
	}
	if (self.dir.ptr) free(self.dir.ptr);
	self.free_block_ids();
	*self = {};
}

fn void Content.free_block_ids(&self)
{
	foreach (s : self.block_ids_) free(s.ptr);
	self.block_ids_.free();
	self.block_index_.free();
}

// After everything is registered: adds shaped blocks' variants, assigns block ids.
fn void Content.freeze(&self, String[] saved_block_ids = {})
{
	expand(&self.blocks);
	self.assign_block_ids(saved_block_ids);
}

// Numeric block ids: "core:air" first, then a world's saved ids, then the
// rest sorted, so the numbers depend only on which blocks exist.
fn void Content.assign_block_ids(&self, String[] saved = {})
{
	self.free_block_ids();
	if (saved.len == 0)
	{
		self.block_ids_.push("core:air".copy(mem));
	}
	else
	{
		foreach (s : saved) self.block_ids_.push(s.copy(mem));
	}
	foreach (i, s : self.block_ids_) self.block_index_[s] = (int)i;
	foreach (&e : self.blocks.entries)
	{
		if (!self.block_index_.has(e.id))
		{
			String id = e.id.copy(mem);
			self.block_index_[id] = (int)self.block_ids_.len();
			self.block_ids_.push(id);
		}
	}
}

fn String[] Content.block_ids(&self) => self.block_ids_.array_view();

// -1 if unknown.
fn int Content.block_index(&self, String id) => self.block_index_.get_or(id, -1);

// A file of the game's (relative to its content folder, inside it), or ""
// if the path isn't one. (In the temp allocator.)
fn String Content.asset_path(&self, String relative)
{
	String rel = relative;
	if (rel.starts_with("core:")) rel = rel[5..];
	if (rel.len == 0 || rel[0] == '/' || rel.contains_char('\\') || rel.contains_char(':')) return "";
	foreach (part : rel.tsplit("/"))
	{
		if (part == "..") return ""; // stays inside
	}
	return string::tformat("%s/%s", self.dir, rel);
}

// --- Shaped blocks ---------------------------------------------------------------

const String[4] FACINGS = { "n", "e", "s", "w" }; // towards -z, +x, +z, -x

fn int facing_index(String f)
{
	foreach (i, s : FACINGS)
	{
		if (s == f) return (int)i;
	}
	return -1;
}

// Shapes you walk through.
fn bool is_not_solid_shape(String shape) => shape == "torch" || shape == "ladder" || shape == "cross";

struct Variant @private
{
	String suffix; // (temp)
	Data state;
}

fn Data state_of(String k1, String v1, String k2 = "", String v2 = "", String k3 = "", bool v3 = false, bool has3 = false) @private
{
	Data d = data::object();
	d.set_string(k1, v1);
	if (k2.len) d.set_string(k2, v2);
	if (has3) d.set_bool(k3, v3);
	return d;
}

// The variant blocks of a shaped block, in this order (block ids follow it):
// (suffix, state); the base block itself is suffix "". (Suffixes: temp.)
fn void variants(Data* def, Vec{Variant}* out) @private
{
	String shape = def.str("shape", "cube");
	if (shape == "slab")
	{
		out.push({ "", state_of("half", "bottom") });
		out.push({ "_top", state_of("half", "top") });
	}
	else if (shape == "stairs" || shape == "ladder")
	{
		for (int i = 0; i < 4; i++) out.push({ i == 0 ? "" : string::tformat("_%s", FACINGS[i]), state_of("facing", FACINGS[i]) });
	}
	else if (shape == "torch")
	{
		out.push({ "", state_of("facing", "floor") });
		foreach (f : FACINGS) out.push({ string::tformat("_wall_%s", f), state_of("facing", f) });
	}
	else if (shape == "door")
	{
		foreach (f : FACINGS)
		{
			foreach (half : (String[2]){ "lower", "upper" })
			{
				foreach (open : (bool[2]){ false, true })
				{
					String suffix = string::tformat("_%s_%s%s", f, half, open ? "_open" : "");
					if (f == "n" && half == "lower" && !open) suffix = "";
					out.push({ suffix, state_of("facing", f, "half", half, "open", open, true) });
				}
			}
		}
	}
	else if (shape == "bed")
	{
		// The foot where it's placed, the head one block further the way the player looked.
		foreach (f : FACINGS)
		{
			foreach (part : (String[2]){ "foot", "head" })
			{
				bool base = f == "n" && part == "foot";
				out.push({ base ? "" : string::tformat("_%s_%s", f, part), state_of("facing", f, "part", part) });
			}
		}
	}
	else
	{
		out.push({ "", data::object() });
	}
}

// Adds every shaped block's variants (and placed leaves) to `blocks`.
fn void expand(Table* blocks) @private => @pool()
{
	Vec{String} ids;
	defer ids.free();
	foreach (&e : blocks.entries) ids.push(e.id.tcopy());
	foreach (id : ids)
	{
		Data* def = blocks.get(id);
		if (def.flag("leaves") && !def.has("variant_of"))
		{
			Data placed = def.copy();
			String pid = string::tformat("%s_placed", id);
			placed.set_string("id", pid);
			placed.set_string("variant_of", id);
			placed.set_bool("leaves", false); // placed by a player: never decays
			if (!def.has("drops")) placed.set_string("drops", id);
			blocks.set(pid, placed);
			def = blocks.get(id); // (the table moved)
		}
		String shape = def.str("shape", "cube").tcopy();
		if (shape == "cube" || def.has("variant_of")) continue;
		if (is_not_solid_shape(shape)) def.set_bool("solid", false);
		def.set_bool("transparent", true);
		Vec{Variant} vs;
		defer
		{
			foreach (&v : vs) v.state.free();
			vs.free();
		}
		variants(def, &vs);
		def.set("state", vs[0].state.copy());
		Vec{Entry} made;
		defer made.free();
		foreach (&vr : vs)
		{
			if (vr.suffix.len == 0) continue;
			Data v = def.copy();
			String vid = string::tformat("%s%s", id, vr.suffix);
			v.set_string("id", vid);
			v.set_string("variant_of", id);
			v.set("state", vr.state.copy());
			if (!def.has("drops")) v.set_string("drops", id);
			if (shape == "door" && vr.state.str("half") == "upper" && def.has("textures_upper"))
			{
				v.set("textures", def.get("textures_upper").copy());
			}
			if (shape == "bed")
			{
				if (vr.state.str("part") == "head" && def.has("textures_head")) v.set("textures", def.get("textures_head").copy());
				v.set_long("texture_turns", facing_index(vr.state.str("facing"))); // top drawn for "n"
			}
			made.push({ vid, v });
		}
		foreach (&m : made) blocks.set(m.id, m.def);
	}
}

// A box in 1/16 units: x0, y0, z0, x1, y1, z1, texture offsets y, x, z
// (textured as if the box were that much lower / further along x, z).
alias Box16 = double[9];

// Quarter turns clockwise seen from above: north -> east -> south -> west.
fn Box16 rotate(Box16 b, int turns) @private
{
	for (int t = 0; t < turns; t++)
	{
		b = { 16 - b[5], b[1], b[0], 16 - b[2], b[4], b[3], b[6], -b[8], b[7] }; // (x, z) -> (16 - z, x)
	}
	return b;
}

fn void model_boxes(String shape, Data* state, Vec{Box16}* boxes) @private
{
	int turns = facing_index(state.str("facing", "n"));
	if (shape == "slab")
	{
		boxes.push(state.str("half") == "top" ? (Box16){ 0, 8, 0, 16, 16, 16, 0, 0, 0 } : (Box16){ 0, 0, 0, 16, 8, 16, 0, 0, 0 });
	}
	else if (shape == "stairs")
	{
		boxes.push({ 0, 0, 0, 16, 8, 16, 0, 0, 0 });
		boxes.push({ 0, 8, 0, 16, 16, 8, 0, 0, 0 }); // the high side towards the facing
	}
	else if (shape == "torch")
	{
		if (state.str("facing") == "floor")
		{
			boxes.push({ 7, 0, 7, 9, 10, 9, 0, 0, 0 });
			return;
		}
		boxes.push({ 7, 4, 0.5, 9, 14, 2.5, 4, 0, -6.5 }); // on a wall, raised against it (textured as the standing one)
	}
	else if (shape == "ladder")
	{
		boxes.push({ 0, 0, 0, 16, 16, 1, 0, 0, 0 });
	}
	else if (shape == "bed")
	{
		boxes.push({ 0, 0, 0, 16, 9, 16, 0, 0, 0 });
		return;
	}
	else if (shape == "door")
	{
		boxes.push({ 0, 0, 0, 16, 16, 3, 0, 0, 0 });
		if (state.flag("open")) turns += 3; // swung open against the side wall
	}
	foreach (&b : *boxes) *b = rotate(*b, turns >= 0 ? turns % 4 : 0);
}

fn Box to_box(Box16 b) @private
{
	return { .mn = { (float)(b[0] / 16.0), (float)(b[1] / 16.0), (float)(b[2] / 16.0) },
		.mx = { (float)(b[3] / 16.0), (float)(b[4] / 16.0), (float)(b[5] / 16.0) },
		.ty = (float)(b[6] / 16.0), .tx = (float)(b[7] / 16.0), .tz = (float)(b[8] / 16.0) };
}

// --- Dyes, names ---------------------------------------------------------------------

// Dyed blocks as items: the block's id and its dye, "core:stone#ff8800"
// (inventories, drops, the network and saves carry them as any other id).
// The block's own id, without the dye (a view of `id`):
fn String undyed(String id)
{
	if (try hash = id.index_of_char('#')) return id[:hash];
	return id;
}

// "#ff8800" or "ff8800".
fn bool parse_dye(String hex, Dye* out)
{
	sz start = hex.len > 0 && hex[0] == '#' ? 1 : 0;
	if (hex.len != start + 6) return false;
	uint v = 0;
	for (sz i = start; i < hex.len; i++)
	{
		char c = hex[i];
		int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
		if (d < 0) return false;
		v = v << 4 | (uint)d;
	}
	*out = 0xFF000000 | v;
	return true;
}

// voxel::NO_DYE: not dyed.
fn Dye dye_of(String id)
{
	Dye d = voxel::NO_DYE;
	if (try hash = id.index_of_char('#'))
	{
		if (parse_dye(id[hash..], &d)) return d;
	}
	return voxel::NO_DYE;
}

// "#ff8800" (in `allocator`).
fn String dye_hex(Allocator allocator, Dye dye) => string::format(allocator, "#%06x", dye & 0xFFFFFF);

// The id with this dye (voxel::NO_DYE: the id alone).
fn String dyed(Allocator allocator, String id, Dye dye)
{
	String base = undyed(id);
	return dye == voxel::NO_DYE ? base.copy(allocator) : string::format(allocator, "%s#%06x", base, dye & 0xFFFFFF);
}

// A texture pixel (sRGB numbers) of a dyed block, stained: the dye where the
// texture is as bright as `average_lum` (its tile's average, 0..1), lighter
// and darker as it is, with DYE_SHOW of its own colour showing through (as
// shaders/color.hlsli's apply_dye).
const float DYE_SHOW = 0.2f;

fn float pixel_lum(char* rgb) => (0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2]) / 255.0f;

fn void dye_pixel(char* rgb, float average_lum, Dye dye)
{
	if (dye == voxel::NO_DYE) return;
	float k = math::clamp(pixel_lum(rgb) / math::max(average_lum, 0.04f), 0.0f, 1.6f);
	int[3] d = { (int)(dye >> 16 & 255), (int)(dye >> 8 & 255), (int)(dye & 255) };
	for (int c = 0; c < 3; c++)
	{
		float stained = (float)d[c] * k * (1.0f - DYE_SHOW) + (float)rgb[c] * DYE_SHOW;
		rgb[c] = (char)math::clamp((int)(stained + 0.5f), 0, 255);
	}
}

// What players see an item or block called: its "name", else its id without
// "core:", words capitalised ("core:wood_pickaxe" -> "Wood Pickaxe"); a dyed
// block's has its colour after it.
fn String Content.display_name(&self, Allocator allocator, String dyed_id)
{
	Dye d = dye_of(dyed_id);
	if (d != voxel::NO_DYE)
	{
		@pool()
		{
			return string::format(allocator, "%s (#%06x)", self.display_name(tmem, undyed(dyed_id)), d & 0xFFFFFF);
		};
	}
	String id = undyed(dyed_id);
	foreach (t : (Table*[2]){ &self.items, &self.blocks })
	{
		Data* def = t.get(id);
		if (def && def.get("name") && def.get("name").is_string()) return def.str("name").copy(allocator);
	}
	if (try colon = id.index_of_char(':')) id = id[colon + 1..];
	DString out;
	out.init(allocator);
	bool word_start = true;
	foreach (c : id)
	{
		if (c == '_')
		{
			out.append_char(' ');
			word_start = true;
		}
		else
		{
			out.append_char(word_start && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
			word_start = false;
		}
	}
	return out.str_view();
}

// A string's djb2 hash (the ore salts come from it).
fn uint string_hash(String s)
{
	uint h = 5381;
	foreach (c : s) h = (h << 5) + h + c; // (ids are ASCII: bytes are code points)
	return h;
}

// --- Generator tables ----------------------------------------------------------------

struct IdValue @private
{
	String id;
	double value;
}

// A biome's "trees": a list of {id, value} pairs, in their order (it reaches
// the generator); a table keyed by id has no order of its own, so that form is
// taken in id order. (Views into `j`.)
fn void id_values(Data* j, Vec{IdValue}* out) @private
{
	if (!j) return;
	if (j.is_array())
	{
		foreach (&e : j.items())
		{
			if (e.is_array() && e.len() == 2 && e.at(0).is_string() && e.at(1).is_number()) out.push({ e.at(0).s, e.at(1).as_double() });
		}
	}
	else if (j.is_object())
	{
		foreach (&m : j.fields())
		{
			if (m.value.is_number()) out.push({ m.key, m.value.as_double() });
		}
		sort::quicksort(out.array_view(), fn int(IdValue a, IdValue b)
		{
			int c = compare_ids(a.id, b.id);
			return c != 0 ? c : (a.value < b.value ? -1 : (a.value > b.value ? 1 : 0));
		});
	}
}

fn BlockId Content.block(&self, String id)
{
	int i = self.block_index(id);
	return (BlockId)(i > 0 && i < self.block_ids_.len() ? i : 0);
}

// A number as an int: truncates.
fn int inum(Data* d, String key, int fallback) @private => (int)d.num(key, fallback);

fn int first_of(Data* list, int fallback) @private
{
	if (!list || list.is_empty() || !list.is_array()) return fallback;
	return (int)list.at(0).as_double();
}

fn int last_of(Data* list, int fallback) @private
{
	if (!list || list.is_empty() || !list.is_array()) return fallback;
	return (int)list.at(list.len() - 1).as_double();
}

// The trees the generator knows, by id, in its order (Generator.tree_blocks'
// kind): views of the ids.
fn void Content.tree_order(&self, Vec{String}* out)
{
	foreach (&e : self.trees.entries)
	{
		if (self.block_index(e.def.str("log")) > 0 && (self.block_index(e.def.str("leaves")) > 0 || e.def.str("style") == "cactus"))
		{
			out.push(e.id);
		}
	}
}

// Sets up `generator` for a world of this seed.
fn void Content.configure_generator(&self, Generator* generator, uint seed)
{
	GeneratorIds ids = {
		.stone = self.block("core:stone"), .dirt = self.block("core:dirt"), .grass = self.block("core:grass"),
		.sand = self.block("core:sand"), .water = self.block("core:water"), .lava = self.block("core:lava"),
		.ice = self.block("core:ice"), .gravel = self.block("core:gravel") };

	// Ores, sorted by id; each one's veins are keyed to its id (the salt), so
	// adding content doesn't move existing veins.
	Vec{OreKind} ores;
	defer ores.free();
	foreach (&e : self.ores.entries)
	{
		Data* def = &e.def;
		OreKind k;
		k.id = self.block(def.str("block"));
		k.replaces = self.block(def.str("replaces", "core:stone"));
		if (k.id == 0 || k.replaces == 0) continue; // unknown block: not generated
		k.min_y = inum(def, "min_y", -96);
		k.max_y = inum(def, "max_y", 0);
		k.veins_per_chunk = math::clamp(inum(def, "veins_per_chunk", 4), 0, 256);
		Data* size = def.get("size");
		if (!size) size = &&(Data){}; // (null: the defaults)
		k.min_size = math::max(1, size.is_null() ? 3 : first_of(size, 3));
		k.max_size = math::max(k.min_size, size.is_null() ? 6 : last_of(size, 6));
		k.salt = string_hash(e.id) & 0x7fffffff;
		ores.push(k);
	}

	// Trees with a known log and leaves (or a cactus), sorted by id: biomes
	// refer to them by index in this order.
	Vec{String} order;
	defer order.free();
	self.tree_order(&order);
	Vec{TreeKind} trees;
	defer trees.free();
	foreach (id : order)
	{
		Data* def = self.trees.get(id);
		TreeKind t;
		t.log = self.block(def.str("log"));
		t.leaves = self.block(def.str("leaves"));
		Data* trunk = def.get("trunk");
		t.min_trunk = math::clamp(trunk ? first_of(trunk, 4) : 4, 2, 20);
		t.max_trunk = math::clamp(trunk ? last_of(trunk, 6) : 6, t.min_trunk, 20);
		String style = def.str("style", "oak");
		t.style = style == "big_oak" ? BIG_OAK : style == "cactus" ? CACTUS : ROUND_OAK;
		trees.push(t);
	}

	Vec{BiomeKind} biomes;
	defer
	{
		foreach (&b : biomes) b.trees.free();
		biomes.free();
	}
	foreach (&e : self.biomes.entries)
	{
		Data* def = &e.def;
		BiomeKind b = voxel::biome_kind();
		b.temperature = (float)def.num("temperature", 0.0);
		b.humidity = (float)def.num("humidity", 0.0);
		b.height = (float)def.num("height", 20.0);
		b.base = (float)def.num("base", 0.0);
		b.surface = self.block(def.str("surface"));
		b.filler = self.block(def.str("filler"));
		b.frozen = def.flag("frozen");
		b.mountains = math::clamp((float)def.num("mountains", 0.0), 0.0f, 60.0f);
		b.rock = self.block(def.str("rock"));
		b.rock_above = inum(def, "rock_above", 1 << 20);
		b.cap = self.block(def.str("cap"));
		b.cap_above = inum(def, "cap_above", 1 << 20);
		b.cover = self.block(def.str("cover"));
		b.cover_above = inum(def, "cover_above", 1 << 20);
		Vec{IdValue} tv;
		defer tv.free();
		id_values(def.get("trees"), &tv);
		foreach (t : tv)
		{
			foreach (i, o : order)
			{
				if (o == t.id)
				{
					b.trees.push({ (int)i, (int)t.value });
					break;
				}
			}
		}
		if (b.surface && b.filler)
		{
			biomes.push(b);
		}
		else
		{
			b.trees.free();
		}
	}
	generator.set_biomes(biomes.array_view());
	generator.configure(seed, ids, ores.array_view(), trees.array_view());
}

// --- Block table and atlas -------------------------------------------------------------

// Block textures packed into one atlas: each 32x32 tile fills a CELL x CELL
// area with copies (faces use the one at FACE_X, FACE_Y), so mipmaps never mix
// neighbouring tiles.
const int TILE = 32;
const int CELL = 4;
const int FACE_X = 1;
const int FACE_Y = 1;

struct Atlas
{
	AtlasLayout layout;
	int width, height;
	char[] rgba;        // width * height * 4, mip level 0
	Vec{char[]} tiles;  // TILE * TILE * 4 each
}

// A tile's uv rectangle (u0, v0, u1, v1): the copy faces use inside its cell.
fn void Atlas.tile_uv(&self, int tile, float[4]* uv)
{
	float cw = 1.0f / (float)self.layout.cols;
	float ch = 1.0f / (float)self.layout.rows;
	(*uv)[0] = ((float)(tile % self.layout.cols) + (float)self.layout.face_x / (float)self.layout.cell) * cw;
	(*uv)[1] = ((float)(tile / self.layout.cols) + (float)self.layout.face_y / (float)self.layout.cell) * ch;
	(*uv)[2] = (*uv)[0] + cw / (float)self.layout.cell;
	(*uv)[3] = (*uv)[1] + ch / (float)self.layout.cell;
}

fn void Atlas.free(&self)
{
	if (self.rgba.ptr) free(self.rgba.ptr);
	foreach (t : self.tiles) free(t.ptr);
	self.tiles.free();
	*self = {};
}

struct BlockTable
{
	Vec{BlockInfo} blocks; // index = block id
	Atlas atlas;
	// Atlas tile of every item's and placeable block's icon (its "icon" or
	// "texture" image, a block's side face, else its colour). Keys: owned.
	Map{String, int} icons;
}

fn int BlockTable.icon(&self, String id) => self.icons.get_or(id, 0);

fn void BlockTable.free(&self)
{
	foreach (&b : self.blocks) b.free();
	self.blocks.free();
	self.atlas.free();
	self.icons.@each(; String k, int v) { free(k.ptr); };
	self.icons.free();
}

struct AtlasBuilder @private
{
	Content* content;
	Atlas* atlas;
	Map{String, int} index; // texture path (+ turns) or "#rrggbbaa" -> tile (keys owned)
}

fn int AtlasBuilder.add(&self, String key, char[] px)
{
	self.atlas.tiles.push(px);
	int t = (int)self.atlas.tiles.len() - 1;
	self.index[key.copy(mem)] = t;
	return t;
}

fn char[] flat(char[4] c) @private
{
	char[] px = mem::alloc_array(char, TILE * TILE * 4);
	for (int i = 0; i < TILE * TILE; i++) px[i * 4 : 4] = c[..];
	return px;
}

// A PNG, resized to TILE x TILE (nearest neighbour); null if it can't be read.
fn char[] load_image(String path) @private
{
	Image im;
	defer im.free();
	if (!image::read_png(path, &im)) return {};
	sz w = im.width;
	sz h = im.height;
	char[] out = mem::alloc_array(char, TILE * TILE * 4);
	for (sz y = 0; y < TILE; y++)
	{
		for (sz x = 0; x < TILE; x++)
		{
			sz sx = x * w / TILE;
			sz sy = y * h / TILE;
			out[(y * TILE + x) * 4 : 4] = im.rgba[(sy * w + sx) * 4 : 4];
		}
	}
	return out;
}

// Turned a quarter clockwise (in place).
fn void rotate_cw(char[] px) @private => @pool()
{
	char[] src = mem::talloc_array(char, px.len);
	src[..] = px[..];
	for (sz y = 0; y < TILE; y++)
	{
		for (sz x = 0; x < TILE; x++) // destination (x, y) <- source (y, T - 1 - x)
		{
			px[(y * TILE + x) * 4 : 4] = src[((TILE - 1 - x) * TILE + y) * 4 : 4];
		}
	}
}

// A tile from an image (key: its path), or -1 if it can't be read.
fn int AtlasBuilder.image_tile(&self, Data* def, String rel) => @pool()
{
	String full = self.content.asset_path(rel);
	if (try t = self.index.get(full)) return t;
	char[] px = full.len ? load_image(full) : (char[]){};
	if (!px.ptr)
	{
		io::eprintfn("Image not found: %s (%s)", rel, def.str("id"));
		return -1;
	}
	return self.add(full, px);
}

fn String color_key(char[4] c) @private => string::tformat("#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);

// A flat tile of the definition's "color" (else `fallback`).
fn int AtlasBuilder.color_tile(&self, Data* def, String fallback) => @pool()
{
	char[4] c = { 255, 0, 255, 255 };
	image::parse_html_color(def.has("color") ? def.str("color") : fallback, &c);
	String key = color_key(c);
	if (try t = self.index.get(key)) return t;
	return self.add(key, flat(c));
}

// One face's tile: its texture ("side"/"top"/"bottom", else "all": an image,
// or a table to paint it from), else the block's colour, else magenta.
fn int AtlasBuilder.face_tile(&self, Data* def, String face, int turns) => @pool()
{
	Data* textures = def.get("textures");
	Data* rel = null;
	if (textures && textures.is_object()) rel = textures.has(face) ? textures.get(face) : textures.get("all");
	turns = turns % 4;
	String turned = turns ? string::tformat("@%d", turns) : "";
	if (rel && rel.is_object())
	{
		DString key;
		key.tinit();
		key.append("paint:");
		json::write(&key, rel, true);
		key.append(turned);
		if (try t = self.index.get(key.str_view())) return t;
		Image painted = image::paint_face(rel, TILE);
		for (int t = 0; t < turns; t++) rotate_cw(painted.rgba);
		return self.add(key.str_view(), painted.rgba);
	}
	if (rel && rel.is_string())
	{
		String full = self.content.asset_path(rel.s);
		String key = full.tconcat(turned);
		if (try t = self.index.get(key)) return t;
		char[] px = full.len ? load_image(full) : (char[]){};
		if (px.ptr)
		{
			for (int t = 0; t < turns; t++) rotate_cw(px);
			return self.add(key, px);
		}
		io::eprintfn("Block texture not found: %s (%s)", rel.s, def.str("id"));
	}
	char[4] c = { 255, 0, 255, 255 };
	if (def.has("color")) image::parse_html_color(def.str("color"), &c);
	String key = color_key(c);
	if (try t = self.index.get(key)) return t;
	return self.add(key, flat(c));
}

fn void AtlasBuilder.build(&self)
{
	Atlas* atlas = self.atlas;
	if (atlas.tiles.is_empty()) self.add("#missing", flat({ 255, 0, 255, 255 }));
	int cols = 1;
	while (cols * cols < (int)atlas.tiles.len()) cols *= 2;
	int rows = ((int)atlas.tiles.len() + cols - 1) / cols;
	int cell = TILE * CELL;
	atlas.layout = { cols, rows, CELL, FACE_X, FACE_Y };
	atlas.width = cols * cell;
	atlas.height = rows * cell;
	atlas.rgba = mem::new_array(char, (sz)atlas.width * atlas.height * 4);
	foreach (i, tile : atlas.tiles)
	{
		int cx = (int)i % cols * cell;
		int cy = (int)i / cols * cell;
		for (int ry = 0; ry < CELL; ry++)
		{
			for (int rx = 0; rx < CELL; rx++)
			{
				for (int y = 0; y < TILE; y++)
				{
					sz dst = ((sz)(cy + ry * TILE + y) * (sz)atlas.width + (sz)(cx + rx * TILE)) * 4;
					atlas.rgba[dst : TILE * 4] = tile[(sz)y * TILE * 4 : TILE * 4];
				}
			}
		}
	}
}

fn void AtlasBuilder.free(&self)
{
	self.index.@each(; String k, int v) { free(k.ptr); };
	self.index.free();
}

fn bool has_alpha(char[] px) @private
{
	for (sz i = 3; i < px.len; i += 4)
	{
		if (px[i] != 255) return true;
	}
	return false;
}

// The engine's shape for a definition's "shape" (-1: drawn as a cube).
fn int engine_shape(String shape) @private
{
	switch (shape)
	{
		case "boxes":
		case "slab":
		case "stairs":
		case "torch":
		case "ladder":
		case "door":
		case "bed":
			return Shape.BOXES.ordinal;
		case "fence":
			return Shape.FENCE.ordinal;
		case "cross":
			return Shape.CROSS.ordinal;
		default:
			return -1;
	}
}

fn BlockTable Content.build_block_table(&self)
{
	BlockTable table;
	AtlasBuilder atlas = { .content = self, .atlas = &table.atlas };
	defer atlas.free();
	Data empty = data::object();
	defer empty.free();
	foreach (id : self.block_ids_)
	{
		Data* def = self.blocks.get(id);
		if (!def) def = &empty;
		BlockInfo b = voxel::block_info();
		b.fluid = def.has("fluid");
		b.solid = def.flag("solid", true) && !b.fluid;
		b.opaque = b.solid && !def.flag("transparent");
		String shape = def.str("shape", "cube");
		if (!(id == "core:air" || (!b.solid && !b.fluid && shape == "cube")))
		{
			String[6] faces = { "side", "side", "top", "bottom", "side", "side" }; // +X -X +Y -Y +Z -Z
			for (int f = 0; f < 6; f++) b.tiles[f] = atlas.face_tile(def, faces[f], f == 2 ? inum(def, "texture_turns", 0) : 0);
		}
		foreach (t : b.tiles)
		{
			b.cutout = b.cutout || (t < (int)table.atlas.tiles.len() && has_alpha(table.atlas.tiles[t]));
		}
		b.emit = (char)math::clamp(inum(def, "light", 0), 0, voxel::LIGHT_MAX);
		b.fluid_level = (char)math::clamp(inum(def, "level", 8), 1, 8);
		b.random_rotation = def.flag("random_rotation");
		b.tint = def.flag("tint");

		bool has_collision = false;
		int es = engine_shape(shape);
		if (es == Shape.BOXES.ordinal)
		{
			Vec{Box16} boxes;
			defer boxes.free();
			if (shape == "boxes")
			{
				Data* list = def.get("boxes");
				if (list)
				{
					foreach (&bx : list.items())
					{
						Box16 box;
						for (sz k = 0; k < 6 && k < bx.len(); k++) box[k] = bx.at(k) ? bx.at(k).as_double() : 0;
						boxes.push(box);
					}
				}
			}
			else
			{
				Data* state = def.get("state");
				Data none = data::object();
				defer none.free();
				model_boxes(shape, state ? state : &none, &boxes);
			}
			b.shape = BOXES;
			foreach (bx : boxes) b.model.push(to_box(bx));
			// Boxes (1-based) that keep their own colours when the block is dyed.
			Data* undyed_boxes = def.get("undyed_boxes");
			if (undyed_boxes)
			{
				foreach (&k : undyed_boxes.items())
				{
					if (k.is_number() && k.as_double() >= 1 && (sz)k.as_double() <= b.model.len()) b.model[(sz)k.as_double() - 1].undyed = true;
				}
			}
			if (!is_not_solid_shape(shape)) b.collision.push_all(b.model.array_view());
			has_collision = true;
		}
		else if (es == Shape.CROSS.ordinal)
		{
			b.shape = CROSS;
			has_collision = true; // none
		}
		else if (es == Shape.FENCE.ordinal)
		{
			b.shape = FENCE;
		}
		if (!has_collision && b.solid) b.collision.push({ .mn = { 0, 0, 0 }, .mx = { 1, 1, 1 } });
		// Its icon: an "icon" image, else its side face.
		int icon = -1;
		if (def.get("icon") && def.get("icon").is_string()) icon = atlas.image_tile(def, def.str("icon"));
		if (icon < 0) icon = b.tiles[0] ? b.tiles[0] : atlas.color_tile(def, "#ff00ff");
		table.icons[id.copy(mem)] = icon;
		table.blocks.push(b);
	}
	foreach (&e : self.items.entries)
	{
		int t = -1;
		if (e.def.get("texture") && e.def.get("texture").is_string()) t = atlas.image_tile(&e.def, e.def.str("texture"));
		int* old = table.icons.get_ref(e.id);
		if (old)
		{
			*old = t >= 0 ? t : atlas.color_tile(&e.def, "#b0a080");
		}
		else
		{
			table.icons[e.id.copy(mem)] = t >= 0 ? t : atlas.color_tile(&e.def, "#b0a080");
		}
	}
	atlas.build();
	return table;
}
