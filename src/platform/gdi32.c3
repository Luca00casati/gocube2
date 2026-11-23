// gdi32: fonts and glyph outlines (the UI's text), as Windows declares them.
module gocube::win @link("gdi32");

struct TextMetric
{
	int height, ascent, descent, internal_leading, external_leading, ave_char_width, max_char_width, weight, overhang,
		digitized_aspect_x, digitized_aspect_y;
	WChar first_char, last_char, default_char, break_char;
	char italic, underlined, struck_out, pitch_and_family, char_set;
}

struct GlyphMetrics
{
	uint black_box_x, black_box_y;
	Point glyph_origin;
	short cell_inc_x, cell_inc_y;
}

struct Fixed
{
	ushort fract;
	short value;
}

struct Mat2
{
	Fixed m11, m12, m21, m22;
}

const Mat2 MAT2_IDENTITY = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
const uint GGO_GRAY8_BITMAP = 6;
const Dword GDI_ERROR = 0xFFFFFFFF;
const int FW_NORMAL = 400;
const Dword DEFAULT_CHARSET = 1;
const Dword OUT_TT_PRECIS = 4;
const Dword CLIP_DEFAULT_PRECIS = 0;
const Dword ANTIALIASED_QUALITY = 4;

extern fn Handle create_compatible_dc(Handle dc) @cname("CreateCompatibleDC");
extern fn Bool delete_dc(Handle dc) @cname("DeleteDC");
extern fn Handle create_font(int height, int width, int escapement, int orientation, int weight, Dword italic, Dword underline, Dword strike_out,
	Dword char_set, Dword out_precision, Dword clip_precision, Dword quality, Dword pitch_and_family, WString face) @cname("CreateFontW");
extern fn Handle select_object(Handle dc, Handle object) @cname("SelectObject");
extern fn Bool delete_object(Handle object) @cname("DeleteObject");
extern fn Bool get_text_metrics(Handle dc, TextMetric* tm) @cname("GetTextMetricsW");
extern fn Dword get_glyph_outline(Handle dc, uint ch, uint format, GlyphMetrics* gm, Dword size, void* buffer, Mat2* mat) @cname("GetGlyphOutlineW");
