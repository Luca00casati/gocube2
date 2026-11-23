// Saving and loading edited chunks: one small file per chunk, run-length
// encoded block ids. Safe to call from worker threads for different files.
// (the same format, so worlds carry over.)
module gocube::voxel;
import gocube::util::vec, std::io, std::sort, gocube::fs;

alias Bytes = Vec{char};

// File layout (little-endian):
//   "GCC1"                     magic + format version
//   u8   uniform               1 = every block is `value`
//   u16  value                 (uniform only)
//   u32  run_count             (non-uniform only)
//   run_count x { u16 length, u16 id }   runs over chunk_index order
// "GCC2" (from when blocks could be dyed) is read too: after the runs,
//   u32  dye_count, dye_count x { u16 index, u8 r, u8 g, u8 b }, skipped
const char[4] CHUNK_MAGIC @private = { 'G', 'C', 'C', '1' };
const char[4] CHUNK_MAGIC_DYED @private = { 'G', 'C', 'C', '2' };

fn String chunk_file_path(Allocator allocator, String dir, int cx, int cy, int cz)
{
	return string::format(allocator, "%s/c.%d.%d.%d.bin", dir, cx, cy, cz);
}

fn void put16(Bytes* out, uint v) @inline
{
	out.push((char)(v & 0xFF));
	out.push((char)((v >> 8) & 0xFF));
}

fn void put32(Bytes* out, uint v) @inline
{
	put16(out, v & 0xFFFF);
	put16(out, v >> 16);
}

// Appends the chunk, encoded, to out.
fn void encode_chunk(ChunkData* chunk, Bytes* out)
{
	out.push_all(&CHUNK_MAGIC);
	out.push(chunk.mixed ? 0 : 1);
	if (!chunk.mixed)
	{
		put16(out, chunk.uniform_value);
		return;
	}
	Vec{ushort} runs; // pairs: length, id
	defer runs.free();
	ushort current = chunk.blocks[0];
	ushort length = 0;
	foreach (b : chunk.blocks)
	{
		if (b == current && length < 0xFFFF)
		{
			length++;
		}
		else
		{
			runs.push(length);
			runs.push(current);
			current = b;
			length = 1;
		}
	}
	runs.push(length);
	runs.push(current);
	put32(out, (uint)(runs.len() / 2));
	foreach (v : runs) put16(out, v);
}

struct Reader @private
{
	char[] data;
	sz pos;
}

fn bool Reader.get16(&self, ushort* v)
{
	if (self.pos + 2 > self.data.len) return false;
	*v = (ushort)(self.data[self.pos] | (ushort)self.data[self.pos + 1] << 8);
	self.pos += 2;
	return true;
}

fn bool Reader.get32(&self, uint* v)
{
	ushort lo;
	ushort hi;
	if (!self.get16(&lo) || !self.get16(&hi)) return false;
	*v = lo | (uint)hi << 16;
	return true;
}

// Decodes into `chunk` (replacing it); false (chunk untouched) if the data isn't a chunk.
fn bool decode_chunk(char[] data, ChunkData* chunk)
{
	bool dyed = data.len >= 4 && data[:4] == CHUNK_MAGIC_DYED[..];
	if (data.len < 5 || (!dyed && data[:4] != CHUNK_MAGIC[..])) return false;
	Reader r = { data, 4 };
	ChunkData result;
	char uniform = data[r.pos++];
	if (uniform)
	{
		ushort value;
		if (!r.get16(&value)) return false;
		result.uniform_value = value;
	}
	else
	{
		uint count;
		if (!r.get32(&count) || count == 0 || count > CHUNK_VOLUME) return false;
		result.blocks = mem::alloc_array(BlockId, CHUNK_VOLUME);
		result.mixed = true;
		sz filled = 0;
		for (uint i = 0; i < count; i++)
		{
			ushort length;
			ushort id;
			if (!r.get16(&length) || !r.get16(&id) || filled + length > CHUNK_VOLUME)
			{
				result.free();
				return false;
			}
			result.blocks[filled : length] = id;
			filled += length;
		}
		if (filled != CHUNK_VOLUME)
		{
			result.free();
			return false;
		}
	}
	if (dyed) // dyed blocks (the dye vat's, from before dyeing went): skipped
	{
		uint count;
		if (!r.get32(&count) || count > CHUNK_VOLUME || data.len - r.pos < (sz)count * 5)
		{
			result.free();
			return false;
		}
		r.pos += (sz)count * 5;
	}
	if (r.pos != data.len)
	{
		result.free();
		return false;
	}
	chunk.free();
	*chunk = result;
	return true;
}

// Writes atomically (temp file + rename). Returns false on I/O error.
fn bool save_chunk_file(String path, ChunkData* chunk)
{
	Bytes bytes;
	defer bytes.free();
	encode_chunk(chunk, &bytes);
	return fs::save_atomically(path, bytes.array_view());
}

// Returns false if the file is missing or invalid (then `chunk` is untouched).
fn bool load_chunk_file(String path, ChunkData* chunk) => @pool()
{
	char[]? bytes = file::load_temp(path);
	if (catch bytes) return false;
	return decode_chunk(bytes, chunk);
}

// Chunk positions of every chunk file in `dir`.
fn void list_chunk_files(String dir, Vec{int[3]}* out) => @pool()
{
	Path? p = path::tnew(dir);
	if (catch p) return;
	PathList? files = path::ls(tmem, p, no_dirs: true);
	if (catch files) return;
	foreach (f : files)
	{
		String name = f.basename();
		if (!name.starts_with("c.") || !name.ends_with(".bin")) continue;
		String[] parts = name[2 .. ^5].tsplit(".");
		if (parts.len != 3) continue;
		int? x = parts[0].to_int();
		int? y = parts[1].to_int();
		int? z = parts[2].to_int();
		if (try x && try y && try z) out.push({ x, y, z });
	}
}
