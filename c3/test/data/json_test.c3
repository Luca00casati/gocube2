// JSON: written and read back the same; only data. (tests/test_save.cpp's JSON cases)
module gocube::data::json_test;
import gocube::data, gocube::data::json, std::io;

fn Data parse_ok(String text)
{
	Data d;
	DString why;
	defer why.free();
	assert(json::parse(text, "t", &d, &why), "%s", why.str_view());
	return d;
}

fn bool parses(String text)
{
	Data d;
	DString why;
	defer why.free();
	bool ok = json::parse(text, "t", &d, &why);
	d.free();
	return ok;
}

fn void written_and_read_back() @test
{
	Data d = data::object();
	defer d.free();
	d.set_long("int", 42);
	d.set_long("negative", -7);
	d.set_double("float", 0.1);
	d.set_double("whole_float", 2.0);
	d.set_long("big", 4294967295);
	d.set_string("text", "quote \" backslash \\ newline \n tab \t bell \x07 café");
	d.set_bool("yes", true);
	d.set_bool("no", false);
	d.set("nothing", data::NULL_DATA);
	Data list = data::array();
	list.push_long(1);
	list.push_string("two");
	list.push(data::NULL_DATA);
	Data inner = data::array();
	inner.push_double(3.5);
	inner.push(data::boolean(false));
	list.push(inner);
	d.set("list", list);
	d.set("empty list", data::array());
	d.set("empty object", data::object());
	Data deepest = data::object();
	Data x = data::array();
	x.push_string("x");
	deepest.set("deepest", x);
	Data nested = data::object();
	nested.set("deeper", deepest);
	d.set("nested", nested);
	foreach (compact : (bool[2]){ false, true })
	{
		String text = json::to_string(mem, &d, compact);
		defer free(text.ptr);
		assert(text.starts_with("{"));
		assert(text.contains_char('\n') != compact);
		Data back = parse_ok(text);
		defer back.free();
		assert(back.same_as(&d));
		assert(back.get("float").type == FLOAT);
		assert(back.get("whole_float").type == FLOAT);
		assert(back.get("int").type == INTEGER);
		assert(back.get("empty list").is_array());
		assert(back.get("empty object").is_object());
		assert(back.get("list").at(2).is_null());
	}
	Data pair = data::array();
	defer pair.free();
	pair.push_long(1);
	pair.push_long(2);
	String s = json::to_string(mem, &pair, true);
	defer free(s.ptr);
	assert(s == "[1,2]");
	// The C++ game's layout: sorted keys, short values on one line.
	Data small = data::object();
	defer small.free();
	small.set_long("b", 2);
	small.set_double("a", 0.5);
	String t = json::to_string(mem, &small);
	defer free(t.ptr);
	assert(t == "{\"a\": 0.5, \"b\": 2}\n", "%s", t);
}

fn void what_it_refuses() @test
{
	// Not JSON: code, comments, trailing commas, single quotes.
	foreach (bad : (String[]){ "return {a = 1}", "{\"a\": 1} // note", "[1, 2,]", "{'a': 1}", "", "{", "[1 2]",
		"{\"a\" 1}", "{1: 2}", "01", "1.", ".5", "-", "+1", "1e", "0x10", "\"\\q\"", "\"\\u12\"", "\"\\ud800\"",
		"\"tab\there\"", "tru", "nul", "1e999", "NaN", "{\"a\": 1} {}" })
	{
		assert(!parses(bad), "%s parsed", bad);
	}
	Data d;
	DString why;
	defer why.free();
	assert(!json::parse("{\n\n  \"a\": \"open}", "t", &d, &why));
	assert(why.str_view().starts_with("t:3:"), "%s", why.str_view());
}

fn void what_it_allows() @test
{
	Data d = parse_ok("\xEF\xBB\xBF {\n\"a\": 1, \"b\": -0.5e1, \"c\": 1E2, \"big\": 9223372036854775808,\n"
		"\"esc\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\\u00e9\\ud83d\\ude00\",\n"
		"\"twice\": 1, \"twice\": 2, \"list\": [10, 20, 30], \"none\": null, \"empty\": {}, \"nothing\": []}");
	defer d.free();
	assert(d.get("a").as_long() == 1);
	assert(d.get("b").as_double() == -5.0);
	assert(d.get("c").type == FLOAT);
	assert(d.get("big").type == FLOAT); // (too large for an integer)
	assert(d.str("esc") == "\"\\/\b\f\n\r\tA\u00e9\U0001F600");
	assert(d.get("twice").as_long() == 2); // (the last wins)
	assert(d.get("list").len() == 3 && d.get("list").at(2).as_long() == 30);
	assert(d.has("none") && d.get("none").is_null());
	assert(d.get("empty").is_object() && d.get("nothing").is_array());
	assert(d.fields()[0].key == "a" && d.fields()[1].key == "b"); // (written order)
	Data five = parse_ok(" 5 ");
	assert(five.as_long() == 5);
}
