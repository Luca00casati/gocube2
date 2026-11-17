# GoCube: notes for Claude

## Build and test
Commands need the Visual Studio developer environment (CMake, MSBuild, fxc); the
generator is Visual Studio's own (not Ninja). No
package manager: every library is source in `thirdparty/` (`thirdparty/README.md`).
From PowerShell outside it:

    $vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"

then, once, `cmake -B build -G "Visual Studio 18 2026" -A x64`; to build and test,
`cmake --build build --config Release` and
`ctest --test-dir build -C Release --output-on-failure` (Debug builds are slow:
Release unless asked).

## World generation must not change
`tests/test_worldgen.cpp` checks the Godot version's fingerprints (seed 777 with
generator 3, seed 4242 with generator 1): block data and light of the chunks around
the origin. Unedited chunks are generated again every time a world is opened, so any
change in what comes out moves caves, ores and light in worlds already played. Block
ids (`core:air`, then sorted ids after shaped blocks' variants are added), the block
table and the generator tables in `src/content/` feed into it as much as
`src/voxel/` does, and so does the content in `mods/core/content/*.lua` (Lua
only: there are no JSON content files). Lua tables keyed by id have no order,
so where order reaches the generator (a biome's trees and plants) it is a list
of `{id, value}` pairs. A real change to generation is a new generator version
(old worlds keep theirs) with new fingerprints.

## Data files are Lua, never JSON
Every data file is a Lua data table (`return { ... }`): a mod's `mod.lua`, a
world's `world.lua`, the base game's content (`mods/core/content/*.lua`, Lua
registry calls). `src/data/lua_data.h` reads them in an empty Lua state with a
step and memory limit (nothing in a data file can run) and writes them. Don't
add JSON files (there are none); old `mod.json`/`world.json` are refused with a message.
A Lua list can't hold nil: an empty slot is `false`, and the writer throws on
a null in a list; an empty table reads back as an empty object. Godot worlds
(JSON) aren't read. A world folder is `world.lua` plus `chunks\c.X.Y.Z.bin`
(the Godot version's chunk file format); `tests/test_save.cpp` makes, edits,
saves and reopens one and checks that every field survives (`WorldSave::save`
only replaces the player fields it is given: keep it that way).

## Mods and the sandbox
`tests/test_mods.cpp` checks what the sandbox promises: a mod's code is stopped
when it runs too long or takes too much memory (also through pcall/xpcall), mods
reach nothing but their copies of string/table/math/utf8, the safe basics and the
game API, bytecode is refused, and a failing mod is undone. Anything new given to
mods (a library, a function) needs a check there. Lua runs single-threaded on the
main thread; never call into it from a worker. Keep `mods/**` and `tests/data/**`
byte for byte (`.gitattributes`): fingerprints and test expectations hash bytes.

## Where the game's rules live
Decisions (mining time, drops, fall damage, food, plants, farming, flowing
fluids; recipes, smelting and fuel when a mod adds them) are the base game's Lua
in `mods/core/rules/`, set with `game.set_rule`; C++
(`src/game/`) keeps the data (inventory, block entities, drops, health, time) and
asks `Mods::rule`, with a plain built-in default if no mod answers. Creatures
the same way: their bodies, physics and models are C++ (`game/creatures.h`,
`game/creature_models.h`), what they do is `rules/creatures.lua` through the
`creatures` API (it runs at the 20 Hz tick; the bodies at 60 Hz). Ports of the
Godot version's GDScript keep its numbers and behaviour; tests in
`tests/test_survival.cpp` pin them down. Deliberate departures: the base game
has no tools, weapons, armour, crafting or smelting (everything is mined by
hand, hardness x 0.375 s, and drops), no enemies (no ninjas: only harmless
Block Eaters), and no health bar (the screen reddens the lower health is). Saved data keeps the Godot `world.json`
fields (`player.inventory`, `health`, `bed`, `block_entities`, `plants`, `spawn`,
`time`).
Every Lua call has the mods' time limit (20 ms, debug builds included; three
overruns turn a mod off, the core mod too): work that grows with how many blocks
changed is spread over ticks (`MAX_CHECKS`, `CELLS_PER_CALL`), and per-cell
bookkeeping uses integer keys, not strings or tables (garbage collection lands
inside a call). `tests/test_survival.cpp`'s flood test checks it.

## Multiplayer: check the client path before calling a feature done
Single player is the host path; most multiplayer bugs come from the client path
never being run. The host does what a client asks as that player
(`Game::break_block`, `Game::use` with an `Actor`, `Game::apply_inventory_action`);
a client's `Game` is remote (`Game::set_remote`): it sends instead of changing
the world, runs no mods' ticks, and shows the host's drops, creatures and
containers. A feature that changes blocks, items, the player or anything the
host owns (drops, containers, creatures, beds, time) is only done when:

1. A client doing it works: add a step to `tests/test_multiplayer.cpp` (host and
   client in one process, `tests/net_play.h`), driving the client like a person
   (aim, `input.use_pressed`/`break_held`, `walk`), not by calling Session
   methods, and wait for the result on both sides. In survival the host keeps
   the client's inventory (`Session::set_saved_players` before it joins) and
   health, and times mining (hold `break_held`).
2. The host refusing it works: out of reach, rate-limited, someone standing
   there, an item it doesn't have. Nothing is left half done.
3. Anything only the host knows (spawn, beds, first night, mode, time) reaches
   the client, or the client asks for it.
4. `gocube_mp_smoke` (CTest's `multiplayer_processes`) still passes: a host and
   a client as two real processes.

The client's world comes from the seed plus the host's edited chunks; a
block change in a chunk it hasn't loaded asks for the whole chunk (kept in
memory: a client has no world folder). Moving a test player far away unloads
the play area on its side: wait for it to load again before aiming.

## Seeing the result
`build\Release\gocube.exe --hidden --frames=10 --screenshot=<file>.png` (add
`--pos=X,Y,Z --look=YAW,PITCH`) renders once the world around has loaded; read the
PNG to check what a rendering change looks like. `--hidden` never reads the
player's settings (`%APPDATA%\GoCube\settings.lua`, `game/settings.h`): tests and
screenshots run with the defaults. `--screen=settings` shows the settings screen.

## Looking like the Godot version
The target is what the Godot version shows on the development PC: Godot's
Compatibility renderer (its only one there). That renderer uses textures and
colours as they are (sRGB numbers), takes emission and unshaded colour into
linear light through its fast approximation of the sRGB curve, adds the sun's
share (`light()`) as linear light, and writes the result through its own
approximation back (no linear toe) into a plain 8-bit target, where
see-through surfaces blend. `shaders/godot_color.hlsli` has both curves; the
world's shaders use them and draw into the plain back buffer, with the atlas
read through its plain view: keep it that way, or the colours drift. The held
item culls back faces (Godot's extruded items are wound inside out, so it
shows their far side). Fog is Godot's depth fog
(`main._set_fog_range`), not its default exponential one.
To compare, take the same shot with both: `gocube.exe --hidden --frames=10
--seed=777 --time=T --pos=... --look=... --screenshot=a.png`, and from
`..\gocube`, `Godot_v4.7.2-stable_win64_console.exe --path . --resolution
1280x720 -s <this repo>\tests\data\godot_screenshot.gd -- --world=_shot
--seed=777 --time=T --pos=... --look=... --out=b.png`. Both pin the clouds
(they drift from the start); compare pixels, not impressions.

## Graphics
Direct3D 11, feature level 11_0 minimum. Not Direct3D 12: the development PC's
Intel HD Graphics 4600 hangs on every Direct3D 12 draw (Intel disabled Direct3D 12
on Haswell). Check rendering changes on that GPU, not only on WARP:
`build\Debug\gocube.exe --hidden --frames=600` must exit 0 (debug layer on).

## Plan
The rewrite follows the GoCube Native Rewrite Plan
(https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy); the Godot version in
`../gocube` is the reference for behaviour.
