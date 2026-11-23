// Block and item images: the few drawn ones are PNG files (read and written
// by our own code: inflate, filters, interlacing; any colour type and depth,
// as 8-bit RGBA); every other face is painted here from a colour and some
// noise, as a block's definition describes it (its `textures` face given as
// a table).
module gocube::content::image;
import gocube::data, gocube::data::json, gocube::util::vec, std::io, std::math;

// An HTML colour: "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa" ('#' optional).
fn bool parse_html_color(String s, char[4]* out)
{
	if (s.len > 0 && s[0] == '#') s = s[1..];
	foreach (c : s)
	{
		if (hex(c) < 0) return false;
	}
	if (s.len == 3 || s.len == 4)
	{
		(*out)[3] = 255;
		foreach (i, c : s) (*out)[i] = (char)(hex(c) * 17);
		return true;
	}
	if (s.len == 6 || s.len == 8)
	{
		(*out)[3] = 255;
		for (sz i = 0; i < s.len / 2; i++) (*out)[i] = (char)(hex(s[2 * i]) * 16 + hex(s[2 * i + 1]));
		return true;
	}
	return false;
}

fn int hex(char c) @private
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// An image as 8-bit RGBA (not premultiplied), width x height. free() it.
struct Image
{
	int width, height;
	char[] rgba;
}

fn void Image.free(&self)
{
	if (self.rgba.ptr) free(self.rgba.ptr);
	*self = {};
}

fn char* Image.at(&self, int x, int y) @inline => &self.rgba[((sz)y * self.width + x) * 4];

// --- Inflate (RFC 1951) ------------------------------------------------------------

struct Bits @private
{
	char[] in;
	sz pos;
	uint bit_buf;
	int bit_count;
	bool overrun;
}

fn uint Bits.get(&self, int n)
{
	while (self.bit_count < n)
	{
		uint b = 0;
		if (self.pos < self.in.len)
		{
			b = self.in[self.pos++];
		}
		else
		{
			self.overrun = true;
		}
		self.bit_buf |= b << self.bit_count;
		self.bit_count += 8;
	}
	uint v = self.bit_buf & ((1u << n) - 1);
	self.bit_buf >>= n;
	self.bit_count -= n;
	return v;
}

// A canonical Huffman code: counts of codes per length, symbols in code order.
struct Huffman @private
{
	short[16] counts;
	short[288] symbols;
}

fn bool Huffman.build(&self, char[] lengths)
{
	self.counts[..] = 0;
	foreach (l : lengths) self.counts[l]++;
	self.counts[0] = 0;
	int left = 1;
	for (int len = 1; len < 16; len++)
	{
		left <<= 1;
		left -= self.counts[len];
		if (left < 0) return false; // over-subscribed
	}
	short[16] offs;
	for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + self.counts[len];
	foreach (sym, l : lengths)
	{
		if (l) self.symbols[offs[l]++] = (short)sym;
	}
	return true;
}

fn int Huffman.decode(&self, Bits* b)
{
	int code = 0;
	int first = 0;
	int index = 0;
	for (int len = 1; len < 16; len++)
	{
		code |= (int)b.get(1);
		int count = self.counts[len];
		if (code - count < first) return self.symbols[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
		if (b.overrun) return -1;
	}
	return -1;
}

const short[29] LEN_BASE @private = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const char[29] LEN_EXTRA @private = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const short[30] DIST_BASE @private = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const char[30] DIST_EXTRA @private = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

fn bool inflate_codes(Bits* b, Vec{char}* out, Huffman* lens, Huffman* dists, sz limit) @private
{
	while (true)
	{
		int sym = lens.decode(b);
		if (sym < 0 || b.overrun) return false;
		if (sym < 256)
		{
			out.push((char)sym);
		}
		else if (sym == 256)
		{
			return true;
		}
		else
		{
			sym -= 257;
			if (sym >= 29) return false;
			sz len = (sz)LEN_BASE[sym] + (sz)b.get(LEN_EXTRA[sym]);
			int d = dists.decode(b);
			if (d < 0 || d >= 30) return false;
			sz dist = (sz)DIST_BASE[d] + (sz)b.get(DIST_EXTRA[d]);
			if (dist > out.len()) return false;
			for (sz i = 0; i < len; i++) out.push((*out)[out.len() - dist]);
		}
		if (out.len() > limit) return false;
	}
}

// Raw deflate data, appended to `out` (at most `limit` bytes); false if it's broken.
fn bool inflate(char[] data, Vec{char}* out, sz limit)
{
	Bits b = { .in = data };
	Huffman fixed_lens;
	Huffman fixed_dists;
	char[288] l;
	l[0:144] = 8;
	l[144:112] = 9;
	l[256:24] = 7;
	l[280:8] = 8;
	fixed_lens.build(l[:288]);
	l[0:30] = 5;
	fixed_dists.build(l[:30]);
	bool last = false;
	while (!last)
	{
		last = b.get(1) == 1;
		uint type = b.get(2);
		switch (type)
		{
			case 0: // stored
				b.bit_buf = 0;
				b.bit_count = 0;
				if (b.pos + 4 > data.len) return false;
				uint len = data[b.pos] | (uint)data[b.pos + 1] << 8;
				uint nlen = data[b.pos + 2] | (uint)data[b.pos + 3] << 8;
				b.pos += 4;
				if ((len ^ 0xFFFF) != nlen || b.pos + len > data.len) return false;
				out.push_all(data[b.pos : len]);
				b.pos += len;
				if (out.len() > limit) return false;
			case 1:
				if (!inflate_codes(&b, out, &fixed_lens, &fixed_dists, limit)) return false;
			case 2:
				int hlit = (int)b.get(5) + 257;
				int hdist = (int)b.get(5) + 1;
				int hclen = (int)b.get(4) + 4;
				const char[19] ORDER = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
				char[19] cl;
				for (int i = 0; i < hclen; i++) cl[ORDER[i]] = (char)b.get(3);
				Huffman code_lens;
				if (!code_lens.build(&cl)) return false;
				char[320] lengths;
				int n = 0;
				while (n < hlit + hdist)
				{
					int sym = code_lens.decode(&b);
					if (sym < 0) return false;
					if (sym < 16)
					{
						lengths[n++] = (char)sym;
						continue;
					}
					char value = 0;
					int repeat;
					switch (sym)
					{
						case 16:
							if (n == 0) return false;
							value = lengths[n - 1];
							repeat = 3 + (int)b.get(2);
						case 17:
							repeat = 3 + (int)b.get(3);
						default:
							repeat = 11 + (int)b.get(7);
					}
					if (n + repeat > hlit + hdist) return false;
					for (int i = 0; i < repeat; i++) lengths[n++] = value;
				}
				Huffman lens;
				Huffman dists;
				if (!lens.build(lengths[:hlit]) || !dists.build(lengths[hlit:hdist])) return false;
				if (!inflate_codes(&b, out, &lens, &dists, limit)) return false;
			default:
				return false;
		}
		if (b.overrun) return false;
	}
	return true;
}

// --- PNG ---------------------------------------------------------------------------

const char[8] PNG_SIGNATURE @private = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
const int MAX_SIDE = 4096;

uint[256] crc_table @private;

fn void make_crc_table() @init @private
{
	for (uint n = 0; n < 256; n++)
	{
		uint c = n;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

fn uint crc32(uint crc, char[] data)
{
	crc = ~crc;
	foreach (b : data) crc = crc_table[(crc ^ b) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

fn uint be32(char[] b, sz at) @private => (uint)b[at] << 24 | (uint)b[at + 1] << 16 | (uint)b[at + 2] << 8 | b[at + 3];

fn int paeth(int a, int b, int c) @private
{
	int p = a + b - c;
	int pa = math::abs(p - a);
	int pb = math::abs(p - b);
	int pc = math::abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

// Undoes the filters of one (sub)image's rows in place; false if a filter is unknown.
fn bool unfilter(char[] data, sz rows, sz stride, int bpp) @private
{
	char* prev = null;
	for (sz r = 0; r < rows; r++)
	{
		char* row = &data[r * (stride + 1)];
		char f = row[0];
		char* cur = row + 1;
		for (sz i = 0; i < stride; i++)
		{
			int a = i >= bpp ? cur[i - bpp] : 0;
			int b = prev ? prev[i] : 0;
			int c = prev && i >= bpp ? prev[i - bpp] : 0;
			int add;
			switch (f)
			{
				case 0: add = 0;
				case 1: add = a;
				case 2: add = b;
				case 3: add = (a + b) / 2;
				case 4: add = paeth(a, b, c);
				default: return false;
			}
			cur[i] = (char)(cur[i] + add);
		}
		prev = cur;
	}
	return true;
}

// Reads a PNG (up to MAX_SIDE x MAX_SIDE): false if it isn't one (or is broken).
fn bool parse_png(char[] b, Image* out)
{
	if (b.len < 8 || b.len > 64 * 1024 * 1024 || b[:8] != PNG_SIGNATURE[..]) return false;
	sz pos = 8;
	uint w = 0;
	uint h = 0;
	int depth = 0;
	int type = 0;
	int interlace = 0;
	bool seen_header = false;
	bool seen_end = false;
	char[4][256] palette;
	for (int i = 0; i < 256; i++) palette[i] = { 0, 0, 0, 255 };
	bool has_trns = false;
	int[3] trns; // gray or rgb that is transparent
	Vec{char} idat;
	defer idat.free();
	while (pos + 12 <= b.len)
	{
		uint len = be32(b, pos);
		if ((sz)len > b.len || pos + 12 + (sz)len > b.len) return false;
		char[] kind = b[pos + 4 : 4];
		char[] body = b[pos + 8 : (sz)len];
		if (crc32(0, b[pos + 4 : (sz)len + 4]) != be32(b, pos + 8 + (sz)len)) return false;
		pos += 12 + (sz)len;
		if (kind == "IHDR")
		{
			if (len != 13) return false;
			w = be32(body, 0);
			h = be32(body, 4);
			depth = body[8];
			type = body[9];
			interlace = body[12];
			if (body[10] != 0 || body[11] != 0 || interlace > 1) return false;
			seen_header = true;
		}
		else if (kind == "PLTE")
		{
			for (uint i = 0; i < len / 3 && i < 256; i++) palette[i] = { body[i * 3], body[i * 3 + 1], body[i * 3 + 2], 255 };
		}
		else if (kind == "tRNS")
		{
			if (type == 3)
			{
				for (uint i = 0; i < len && i < 256; i++) palette[i][3] = body[i];
			}
			else if (type == 0 && len >= 2)
			{
				has_trns = true;
				trns[0] = (int)(body[0] << 8 | body[1]);
			}
			else if (type == 2 && len >= 6)
			{
				has_trns = true;
				for (int c = 0; c < 3; c++) trns[c] = (int)(body[c * 2] << 8 | body[c * 2 + 1]);
			}
		}
		else if (kind == "IDAT")
		{
			idat.push_all(body);
		}
		else if (kind == "IEND")
		{
			seen_end = true;
			break;
		}
	}
	if (!seen_header || !seen_end || w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE) return false;
	int channels;
	switch (type)
	{
		case 0: channels = 1;
		case 2: channels = 3;
		case 3: channels = 1;
		case 4: channels = 2;
		case 6: channels = 4;
		default: return false;
	}
	if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false;
	if ((type == 2 || type == 4 || type == 6) && depth < 8) return false;
	if (type == 3 && depth == 16) return false;
	// zlib: a 2-byte header (deflate, no preset dictionary), then deflate.
	if (idat.len() < 2) return false;
	char cmf = idat[0];
	char flg = idat[1];
	if ((cmf & 0x0F) != 8 || ((uint)cmf << 8 | flg) % 31 != 0 || (flg & 0x20)) return false;
	int bits_pp = channels * depth;
	int bpp = math::max(1, bits_pp / 8);
	// The passes (one, or Adam7's seven): (x0, y0, dx, dy).
	int[4][7] adam7 = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
	int passes = interlace ? 7 : 1;
	sz expected = 0;
	for (int p = 0; p < passes; p++)
	{
		int[4] g = interlace ? adam7[p] : (int[4]){ 0, 0, 1, 1 };
		sz pw = ((sz)w - g[0] + g[2] - 1) / g[2];
		sz ph = ((sz)h - g[1] + g[3] - 1) / g[3];
		if (pw > 0 && ph > 0) expected += ph * (1 + (pw * bits_pp + 7) / 8);
	}
	Vec{char} raw;
	defer raw.free();
	if (!inflate(idat.array_view()[2..], &raw, expected) || raw.len() < expected) return false;
	char[] px = mem::alloc_array(char, (sz)w * h * 4);
	sz at = 0;
	for (int p = 0; p < passes; p++)
	{
		int[4] g = interlace ? adam7[p] : (int[4]){ 0, 0, 1, 1 };
		sz pw = ((sz)w - g[0] + g[2] - 1) / g[2];
		sz ph = ((sz)h - g[1] + g[3] - 1) / g[3];
		if (pw <= 0 || ph <= 0) continue;
		sz stride = (pw * bits_pp + 7) / 8;
		char[] part = raw.array_view()[at : ph * (stride + 1)];
		at += ph * (stride + 1);
		if (!unfilter(part, ph, stride, bpp))
		{
			free(px.ptr);
			return false;
		}
		for (sz y = 0; y < ph; y++)
		{
			char* row = &part[y * (stride + 1) + 1];
			for (sz x = 0; x < pw; x++)
			{
				int[4] s; // samples, as read
				for (int c = 0; c < channels; c++)
				{
					if (depth == 16)
					{
						sz i = (x * channels + c) * 2;
						s[c] = (int)(row[i] << 8 | row[i + 1]);
					}
					else if (depth == 8)
					{
						s[c] = row[x * channels + c];
					}
					else
					{
						sz bit = x * depth;
						s[c] = (row[bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1 << depth) - 1);
					}
				}
				char[4] o;
				int max = (1 << depth) - 1;
				switch (type)
				{
					case 0:
						char v = (char)(s[0] * 255 / max);
						o = { v, v, v, has_trns && s[0] == trns[0] ? 0 : 255 };
					case 2:
						bool clear = has_trns && s[0] == trns[0] && s[1] == trns[1] && s[2] == trns[2];
						o = { (char)(s[0] * 255 / max), (char)(s[1] * 255 / max), (char)(s[2] * 255 / max), clear ? 0 : 255 };
					case 3:
						o = palette[s[0] & 255];
					case 4:
						char v = (char)(s[0] * 255 / max);
						o = { v, v, v, (char)(s[1] * 255 / max) };
					default:
						o = { (char)(s[0] * 255 / max), (char)(s[1] * 255 / max), (char)(s[2] * 255 / max), (char)(s[3] * 255 / max) };
				}
				sz dx = g[0] + x * g[2];
				sz dy = g[1] + y * g[3];
				px[(dy * w + dx) * 4 : 4] = o[..];
			}
		}
	}
	out.free();
	*out = { (int)w, (int)h, px };
	return true;
}

fn bool read_png(String path, Image* out) => @pool()
{
	char[]? bytes = file::load_temp(path);
	if (catch bytes) return false;
	return parse_png(bytes, out);
}

fn void put_be32(Vec{char}* out, uint v) @private
{
	out.push((char)(v >> 24));
	out.push((char)(v >> 16));
	out.push((char)(v >> 8));
	out.push((char)v);
}

fn void put_chunk(Vec{char}* out, String kind, char[] body) @private
{
	put_be32(out, (uint)body.len);
	sz start = out.len();
	out.push_all(kind);
	out.push_all(body);
	put_be32(out, crc32(0, out.array_view()[start..]));
}

// An image as a PNG (RGB, or RGBA if `alpha`), appended to `out`. The data
// isn't compressed (stored deflate blocks): screenshots, quickly.
fn void encode_png(int width, int height, char[] rgba, bool alpha, Vec{char}* out)
{
	out.push_all(&PNG_SIGNATURE);
	char[13] ihdr;
	ihdr[0] = (char)(width >> 24);
	ihdr[1] = (char)(width >> 16);
	ihdr[2] = (char)(width >> 8);
	ihdr[3] = (char)width;
	ihdr[4] = (char)(height >> 24);
	ihdr[5] = (char)(height >> 16);
	ihdr[6] = (char)(height >> 8);
	ihdr[7] = (char)height;
	ihdr[8] = 8;
	ihdr[9] = alpha ? 6 : 2;
	put_chunk(out, "IHDR", &ihdr);
	// The rows, each with filter 0.
	int channels = alpha ? 4 : 3;
	Vec{char} raw;
	defer raw.free();
	raw.reserve((sz)height * (1 + (sz)width * channels));
	for (int y = 0; y < height; y++)
	{
		raw.push(0);
		for (int x = 0; x < width; x++)
		{
			char* p = &rgba[((sz)y * width + x) * 4];
			raw.push_all(p[:channels]);
		}
	}
	// zlib around stored blocks.
	Vec{char} z;
	defer z.free();
	z.push(0x78);
	z.push(0x01);
	char[] r = raw.array_view();
	sz at = 0;
	do
	{
		sz n = math::min(r.len - at, (sz)65535);
		z.push(at + n == r.len ? 1 : 0);
		z.push((char)n);
		z.push((char)(n >> 8));
		z.push((char)~n);
		z.push((char)(~n >> 8));
		z.push_all(r[at : n]);
		at += n;
	} while (at < r.len);
	uint a = 1;
	uint bsum = 0;
	foreach (c : r)
	{
		a = (a + c) % 65521;
		bsum = (bsum + a) % 65521;
	}
	put_be32(&z, bsum << 16 | a);
	put_chunk(out, "IDAT", z.array_view());
	put_chunk(out, "IEND", {});
}

// --- Painted faces -----------------------------------------------------------------

fn uint mix(uint h) @private
{
	h ^= h >> 16;
	h *= 0x7feb352d;
	h ^= h >> 15;
	h *= 0x846ca68b;
	h ^= h >> 16;
	return h;
}

struct Rgb @private
{
	float r, g, b;
}

fn Rgb color_of(Data* v, String fallback = "#ff00ff") @private
{
	char[4] c = { 255, 0, 255, 255 };
	if (!(v && v.is_string() && parse_html_color(v.s, &c))) parse_html_color(fallback, &c);
	return { c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f };
}

fn double number(Data* t, String key, double fallback, double lo, double hi) @private
{
	Data* v = t ? t.get(key) : null;
	return v && v.is_number() ? math::clamp(v.as_double(), lo, hi) : fallback;
}

// A table's feature `key`: one table, or a list of them.
fn void features(Data* spec, String key, Vec{Data*}* out) @private
{
	out.clear();
	Data* v = spec.get(key);
	if (!v) return;
	if (v.is_object())
	{
		out.push(v);
	}
	else if (v.is_array())
	{
		foreach (&e : v.items())
		{
			if (e.is_object() && out.len() < 16) out.push(e);
		}
	}
}

struct Painter @private
{
	int n;        // pixels across
	uint seed;    // from the spec
	float noise, blotch;
	int shades, blotch_size;
	Rgb[] px;
	float[] value; // each pixel's 0..1 lightness draw
}

fn int Painter.wrap(&self, int v) @inline => ((v % self.n) + self.n) % self.n;

fn float Painter.rnd(&self, int x, int y, uint salt)
{
	uint h = mix(self.seed ^ mix((uint)x * 0x9e3779b1 + (uint)y * 0x85ebca77 + salt * 0xc2b2ae3d));
	return (float)(h >> 8) / 16777216.0f;
}

fn float Painter.at_cell(&self, int cx, int cy, int cells, uint salt) @inline
	=> self.rnd(((cx % cells) + cells) % cells, ((cy % cells) + cells) % cells, salt);

// Value noise that wraps every n pixels, `size` pixels across.
fn float Painter.smooth(&self, float x, float y, int size, uint salt)
{
	int cells = math::max(1, self.n / math::max(1, size));
	float s = (float)self.n / (float)cells;
	float fx = x / s;
	float fy = y / s;
	int x0 = (int)math::floor(fx);
	int y0 = (int)math::floor(fy);
	float tx = fx - (float)x0;
	float ty = fy - (float)y0;
	tx = tx * tx * (3 - 2 * tx);
	ty = ty * ty * (3 - 2 * ty);
	float a = self.at_cell(x0, y0, cells, salt) + (self.at_cell(x0 + 1, y0, cells, salt) - self.at_cell(x0, y0, cells, salt)) * tx;
	float b = self.at_cell(x0, y0 + 1, cells, salt)
		+ (self.at_cell(x0 + 1, y0 + 1, cells, salt) - self.at_cell(x0, y0 + 1, cells, salt)) * tx;
	return a + (b - a) * ty;
}

// `c` lighter or darker by a 0..1 draw, in `shades` steps.
fn Rgb Painter.shade(&self, Rgb c, float v)
{
	int level = math::min(self.shades - 1, (int)(v * (float)self.shades));
	float f = self.shades > 1 ? 1.0f + self.noise * (((float)level + 0.5f) / (float)self.shades * 2.0f - 1.0f) : 1.0f;
	return { c.r * f, c.g * f, c.b * f };
}

fn void Painter.set(&self, int x, int y, Rgb c) @inline => self.px[self.wrap(y) * self.n + self.wrap(x)] = c;
fn float Painter.v(&self, int x, int y) @inline => self.value[self.wrap(y) * self.n + self.wrap(x)];

fn void Painter.cells(&self, Data* t, Rgb base, uint salt)
{
	int n = self.n;
	int size = (int)number(t, "size", 5, 2, n);
	int count = math::max(1, n / size);
	float s = (float)n / (float)count;
	Rgb edge = color_of(t.get("edge"), "#000000");
	Data* colors = t.get("colors");
	sz ncolors = colors && colors.is_array() ? colors.len() : 0;
	for (int y = 0; y < n; y++)
	{
		for (int x = 0; x < n; x++)
		{
			float d1 = 1e9f;
			float d2 = 1e9f;
			int best = 0;
			for (int cy = 0; cy < count; cy++)
			{
				for (int cx = 0; cx < count; cx++)
				{
					float px_ = ((float)cx + 0.15f + 0.7f * self.rnd(cx, cy, salt)) * s;
					float py_ = ((float)cy + 0.15f + 0.7f * self.rnd(cx, cy, salt + 1)) * s;
					float dx = math::abs((float)x + 0.5f - px_);
					float dy = math::abs((float)y + 0.5f - py_);
					dx = math::min(dx, (float)n - dx);
					dy = math::min(dy, (float)n - dy);
					float d = math::sqrt(dx * dx + dy * dy);
					if (d < d1)
					{
						d2 = d1;
						d1 = d;
						best = cy * count + cx;
					}
					else if (d < d2)
					{
						d2 = d;
					}
				}
			}
			float own = self.rnd(best, 0, salt + 2);
			if (d2 - d1 < 1.0f)
			{
				self.set(x, y, self.shade(edge, self.v(x, y)));
			}
			else
			{
				Rgb c = ncolors == 0 ? base : color_of(colors.at((sz)(own * (float)ncolors) % ncolors));
				self.set(x, y, self.shade(c, 0.5f * self.v(x, y) + 0.5f * own));
			}
		}
	}
}

fn void Painter.stripes(&self, Data* t, uint salt)
{
	int n = self.n;
	Rgb c = color_of(t.get("color"));
	bool across = t.flag("across");
	int every = (int)number(t, "every", 4, 1, n);
	int width = (int)number(t, "width", 1, 1, every);
	int offset = (int)number(t, "offset", 0, (double)-n, n);
	float wobble = (float)number(t, "wobble", 0, 0, 4);
	float broken = (float)number(t, "broken", 0, 0, 1);
	bool seams = t.has("seams");
	Rgb seam = color_of(t.get("seams"));
	for (int a = 0; a < n; a++) // along the stripes
	{
		for (int b = 0; b < n; b++) // across them
		{
			int shift = wobble > 0 ? (int)math::round((self.smooth((float)b * 3.0f, (float)a, 4, salt) - 0.5f) * 2.0f * wobble) : 0;
			int pos = b + offset + shift;
			int x = across ? a : b;
			int y = across ? b : a;
			int stripe = (int)math::floor((float)pos / (float)every);
			if (((pos % every) + every) % every < width)
			{
				if (broken <= 0 || self.rnd(stripe, a / 3, salt + 1) >= broken) self.set(x, y, self.shade(c, self.v(x, y)));
			}
			else if (seams) // one seam in each board, across it
			{
				int at = (int)(self.rnd(stripe, 0, salt + 2) * (float)n);
				if (a == at) self.set(x, y, self.shade(seam, self.v(x, y)));
			}
		}
	}
}

fn void Painter.rings(&self, Data* t)
{
	int n = self.n;
	Rgb c = color_of(t.get("color"));
	int every = (int)number(t, "every", 3, 2, n);
	bool has_border = t.has("border");
	Rgb border = color_of(t.get("border"));
	float bw = (float)number(t, "border_width", 2, 0, n / 2.0);
	float mid = (float)n / 2.0f;
	for (int y = 0; y < n; y++)
	{
		for (int x = 0; x < n; x++)
		{
			float dx = (float)x + 0.5f - mid;
			float dy = (float)y + 0.5f - mid;
			float d = math::sqrt(dx * dx + dy * dy) + (self.rnd(x, y, 77) - 0.5f) * 0.6f;
			if (has_border && d >= mid - bw)
			{
				self.set(x, y, self.shade(border, self.v(x, y)));
			}
			else if ((int)d % every == every - 1)
			{
				self.set(x, y, self.shade(c, self.v(x, y)));
			}
		}
	}
}

fn void Painter.band(&self, Data* t, uint salt)
{
	int n = self.n;
	Rgb c = color_of(t.get("color"));
	int depth = (int)number(t, "depth", 3, 0, n);
	int ragged = (int)number(t, "ragged", 1, 0, n);
	for (int x = 0; x < n; x++)
	{
		int d = depth + (int)(self.rnd(x, 0, salt) * (float)(ragged + 1));
		for (int y = 0; y < math::min(d, n); y++) self.set(x, y, self.shade(c, self.v(x, y)));
	}
}

fn void Painter.spots(&self, Data* t, uint salt)
{
	int n = self.n;
	Rgb c = color_of(t.get("color"));
	Rgb light = t.has("light") ? color_of(t.get("light")) : c;
	Rgb dark = t.has("dark") ? color_of(t.get("dark")) : c;
	int count = (int)number(t, "count", 4, 0, 64);
	int size = (int)number(t, "size", 3, 1, 32);
	bool dash = t.str("shape") == "dash";
	for (int k = 0; k < count; k++)
	{
		int x = (int)(self.rnd(k, 1, salt) * (float)n);
		int y = (int)(self.rnd(k, 2, salt) * (float)n);
		for (int i = 0; i < size; i++)
		{
			float r = self.rnd(k, 10 + i, salt);
			self.set(x, y, r < 0.25f ? light : (r > 0.8f ? dark : c));
			if (dash)
			{
				x++;
			}
			else // a little cluster: a step to a neighbour
			{
				int dir = (int)(self.rnd(k, 100 + i, salt) * 4.0f) & 3;
				x += dir == 0 ? 1 : (dir == 1 ? -1 : 0);
				y += dir == 2 ? 1 : (dir == 3 ? -1 : 0);
			}
		}
	}
}

// A face painted from `spec`, `size` x `size` texels (a power of two, 8 or
// more): a grid of "pixels" (default 16 across) of the colour "color", each one
// lighter or darker ("noise": how much, in "shades" steps; "blotch": how much
// it follows larger patches, "blotch_size" pixels across), or one of the
// "colors" ramp instead. On top, in this order (each a table, or a list of
// them): "cells" (stones: "size", "edge" colour, "colors" to pick from),
// "stripes" ("color", "every", "width", "offset", "across" for rows,
// "wobble", "broken", "seams" colour between boards), "rings" (a log's end:
// "color", "every", "border" colour, "border_width"), "band" (a grassy top:
// "color", "depth", "ragged") and "spots" (ore: "color", "light", "dark",
// "count", "size", "shape" = "dash" for short lines). The same spec always
// paints the same pixels, and the face tiles without seams. Unreadable
// values fall back to defaults; a bad colour is magenta.
fn Image paint_face(Data* spec, int size) => @pool()
{
	uint seed = 2166136261; // FNV-1a of the spec: the same spec, the same pixels
	DString text;
	text.tinit();
	json::write(&text, spec, true);
	foreach (ch : text.str_view()) seed = (seed ^ ch) * 16777619;
	Data* obj = spec.is_object() ? spec : null;
	int n = (int)number(obj, "pixels", 16, 1, size);
	int p2 = 1;
	while (p2 * 2 <= n) p2 *= 2;
	n = p2;
	Painter p = { .n = n, .seed = seed, .noise = (float)number(obj, "noise", 0.1, 0, 1), .blotch = (float)number(obj, "blotch", 0.3, 0, 1),
		.shades = (int)number(obj, "shades", 4, 1, 16), .blotch_size = (int)number(obj, "blotch_size", 4, 1, n) };
	Rgb base = color_of(obj ? obj.get("color") : null);
	Vec{Rgb} ramp;
	defer ramp.free();
	if (obj && obj.get("colors") && obj.get("colors").is_array())
	{
		foreach (&c : obj.get("colors").items()) ramp.push(color_of(c));
	}
	p.px = mem::talloc_array(Rgb, (sz)n * n);
	p.value = mem::talloc_array(float, (sz)n * n);
	for (int y = 0; y < n; y++)
	{
		for (int x = 0; x < n; x++)
		{
			float v = p.rnd(x, y, 1) * (1 - p.blotch) + p.smooth((float)x, (float)y, p.blotch_size, 2) * p.blotch;
			p.value[y * n + x] = v;
			p.px[y * n + x] = ramp.is_empty() ? p.shade(base, v) : ramp[math::min(ramp.len() - 1, (sz)(v * (float)ramp.len()))];
		}
	}
	if (obj)
	{
		uint salt = 1000;
		Vec{Data*} f;
		defer f.free();
		features(obj, "cells", &f);
		foreach (t : f) p.cells(t, base, salt += 10);
		features(obj, "stripes", &f);
		foreach (t : f) p.stripes(t, salt += 10);
		features(obj, "rings", &f);
		foreach (t : f) p.rings(t);
		features(obj, "band", &f);
		foreach (t : f) p.band(t, salt += 10);
		features(obj, "spots", &f);
		foreach (t : f) p.spots(t, salt += 10);
	}
	Image out = { size, size, mem::alloc_array(char, (sz)size * size * 4) };
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			Rgb c = p.px[(y * n / size) * n + (x * n / size)];
			char* o = out.at(x, y);
			o[0] = (char)math::round(math::clamp(c.r, 0.0f, 1.0f) * 255.0f);
			o[1] = (char)math::round(math::clamp(c.g, 0.0f, 1.0f) * 255.0f);
			o[2] = (char)math::round(math::clamp(c.b, 0.0f, 1.0f) * 255.0f);
			o[3] = 255;
		}
	}
	return out;
}
