# GoCube

Voxel sandbox for Windows, in C++20 with Direct3D 11, gameplay and mods in Lua.
A rewrite of the Godot version of GoCube; the plan is in the
[GoCube Native Rewrite Plan](https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy).

Status: milestone 4. Walk, fly, break and place blocks in a world that is saved in
the Godot version's format: worlds it saved open here, and worlds saved here open
there. Generation is exactly the Godot version's. Content and code come from mods,
in sandboxed Lua; the base game is one (`mods/core`). (Survival, inventories and
menus come with the next milestone; editing is creative-style for now.)

## Requirements
- Windows 10 or 11, x64
- A GPU with Direct3D 11 at feature level 11_0 (or `--warp` to render in software)
- Visual Studio 2022 or 2026 with "Desktop development with C++" (it brings MSVC,
  CMake, Ninja, vcpkg and the Windows SDK's shader compiler)
- Optional: Windows' **Graphics Tools** (Settings > System > Optional features) for
  the Direct3D debug layer in debug builds

Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell) graphics
such as the HD 4600, where every Direct3D 12 draw hangs the GPU; their Direct3D 11
drivers are mature, and nothing a voxel game needs is missing from Direct3D 11.

## Build and test
From a **Developer PowerShell for VS** (it sets `VCPKG_ROOT` and puts `fxc` on the
path), in this folder:

    cmake --preset windows        # first time: vcpkg builds the dependencies
    cmake --build --preset debug  # or: release
    ctest --preset debug          # or: release

Or open the folder in Visual Studio, which reads `CMakePresets.json`.
The game is `build\Debug\gocube.exe` (`build\Release\gocube.exe`); it finds `mods\`
in its folder or a folder above it.

## Playing
    gocube --world=MyWorld                # %APPDATA%\GoCube\worlds\MyWorld (made if new)
    gocube --world-dir="%APPDATA%\Godot\app_userdata\GoCube\worlds\MyWorld"   # a Godot version world

Click in the window to play (Esc lets the mouse go).

| Key | |
| --- | --- |
| W A S D | Walk |
| Space | Jump (swim up, climb, fly up) |
| Shift | Sneak (fly down; landing ends flight) |
| Ctrl | Sprint |
| F | Fly |
| Left click | Break the block aimed at (a door or bed goes whole) |
| Right click | Place the selected block (or open and close a door; sneak to place against it) |
| Wheel, middle click | Choose the block to place; pick the one aimed at |

The world is saved every minute and when the game closes: edited chunks to
`chunks\`, the player to `world.json`.

| Option | What it does |
| --- | --- |
| `--world=NAME` | Play the world NAME in `%APPDATA%\GoCube\worlds` (a new one is made if needed) |
| `--world-dir=DIR` | Play the world in this folder (for example one the Godot version saved) |
| `--seed=N` | Seed for a new world (default 777) |
| `--generator=N` | World generator version for an unsaved world: 1 (before biomes), 2, 3 (default, the newest) |
| `--view=N` | View distance in blocks (default 128) |
| `--pos=X,Y,Z`, `--look=YAW,PITCH` | Start flying at this position (feet), and view direction in degrees |
| `--warp` | Render on WARP, Windows' software adapter (no GPU needed) |
| `--hidden` | Don't show the window (tests) |
| `--frames=N` | Quit after N frames; exit code 1 if the debug layer reported errors or warnings |
| `--screenshot=FILE` | Save the last frame as a PNG (frames are counted once the world around has loaded) |
| `--no-vsync` | Present as fast as possible |
| `--debug`, `--no-debug` | Force the Direct3D 11 debug layer on or off (default: on in debug builds) |

## Mods
A mod is a folder with `mod.json`, optional JSON content files and an optional
`main.lua`; the game loads `mods/` next to it (the base game first) and
`%APPDATA%/GoCube/mods`. `src/mods/mods.h` describes the format and the Lua API
(`registry`, `events`, `world`); `mods/core/main.lua` is a small example.

Mods are sandboxed: no files, OS, network or debug access, no bytecode, no
`load`; a mod's code is stopped when it runs too long or asks for too much
memory (`pcall` can't catch that), and a mod whose code fails is undone (its
content and event handlers removed, mods that depend on it skipped). One known
gap: Lua's string pattern functions (`string.find`, `match`, `gmatch`, `gsub`)
run in C, where the time limit can't reach them, so a pathological pattern on a
long string could stall the game.

Each mod gets a fingerprint of its files (the Godot version's algorithm), for
checking that players have the same mods. `mods/` and `tests/data/` are kept byte
for byte by git (`.gitattributes`), since fingerprints depend on the bytes.

## Layout
- `src/voxel/`: the voxel engine (chunks, world generation, light, meshing, chunk
  files), copied unchanged from the Godot version's GDExtension (GoCube `4425f46`).
- `src/content/`: mods' JSON content, block ids, the block table, the texture atlas
  and the generator's tables, as the Godot version builds them.
- `src/world/`: chunk streaming on worker threads, editing, the raycast, collision;
  the world folder (`world.json`, `chunks\`).
- `src/game/`: the player (movement, breaking, placing) and the block rules
  (shaped blocks' variants, doors, beds, support).
- `src/mods/`: finding, ordering and loading mods; the Lua sandbox and API;
  fingerprints.
- `src/render/`, `shaders/`: Direct3D 11 and the terrain shaders (HLSL, compiled by
  FXC at build time).
- `mods/core/`: the base game: content, assets and `main.lua` (the Godot version's
  `main.gd`, which registered one block and one recipe from code).
- `thirdparty/lua/`: Lua 5.4.8's sources, built with a fixed string-hash seed.
- `tests/`: doctest unit tests: the generation fingerprints, player movement and
  editing, saving, and mods (the sandbox, undoing, events, fingerprints). `tests/data/godot_world` is a world the Godot version saved
  (made by `tests/data/make_godot_world.gd`; `check_godot_opens.gd` checks the other
  way round).

## Dependencies
Pinned through `vcpkg.json` (its `builtin-baseline`), linked statically:
[Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[Lua](https://www.lua.org) 5.4.8 (MIT, in `thirdparty/lua`),
[nlohmann/json](https://github.com/nlohmann/json) (MIT),
[stb](https://github.com/nothings/stb) (MIT or public domain),
[doctest](https://github.com/doctest/doctest) (MIT, tests only).
