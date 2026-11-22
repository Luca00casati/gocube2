// GoCube's build: no build system, no configure step. nob.c calls the
// compiler, linker and shader compiler (cl, link, fxc) directly, with the
// help of nob.h (https://github.com/tsoding/nob.h, MIT / public domain).
//
// From a Developer PowerShell for VS (cl, link and fxc on the path), in
// this folder:
//
//     cl nob.c                     # once: nob rebuilds itself when nob.c changes
//     nob [CONFIG] [unity]         # build (Dev unless CONFIG says otherwise)
//     nob test [CONFIG] [unity]    # build, then run the tests
//     nob run [CONFIG] [-- ARGS]   # build, then run the game
//     nob clean                    # delete build\
//
// CONFIG is one of:
//   release   optimized (/O2): the game as players get it; slow to compile
//   dev       not optimized, but without Debug's checks: compiles about four
//             times faster than Release, runs well enough to play and test
//   debug     not optimized, with the debug CRT's checks (iterators, the heap,
//             /RTC1) and Direct3D's debug layer: the slowest to run
// unity: compiles a few files that #include several sources each (up to
// UNITY_SIZE), so the standard library's templates are compiled once for
// all of them instead of once per file: much faster from scratch (CI), but a
// change compiles the whole batch again. It builds into its own folder
// (build\<Config>-Unity) so its objects never mix with the others.
//
// What it builds, for a configuration, in build\<Config>:
//   gocube.exe                  the game
//   mods\                       mods\ copied, and the base game's core.dll
//                               built into mods\core (src/core_mod)
//   gocube_tests.exe            the unit tests
//   gocube_mp_smoke.exe         multiplayer in two processes
//   test_mods\                  the test mods: kit_mods\kit (with kit.dll)
//                               and dlls\NAME.dll (tests/mods/test_mod.cpp)
// Shaders are compiled by fxc into headers (build\shaders). An object file is
// compiled again when its source, nob itself, the precompiled header, or one of
// the headers it included is newer: the compiler lists those for each object
// (/sourceDependencies, a .json next to it), so changing a header compiles
// only the files that use it. src/pch.h, the standard library headers, is
// precompiled once per configuration and included in every file (/FI).

#define NOB_IMPLEMENTATION
#include "nob.h"

#define BUILD "build"
#define VERSION "0.1.0"

// --- What there is to build -------------------------------------------------------

// Everything that doesn't touch the window or the GPU: shared by the game and the tests.
static const char *CORE_SOURCES[] = {
	"src/audio/mixing.cpp",
	"src/content/content.cpp",
	"src/content/image.cpp",
	"src/content/item_models.cpp",
	"src/content/skin.cpp",
	"src/data/data.cpp",
	"src/data/json.cpp",
	"src/game/block_entities.cpp",
	"src/game/blocks.cpp",
	"src/game/commands.cpp",
	"src/game/creature_models.cpp",
	"src/game/creatures.cpp",
	"src/game/drops.cpp",
	"src/game/dye.cpp",
	"src/game/figure.cpp",
	"src/game/game.cpp",
	"src/game/game_dye.cpp",
	"src/game/game_health.cpp",
	"src/game/hand.cpp",
	"src/game/inventory.cpp",
	"src/game/player.cpp",
	"src/game/settings.cpp",
	"src/game/vitals.cpp",
	"src/mods/mod_pack.cpp",
	"src/mods/mods.cpp",
	"src/net/session.cpp",
	"src/net/transport.cpp",
	"src/platform/compression.cpp",
	"src/profiler.cpp",
	"src/ui/gui.cpp",
	"src/voxel/generator.cpp",
	"src/voxel/light.cpp",
	"src/voxel/mesher.cpp",
	"src/voxel/storage.cpp",
	"src/world/save.cpp",
	"src/world/world.cpp",
};

// The game itself: the window, the GPU, sound, the UI.
static const char *GAME_SOURCES[] = {
	"src/audio/audio.cpp",
	"src/main.cpp",
	"src/platform/window.cpp",
	"src/render/chunk_renderer.cpp",
	"src/render/renderer.cpp",
	"src/ui/gui_d2d.cpp",
	"src/ui/screens.cpp",
	"src/ui/settings_screen.cpp",
};

// What a mod's DLL links besides its own code: the data value and JSON (src/mods/mod_sdk.h).
static const char *SDK_SOURCES[] = {
	"src/data/data.cpp",
	"src/data/json.cpp",
};

static const char *TEST_SOURCES[] = {
	"tests/main.cpp",
	"tests/test_audio.cpp",
	"tests/test_commands.cpp",
	"tests/test_dye.cpp",
	"tests/test_gui.cpp",
	"tests/test_image.cpp",
	"tests/test_mods.cpp",
	"tests/test_multiplayer.cpp",
	"tests/test_player.cpp",
	"tests/test_rendering.cpp",
	"tests/test_save.cpp",
	"tests/test_survival.cpp",
	"tests/test_transport.cpp",
	"tests/test_worldgen.cpp",
};

// Compiled optimized even in Dev: small files where the game (and the tests)
// spend most of their time, generating, lighting and meshing chunks and
// handling data. Without them, Dev's tests take twice as long. (Without the
// precompiled header: its optimization options must match the file's.)
static const char *OPTIMIZED_IN_DEV[] = {
	"src/data/data.cpp",
	"src/data/json.cpp",
	"src/voxel/generator.cpp",
	"src/voxel/light.cpp",
	"src/voxel/mesher.cpp",
	"src/voxel/storage.cpp",
	"src/world/world.cpp",
};

// The mods in tests/mods/test_mod.cpp: compiled once, linked into a DLL each.
static const char *TEST_MODS[] = {"fine", "failing", "crasher", "counter", "flaky", "crashy", "builder", "greeter"};

typedef struct {
	const char *file, *entry, *profile, *name;
} Shader;

static const Shader SHADERS[] = {
	{"shaders/chunk.hlsl", "vs_main", "vs_5_0", "chunk_vs"},
	{"shaders/chunk.hlsl", "ps_opaque", "ps_5_0", "chunk_ps_opaque"},
	{"shaders/chunk.hlsl", "ps_cutout", "ps_5_0", "chunk_ps_cutout"},
	{"shaders/chunk.hlsl", "ps_fluid", "ps_5_0", "chunk_ps_fluid"},
	{"shaders/lines.hlsl", "vs_main", "vs_5_0", "lines_vs"},
	{"shaders/lines.hlsl", "ps_main", "ps_5_0", "lines_ps"},
	{"shaders/sky.hlsl", "vs_main", "vs_5_0", "sky_vs"},
	{"shaders/sky.hlsl", "ps_main", "ps_5_0", "sky_ps"},
	{"shaders/items.hlsl", "vs_main", "vs_5_0", "items_vs"},
	{"shaders/items.hlsl", "ps_main", "ps_5_0", "items_ps"},
	{"shaders/creatures.hlsl", "vs_main", "vs_5_0", "creatures_vs"},
	{"shaders/creatures.hlsl", "ps_main", "ps_5_0", "creatures_ps"},
	{"shaders/blit.hlsl", "vs_main", "vs_5_0", "blit_vs"},
	{"shaders/blit.hlsl", "ps_main", "ps_5_0", "blit_ps"},
};

// Windows' own libraries the executables link.
static const char *SYSTEM_LIBS[] = {
	"kernel32.lib", "user32.lib", "gdi32.lib", "advapi32.lib", "shell32.lib", "ole32.lib", "oleaut32.lib", "uuid.lib",
	"bcrypt.lib", "ws2_32.lib", "winmm.lib", "cabinet.lib", "windowscodecs.lib",
};
static const char *GAME_LIBS[] = {"d2d1.lib", "dwrite.lib", "xaudio2.lib", "d3d11.lib", "dxgi.lib", "dxguid.lib"};

// --- Helpers ----------------------------------------------------------------------

typedef enum { RELEASE, DEV, DEBUG } Config;
static Config config = DEV;
static bool debug = false;           // config == DEBUG
static bool unity = false;           // a unity build (see the top)
#define UNITY_SIZE 6                 // the most sources one unity file includes
static const char *out_dir;          // build\<Config>[-Unity]
static const char *root;             // this folder, with '/'
static uint64_t write_time(const char *path);
static uint64_t nob_time;            // nob's own time: when it changed, maybe the flags did (all is built again)
static const char *pch_file;         // the precompiled header (obj\pch.pch)
static const char *pch_obj;          // and its object, linked into everything built with it

static const char *config_name(void) {
	return config == DEBUG ? "Debug" : config == DEV ? "Dev" : "Release";
}

static const char *path_join(const char *a, const char *b) {
	return temp_sprintf("%s/%s", a, b);
}

// MSVC's linker: next to cl.exe (a Unix-like shell's PATH may have another `link` first).
static const char *linker(void) {
	static char path[MAX_PATH] = {0};
	if (!path[0]) {
		char *file = NULL;
		if (SearchPathA(NULL, "cl.exe", NULL, MAX_PATH, path, &file) == 0 || !file) {
			return "link.exe";
		}
		strcpy(file, "link.exe");
	}
	return path;
}

static bool is_dir(const char *path) {
	const DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// A folder and every folder on the way to it.
static bool make_dirs(const char *path) {
	if (is_dir(path)) {
		return true;
	}
	char *p = temp_strdup(path);
	for (char *c = p; *c; c++) {
		if ((*c == '/' || *c == '\\') && c != p) {
			const char saved = *c;
			*c = '\0';
			if (!is_dir(p) && !mkdir_if_not_exists(p)) {
				return false;
			}
			*c = saved;
		}
	}
	return mkdir_if_not_exists(p);
}

static bool ends_with(const char *s, const char *end) {
	const size_t n = strlen(s), m = strlen(end);
	return n >= m && strcmp(s + n - m, end) == 0;
}

// The object file for a source in a group ("core", "game"...).
static const char *object_for(const char *group, const char *source) {
	char *name = temp_strdup(source);
	for (char *c = name; *c; c++) {
		if (*c == '/' || *c == '\\') {
			*c = '_';
		}
	}
	return temp_sprintf("%s/obj/%s/%s.obj", out_dir, group, name);
}

// Compiler options every C++ file gets. `optimized`: a Dev file in OPTIMIZED_IN_DEV.
static void cl_common(Cmd *cmd, bool optimized) {
	cmd_append(cmd, "cl.exe", "/nologo", "/c", "/std:c++20", "/W4", "/permissive-", "/utf-8", "/EHsc", "/Z7", "/GR");
	cmd_append(cmd, "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/Isrc");
	// Reproducible objects (no time stamps): a mod's DLL must be the same
	// from the same sources (its fingerprint), and nothing else minds.
	cmd_append(cmd, "/Brepro", "/FIpch.h");
	// (GOCUBE_UNOPTIMIZED: for the tests' timing limits.)
	if (config == DEBUG) {
		cmd_append(cmd, "/Od", "/Ob0", "/RTC1", "/MTd", "/D_DEBUG", "/DGOCUBE_UNOPTIMIZED");
	} else if (config == DEV) {
		cmd_append(cmd, optimized ? "/O2" : "/Od", "/MT", "/DNDEBUG", "/DGOCUBE_UNOPTIMIZED");
	} else {
		cmd_append(cmd, "/O2", "/Ob2", "/MT", "/DNDEBUG");
	}
}

// A file's last write time (0 if it isn't there). (Asking without opening it:
// nob's needs_rebuild opens files, which fails while compilers read them.)
static uint64_t write_time(const char *path) {
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
		return 0;
	}
	return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

// Whether `output` is missing or older than any input (or `also`, a time).
static bool stale(const char *output, const char **inputs, size_t count, uint64_t also) {
	const uint64_t out = write_time(output);
	if (out == 0 || also > out) {
		return true;
	}
	for (size_t i = 0; i < count; i++) {
		if (write_time(inputs[i]) > out) {
			return true;
		}
	}
	return false;
}

// Whether `path` is in this folder (a header of ours; the compiler's and
// Windows' headers are taken as unchanged). Case and slashes don't matter.
static bool ours(const char *path) {
	const size_t n = strlen(root);
	for (size_t i = 0; i < n; i++) {
		char a = path[i], b = root[i];
		if (!a) {
			return false;
		}
		a = a == '\\' ? '/' : (char)tolower((unsigned char)a);
		b = b == '\\' ? '/' : (char)tolower((unsigned char)b);
		if (a != b) {
			return false;
		}
	}
	return path[n] == '/' || path[n] == '\\';
}

// Whether an object is out of date: missing, or older than its source, nob,
// the precompiled header, or a header of ours it included (the list the
// compiler wrote: {"Data": {"Includes": ["c:\\...\\data.h", ...], ...}}).
static bool object_stale(const char *obj, const char *source) {
	const uint64_t out = write_time(obj);
	const char *deps = temp_sprintf("%s.json", obj);
	if (out == 0 || nob_time > out || write_time(source) > out || write_time(pch_file) > out || write_time(deps) == 0) {
		return true;
	}
	String_Builder sb = {0};
	if (!read_entire_file(deps, &sb)) {
		return true;
	}
	sb_append_null(&sb);
	bool result = false;
	const char *p = strstr(sb.items, "\"Includes\"");
	const char *end = p ? strchr(p, ']') : NULL;
	if (!p || !end) {
		result = true;
	} else {
		p = strchr(p, '[');
		char path[4 * MAX_PATH];
		while (!result && (p = strchr(p + 1, '"')) != NULL && p < end) {
			size_t n = 0;
			for (p++; *p && *p != '"'; p++) {
				if (*p == '\\' && p[1]) {
					p++; // (\\ and \")
				}
				if (n + 1 < sizeof(path)) {
					path[n++] = *p;
				}
			}
			path[n] = '\0';
			result = ours(path) && write_time(path) > out;
		}
	}
	sb_free(sb);
	return result;
}

typedef void (*Flags)(Cmd *cmd);

// The compilers running: every group's in one pool (as many at once as
// there are cores, and one more), waited for before anything is linked.
static Procs linking;
static Procs compiling = {0};

// Writes `text` to `path` unless it already holds exactly that (so its time,
// and what was compiled from it, stays as it was).
static bool write_if_changed(const char *path, const char *text) {
	String_Builder old = {0};
	const size_t n = strlen(text);
	if (write_time(path) != 0 && read_entire_file(path, &old) && old.count == n && memcmp(old.items, text, n) == 0) {
		sb_free(old);
		return true;
	}
	sb_free(old);
	return write_entire_file(path, text, n);
}

// For a unity build: the group's sources as a few files that #include them
// (build\<Config>-Unity\unity\GROUP_N.cpp), in `files`.
static bool unity_files(const char *group, const char **sources, size_t count, File_Paths *files) {
	const char *dir = temp_sprintf("%s/unity", out_dir);
	if (!make_dirs(dir)) {
		return false;
	}
	const size_t n = (count + UNITY_SIZE - 1) / UNITY_SIZE;
	for (size_t k = 0; k < n; k++) {
		String_Builder sb = {0};
		sb_append_cstr(&sb, "// A unity build's batch: written by nob.c.\n");
		for (size_t i = k * count / n; i < (k + 1) * count / n; i++) {
			sb_append_cstr(&sb, temp_sprintf("#include \"%s/%s\"\n", root, sources[i]));
		}
		sb_append_null(&sb);
		const char *path = temp_sprintf("%s/%s_%zu.cpp", dir, group, k + 1);
		const bool ok = write_if_changed(path, sb.items);
		sb_free(sb);
		if (!ok) {
			return false;
		}
		da_append(files, _strdup(path));
	}
	return true;
}

// Starts compiling a group's sources into its objects, those that are out of
// date; adds every object to `objects`. (Waited for with procs_flush(&compiling).)
static bool compile(const char *group, const char **sources, size_t count, Flags flags, File_Paths *objects) {
	if (!make_dirs(temp_sprintf("%s/obj/%s", out_dir, group))) {
		return false;
	}
	// The files in OPTIMIZED_IN_DEV on their own: optimized in Dev, never in a
	// unity batch (in any configuration, so the batches are the same in all).
	File_Paths plain = {0}, optimized = {0}, batches = {0};
	for (size_t i = 0; i < count; i++) {
		bool fast = false;
		for (size_t j = 0; (config == DEV || unity) && j < ARRAY_LEN(OPTIMIZED_IN_DEV); j++) {
			fast = fast || strcmp(sources[i], OPTIMIZED_IN_DEV[j]) == 0;
		}
		da_append(fast ? &optimized : &plain, sources[i]);
	}
	File_Paths *rest = &plain;
	if (unity && plain.count > 1) {
		if (!unity_files(group, plain.items, plain.count, &batches)) {
			return false;
		}
		rest = &batches;
	}
	Cmd cmd = {0};
	bool ok = true;
	for (size_t i = 0; i < rest->count + optimized.count; i++) {
		const bool fast = i >= rest->count && config == DEV;
		const char *source = i >= rest->count ? optimized.items[i - rest->count] : rest->items[i];
		const char *obj = object_for(group, source);
		da_append(objects, _strdup(obj));
		if (!object_stale(obj, source)) {
			continue;
		}
		cl_common(&cmd, fast);
		if (flags) {
			flags(&cmd);
		}
		if (!fast) {
			cmd_append(&cmd, "/Yupch.h", temp_sprintf("/Fp%s", pch_file));
		}
		cmd_append(&cmd, "/sourceDependencies", temp_sprintf("%s.json", obj));
		cmd_append(&cmd, temp_sprintf("/Fo%s", obj), source);
		if (!cmd_run(&cmd, .async = &compiling)) {
			ok = false;
		}
	}
	cmd_free(cmd);
	da_free(plain);
	da_free(optimized);
	da_free(batches);
	return ok;
}

// Links objects into `output` if any is newer (or nob is: its flags may
// have changed). The links run at once: wait for them with procs_flush(&linking). A DLL may export a mod's functions under the names the game
// looks for (`exports`: PREFIX gives PREFIX_load and PREFIX_unload).
static bool link_target(const char *output, File_Paths *objects, const char **libs, size_t lib_count, bool dll,
		bool windows_app, const char *exports) {
	if (!stale(output, objects->items, objects->count, nob_time)) {
		return true;
	}
	const char *name = path_name(output);
	Cmd cmd = {0};
	cmd_append(&cmd, linker(), "/nologo", "/DEBUG", "/INCREMENTAL:NO", temp_sprintf("/OUT:%s", output));
	cmd_append(&cmd, temp_sprintf("/PDB:%s/pdb/%s.pdb", out_dir, name));
	if (dll) {
		// Reproducible: the same sources give the same DLL, so the same mod's
		// fingerprint (players must match) doesn't change between builds.
		cmd_append(&cmd, "/DLL", "/Brepro", "/PDBALTPATH:%_PDB%", temp_sprintf("/IMPLIB:%s/lib/%s.lib", out_dir, name));
		if (exports) {
			cmd_append(&cmd, temp_sprintf("/EXPORT:gocube_mod_load=%s_load", exports),
					temp_sprintf("/EXPORT:gocube_mod_unload=%s_unload", exports));
		}
	} else if (windows_app) {
		cmd_append(&cmd, "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup");
	} else {
		cmd_append(&cmd, "/SUBSYSTEM:CONSOLE");
	}
	if (config == RELEASE) {
		cmd_append(&cmd, "/OPT:REF", "/OPT:ICF");
	}
	da_append_many(&cmd, objects->items, objects->count);
	da_append_many(&cmd, libs, lib_count);
	const bool ok = cmd_run(&cmd, .async = &linking);
	cmd_free(cmd);
	return ok;
}

// Copies a folder's files into another where they're newer (or missing), and
// deletes the files there that the source no longer has (a mod's fingerprint
// covers every file in its folder), except the DLLs built into it.
static bool copy_newer(const char *from, const char *to) {
	File_Paths children = {0};
	if (!read_entire_dir(from, &children)) {
		return false;
	}
	if (!make_dirs(to)) {
		return false;
	}
	bool ok = true;
	for (size_t i = 0; i < children.count && ok; i++) {
		const char *name = children.items[i];
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			continue;
		}
		const char *src = temp_strdup(path_join(from, name));
		const char *dst = temp_strdup(path_join(to, name));
		if (get_file_type(src) == NOB_FILE_DIRECTORY) {
			ok = copy_newer(src, dst);
		} else if (stale(dst, &src, 1, 0)) {
			ok = copy_file(src, dst);
		}
	}
	da_free(children);
	File_Paths there = {0};
	if (ok && read_entire_dir(to, &there)) {
		for (size_t i = 0; i < there.count && ok; i++) {
			const char *name = there.items[i];
			const char *dst = temp_strdup(path_join(to, name));
			if (get_file_type(dst) == NOB_FILE_REGULAR && !ends_with(name, ".dll") &&
					!file_exists(path_join(from, name))) {
				ok = delete_file(dst);
			}
		}
		da_free(there);
	}
	return ok;
}

// --- The parts ------------------------------------------------------------------

static bool shaders(void) {
	if (!make_dirs(BUILD "/shaders")) {
		return false;
	}
	Procs procs = {0};
	Cmd cmd = {0};
	bool ok = true;
	for (size_t i = 0; i < ARRAY_LEN(SHADERS); i++) {
		const Shader *s = &SHADERS[i];
		const char *out = temp_sprintf(BUILD "/shaders/%s.h", s->name);
		const char *inputs[] = {s->file, "shaders/color.hlsli"};
		if (!stale(out, inputs, ARRAY_LEN(inputs), 0)) {
			continue;
		}
		cmd_append(&cmd, "fxc.exe", "/nologo", "/T", s->profile, "/E", s->entry, "/O3", "/WX", "/Vn",
				temp_sprintf("g_%s", s->name), "/Fh", out, s->file);
		if (!cmd_run(&cmd, .async = &procs)) {
			ok = false;
		}
	}
	return procs_flush(&procs) && ok;
}

static void game_flags(Cmd *cmd) {
	cmd_append(cmd, "/I" BUILD "/shaders", "/DGOCUBE_VERSION=\"" VERSION "\"");
	if (debug) {
		cmd_append(cmd, "/DGOCUBE_DEBUG");
	}
}

// The precompiled header (src/pch.h), with the options every file gets
// (they must match). Built again when pch.h or nob changes.
static bool precompiled_header(void) {
	pch_file = temp_sprintf("%s/obj/pch.pch", out_dir);
	pch_obj = temp_sprintf("%s/obj/pch.obj", out_dir);
	const char *source = temp_sprintf("%s/obj/pch.cpp", out_dir); // (empty: /FI brings pch.h)
	const char *text = "// Builds the precompiled header (src/pch.h); written by nob.c.\n";
	if (!make_dirs(temp_sprintf("%s/obj", out_dir)) ||
			(!file_exists(source) && !write_entire_file(source, text, strlen(text)))) {
		return false;
	}
	const char *inputs[] = {"src/pch.h"};
	if (!stale(pch_file, inputs, 1, nob_time) && write_time(pch_obj) != 0) {
		return true;
	}
	Cmd cmd = {0};
	cl_common(&cmd, false);
	cmd_append(&cmd, "/Ycpch.h", temp_sprintf("/Fp%s", pch_file), temp_sprintf("/Fo%s", pch_obj), source);
	const bool ok = cmd_run(&cmd);
	cmd_free(cmd);
	return ok;
}

static void test_flags(Cmd *cmd) {
	cmd_append(cmd, "/D_CRT_SECURE_NO_WARNINGS", temp_sprintf("/DGOCUBE_SOURCE_DIR=\"%s\"", root),
			temp_sprintf("/DGOCUBE_MODS_DIR=\"%s/%s/mods\"", root, out_dir),
			temp_sprintf("/DGOCUBE_TEST_MODS_DIR=\"%s/%s/test_mods\"", root, out_dir));
}

// A mod's DLL: its objects and the SDK's. `exports`: as for link_target.
static bool mod_dll(File_Paths *objects, File_Paths *sdk, const char *output, const char *exports) {
	File_Paths all = {0};
	da_append_many(&all, objects->items, objects->count);
	da_append_many(&all, sdk->items, sdk->count);
	const bool ok = link_target(output, &all, NULL, 0, true, false, exports);
	da_free(all);
	return ok;
}

static bool build(void) {
	out_dir = temp_sprintf(BUILD "/%s%s", config_name(), unity ? "-Unity" : "");
	nob_log(INFO, "Building %s", out_dir);
	if (!make_dirs(path_join(out_dir, "pdb")) || !make_dirs(path_join(out_dir, "lib"))) {
		return false;
	}
	nob_time = write_time(temp_running_executable_path());
	if (!shaders() || !precompiled_header()) {
		return false;
	}

	// Everything is compiled first, all at once; then linked.
	File_Paths core = {0}, game = {0}, sdk = {0}, tests = {0}, smoke = {0}, core_mod = {0}, kit = {0}, test_mod = {0};
	const char *smoke_src[] = {"tests/mp_smoke.cpp"};
	const char *core_mod_src[] = {"src/core_mod/core_mod.cpp"};
	const char *kit_src[] = {"tests/mods/kit.cpp"};
	const char *test_mod_src[] = {"tests/mods/test_mod.cpp"};
	// (The slowest first, so the last to finish are short: the tests' files
	// and the base game's mod are the biggest.)
	bool ok = compile("tests", TEST_SOURCES, ARRAY_LEN(TEST_SOURCES), test_flags, &tests);
	ok = compile("core_mod", core_mod_src, 1, NULL, &core_mod) && ok;
	ok = compile("mp_smoke", smoke_src, 1, test_flags, &smoke) && ok;
	ok = compile("game", GAME_SOURCES, ARRAY_LEN(GAME_SOURCES), game_flags, &game) && ok;
	ok = compile("core", CORE_SOURCES, ARRAY_LEN(CORE_SOURCES), NULL, &core) && ok;
	ok = compile("test_mods", test_mod_src, 1, NULL, &test_mod) && ok;
	ok = compile("kit_mod", kit_src, 1, NULL, &kit) && ok;
	ok = compile("sdk", SDK_SOURCES, ARRAY_LEN(SDK_SOURCES), NULL, &sdk) && ok;
	ok = procs_flush(&compiling) && ok;
	if (!ok) {
		return false;
	}
	da_append(&core, pch_obj);
	da_append(&sdk, pch_obj);

	// The game, and its mods next to it (the base game's code built into mods\core).
	da_append_many(&game, core.items, core.count);
	da_append_many(&tests, core.items, core.count);
	da_append_many(&smoke, core.items, core.count);
	const char *libs[ARRAY_LEN(SYSTEM_LIBS) + ARRAY_LEN(GAME_LIBS)];
	memcpy((void *)libs, SYSTEM_LIBS, sizeof(SYSTEM_LIBS));
	memcpy((void *)(libs + ARRAY_LEN(SYSTEM_LIBS)), GAME_LIBS, sizeof(GAME_LIBS));
	const char *mods = path_join(out_dir, "mods");
	ok = link_target(path_join(out_dir, "gocube.exe"), &game, libs, ARRAY_LEN(libs), false, config == RELEASE, NULL) &&
			copy_newer("mods", mods) &&
			mod_dll(&core_mod, &sdk, path_join(mods, "core/core.dll"), NULL);

	// The tests and their mods.
	const char *test_mods = path_join(out_dir, "test_mods");
	ok = ok && copy_newer("tests/data/kit_mods", path_join(test_mods, "kit_mods")) &&
			mod_dll(&kit, &sdk, path_join(test_mods, "kit_mods/kit/kit.dll"), NULL) &&
			make_dirs(path_join(test_mods, "dlls"));
	for (size_t i = 0; i < ARRAY_LEN(TEST_MODS) && ok; i++) {
		ok = mod_dll(&test_mod, &sdk, temp_sprintf("%s/dlls/%s.dll", test_mods, TEST_MODS[i]),
				temp_sprintf("test_%s", TEST_MODS[i]));
	}
	ok = ok && link_target(path_join(out_dir, "gocube_tests.exe"), &tests, SYSTEM_LIBS, ARRAY_LEN(SYSTEM_LIBS), false, false, NULL);
	ok = ok && link_target(path_join(out_dir, "gocube_mp_smoke.exe"), &smoke, SYSTEM_LIBS, ARRAY_LEN(SYSTEM_LIBS), false, false, NULL);
	ok = procs_flush(&linking) && ok;
	if (ok) {
		nob_log(INFO, "Built %s", out_dir);
	}
	return ok;
}

// --- Running --------------------------------------------------------------------

// The test runs go two at a time: the multiplayer unit tests (they play in
// real time, so mostly they wait) beside the rest of the unit tests (they
// compute), then the two smaller runs. More at once is no faster on two cores,
// and starves the tests that run against the clock. Every test uses its own
// ports and folders (by process), so they can run side by side.

typedef struct {
	const char *name;
	const char *log; // stdout; stderr is log + ".err"
	Proc proc;
} Test_Run;

// Starts a test run, its output going to build\<Config>\test_logs.
static bool start_test(Test_Run *run, const char *name, Cmd *cmd) {
	run->name = name;
	run->log = temp_sprintf("%s/test_logs/%s.txt", out_dir, name);
	Procs procs = {0};
	const bool ok = cmd_run(cmd, .async = &procs, .stdout_path = run->log,
			.stderr_path = temp_sprintf("%s.err", run->log));
	run->proc = ok ? procs.items[0] : NOB_INVALID_PROC;
	da_free(procs);
	return ok;
}

// Prints a log's lines: all of them, or (`summary`) those from the line
// starting with `from` on.
static void print_log(const char *path, const char *from) {
	String_Builder sb = {0};
	if (!read_entire_file(path, &sb)) {
		return;
	}
	sb_append_null(&sb);
	const char *text = sb.items;
	if (from) {
		const char *at = strstr(text, from);
		text = at ? at : "";
	}
	fputs(text, stdout);
	da_free(sb);
}

// Waits for a test run. Passed: prints the unit tests' summary; failed:
// everything it printed.
static bool finish_test(Test_Run *r) {
	const bool ok = r->proc != NOB_INVALID_PROC && proc_wait(r->proc);
	if (!ok) {
		print_log(r->log, NULL);
		print_log(temp_sprintf("%s.err", r->log), NULL);
	} else if (strncmp(r->name, "unit", 4) == 0) {
		print_log(r->log, "[tests] test cases:");
	}
	fflush(stdout);
	nob_log(ok ? INFO : ERROR, "--- %s: %s (log: %s)", r->name, ok ? "passed" : "FAILED", r->log);
	return ok;
}

static int tests(void) {
	if (!make_dirs(path_join(out_dir, "test_logs"))) {
		return 1;
	}
	const clock_t t0 = clock();
	const char *tests_exe = path_join(out_dir, "gocube_tests.exe");
	Test_Run runs[4] = {0};
	Cmd cmd = {0};
	cmd_append(&cmd, tests_exe, "-tc=multiplayer:*");
	start_test(&runs[0], "unit_multiplayer", &cmd);
	cmd_append(&cmd, tests_exe, "-tce=multiplayer:*");
	start_test(&runs[1], "unit", &cmd);
	int failed = !finish_test(&runs[0]) + !finish_test(&runs[1]);
	// Multiplayer across two real processes (host and client over localhost).
	cmd_append(&cmd, path_join(out_dir, "gocube_mp_smoke.exe"));
	start_test(&runs[2], "multiplayer_processes", &cmd);
	// Opens a hidden window, renders 60 frames on WARP (the software adapter, so it
	// runs without a GPU) and fails if Direct3D 11's debug layer reported anything.
	cmd_append(&cmd, path_join(out_dir, "gocube.exe"), "--warp", "--hidden", "--frames=60");
	start_test(&runs[3], "smoke", &cmd);
	failed += !finish_test(&runs[2]) + !finish_test(&runs[3]);
	cmd_free(cmd);

	const double seconds = (double)(clock() - t0) / CLOCKS_PER_SEC;
	if (failed) {
		nob_log(ERROR, "%d of 4 test runs failed (%s, %.1f s)", failed, config_name(), seconds);
		return 1;
	}
	nob_log(INFO, "All 4 test runs passed (%s, %.1f s)", config_name(), seconds);
	return 0;
}

static void usage(void) {
	fprintf(stderr, "usage: nob [build|test|run|clean] [release|dev|debug] [unity] [-- game arguments]\n");
}

int main(int argc, char **argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);

	// Run from this folder (where nob.c is): every path is relative to it.
	root = temp_strdup(get_current_dir_temp());
	for (char *c = (char *)root; *c; c++) {
		if (*c == '\\') {
			*c = '/';
		}
	}
	if (!file_exists("nob.c") || !file_exists("src/main.cpp")) {
		nob_log(ERROR, "Run nob in GoCube's folder (where nob.c is)");
		return 1;
	}

	const char *action = "build";
	int i = 1;
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		} else if (strcmp(a, "release") == 0) {
			config = RELEASE;
		} else if (strcmp(a, "dev") == 0) {
			config = DEV;
		} else if (strcmp(a, "debug") == 0) {
			config = DEBUG;
		} else if (strcmp(a, "unity") == 0) {
			unity = true;
		} else if (strcmp(a, "build") == 0 || strcmp(a, "test") == 0 || strcmp(a, "run") == 0 ||
				strcmp(a, "clean") == 0) {
			action = a;
		} else {
			usage();
			return 1;
		}
	}

	debug = config == DEBUG;
	if (strcmp(action, "clean") == 0) {
		Cmd cmd = {0};
		cmd_append(&cmd, "cmd.exe", "/c", "if exist " BUILD " rmdir /s /q " BUILD);
		return cmd_run(&cmd) ? 0 : 1;
	}
	if (!build()) {
		nob_log(ERROR, "The build failed");
		return 1;
	}
	if (strcmp(action, "test") == 0) {
		return tests();
	}
	if (strcmp(action, "run") == 0) {
		Cmd cmd = {0};
		cmd_append(&cmd, path_join(out_dir, "gocube.exe"));
		for (; i < argc; i++) {
			cmd_append(&cmd, argv[i]);
		}
		return cmd_run(&cmd) ? 0 : 1;
	}
	return 0;
}
