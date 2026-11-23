// Files: written whole or not at all. (Paths are UTF-8.)
module gocube::fs;
import std::io, gocube::win;

// Writes `path` whole or not at all: a temporary file, then put in its place.
fn bool save_atomically(String path, char[] data) => @pool()
{
	String tmp = path.tconcat(".tmp");
	if (catch file::save(tmp, data))
	{
		(void)file::delete(tmp);
		return false;
	}
	WString? from = tmp.to_temp_wstring();
	WString? to = path.to_temp_wstring();
	if (catch from) return false;
	if (catch to) return false;
	return win::move_file_ex(from, to, win::MOVEFILE_REPLACE_EXISTING | win::MOVEFILE_WRITE_THROUGH) != 0;
}
