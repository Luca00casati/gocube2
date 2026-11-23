// Messages on the wire: little-endian numbers, strings and byte strings with a
// length in front, and data values (Data) as JSON (nested at most
// MAX_DATA_DEPTH deep, whatever a peer sends). A Reader never reads past its
// end: what isn't there reads as zero or empty and marks it bad (`ok`), and
// strings and data have a size limit, so a peer's message can't make us read
// or allocate without bound.
module gocube::net::wire;
import gocube::data, gocube::data::json, gocube::util::vec, std::math;

struct Writer
{
	Vec{char} bytes;
}

// A message of `type` (free() its bytes).
fn Writer new(char type)
{
	Writer w;
	w.bytes.push(type);
	return w;
}

fn void Writer.free(&self) => self.bytes.free();
fn char[] Writer.view(&self) => self.bytes.array_view();
fn Writer* Writer.raw(&self, void* p, sz n) @inline
{
	self.bytes.push_all(((char*)p)[:n]);
	return self;
}
fn Writer* Writer.u8(&self, char v) => self.raw(&v, 1);
fn Writer* Writer.u16(&self, ushort v) => self.raw(&v, 2);
fn Writer* Writer.u32(&self, uint v) => self.raw(&v, 4);
fn Writer* Writer.i32(&self, int v) => self.raw(&v, 4);
fn Writer* Writer.f32(&self, float v) => self.raw(&v, 4);
fn Writer* Writer.f64(&self, double v) => self.raw(&v, 8);
fn Writer* Writer.boolean(&self, bool v) => self.u8(v ? 1 : 0);
fn Writer* Writer.vec3(&self, float[3] v) => self.f32(v[0]).f32(v[1]).f32(v[2]);
fn Writer* Writer.str(&self, String s) => self.u32((uint)s.len).raw(s.ptr, s.len);
fn Writer* Writer.blob(&self, char[] b) => self.u32((uint)b.len).raw(b.ptr, b.len);

fn Writer* Writer.data(&self, Data* d) => @pool()
{
	DString text;
	text.tinit();
	json::write(&text, d, true);
	return self.str(text.str_view());
}

const sz MAX_STRING = 4096;

struct Reader
{
	char[] p;
	sz i;
	bool bad;
}

fn Reader reader(char[] data) => { .p = data };

fn bool Reader.ok(&self) => !self.bad;
fn bool Reader.at_end(&self) => self.i == self.p.len;

fn void Reader.raw(&self, void* out, sz n)
{
	if (self.bad || n > self.p.len - self.i)
	{
		self.bad = true;
		return;
	}
	mem::copy(out, &self.p[self.i], n);
	self.i += n;
}

fn char Reader.u8(&self)
{
	char v;
	self.raw(&v, 1);
	return v;
}

fn ushort Reader.u16(&self)
{
	ushort v;
	self.raw(&v, 2);
	return v;
}

fn uint Reader.u32(&self)
{
	uint v;
	self.raw(&v, 4);
	return v;
}

fn int Reader.i32(&self)
{
	int v;
	self.raw(&v, 4);
	return v;
}

// Floats that aren't finite read as 0 and mark the message bad.
fn float Reader.f32(&self)
{
	float v;
	self.raw(&v, 4);
	if (math::is_nan(v) || math::is_inf(v))
	{
		self.bad = true;
		v = 0;
	}
	return v;
}

fn double Reader.f64(&self)
{
	double v;
	self.raw(&v, 8);
	if (math::is_nan(v) || math::is_inf(v))
	{
		self.bad = true;
		v = 0;
	}
	return v;
}

fn bool Reader.boolean(&self) => self.u8() != 0;

fn float[3] Reader.vec3(&self)
{
	float x = self.f32();
	float y = self.f32();
	float z = self.f32();
	return { x, y, z };
}

// A string: a view into the message (copy it to keep it).
fn String Reader.str(&self, sz max = MAX_STRING)
{
	uint len = self.u32();
	if (self.bad || (sz)len > max || (sz)len > self.p.len - self.i)
	{
		self.bad = true;
		return "";
	}
	String s = (String)self.p[self.i : (sz)len];
	self.i += len;
	return s;
}

// Bytes: a view into the message.
fn char[] Reader.blob(&self, sz max)
{
	uint len = self.u32();
	if (self.bad || (sz)len > max || (sz)len > self.p.len - self.i)
	{
		self.bad = true;
		return {};
	}
	char[] b = self.p[self.i : (sz)len];
	self.i += len;
	return b;
}

// A data value (null and a bad message if it isn't one; free() it).
fn Data Reader.data(&self, sz max = 1 << 20)
{
	String text = self.str(max);
	if (self.bad) return data::NULL_DATA;
	Data d;
	DString error;
	defer error.free();
	if (!json::parse(text, "message", &d, &error))
	{
		self.bad = true;
		return data::NULL_DATA;
	}
	return d;
}
