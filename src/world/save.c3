// A world's folder:
//   world.json   JSON (data/json.c3): seed, game mode, the block id table,
//                the player and everything else the game keeps (block
//                entities, growing plants...)
//   chunks/      one file per edited chunk (unedited chunks are generated again)
// Fields this version doesn't use are kept as they are, so a world opened and
// saved loses nothing. (the same files.)
module gocube::world::save;
import gocube::data, gocube::data::json, gocube::log, gocube::win, gocube::util::vec, std::io;

const int FORMAT_VERSION = 3;

struct WorldSave
{
	String dir; // owned
	Data meta;
}

fn void WorldSave.free(&self)
{
	if (self.dir.ptr) free(self.dir.ptr);
	self.meta.free();
	*self = {};
}

// Opens (or, if it has no world.json, starts) the world in `dir`. New worlds
// get `new_seed`. "" or why it can't be opened (a damaged world.json, one from
// a newer version) (a constant string).
fn String WorldSave.open(&self, String dir, uint new_seed, String new_mode = "creative") => @pool()
{
	self.free();
	self.dir = dir.copy(mem);
	String path = string::tformat("%s/world.json", dir);
	if (file::exists(path))
	{
		DString why;
		why.tinit();
		// A world that can't be read safely is left alone.
		if (!json::read_file(path, &self.meta, &why) || !self.meta.is_object() || !self.meta.get("seed") || !self.meta.get("seed").is_number())
		{
			self.meta.free();
			self.meta = data::object();
			return "its world.json is damaged, so it isn't opened (to keep it safe).";
		}
		if (self.meta.num("format", 1) < FORMAT_VERSION)
		{
			// (Formats 1 and 2: the Godot version's worlds, whose terrain was
			// generated differently.)
			return "it was made by an old version of GoCube that this one can't open.";
		}
		if (self.meta.num("format", FORMAT_VERSION) > FORMAT_VERSION)
		{
			return "it was saved by a newer version of GoCube. Update the game to play it.";
		}
		if (!self.meta.has("mode")) self.meta.set_string("mode", "creative");
	}
	else
	{
		self.meta = data::object();
		self.meta.set_long("seed", new_seed);
		self.meta.set_string("mode", new_mode);
		self.meta.set_string("created", win::now_string());
	}
	self.meta.set_long("format", FORMAT_VERSION);
	if (catch path::mkdir(path::tnew(self.chunks_dir(tmem))!!, true))
	{
		if (!path::is_dir(path::tnew(self.chunks_dir(tmem))!!)) return "its folder can't be written";
	}
	return "";
}

fn String WorldSave.chunks_dir(&self, Allocator allocator) => string::format(allocator, "%s/chunks", self.dir);
fn uint WorldSave.seed(&self) => (uint)(long)self.meta.num("seed", 0);
fn String WorldSave.mode(&self) => self.meta.str("mode", "creative");

// The world's own block id table (empty for a new world): numeric ids in its
// chunk files mean these blocks. (Views into world.json's data.)
fn void WorldSave.block_ids(&self, Vec{String}* out)
{
	Data* ids = self.meta.get("block_ids");
	if (!ids) return;
	foreach (&id : ids.items())
	{
		if (id.is_string()) out.push(id.s);
	}
}

// After the content is frozen with block_ids(): the table now in use (the
// saved one plus any new blocks), written at once (before any chunk can be
// saved with these numbers).
fn void WorldSave.set_block_ids(&self, String[] ids)
{
	Data list = data::array();
	foreach (id : ids) list.push_string(id);
	self.meta.set("block_ids", list);
	self.write_meta();
}

// The saved player ("position" is the feet; "rotation" is [pitch, yaw]), or null.
fn Data* WorldSave.player(&self)
{
	Data* p = self.meta.get("player");
	return p && p.is_object() ? p : null;
}

// Another field of world.json (null if it has none), and setting one
// (written with the next save; taken over): "block_entities", "plants", "time"...
fn Data* WorldSave.field(&self, String key) => self.meta.get(key);
fn void WorldSave.set_field(&self, String key, Data value) => self.meta.set(key, value);

// Writes world.json (atomically); `player` (if not null) replaces the saved
// player's fields it has, keeping the others (inventory, health...).
fn bool WorldSave.save(&self, Data* player = null) => @pool()
{
	if (player && !player.is_empty())
	{
		Data* saved = self.meta.get("player");
		if (!saved || !saved.is_object())
		{
			self.meta.set("player", data::object());
			saved = self.meta.get("player");
		}
		foreach (&f : player.fields()) saved.set(f.key, f.value.copy());
	}
	self.meta.set_string("saved", win::now_string());
	return self.write_meta();
}

// To a temporary file, then renamed over the old one: a crash mid-write can't
// leave a broken world.json.
fn bool WorldSave.write_meta(&self) => @pool()
{
	String path = string::tformat("%s/world.json", self.dir);
	if (!json::save_file(path, &self.meta))
	{
		log::log("Can't save %s", path);
		return false;
	}
	return true;
}
