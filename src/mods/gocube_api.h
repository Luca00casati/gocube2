/* The modding API, version 2: a mod's code is a DLL (x64) the game loads
 * into itself. This header is the whole boundary between the game and a mod,
 * in C, so a mod can be built with any compiler and runtime; mods in C++
 * use mods/mod_sdk.h over it.
 *
 * A mod's DLL exports
 *   const char *gocube_mod_load(const GcApi *api, void **instance);
 *     runs once, while mods load: registers content, event handlers, rules
 *     and commands through `api` (kept: it stays valid until the mod is
 *     unloaded). Returns NULL, having set *instance to the mod's own state
 *     (anything; passed back to unload), or an error message (then the mod
 *     is undone: its registrations dropped, the DLL unloaded).
 *   void gocube_mod_unload(void *instance);              (optional)
 *     when the game is done with the mod, before the DLL is unloaded.
 * The game may load the same DLL for several worlds at once (a host and a
 * client in one process): keep everything in the instance, nothing global.
 *
 * Values cross as JSON text (data/json.h), e.g. `[1, "core:stone",
 * {"hardness": 2}]`. Arguments are a JSON array; a result is an array of
 * one value, or empty for nothing.
 * Strings are UTF-8, ending in a zero. A string the game hands a mod stays
 * valid until that mod's next call into the API; one a mod hands the game
 * must stay valid until its function returns (the game copies it).
 *
 * Everything runs on the game's thread. Native code has no sandbox: a mod
 * can do anything the game can. The game does catch a crash or an uncaught
 * exception in a mod's function (the mod is then turned off), but a mod
 * that corrupts memory can still bring the game down.
 */
#ifndef GOCUBE_API_H
#define GOCUBE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOCUBE_API_VERSION 2

typedef struct GcMod GcMod; /* the game's handle for one loaded mod */

/* A mod's function (an event handler, a rule, a command): `args` a list as
 * text. Returns 0 and sets *result to its result (a list of one value or
 * empty, as text; NULL: nothing), or nonzero and sets *result to an error
 * message. `user` is what the mod registered it with. */
typedef int (*GcFunction)(void *user, const char *args, const char **result);

typedef struct GcApi {
	uint32_t version; /* GOCUBE_API_VERSION */
	uint32_t size;    /* sizeof(GcApi): later versions only add to the end */
	GcMod *mod;       /* the first argument of everything below */

	/* Any of the game's functions by name, with a list of arguments.
	 * Returns 0 and sets *result to its result (a list of one value, or
	 * empty), or nonzero and sets *result to the error. The functions (see
	 * src/mods/mods.h for what each takes and gives):
	 *   registry.register (section, id, def) -> qualified id   (while loading)
	 *   registry.get (section, id) -> def or nothing
	 *   registry.all (section) -> {[id] = def}
 *   registry.with (section, field) -> {[id] = def} of those with that field
	 *   world.get_block, world.set_block, world.supported, world.drop_item,
	 *   world.grow_tree, world.raycast, world.box_free
	 *   creatures.spawn, creatures.remove, creatures.all, creatures.get, creatures.set
	 *   game.random, game.players, game.day, game.damage_player,
	 *   game.play_sound, game.effect, game.command,
	 *   game.rule_before (name, args...) -> what the rule this mod replaced gives
	 *   mod.info () -> {id, version, dir} */
	int (*call)(GcMod *mod, const char *function, const char *args, const char **result);

	/* The world's block at a position ("core:stone"), or NULL where nothing
	 * is loaded (or there's no world yet). The string stays valid while
	 * playing. Faster than call("world.get_block"). */
	const char *(*get_block)(GcMod *mod, int x, int y, int z);
	/* Sets a block (a dyed id, "core:stone#ff8800", sets its dye too):
	 * 1 if it changed, 0 if it couldn't (not loaded), -1 for an unknown
	 * block or no world. */
	int (*set_block)(GcMod *mod, int x, int y, int z, const char *id);
	/* Whether block `id` could stand at a position (what holds it up): 1 or
	 * 0, -1 for an unknown block or no world. As call("world.supported"). */
	int (*supported)(GcMod *mod, int x, int y, int z, const char *id);

	/* While loading: an event handler ("tick", "block_placed",
	 * "block_broken", "block_changed", "creature_hit"), a rule the game asks
	 * (the last mod to set one wins), a chat command (`def` a JSON object:
	 * {"usage": ..., "help": ..., "op": ...}). NULL, or what's wrong. */
	const char *(*on)(GcMod *mod, const char *event, GcFunction fn, void *user);
	const char *(*set_rule)(GcMod *mod, const char *name, GcFunction fn, void *user);
	const char *(*add_command)(GcMod *mod, const char *name, const char *def, GcFunction fn, void *user);

	/* A line in the game's log, under the mod's id. */
	void (*log)(GcMod *mod, const char *line);
} GcApi;

typedef const char *(*GcModLoad)(const GcApi *api, void **instance);
typedef void (*GcModUnload)(void *instance);

#ifdef __cplusplus
}
#endif

#endif
