# GoCube

Voxel sandbox for Windows, in C++20 with Direct3D 11, gameplay and mods in Lua.

Survival: mining by hand, drops, chests, health, the day and night, trees
coming up on grass by themselves, flowing water and lava; creatures (rare,
harmless Block Eaters by day that eat the ground; no enemies); sound;
creative; multiplayer (one player hosts, others join); a title menu and pause
menu. A sky with a sun, moon, stars and drifting clouds (and their shadows),
fog, block shadows, light from torches (placed and held), dropped items and a
first-person arm in your skin.
The terrain comes from 3D noise: hills, cliffs, overhangs, seas with sand and
gravel beaches, caves, lava deep down, ore veins. Players wear skins in the
usual box-figure layout (`%APPDATA%\GoCube\skin.ppm`, 64x64 or 64x32, as a
PPM: magenta is see-through), and everyone sees each player's. The rules of the game
and its content are the base game's Lua (`mods/core`), sandboxed like every mod;
every data file (a mod's manifest, a world's `world.lua`) is a Lua data table.

## Requirements
- Windows 10 or 11, x64
- A GPU with Direct3D 11 at feature level 11_0 (or `--warp` to render in software)
- Visual Studio 2026 with "Desktop development with C++" (it brings MSVC, MSBuild,
  CMake and the Windows SDK's shader compiler; with Visual Studio 2022, configure
  with `cmake -B build -G "Visual Studio 17 2022" -A x64`). Nothing else: every library
  is in `thirdparty/` as source.
- Optional: Windows' **Graphics Tools** (Settings > System > Optional features) for
  the Direct3D debug layer in debug builds

Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell) graphics
such as the HD 4600, where every Direct3D 12 draw hangs the GPU; their Direct3D 11
drivers are mature, and nothing a voxel game needs is missing from Direct3D 11.

## Build and test
From a **Developer PowerShell for VS** (it puts CMake and `fxc` on the path), in
this folder (the build is Visual Studio's own: MSBuild, `build\gocube.slnx`):

    cmake -B build -G "Visual Studio 18 2026" -A x64      # once
    cmake --build build --config Release                  # or: Debug
    ctest --test-dir build -C Release --output-on-failure # or: Debug

Or open `build\gocube.slnx` in Visual Studio (after the first command).
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
| Right button | Place the held block; open a chest, use a door or bed, eat (sneak to place against them) |
| 1-9, wheel | Choose a hotbar slot |
| Middle button | Creative: take the block aimed at |
| E | Inventory (creative: every block and item) |
| Esc | Pause menu (settings; save and quit) |

Survival without tools, weapons, armour, crafting or smelting: everything is
mined by hand (hardness x 0.375 s) and drops itself
(or what it gives: stone gives cobblestone, coal ore coal), chests, health
(falls, lava, drowning, falling out of the world; no bar: the screen reddens the lower it is,
and healing with time fades it), death and respawn at your bed or the world spawn, a day that
lasts a real day by default and a bed to sleep through the night, falling sand,
trees sprouting on grass, decaying leaves. One kind of grass and of wood (oak), no
saplings, no small plants (flowers, tall grass), no farming and no food. The world is saved every minute and
when you quit.

Settings (the title menu's Settings button, or the pause menu's): view distance,
mouse sensitivity, frame rate limit, vsync, smooth lighting, sun shadows,
render scale, volumes, the clock, the keys, and which mods load (and the mods
downloaded from servers). Changes show at once; they're kept in
`%APPDATA%\GoCube\settings.lua`, with your name and the last address you joined.

Playing together: in the title menu, tick "Open the world I play to others" and
play a world (others join it on port 24680: TCP, and UDP too if it gets through,
for smoother movement), or type the host's address and
"Join their world". The host's world is the world: it checks and does what
joined players do, keeps their inventories and health (saved in its world, by
name), and runs the creatures, items, chests and time; the night is
skipped once everyone is in bed. Everyone plays with the host's mods: joining
a host with other mods (the same base game), you're asked whether to download
the ones you lack (each checked against its fingerprint, kept in
`%APPDATA%\GoCube\server_mods`); the game then starts again with exactly the
host's mods.

| Option | What it does |
| --- | --- |
| `--mode=survival\|creative` | Mode for a new world (default survival) |
| `--world=NAME` | Play the world NAME in `%APPDATA%\GoCube\worlds` (a new one is made if needed) |
| `--world-dir=DIR` | Play the world in this folder |
| `--seed=N` | Seed for a new world (default 777) |
| `--view=N` | View distance in blocks (default: the settings', 128) |
| `--pos=X,Y,Z`, `--look=YAW,PITCH` | Start flying at this position (feet), and view direction in degrees |
| `--time=0..1` | Start at this time of day (0.5: noon) |
| `--drop=ID,...` | Drop these items in front of the player (trying things out) |
| `--spawn=KIND[=BLOCK],...` | Put creatures in front of the player (`block_eater=core:stone`) |
| `--mannequin` | Your own figure, in your skin, in front of you (what others see) |
| `--details` | Start with the details screen (F3) open: frame times by part (the profiler) |
| `--profile` | When the world ends, log the frame times by part, the GPU's and the world threads' (with `--frames`, `--no-vsync`: measure what's slow) |
| `--screen=inventory\|settings` | Open the inventory or the settings at the start (screenshots) |
| `--menu` | The title menu even with testing options (`--frames`, `--screenshot` apply to it) |
| `--warp` | Render on WARP, Windows' software adapter (no GPU needed) |
| `--hidden` | Don't show the window (tests; the settings file isn't read: the defaults) |
| `--frames=N` | Quit after N frames; exit code 1 if the debug layer reported errors or warnings |
| `--screenshot=FILE` | Save the last frame as a PNG (frames are counted once the world around has loaded) |
| `--render-scale=F` | Draw the world at this part of the window's resolution (0.25 to 1; stretched over it; default: the settings') |
| `--host[=PORT]` | Open the world to others (default port 24680) |
| `--join=ADDRESS[:PORT]` | Play in the world of the host at this address |
| `--name=NAME` | The name others see (default "Player"; the host keeps a survival player's things by it) |
| `--accept-mods` | Joining a host with other mods: download them without asking |
| `--no-sound` | Play without sound (`--hidden` never plays any) |
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
`content/` registers its blocks, items, ores, trees, biome and sounds,
and its `main.lua` and `rules/` set the game's rules (mining time, drops, fall
damage, food, plants, fluids) with `game.set_rule`; a mod can replace
one, or wrap it (`game.rule(name)` gives the one before). The game still
supports what the base game left out: recipes and the `hand_recipes` rule
(crafting in the inventory), furnaces (`block_entity = "furnace"`, the `smelting`
and `fuel_seconds` rules), tools and weapons (`damage` on an item), armour, food
(`food` on an item), and planting and crops (the `use_item` rule, `grows_into`);
`tests/data/kit_mods/kit` is a small mod that adds them back.

Mods are sandboxed: no files, OS, network or debug access, no bytecode, no
`load`; a mod's code is stopped when it runs too long or asks for too much
memory (`pcall` can't catch that), and a mod whose code fails is undone (its
content and event handlers removed, mods that depend on it skipped). One known
gap: Lua's string pattern functions (`string.find`, `match`, `gmatch`, `gsub`)
run in C, where the time limit can't reach them, so a pathological pattern on a
long string could stall the game.

Each mod gets a fingerprint of its files, for checking that players have the
same mods. `mods/` and `tests/data/` are kept byte
for byte by git (`.gitattributes`), since fingerprints depend on the bytes.

## Layout
- `src/voxel/`: the voxel engine (chunks, world generation, light, meshing, chunk
  files).
- `src/content/`: the content mods register, block ids, the block table, the texture
  atlas, item models, skins and the generator's tables.
  Most block faces are painted from a colour and noise (`image.h`; the base game's
  in `mods/core/content/looks.lua`); the few drawn images (torches, doors,
  beds, chests, items, the default skin) are PPM files, where magenta is see-through.
- `src/world/`: chunk streaming on worker threads, editing, the raycast, collision;
  the world folder (`world.lua`, `chunks\`).
- `src/game/`: the player (movement, breaking, placing), the block rules
  (shaped blocks' variants, doors, beds, support), survival (inventory,
  health, drops), creatures and their models, the first-person arm.
- `src/mods/`: finding, ordering and loading mods; the Lua sandbox and API;
  fingerprints.
- `src/net/`: multiplayer: the host's checks, what
  travels between host and clients, the message format, and the transport (TCP,
  and UDP for positions, on the same port; Winsock only).
- `src/render/`, `shaders/`: Direct3D 11, the terrain, sky, item and creature
  shaders (HLSL, compiled by FXC at build time).
- `src/profiler.h`: the frame profiler (F3, `--profile`).
- `src/ui/`: the UI, our own: an immediate-mode core (`gui.h`: windows, layout,
  buttons, text fields, sliders, drop-downs, scrolling; tested with a fake font)
  drawn with Direct2D and DirectWrite (`gui_d2d.h`) in the Share Tech font
  (built into the executable); the inventory, container and settings screens.
- `mods/core/`: the base game: content, assets, rules and `main.lua`.
- `thirdparty/lua/`: Lua 5.4.8's sources, built with a fixed string-hash seed.
- `tests/`: doctest unit tests: world generation (a fingerprint, what the land
  is like), player movement and editing, saving, mods (the sandbox, undoing,
  events, fingerprints), survival, the UI, and what rendering works out on the
  CPU (item models, held light, block shadows, skins, the profiler),
  multiplayer (host and client in one process). `tests/mp_smoke.cpp` plays a
  round between a host and a client in two processes (CTest runs it).

## Dependencies
In `thirdparty/` as source, pinned to releases and built with the game, linked
statically (versions and licences: `thirdparty/README.md`):
[Lua](https://www.lua.org) 5.4.8 (MIT),
[doctest](https://github.com/doctest/doctest) 2.5.3 (MIT, tests only), and the
UI's font, [Share Tech](https://fonts.google.com/specimen/Share+Tech) (SIL Open
Font License).
Everything else is Windows' own: Direct3D 11, Direct2D and DirectWrite (the
UI), XAudio2 (sound), Winsock (multiplayer), WIC (screenshots), the
Compression API (mod packs).
