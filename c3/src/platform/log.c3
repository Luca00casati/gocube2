// Logging to stderr (the console in debug builds) and the debugger's output
// window. (src/log.h)
module gocube::log;
import std::io, gocube::win;

fn void log(String fmt, args...) @format(0) => @pool()
{
	String s = string::tformat(fmt, ...args);
	io::eprintn(s);
	if (try w = s.tconcat("\n").to_temp_wstring()) win::output_debug_string(w);
}
