# GoCube: notes for Claude

## Build and test
Commands need the Visual Studio developer environment (MSVC, Ninja, `VCPKG_ROOT`).
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
`src/voxel/` does. A real change to generation is a new generator version (old
worlds keep theirs) with new fingerprints.

## Worlds stay compatible with the Godot version
A world folder is `world.json` plus `chunks\c.X.Y.Z.bin` (the Godot version's
format). `tests/test_save.cpp` opens `tests/data/godot_world` (saved by the Godot
version) and checks its edits, edits and saves it, and checks that world.json keeps
every field this version doesn't use (inventory, block entities, plants): keep it
that way, `WorldSave::save` only replaces the player fields it is given. To check
the other direction, run the test with `GOCUBE_TEST_WORLD_OUT=<dir>`, copy `<dir>`
into a Godot user data folder's `worlds\_cpp_edited` and run
`tests/data/check_godot_opens.gd` with the Godot version (headless, `-s`).

## Mods and the sandbox
`tests/test_mods.cpp` checks what the sandbox promises: a mod's code is stopped
when it runs too long or takes too much memory (also through pcall/xpcall), mods
reach nothing but their copies of string/table/math/utf8, the safe basics and the
game API, bytecode is refused, and a failing mod is undone. Anything new given to
mods (a library, a function) needs a check there. Lua runs single-threaded on the
main thread; never call into it from a worker. Keep `mods/**` and `tests/data/**`
byte for byte (`.gitattributes`): fingerprints and test expectations hash bytes.

## Seeing the result
`build\Release\gocube.exe --hidden --frames=10 --screenshot=<file>.png` (add
`--pos=X,Y,Z --look=YAW,PITCH`) renders once the world around has loaded; read the
PNG to check what a rendering change looks like.

## Graphics
Direct3D 11, feature level 11_0 minimum. Not Direct3D 12: the development PC's
Intel HD Graphics 4600 hangs on every Direct3D 12 draw (Intel disabled Direct3D 12
on Haswell). Check rendering changes on that GPU, not only on WARP:
`build\Debug\gocube.exe --hidden --frames=600` must exit 0 (debug layer on).

## Plan
The rewrite follows the GoCube Native Rewrite Plan
(https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy); the Godot version in
`../gocube` is the reference for behaviour.
