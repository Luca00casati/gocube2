// Data files and data as text: JSON (RFC 8259), read and written by our own
// code. A world's world.json, the settings, the game's content and what
// travels over the network are JSON. (src/data/json.*: the same output.)
//
// Objects keep their keys in the order they're written; a number without a
// fraction or exponent that fits is an integer, any other a float. Reading is
// strict JSON (no comments, no trailing commas), nested at most
// MAX_DATA_DEPTH deep, whatever the input.
module gocube::data::json;
import gocube::data, gocube::fs, gocube::util::vec, std::io, std::math, std::sort, std::core::string, libc;

const int MAX_DATA_DEPTH = 32;

faultdef MALFORMED;

struct Parser @private
{
	String s;
	String name;
	sz i;
	DString* error;
}

fn fault Parser.fail(&self, String why)
{
	int line = 1;
	for (sz k = 0; k < self.i && k < self.s.len; k++)
	{
		if (self.s[k] == '\n') line++;
	}
	if (!*self.error) self.error.init(mem); // (the caller's: not the pool's)
	self.error.clear();
	self.error.appendf("%s:%d: %s", self.name, line, why);
	return MALFORMED;
}

fn char Parser.peek(&self) @inline => self.i < self.s.len ? self.s[self.i] : '\0';

fn void Parser.skip(&self)
{
	while (self.i < self.s.len)
	{
		char c = self.s[self.i];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
		self.i++;
	}
}

fn void? Parser.expect(&self, char c)
{
	self.skip();
	if (self.peek() != c)
	{
		if (self.i >= self.s.len) return self.fail(string::tformat("the text ends too soon (expected '%c')", c))~;
		return self.fail(string::tformat("expected '%c'", c))~;
	}
	self.i++;
}

fn bool Parser.word(&self, String w)
{
	if (self.i + w.len > self.s.len || self.s[self.i : w.len] != w) return false;
	self.i += w.len;
	return true;
}

fn int hex_digit(char c) @private
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

fn uint? Parser.hex4(&self)
{
	uint v = 0;
	for (int k = 0; k < 4; k++)
	{
		int d = hex_digit(self.peek());
		if (d < 0) return self.fail("\\u needs four hex digits")~;
		v = v * 16 + (uint)d;
		self.i++;
	}
	return v;
}

fn void put_utf8(DString* out, uint c)
{
	if (c < 0x80)
	{
		out.append_char((char)c);
	}
	else if (c < 0x800)
	{
		out.append_char((char)(0xC0 | (c >> 6)));
		out.append_char((char)(0x80 | (c & 0x3F)));
	}
	else if (c < 0x10000)
	{
		out.append_char((char)(0xE0 | (c >> 12)));
		out.append_char((char)(0x80 | ((c >> 6) & 0x3F)));
		out.append_char((char)(0x80 | (c & 0x3F)));
	}
	else
	{
		out.append_char((char)(0xF0 | (c >> 18)));
		out.append_char((char)(0x80 | ((c >> 12) & 0x3F)));
		out.append_char((char)(0x80 | ((c >> 6) & 0x3F)));
		out.append_char((char)(0x80 | (c & 0x3F)));
	}
}

// A string (the opening '"' next); the result on the heap.
fn String? Parser.string(&self)
{
	self.i++; // "
	DString out;
	out.init(mem);
	defer catch out.free();
	while (true)
	{
		if (self.i >= self.s.len) return self.fail("a string isn't closed")~;
		char c = self.s[self.i++];
		if (c == '"')
		{
			String result = out.copy_str(mem);
			out.free();
			return result;
		}
		if (c < 0x20) return self.fail("a control character in a string (write it as \\n, \\t, \\u0000...)")~;
		if (c != '\\')
		{
			out.append_char(c);
			continue;
		}
		char e = self.peek();
		self.i++;
		switch (e)
		{
			case '"': out.append_char('"');
			case '\\': out.append_char('\\');
			case '/': out.append_char('/');
			case 'b': out.append_char('\b');
			case 'f': out.append_char('\f');
			case 'n': out.append_char('\n');
			case 'r': out.append_char('\r');
			case 't': out.append_char('\t');
			case 'u':
				uint c32 = self.hex4()!;
				if (c32 >= 0xD800 && c32 <= 0xDBFF) // a surrogate pair
				{
					if (!self.word("\\u")) return self.fail("a lone surrogate in \\u")~;
					uint low = self.hex4()!;
					if (low < 0xDC00 || low > 0xDFFF) return self.fail("a lone surrogate in \\u")~;
					c32 = 0x10000 + ((c32 - 0xD800) << 10) + (low - 0xDC00);
				}
				else if (c32 >= 0xDC00 && c32 <= 0xDFFF)
				{
					return self.fail("a lone surrogate in \\u")~;
				}
				put_utf8(&out, c32);
			default:
				return self.fail(string::tformat("unknown escape \\%c", e))~;
		}
	}
}

fn bool Parser.digits(&self)
{
	sz from = self.i;
	while (self.peek() >= '0' && self.peek() <= '9') self.i++;
	return self.i > from;
}

fn Data? Parser.number(&self)
{
	sz start = self.i;
	bool is_float = false;
	if (self.peek() == '-') self.i++;
	if (self.peek() == '0')
	{
		self.i++;
	}
	else if (!self.digits())
	{
		return self.fail("a malformed number")~;
	}
	if (self.peek() == '.')
	{
		is_float = true;
		self.i++;
		if (!self.digits()) return self.fail("a malformed number")~;
	}
	if (self.peek() == 'e' || self.peek() == 'E')
	{
		is_float = true;
		self.i++;
		if (self.peek() == '+' || self.peek() == '-') self.i++;
		if (!self.digits()) return self.fail("a malformed number")~;
	}
	String text = self.s[start .. self.i - 1];
	if (!is_float)
	{
		if (try v = text.to_long()) return data::integer(v);
		// (too large for an integer: a float)
	}
	double? d = text.to_double();
	if (catch d) return self.fail("a number too large for data")~;
	if (math::is_nan(d) || math::is_inf(d)) return self.fail("a number too large for data")~;
	return data::number(d);
}

fn Data? Parser.value(&self, int depth)
{
	self.skip();
	char c = self.peek();
	if (c == '{' || c == '[')
	{
		if (depth > MAX_DATA_DEPTH) return self.fail("data nested too deeply")~;
		self.i++;
		char close = c == '{' ? '}' : ']';
		Data out = c == '{' ? data::object() : data::array();
		defer catch out.free();
		self.skip();
		if (self.peek() == close)
		{
			self.i++;
			return out;
		}
		while (true)
		{
			if (c == '{')
			{
				self.skip();
				if (self.peek() != '"') return self.fail("expected a key (a string)")~;
				String key = self.string()!;
				defer free(key.ptr);
				self.expect(':')!;
				Data v = self.value(depth + 1)!;
				out.set(key, v); // (a key given twice: the last wins)
			}
			else
			{
				out.push(self.value(depth + 1)!);
			}
			self.skip();
			if (self.peek() == ',')
			{
				self.i++;
				continue;
			}
			self.expect(close)!;
			return out;
		}
	}
	if (c == '"') return { .type = STRING, .s = self.string()! };
	if (c == '-' || (c >= '0' && c <= '9')) return self.number();
	if (self.word("true")) return data::boolean(true);
	if (self.word("false")) return data::boolean(false);
	if (self.word("null")) return data::NULL_DATA;
	if (self.i >= self.s.len) return self.fail("the text ends too soon")~;
	return self.fail(string::tformat("unexpected '%c'", c))~;
}

// Reads `text` (named `name` in errors) into `out`. False and `error` if it isn't JSON.
fn bool parse(String text, String name, Data* out, DString* error) => @pool()
{
	Parser p = { .s = text, .name = name, .error = error };
	if (text.len >= 3 && text[:3] == "\xEF\xBB\xBF") p.i = 3; // (a UTF-8 byte order mark)
	Data? v = p.value(0);
	if (catch v) return false;
	p.skip();
	if (p.i < text.len)
	{
		v.free();
		(void)p.fail("unexpected text after the value");
		return false;
	}
	*out = v;
	return true;
}

fn bool read_file(String path, Data* out, DString* error) => @pool()
{
	char[]? text = file::load_temp(path);
	if (catch text)
	{
		if (!*error) error.init(mem);
		error.clear();
		error.append("can't read it");
		return false;
	}
	String name = path;
	if (try slash = path.rindex_of_char('/')) name = path[slash + 1..];
	if (try slash = name.rindex_of_char('\\')) name = name[slash + 1..];
	return parse((String)text, name, out, error);
}

// --- Writing -----------------------------------------------------------------------

fn void put_string(DString* out, String s)
{
	out.append_char('"');
	foreach (c : s)
	{
		switch (c)
		{
			case '"': out.append("\\\"");
			case '\\': out.append("\\\\");
			case '\n': out.append("\\n");
			case '\r': out.append("\\r");
			case '\t': out.append("\\t");
			default:
				if (c < 0x20 || c == 0x7F)
				{
					out.appendf("\\u%04x", c);
				}
				else
				{
					out.append_char(c);
				}
		}
	}
	out.append_char('"');
}

// The shortest form that reads back the same, always with a '.' or exponent
// (so it stays a float).
fn void put_number(DString* out, double d) @private
{
	char[64] buf;
	CInt n;
	for (int precision = 1; precision <= 17; precision++)
	{
		n = libc::snprintf(&buf, 64, "%.*g", precision, d);
		double? back = ((String)buf[:n]).to_double();
		if (try back && back == d) break;
	}
	String s = (String)buf[:n];
	out.append(s);
	if (!s.contains_char('.') && !s.contains_char('e') && !s.contains_char('E')) out.append(".0");
}

fn void put_scalar(DString* out, Data* v) @private
{
	switch (v.type)
	{
		case NULL: out.append("null");
		case BOOLEAN: out.append(v.b ? "true" : "false");
		case INTEGER: out.appendf("%d", v.i);
		case FLOAT: put_number(out, v.f);
		case STRING: put_string(out, v.s);
		default:
	}
}

// An object's fields, sorted by key (pointers into it, in the temp allocator).
fn Member*[] sorted(Data* o) @private
{
	Member*[] f = mem::talloc_array(Member*, o.ml().len());
	foreach (i, &m : *o.ml()) f[i] = m;
	sort::quicksort(f, fn int(Member* a, Member* b) => compare_bytes(a.key, b.key));
	return f;
}

fn int compare_bytes(String a, String b) @private
{
	sz n = math::min(a.len, b.len);
	for (sz i = 0; i < n; i++)
	{
		if (a[i] != b[i]) return (int)a[i] - (int)b[i];
	}
	return a.len < b.len ? -1 : (a.len > b.len ? 1 : 0);
}

// The whole value on one line, if it's short and holds no structures of structures.
fn bool put_inline(DString* out, Data* v, int depth) @private
{
	DString line;
	line.tinit();
	if (v.type == ARRAY)
	{
		line.append_char('[');
		foreach (i, &e : *v.dl())
		{
			if (e.is_structured() && !e.is_empty())
			{
				if (e.type == OBJECT) return false;
				foreach (&x : *e.dl())
				{
					if (x.is_structured()) return false;
				}
			}
			if (i > 0) line.append(", ");
			put(&line, e, depth + 1, false);
		}
		line.append_char(']');
	}
	else
	{
		line.append_char('{');
		foreach (i, m : sorted(v))
		{
			if (m.value.is_structured() && !m.value.is_empty()) return false;
			if (i > 0) line.append(", ");
			put_string(&line, m.key);
			line.append(": ");
			put(&line, &m.value, depth + 1, false);
		}
		line.append_char('}');
	}
	if (line.len() + (sz)depth * 4 > 100) return false;
	out.append(line.str_view());
	return true;
}

fn void put(DString* out, Data* v, int depth, bool compact) @private
{
	if (!v.is_structured())
	{
		put_scalar(out, v);
		return;
	}
	bool is_array = v.type == ARRAY;
	if (v.is_empty())
	{
		out.append(is_array ? "[]" : "{}");
		return;
	}
	if (compact)
	{
		out.append_char(is_array ? '[' : '{');
		if (is_array)
		{
			foreach (i, &e : *v.dl())
			{
				if (i > 0) out.append_char(',');
				put(out, e, depth + 1, true);
			}
		}
		else
		{
			foreach (i, m : sorted(v))
			{
				if (i > 0) out.append_char(',');
				put_string(out, m.key);
				out.append_char(':');
				put(out, &m.value, depth + 1, true);
			}
		}
		out.append_char(is_array ? ']' : '}');
		return;
	}
	if (put_inline(out, v, depth)) return;
	out.append(is_array ? "[\n" : "{\n");
	sz n = v.len();
	if (is_array)
	{
		foreach (i, &e : *v.dl())
		{
			out.append_char_repeat('\t', (sz)(depth + 1));
			put(out, e, depth + 1, false);
			out.append(i + 1 < n ? ",\n" : "\n");
		}
	}
	else
	{
		foreach (i, m : sorted(v))
		{
			out.append_char_repeat('\t', (sz)(depth + 1));
			put_string(out, m.key);
			out.append(": ");
			put(out, &m.value, depth + 1, false);
			out.append(i + 1 < n ? ",\n" : "\n");
		}
	}
	out.append_char_repeat('\t', (sz)depth);
	out.append_char(is_array ? ']' : '}');
}

// Appends `v` as JSON: objects' keys sorted, one entry a line where it doesn't
// fit on one, a line break at the end. `compact`: on one line, no spaces (for
// the network). (nan and inf can't be written: check first with is_finite.)
fn void write(DString* out, Data* v, bool compact = false) => @pool()
{
	put(out, v, 0, compact);
	if (!compact) out.append_char('\n');
}

// The same, as a new string.
fn String to_string(Allocator allocator, Data* v, bool compact = false)
{
	DString out;
	out.init(mem);
	defer out.free();
	write(&out, v, compact);
	return out.copy_str(allocator);
}

// Whether it holds no nan or inf (which JSON can't hold).
fn bool is_finite(Data* v)
{
	switch (v.type)
	{
		case FLOAT: return !math::is_nan(v.f) && !math::is_inf(v.f);
		case ARRAY:
			foreach (&e : *v.dl())
			{
				if (!is_finite(e)) return false;
			}
			return true;
		case OBJECT:
			foreach (&m : *v.ml())
			{
				if (!is_finite(&m.value)) return false;
			}
			return true;
		default: return true;
	}
}

// Writes it next to `path` and moves it over (never half a file). False on failure.
fn bool save_file(String path, Data* v) => @pool()
{
	if (!is_finite(v)) return false;
	DString text;
	text.tinit();
	write(&text, v);
	return fs::save_atomically(path, text.str_view());
}
