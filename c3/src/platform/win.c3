// The Win32 calls the game makes (kernel32, user32, gdi32, ...), declared as
// Windows declares them. Strings are UTF-16 (WString): to_wstring() them.
module gocube::win @link("kernel32");
import std::io;

alias Handle = void*;
alias Bool = int;
alias Dword = uint;
alias Hresult = int;
alias WChar = ushort;

const Dword MOVEFILE_REPLACE_EXISTING = 0x1;
const Dword MOVEFILE_WRITE_THROUGH = 0x8;

extern fn Bool move_file_ex(WString from, WString to, Dword flags) @cname("MoveFileExW");
extern fn Dword get_last_error() @cname("GetLastError");
extern fn void output_debug_string(WString s) @cname("OutputDebugStringW");
extern fn Bool query_performance_counter(long* count) @cname("QueryPerformanceCounter");
extern fn Bool query_performance_frequency(long* freq) @cname("QueryPerformanceFrequency");
extern fn void sleep(Dword ms) @cname("Sleep");
extern fn Dword get_current_process_id() @cname("GetCurrentProcessId");

fn bool succeeded(Hresult hr) @inline => hr >= 0;
fn bool failed(Hresult hr) @inline => hr < 0;

struct SystemTime
{
	ushort year, month, day_of_week, day, hour, minute, second, milliseconds;
}

extern fn void get_local_time(SystemTime* t) @cname("GetLocalTime");

// "YYYY-MM-DDTHH:MM:SS", local time (in the temp allocator).
fn String now_string()
{
	SystemTime t;
	get_local_time(&t);
	return string::tformat("%04d-%02d-%02dT%02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
}
