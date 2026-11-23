// GoCube's build: no build system, no configure step. nob.c compiles the
// shaders with fxc, then has c3c build, test or run the game (c3/project.json,
// into build\c3), with the help of nob.h (https://github.com/tsoding/nob.h,
// MIT / public domain).
//
// From a Developer PowerShell for VS (cl and fxc on the path), in this folder:
//
//     cl nob.c                  # once: nob rebuilds itself when nob.c changes
//     nob                       # build build\c3\gocube.exe
//     nob test                  # build, then run the tests
//     nob run [-- ARGS]         # build, then play (ARGS: see c3/src/app/options.c3)
//     nob clean                 # delete build\
//
// c3c is taken from the path, or from where the C3C environment variable says.
// A shader is compiled again (build\shaders\NAME.cso) when its source or
// shaders/color.hlsli is newer.

#define NOB_IMPLEMENTATION
#include "nob.h"

#define BUILD "build"

typedef struct {
	const char *file, *entry, *profile, *name;
} Shader;

// What c3/src/render/shaders.c3 embeds.
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
	{"shaders/gui.hlsl", "vs_main", "vs_5_0", "gui_vs"},
	{"shaders/gui.hlsl", "ps_main", "ps_5_0", "gui_ps"},
};

// A file's last write time (0 if it isn't there). (Asking without opening it:
// nob's needs_rebuild opens files, which fails while compilers read them.)
static uint64_t write_time(const char *path) {
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
		return 0;
	}
	return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

// Whether `output` is missing or older than any input.
static bool stale(const char *output, const char **inputs, size_t count) {
	const uint64_t out = write_time(output);
	if (out == 0) {
		return true;
	}
	for (size_t i = 0; i < count; i++) {
		if (write_time(inputs[i]) > out) {
			return true;
		}
	}
	return false;
}

static bool shaders(void) {
	if (!mkdir_if_not_exists(BUILD) || !mkdir_if_not_exists(BUILD "/shaders")) {
		return false;
	}
	Procs procs = {0};
	Cmd cmd = {0};
	bool ok = true;
	for (size_t i = 0; i < ARRAY_LEN(SHADERS); i++) {
		const Shader *s = &SHADERS[i];
		const char *out = temp_sprintf(BUILD "/shaders/%s.cso", s->name);
		const char *inputs[] = {s->file, "shaders/color.hlsli"};
		if (!stale(out, inputs, ARRAY_LEN(inputs))) {
			continue;
		}
		cmd_append(&cmd, "fxc.exe", "/nologo", "/T", s->profile, "/E", s->entry, "/O3", "/WX", "/Fo", out, s->file);
		if (!cmd_run(&cmd, .async = &procs)) {
			ok = false;
		}
	}
	return procs_flush(&procs) && ok;
}

static void usage(void) {
	fprintf(stderr, "usage: nob [build|test|run|clean] [-- game arguments]\n");
}

int main(int argc, char **argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);

	if (!file_exists("nob.c") || !file_exists("c3/project.json")) {
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
		} else if (strcmp(a, "build") == 0 || strcmp(a, "test") == 0 || strcmp(a, "run") == 0 ||
				strcmp(a, "clean") == 0) {
			action = a;
		} else {
			usage();
			return 1;
		}
	}

	Cmd cmd = {0};
	if (strcmp(action, "clean") == 0) {
		cmd_append(&cmd, "cmd.exe", "/c", "if exist " BUILD " rmdir /s /q " BUILD);
		return cmd_run(&cmd) ? 0 : 1;
	}
	if (!shaders()) {
		nob_log(ERROR, "The shaders didn't compile");
		return 1;
	}
	const char *c3c = getenv("C3C");
	cmd_append(&cmd, c3c && *c3c ? c3c : "c3c", action, "--path", "c3");
	if (strcmp(action, "run") == 0 && i < argc) {
		cmd_append(&cmd, "--");
		for (; i < argc; i++) {
			cmd_append(&cmd, argv[i]);
		}
	}
	return cmd_run(&cmd) ? 0 : 1;
}
