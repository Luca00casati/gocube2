# GoCube: notes for Claude

## Build and test
Commands need the Visual Studio developer environment (CMake, MSBuild, fxc); the
generator is Visual Studio's own (not Ninja). No
package manager: every library is source in `thirdparty/` (`thirdparty/README.md`).
From PowerShell outside it:

    $vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"

then `cmake --preset windows`, `cmake --build --preset debug`, `ctest --preset debug`.

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
add JSON files (only `CMakePresets.json` remains: CMake reads only JSON); old `mod.json`/`world.json` are refused with a message.
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
Decisions (mining time, drops, recipes, smelting, fuel, fall damage, food, plants,
farming, flowing fluids) are the base game's Lua in `mods/core/rules/`, set with `game.set_rule`; C++
(`src/game/`) keeps the data (inventory, block entities, drops, health, time) and
asks `Mods::rule`, with a plain built-in default if no mod answers. Ports of the
Godot version's GDScript keep its numbers and behaviour; tests in
`tests/test_survival.cpp` pin them down. Saved data keeps the Godot `world.json`
fields (`player.inventory`, `health`, `bed`, `block_entities`, `plants`, `spawn`,
`time`).
Every Lua call has the mods' time limit (20 ms, debug builds included; three
overruns turn a mod off, the core mod too): work that grows with how many blocks
changed is spread over ticks (`MAX_CHECKS`, `CELLS_PER_CALL`), and per-cell
bookkeeping uses integer keys, not strings or tables (garbage collection lands
inside a call). `tests/test_survival.cpp`'s flood test checks it.

## Seeing the result
`build\Release\gocube.exe --hidden --frames=10 --screenshot=<file>.png` (add
`--pos=X,Y,Z --look=YAW,PITCH`) renders once the world around has loaded; read the
PNG to check what a rendering change looks like.

## Looking like the Godot version
The target is what the Godot version shows on the development PC: Godot's
Compatibility renderer (its only one there). That renderer uses textures and
colours as they are (sRGB numbers) and writes emission and unshaded colour
out as pixel values; only the sun's share (`light()`) is added as linear
light. `shaders/chunk.hlsl` and `shaders/items.hlsl` do the same
(`to_linear(everything but the sun) + sun`), with the atlas read through its
plain view: keep it that way, or the colours drift. Fog is Godot's depth fog
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
