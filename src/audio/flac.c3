// Reading .flac files (lossless: the samples come back exactly as
// recorded, at about half the size): the stream's STREAMINFO, then frames
// of each channel's samples, each predicted from the ones before (fixed
// polynomials or LPC) plus a Rice-coded residual. Mono or stereo, 4 to 24
// bits; read as 16-bit PCM (or 32-bit float for more than 16 bits).
module gocube::sound;
import gocube::util::vec;
import std::math;

// Big-endian bits, as FLAC writes them; reading past the end sets `over`
// (and reads zeroes).
struct Bits @private
{
	char[] b;
	usz bit;
	bool over;
}

fn uint Bits.read(&self, int n)
{
	ulong v;
	while (n > 0)
	{
		usz at = self.bit >> 3;
		if (at >= self.b.len)
		{
			self.over = true;
			return 0;
		}
		int left = 8 - (int)(self.bit & 7);
		int take = math::min(left, n);
		v = v << take | (ulong)((self.b[at] >> (left - take)) & ((1 << take) - 1));
		n -= take;
		self.bit += (usz)take;
	}
	return (uint)v;
}

fn int Bits.signed(&self, int n)
{
	if (n == 0) return 0;
	uint u = self.read(n);
	return (int)(u << (32 - n)) >> (32 - n);
}

// Zeroes up to a one: how many.
fn uint Bits.unary(&self)
{
	uint q;
	while (true)
	{
		usz at = self.bit >> 3;
		if (at >= self.b.len)
		{
			self.over = true;
			return q;
		}
		int off = (int)(self.bit & 7);
		char rest = (char)(self.b[at] << off);
		if (rest == 0)
		{
			q += (uint)(8 - off);
			self.bit += (usz)(8 - off);
			continue;
		}
		int z;
		while (!(rest & 0x80))
		{
			rest <<= 1;
			z++;
		}
		q += (uint)z;
		self.bit += (usz)(z + 1);
		return q;
	}
}

fn void Bits.align(&self) => self.bit = (self.bit + 7) & ~(usz)7;

// The residual: partitions of Rice codes (each its own parameter, or raw
// "escaped" bits), added to `out[order..]`.
fn bool residual(Bits* r, int block, int order, int* out) @private
{
	uint method = r.read(2);
	if (method > 1) return false;
	int param_bits = method == 0 ? 4 : 5;
	uint escape = method == 0 ? 15 : 31;
	int partitions = 1 << r.read(4);
	int per = block / partitions;
	if (per * partitions != block || per < order) return false;
	int i = order;
	for (int p = 0; p < partitions; p++)
	{
		int n = p == 0 ? per - order : per;
		uint k = r.read(param_bits);
		if (k == escape)
		{
			int raw = (int)r.read(5);
			for (int j = 0; j < n; j++) out[i++] = r.signed(raw);
		}
		else
		{
			for (int j = 0; j < n; j++)
			{
				uint u = r.unary() << k | r.read((int)k);
				out[i++] = (int)(u >> 1) ^ -(int)(u & 1);
			}
		}
		if (r.over) return false;
	}
	return true;
}

// One channel's samples of a frame (`bps`: its bits a sample).
fn bool subframe(Bits* r, int bps, int block, int* out) @private
{
	if (r.read(1)) return false; // (a zero bit first)
	uint type = r.read(6);
	int wasted;
	if (r.read(1)) wasted = (int)r.unary() + 1; // low bits all zero, left out
	bps -= wasted;
	if (bps <= 0) return false;
	if (type == 0) // constant
	{
		int v = r.signed(bps);
		for (int i = 0; i < block; i++) out[i] = v;
	}
	else if (type == 1) // verbatim
	{
		for (int i = 0; i < block; i++) out[i] = r.signed(bps);
	}
	else if (type >= 8 && type <= 12) // a fixed polynomial
	{
		int order = (int)type - 8;
		if (order > block) return false;
		for (int i = 0; i < order; i++) out[i] = r.signed(bps);
		if (!residual(r, block, order, out)) return false;
		for (int i = order; i < block; i++)
		{
			long p;
			switch (order)
			{
				case 1: p = out[i - 1];
				case 2: p = 2 * (long)out[i - 1] - out[i - 2];
				case 3: p = 3 * (long)out[i - 1] - 3 * (long)out[i - 2] + out[i - 3];
				case 4: p = 4 * (long)out[i - 1] - 6 * (long)out[i - 2] + 4 * (long)out[i - 3] - out[i - 4];
			}
			out[i] = (int)(out[i] + p);
		}
	}
	else if (type >= 32) // LPC
	{
		int order = (int)type - 31;
		if (order > block) return false;
		for (int i = 0; i < order; i++) out[i] = r.signed(bps);
		int precision = (int)r.read(4) + 1;
		int shift = r.signed(5);
		if (precision == 16 || shift < 0) return false;
		int[32] coef;
		for (int j = 0; j < order; j++) coef[j] = r.signed(precision);
		if (!residual(r, block, order, out)) return false;
		for (int i = order; i < block; i++)
		{
			long sum;
			for (int j = 0; j < order; j++) sum += (long)coef[j] * out[i - 1 - j];
			out[i] = (int)(out[i] + (sum >> shift));
		}
	}
	else
	{
		return false;
	}
	if (wasted) for (int i = 0; i < block; i++) out[i] <<= wasted;
	return !r.over;
}

fn ushort crc16(char[] bytes) @private
{
	uint crc;
	foreach (b : bytes)
	{
		crc ^= (uint)b << 8;
		for (int k = 0; k < 8; k++) crc = crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1;
		crc &= 0xFFFF;
	}
	return (ushort)crc;
}

fn bool flac_fail(Samples* w, String* error, String why) @private
{
	w.free();
	*error = why;
	return false;
}

// False and `error` (temp) if it isn't a FLAC stream that plays.
fn bool parse_flac(char[] bytes, Samples* out, String* error)
{
	Samples w;
	if (bytes.len < 8 || (String)bytes[0:4] != "fLaC") return flac_fail(&w, error, "not a .flac file");
	// The metadata blocks: STREAMINFO first; the rest (tags, pictures) skipped.
	usz pos = 4;
	int bps;
	int max_block;
	ulong total;
	bool info;
	while (true)
	{
		if (pos + 4 > bytes.len) return flac_fail(&w, error, "its header is cut short");
		bool last = (bytes[pos] & 0x80) != 0;
		int type = bytes[pos] & 0x7F;
		usz len = (usz)bytes[pos + 1] << 16 | (usz)bytes[pos + 2] << 8 | (usz)bytes[pos + 3];
		if (pos + 4 + len > bytes.len) return flac_fail(&w, error, "its header is cut short");
		if (type == 0 && len >= 34)
		{
			Bits s = { bytes[pos + 4 : 34], 0, false };
			s.read(16); // the smallest block
			max_block = (int)s.read(16);
			s.read(24);
			s.read(24);
			w.rate = s.read(20);
			w.channels = (ushort)(s.read(3) + 1);
			bps = (int)s.read(5) + 1;
			total = (ulong)s.read(4) << 32 | s.read(32);
			info = true;
		}
		pos += 4 + len;
		if (last) break;
	}
	if (!info) return flac_fail(&w, error, "it has no STREAMINFO");
	if (w.channels != 1 && w.channels != 2) return flac_fail(&w, error, string::tformat("it has %d channels (sounds are mono or stereo)", w.channels));
	if (bps < 4 || bps > 24 || w.rate < 1000 || w.rate > 200000 || max_block < 16) return flac_fail(&w, error, "its sample format isn't one that plays");
	bool to_float = bps > 16;
	w.format = to_float ? FLOAT : PCM;
	w.bits = to_float ? 32 : 16;
	int* left = mem::new_array(int, max_block);
	int* right = mem::new_array(int, max_block);
	defer
	{
		free(left);
		free(right);
	}
	if (total) w.data.reserve((sz)total * w.channels * (w.bits / 8));
	ulong done;
	Bits r = { bytes, pos * 8, false };
	while (r.bit / 8 + 2 <= bytes.len && (!total || done < total))
	{
		usz start = r.bit / 8;
		if (r.read(14) != 0x3FFE) return flac_fail(&w, error, "a frame doesn't start where it should");
		r.read(2); // reserved; fixed or variable block size
		uint block_code = r.read(4);
		uint rate_code = r.read(4);
		uint channel_code = r.read(4);
		uint size_code = r.read(3);
		r.read(1);
		// The frame (or sample) number: UTF-8 style, 1 to 7 bytes.
		uint first = r.read(8);
		int more;
		while (more < 7 && (first << more) & 0x80) more++;
		for (int i = 1; i < more; i++) r.read(8);
		int block;
		switch
		{
			case block_code == 1: block = 192;
			case block_code >= 2 && block_code <= 5: block = 576 << (block_code - 2);
			case block_code == 6: block = (int)r.read(8) + 1;
			case block_code == 7: block = (int)r.read(16) + 1;
			case block_code >= 8: block = 256 << (block_code - 8);
			default: return flac_fail(&w, error, "a frame's size is broken");
		}
		if (rate_code == 12) r.read(8);
		if (rate_code == 13 || rate_code == 14) r.read(16);
		r.read(8); // the header's CRC-8 (the frame's CRC-16 covers it too)
		if (block > max_block) return flac_fail(&w, error, "a frame is bigger than it said");
		int[8] sizes = { bps, 8, 12, 0, 16, 20, 24, 32 };
		int fbps = sizes[size_code];
		if (fbps != bps) return flac_fail(&w, error, "a frame's sample size is broken");
		int channels = channel_code < 8 ? (int)channel_code + 1 : 2;
		if (channels != w.channels || channel_code > 10) return flac_fail(&w, error, "a frame's channels are broken");
		// The side channel (left minus right) has a bit more.
		bool ok = subframe(&r, bps + (channel_code == 9 ? 1 : 0), block, left);
		if (ok && channels == 2) ok = subframe(&r, bps + (channel_code == 8 || channel_code == 10 ? 1 : 0), block, right);
		if (!ok) return flac_fail(&w, error, "a frame is broken");
		r.align();
		usz end = r.bit / 8;
		uint crc = r.read(16);
		if (r.over || crc != crc16(bytes[start : end - start])) return flac_fail(&w, error, "a frame is damaged (its CRC is wrong)");
		for (int i = 0; i < block; i++)
		{
			int a = left[i];
			int b = right[i];
			switch (channel_code)
			{
				case 8: b = a - b;   // left, side
				case 9: a = a + b;   // side, right
				case 10:             // mid, side
					long mid = (long)a << 1 | (b & 1);
					a = (int)((mid + b) >> 1);
					b = (int)((mid - b) >> 1);
			}
			for (int c = 0; c < channels; c++)
			{
				int v = c == 0 ? a : b;
				if (to_float)
				{
					float f = (float)v / (float)(1 << (bps - 1));
					w.data.push_all(((char*)&f)[:4]);
				}
				else
				{
					short s = (short)(v << (16 - bps));
					w.data.push_all(((char*)&s)[:2]);
				}
			}
		}
		done += (ulong)block;
	}
	if (total && done < total) return flac_fail(&w, error, "it is cut short");
	if (total && done > total) w.data.resize((sz)total * w.channels * (w.bits / 8)); // (the last block padded)
	if (w.data.is_empty()) return flac_fail(&w, error, "it has no samples");
	*out = w;
	return true;
}
