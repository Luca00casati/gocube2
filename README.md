# GoCube

Voxel sandbox for Windows, in C++20 with Direct3D 11, gameplay and mods in Lua.
A rewrite of the Godot version of GoCube; the plan is in the
[GoCube Native Rewrite Plan](https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy).

Status: milestone 6. Survival: mining, drops, crafting, chests and furnaces,
health, the day and night, growing plants, farming, flowing water and lava; creative; a title menu and pause menu.
It looks like the Godot version: its sky, fog, block and cloud shadows, light
from torches (placed and held), dropped items and the first-person arm.
Worlds are generated exactly as the Godot version does. The rules of the game
and its content are the base game's Lua (`mods/core`), sandboxed like every mod;
every data file (a mod's manifest, a world's `world.lua`) is a Lua data table,
not JSON. (So worlds the Godot version saved, as JSON, don't open here.)

## Requirements
- Windows 10 or 11, x64
- A GPU with Direct3D 11 at feature level 11_0 (or `--warp` to render in software)
- Visual Studio 2022 or 2026 with "Desktop development with C++" (it brings MSVC,
  CMake, Ninja and the Windows SDK's shader compiler). Nothing else: every library
  is in `thirdparty/` as source.
- Optional: Windows' **Graphics Tools** (Settings > System > Optional features) for
  the Direct3D debug layer in debug builds

Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell) graphics
such as the HD 4600, where every Direct3D 12 draw hangs the GPU; their Direct3D 11
drivers are mature, and nothing a voxel game needs is missing from Direct3D 11.

## Build and test
From a **Developer PowerShell for VS** (it puts MSVC, Ninja and `fxc` on the
path), in this folder:

    cmake --preset windows        # once
    cmake --build --preset debug  # or: release
    ctest --preset debug          # or: release

Or open the folder in Visual Studio, which reads `CMakePresets.json`.
The game is `build\Debug\gocube.exe` (`build\Release\gocube.exe`); it finds `mods\`
in its folder or a folder above it.

## Playing
Run `gocube` for the title menu: your worlds (play, delete to the Recycle Bin),
and a new world (name, seed,
survival or creative). Or straight into a world:

    gocube --world=MyWorld                # %APPDATA%\GoCube\worlds\MyWorld (made if new)
    gocube --world-dir="D:\Worlds\MyWorld"   # a world in any folder

| Key | |
| --- | --- |
| W A S D | Walk |
| Space | Jump (swim up, climb, fly up) |
| Shift | Sneak (fly down; landing ends flight) |
| Ctrl | Sprint |
| F | Fly |
| Left button | Break the block aimed at (hold it in survival; doors and beds go whole) |
| Right button | Place the held block; open a chest or furnace, use a door or bed, eat (sneak to place against them) |
| 1-9, wheel | Choose a hotbar slot |
| Middle button | Creative: take the block aimed at |
| E | Inventory and crafting (creative: every block and item) |
| Esc | Pause menu (save and quit) |

Survival as in the Godot version: mining takes time (faster with the right tool,
some blocks need one), broken blocks drop items, crafting from the inventory,
chests and furnaces (smelting with fuel), health (falls, lava, drowning; healing
with time and food), death and respawn at your bed or the world spawn, a day that
lasts a real day by default and a bed to sleep through the night, falling sand,
growing crops and saplings, decaying leaves. The world is saved every minute and
when you quit.

| Option | What it does |
| --- | --- |
| `--mode=survival\|creative` | Mode for a new world (default survival) |
| `--world=NAME` | Play the world NAME in `%APPDATA%\GoCube\worlds` (a new one is made if needed) |
| `--world-dir=DIR` | Play the world in this folder |
| `--seed=N` | Seed for a new world (default 777) |
| `--generator=N` | World generator version for an unsaved world: 1 (before biomes), 2, 3 (default, the newest) |
| `--view=N` | View distance in blocks (default 128) |
| `--pos=X,Y,Z`, `--look=YAW,PITCH` | Start flying at this position (feet), and view direction in degrees |
| `--time=0..1` | Start at this time of day (0.5: noon) |
| `--drop=ID,...` | Drop these items in front of the player (trying things out) |
| `--screen=inventory` | Open the inventory at the start (screenshots) |
| `--menu` | The title menu even with testing options (`--frames`, `--screenshot` apply to it) |
| `--warp` | Render on WARP, Windows' software adapter (no GPU needed) |
| `--hidden` | Don't show the window (tests) |
| `--frames=N` | Quit after N frames; exit code 1 if the debug layer reported errors or warnings |
| `--screenshot=FILE` | Save the last frame as a PNG (frames are counted once the world around has loaded) |
| `--render-scale=F` | Draw the world at this part of the window's resolution (0.25 to 1; stretched over it) |
| `--no-vsync` | Present as fast as possible |
| `--debug`, `--no-debug` | Force the Direct3D 11 debug layer on or off (default: on in debug builds) |

## Mods
A mod is a folder with `mod.lua` (its manifest) and a `main.lua`; the game loads `mods/` next
to it (the base game first) and `%APPDATA%/GoCube/mods`. Everything a mod adds
is Lua: content too (`registry.register_block("ruby_ore", {...})`); there are no
JSON files. A manifest is a data table:

    return {id = "mymod", name = "My Mod", version = "1.0", entry = "main.lua"}
 `src/mods/mods.h` describes the format and the Lua API
(`registry`, `events`, `world`, `game`). The base game is the example: its
`content/` registers its blocks, items, recipes, ores, trees, biome and sounds,
and its `main.lua` and `rules/` set the game's rules (mining time, drops, crafting,
smelting, fuel, fall damage, food, plants, farming, fluids) with `game.set_rule`; a mod can replace
one, or wrap it (`game.rule(name)` gives the one before).

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
- `src/content/`: the content mods register, block ids, the block table, the texture
  atlas, item models and the generator's tables, as the Godot version builds them.
- `src/world/`: chunk streaming on worker threads, editing, the raycast, collision;
  the world folder (`world.lua`, `chunks\`).
- `src/game/`: the player (movement, breaking, placing) and the block rules
  (shaped blocks' variants, doors, beds, support).
- `src/mods/`: finding, ordering and loading mods; the Lua sandbox and API;
  fingerprints.
- `src/render/`, `shaders/`: Direct3D 11, the terrain, sky and item shaders (HLSL,
  compiled by FXC at build time), drawn as the Godot version shows them.
- `mods/core/`: the base game: content, assets and `main.lua` (the Godot version's
  `main.gd`, which registered one block and one recipe from code).
- `thirdparty/lua/`: Lua 5.4.8's sources, built with a fixed string-hash seed.
- `tests/`: doctest unit tests: the generation fingerprints, player movement and
  editing, saving, mods (the sandbox, undoing, events, fingerprints), survival,
  and what rendering works out on the CPU (item models, held light, block
  shadows). `tests/data/godot_screenshot.gd` takes the Godot version's side of a
  screenshot comparison.

## Dependencies
In `thirdparty/` as source, pinned to releases and built with the game, linked
statically (versions and licences: `thirdparty/README.md`):
[Lua](https://www.lua.org) 5.4.8 (MIT),
[Dear ImGui](https://github.com/ocornut/imgui) 1.93.0 (MIT),
[libpng](https://github.com/pnggroup/libpng) 1.6.59 (libpng licence) with
[zlib](https://github.com/madler/zlib) 1.3.2 (zlib licence),
[nlohmann/json](https://github.com/nlohmann/json) 3.12.0 (MIT; in-memory values only),
[doctest](https://github.com/doctest/doctest) 2.5.3 (MIT, tests only).
