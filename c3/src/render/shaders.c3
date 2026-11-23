// The shaders: their HLSL (shaders/) embedded in the game, compiled on first
// use by Windows' own shader compiler (d3dcompiler_47.dll, part of Windows 10
// and 11), so building the game needs nothing but c3c. Each is compiled once
// per run and kept until the game ends. Main thread only.
module gocube::shaders @link("d3dcompiler");
import gocube::log, std::io;

enum Shader : char
{
	CHUNK_VS,
	CHUNK_PS_OPAQUE,
	CHUNK_PS_CUTOUT,
	CHUNK_PS_FLUID,
	LINES_VS,
	LINES_PS,
	SKY_VS,
	SKY_PS,
	ITEMS_VS,
	ITEMS_PS,
	CREATURES_VS,
	CREATURES_PS,
	BLIT_VS,
	BLIT_PS,
	GUI_VS,
	GUI_PS,
}

const String CHUNK = $embed("../../../shaders/chunk.hlsl");
const String LINES = $embed("../../../shaders/lines.hlsl");
const String SKY = $embed("../../../shaders/sky.hlsl");
const String ITEMS = $embed("../../../shaders/items.hlsl");
const String CREATURES = $embed("../../../shaders/creatures.hlsl");
const String BLIT = $embed("../../../shaders/blit.hlsl");
const String GUI = $embed("../../../shaders/gui.hlsl");
// What `#include "color.hlsli"` brings in.
const String COLOR = $embed("../../../shaders/color.hlsli");

struct Source
{
	String file, text, entry, profile;
}

// In Shader's order.
const Source[*] SOURCES = {
	{ "chunk.hlsl", CHUNK, "vs_main", "vs_5_0" },
	{ "chunk.hlsl", CHUNK, "ps_opaque", "ps_5_0" },
	{ "chunk.hlsl", CHUNK, "ps_cutout", "ps_5_0" },
	{ "chunk.hlsl", CHUNK, "ps_fluid", "ps_5_0" },
	{ "lines.hlsl", LINES, "vs_main", "vs_5_0" },
	{ "lines.hlsl", LINES, "ps_main", "ps_5_0" },
	{ "sky.hlsl", SKY, "vs_main", "vs_5_0" },
	{ "sky.hlsl", SKY, "ps_main", "ps_5_0" },
	{ "items.hlsl", ITEMS, "vs_main", "vs_5_0" },
	{ "items.hlsl", ITEMS, "ps_main", "ps_5_0" },
	{ "creatures.hlsl", CREATURES, "vs_main", "vs_5_0" },
	{ "creatures.hlsl", CREATURES, "ps_main", "ps_5_0" },
	{ "blit.hlsl", BLIT, "vs_main", "vs_5_0" },
	{ "blit.hlsl", BLIT, "ps_main", "ps_5_0" },
	{ "gui.hlsl", GUI, "vs_main", "vs_5_0" },
	{ "gui.hlsl", GUI, "ps_main", "ps_5_0" },
};

// ID3DBlob.
struct BlobVtbl
{
	void* query_interface;
	void* add_ref;
	void* release;
	void* get_buffer_pointer;
	void* get_buffer_size;
}

struct Blob
{
	BlobVtbl* vt;
}

alias BlobPointerFn @private = fn void*(void*);
alias BlobSizeFn @private = fn usz(void*);
alias BlobReleaseFn @private = fn uint(void*);

fn char[] Blob.bytes(&self) => ((char*)((BlobPointerFn)self.vt.get_buffer_pointer)(self))[:((BlobSizeFn)self.vt.get_buffer_size)(self)];

const uint OPTIMIZATION_LEVEL3 = 1 << 15;
const uint WARNINGS_ARE_ERRORS = 1 << 18;

extern fn int d3d_compile(void* src, usz size, ZString name, void* defines, void* include, ZString entry, ZString target,
	uint flags1, uint flags2, Blob** code, Blob** errors) @cname("D3DCompile");

// The compiled shaders (null: not yet). Never released: they last the run.
Blob*[Shader::values.len] compiled @private;

// The bytecode of a shader, compiled now if it hasn't been (empty if it
// doesn't compile: the error is logged, and creating the shader fails).
fn char[] code(Shader s) => @pool()
{
	if (compiled[s]) return compiled[s].bytes();
	Source src = SOURCES[s];
	String text = src.text.treplace("#include \"color.hlsli\"", COLOR);
	Blob* blob;
	Blob* errors;
	int hr = d3d_compile(text.ptr, text.len, src.file.zstr_tcopy(), null, null, src.entry.zstr_tcopy(), src.profile.zstr_tcopy(),
		OPTIMIZATION_LEVEL3 | WARNINGS_ARE_ERRORS, 0, &blob, &errors);
	if (errors)
	{
		char[] message = errors.bytes();
		log::log("Shader %s (%s): %s", src.file, src.entry, (String)message);
		((BlobReleaseFn)errors.vt.release)(errors);
	}
	if (hr < 0 || !blob)
	{
		log::log("The shader %s (%s) didn't compile (0x%08x)", src.file, src.entry, (uint)hr);
		return {};
	}
	compiled[s] = blob;
	return blob.bytes();
}
