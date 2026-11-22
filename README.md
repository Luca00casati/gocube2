# GoCube

Voxel sandbox for Windows, in C++20 with Direct3D 11; mods are data and DLLs.

Survival: mining by hand, drops, chests, health, the day and night, trees
coming up on grass by themselves, flowing water and lava; creatures (rare,
harmless Block Eaters by day that eat the ground; no enemies); sound;
creative; multiplayer (one player hosts, others join); a title menu and pause
menu. A sky with a sun, moon, stars and drifting clouds (and their shadows),
fog, block shadows, light from torches (placed and held), dropped items and a
first-person arm in your skin.
The terrain comes from 3D noise: hills, cliffs, overhangs, seas with sand and
gravel beaches, caves, lava deep down, ore veins. Players wear skins in the
usual box-figure layout (`%APPDATA%\GoCube\skin.png`, 64x64 or 64x32, a
PNG with see-through parts), and everyone sees each player's. The base game is a mod
like any other (`mods/core`): its content is data (`content.json`) and its rules
are its DLL (`core.dll`, from `src/core_mod/`). Every data file (a mod's
manifest, a world's `world.json`, the settings) is JSON, read and written by
our own code.

## Requirements
- Windows 10 or 11, x64
- A GPU with Direct3D 11 at feature level 11_0 (or `--warp` to render in software)
- Visual Studio 2026 (or 2022) with "Desktop development with C++": the MSVC
  compiler and linker and the Windows SDK's shader compiler. Nothing else: no
  build system and no third-party libraries (the build uses
  [nob.h](https://github.com/tsoding/nob.h), one header, in this folder).
- Optional: Windows' **Graphics Tools** (Settings > System > Optional features) for
  the Direct3D debug layer in debug builds

Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell) graphics
such as the HD 4600, where every Direct3D 12 draw hangs the GPU; their Direct3D 11
drivers are mature, and nothing a voxel game needs is missing from Direct3D 11.

## Build and test
The build is `nob.c`, a C program that calls the compiler, linker and shader
compiler (`cl`, `link`, `fxc`) directly: no build system, nothing to
configure. From a **Developer PowerShell for VS** (it puts them on the path),
in this folder:

    cl nob.c                  # once: afterwards nob rebuilds itself when nob.c changes
    nob                       # build Dev: the one to work with
    nob test                  # build, then run the tests
    nob run -- --world=Test   # build, then play (arguments after --)
    nob release               # build Release (also: nob test release, nob run release)
    nob clean                 # delete build\

Three configurations, each in its own folder (`build\Release`, `build\Dev`,
`build\Debug`):

- **release**: optimized (`/O2`), the game as players get it. The slowest to
  compile (about 3 minutes from scratch on 2 cores).
- **dev** (the default): not optimized, except the few files where the game spends its
  time (chunks: generating, lighting, meshing; data and JSON). Compiles
  about three times faster than Release (a change to one file: seconds) and
  runs well enough to play and test.
- **debug**: not optimized at all, with the debug runtime's checks
  (iterators, the heap, `/RTC1`) and Direct3D's debug layer. The slowest to run.

Only what changed is compiled again, all of it at once (as many files as
there are cores): an object when its source, nob itself, or a header it
includes is newer (the compiler lists those headers for each object), so
changing a header compiles only the files that use it. The standard
library headers are precompiled once (`src/pch.h`).

`unity` (e.g. `nob test release unity`, what CI does) compiles a few files
that each include several sources, so code they share is compiled once per
batch, not once per file: faster from scratch, slower after a small change
(the whole batch again). It builds in `build\<Config>-Unity`. Because of it,
two sources of the same part of the game mustn't both define a file-local
name (in an anonymous namespace) with the same parameters, even the same
way: share it in a header instead (`src/maths.h`, `tests/helpers.h`), or
give one another name.

The tests are four runs, two at a time: the unit tests (`gocube_tests`) in
two processes, the multiplayer ones (`-tc=multiplayer:*`: they play in real
time, so mostly they wait) beside the rest (`-tce=multiplayer:*`); then
multiplayer between two processes (`gocube_mp_smoke`) beside the game
rendering 60 frames on WARP. Each run's
output is in `build\<Config>\test_logs` (printed in full if it fails), with
the slowest test cases listed at its end.

The game is `build\<Config>\gocube.exe`, with its mods next to it (`mods\`
is copied there and the base game's `core.dll` built into
`mods\core`); it finds `mods\` in its folder or a folder above it.

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
| Middle button | Creative: take the block aimed at (dyed too) |
| E | Inventory (creative: every block and item to take from) |
| Q | Throw what you hold (Ctrl+Q: the whole stack) |
| T, / | Chat (/ starts a command); Enter sends, Esc closes, up and down for what you sent, Tab completes a command |
| Esc | Pause menu (settings; save and quit) |

Survival without tools, weapons, armour or smelting, and only two recipes
(planks from a log, a dye vat from planks): everything is
mined by hand (hardness x 0.375 s) and drops itself
(or what it gives: stone gives cobblestone, coal ore coal, gem ores their gems), chests, health
(falls, lava, drowning, falling out of the world; no bar: the screen reddens the lower it is,
and healing with time fades it), death and respawn at your bed or the world spawn, a day that
lasts a real day by default and a bed to sleep through the night, falling sand,
trees sprouting on grass, decaying leaves. One kind of grass and of wood (oak), no
saplings, no small plants (flowers, tall grass), no farming and no food. The world is saved every minute and
when you quit.

Creative uses the same inventory as survival (a new creative player starts
with it empty): take blocks and items from the inventory screen's palette,
pick up what lies around; placing never uses anything up, and nothing hurts.

Settings (the title menu's Settings button, or the pause menu's): view distance,
mouse sensitivity, frame rate limit, vsync, smooth lighting, sun shadows,
render scale, volumes, the clock, the keys, and which mods load (and the mods
downloaded from servers). Changes show at once; they're kept in
`%APPDATA%\GoCube\settings.json`, with your name and the last address you joined.

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
host's mods. A downloaded mod with code (a DLL) only loads if you allow it
(Settings: "Servers' mods may run code", off by default): native code has the
game's full access to your computer.

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
| `--screen=inventory\|settings\|chat` | Open the inventory, the settings or the chat at the start (screenshots) |
| `--say=LINE` | Type LINE into the chat once the world around has loaded (a command too; once per line) |
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

## Dyeing
Any block can be any colour. Five gem ores (red, green and blue the commonest,
white deeper, black deepest and rarest) drop gems; throw gems (Q) into a dye
vat to make its dye, then throw blocks in: they come back that colour, one
charge each (a gem gives 8; a vat holds 16 gems). Red, green and blue choose
the hue, mixed like light (red and green make yellow, 2 red and 1 green
orange); white and black make it lighter or darker by their share (red and
white make pink, blue and black navy; white and black alone, greys). A right
click on the vat with an empty hand tips the dye out. A dyed block keeps its
texture in the dye's colour, and stays dyed when it's broken and placed again.
As an item its id has the colour after it: `core:stone#ff8800` (also in
commands: `/give @s stone#ff8800 64`, `/setblock ~ ~ ~ planks#3366cc`).

## Chat and commands
T opens the chat; a line starting with `/` is a command. Whoever plays the
world runs them (the host runs a joined player's as that player). Operators
may change the world: the host, or whoever plays alone, always is one, and
gives others the right with `/op NAME` (kept with the world). Everyone can use
`/help`, `/msg`, `/list` and `/seed`.

| Command | |
| --- | --- |
| `/help [command]` | The commands you may use, or what one does |
| `/msg <player> <text>` | A message only that player sees |
| `/list`, `/seed` | Who is playing; the world's seed |
| `/say <text>` | A message to everyone |
| `/tp <x y z>`, `/tp <player> [<x y z> or <player>]` | Move (`~` is where you are: `/tp ~ ~10 ~`) |
| `/give <player> <item> [count]` | Items into a player's inventory (`torch` means `core:torch`; `@s` is you; `stone#ff8800` a dyed block) |
| `/time [set day\|noon\|night\|midnight\|0..1]` | The time of day |
| `/gamemode [survival\|creative]` | The world's game mode, for everyone (inventories stay as they are) |
| `/kill [player]` | Ends a survival player's life |
| `/spawnpoint` | The world's spawn where you stand |
| `/setblock <x y z> <block>`, `/fill <x1 y1 z1> <x2 y2 z2> <block>` | Change blocks (a fill: 32768 at most) |
| `/summon <creature> [x y z]` | A creature |
| `/kick <player> [reason]`, `/op <player>`, `/deop <player>` | Players |
| `/run <mod:name>` | A mod's command file |

## Mods
A mod is a folder with `mod.json` (its manifest); the game loads `mods/` next
to it (the base game first) and `%APPDATA%/GoCube/mods`:

    {"id": "mymod", "name": "My Mod", "version": "1.0", "content": "content.json", "entry": "mymod.dll", "api_version": 2}

A mod can be **data only**: its content as JSON (`content.json`:
`{"blocks": {"ruby_ore": {"hardness": 3, "color": "#c02040"}}, "items": {...}}`)
and chat command files (`functions/NAME.txt`, a command a line, run with
`/run mymod:NAME`). Nothing of it runs as code. Or it has **code**: a DLL
(x64) the game loads into itself, which registers content, event handlers,
the game's rules and chat commands. The boundary is a small C API
(`src/mods/gocube_api.h`: the DLL exports `gocube_mod_load`; values cross as
JSON text), so a mod can be built with any compiler. In C++,
`src/mods/mod_sdk.h` makes it short (build it with `src/data/data.cpp` and
`src/data/json.cpp`; `nob.c`'s `mod_dll` shows how):

    #include "mods/mod_sdk.h"
    struct MyMod : gc::sdk::Mod {
        explicit MyMod(const GcApi *api) : Mod(api) {
            add_command("heal", {{"usage", "<player>"}},
                    [this](const gc::Data &who, const gc::Data &args) { return std::string("healed"); });
            set_rule("fall_damage", [this](const gc::Data &a) { // half the base game's
                return gc::Data(rule_before("fall_damage", a).get<int>() / 2);
            });
        }
    };
    GOCUBE_MOD(MyMod)

`src/mods/mods.h` lists what mods can call (`registry`, `events`, `world`,
`creatures`, `game`) and the rules the game asks. The base game is the
example: `mods/core/content.json` holds its blocks, items, ores, trees, biome
and sounds, and `src/core_mod/core_mod.cpp` sets the game's rules (mining
time, drops, fall damage, food, plants, fluids, creatures, the two recipes);
a mod can replace one, or wrap it (`rule_before` gives the one it replaced).
The game still supports what the base game left out: furnaces
(`block_entity = "furnace"`, the `smelting` and `fuel_seconds` rules), tools
and weapons (`damage` on an item), armour, food (`food` on an item), and
planting and crops (the `use_item` rule, `grows_into`);
`tests/data/kit_mods/kit` (its code: `tests/mods/kit.cpp`) is a small mod that
adds them back. Dyes: an item with `dye = "#rrggbb"` is a gem for the dye vat
(a grey shades, any other colour is a hue); a block with `dye_vat = true` and
`block_entity = "vat"` is a vat (`full` names its variant with dye in it);
`undyed_boxes` lists the boxes of a shaped block that keep their own colours
when it's dyed; `world.set_block` takes a dyed id.

A mod's code is native and isn't sandboxed: it can do anything the game can.
The game catches a crash or an uncaught exception in a mod's function (the
mod is then turned off), and a mod whose loading fails is undone (its content
and handlers removed, its DLL unloaded, mods that depend on it skipped); a
mod that corrupts memory can still bring the game down. The same DLL may be
loaded for two worlds at once (a host and a client in one process), so a mod
keeps its state in its object, never in globals.

Each mod gets a fingerprint of its files, for checking that players have the
same mods (mod DLLs are built reproducibly, `/Brepro`: the same sources and
compiler give the same fingerprint; a Debug build's differs from a Release
build's). `mods/` and `tests/data/` are kept byte
for byte by git (`.gitattributes`), since fingerprints depend on the bytes.

## Layout
- `src/voxel/`: the voxel engine (chunks, world generation, light, meshing, chunk
  files).
- `src/content/`: the content mods register, block ids, the block table, the texture
  atlas, item models, skins and the generator's tables.
  Most block faces are painted from a colour and noise (`image.h`; the base game's
  in `mods/core/content.json`); the few drawn images (torches, doors,
  beds, chests, items, the default skin) are PNG files, read by Windows' own decoder (WIC).
- `src/world/`: chunk streaming on worker threads, editing, the raycast, collision;
  the world folder (`world.json`, `chunks\`).
- `src/game/`: the player (movement, breaking, placing), the block rules
  (shaped blocks' variants, doors, beds, support), survival (inventory,
  health, drops), creatures and their models, the first-person arm.
- `src/mods/`: finding, ordering and loading mods; the C API for mods' DLLs
  (`gocube_api.h`) and the C++ helper for writing one (`mod_sdk.h`);
  fingerprints.
- `src/core_mod/`: the base game's code (`core.dll`).
- `src/data/`: the game's data value and JSON (`json.h`, our own).
- `src/net/`: multiplayer: the host's checks, what
  travels between host and clients, the message format, and the transport (TCP,
  and UDP for positions, on the same port; Winsock only).
- `src/render/`, `shaders/`: Direct3D 11, the terrain, sky, item and creature
  shaders (HLSL, compiled by FXC at build time).
- `src/profiler.h`: the frame profiler (F3, `--profile`).
- `src/ui/`: the UI, our own: an immediate-mode core (`gui.h`: windows, layout,
  buttons, text fields, sliders, drop-downs, scrolling; tested with a fake font)
  drawn with Direct2D and DirectWrite (`gui_d2d.h`) in Windows' own UI
  font (Segoe UI); the inventory, container and settings screens.
- `mods/core/`: the base game: its manifest, content and assets.
- `tests/`: unit tests (with our own small framework, `tests/check.h`): world generation (a fingerprint, what the land
  is like), player movement and editing, saving, mods (DLLs loading, failing
  and crashing, undoing, events, commands, fingerprints; the test mods' code
  is in `tests/mods/`), survival, the UI, and what rendering works out on the
  CPU (item models, held light, block shadows, skins, the profiler),
  multiplayer (host and client in one process). `tests/mp_smoke.cpp` plays a
  round between a host and a client in two processes (CTest runs it).

## Dependencies
None but Windows' own: Direct3D 11, Direct2D and DirectWrite (the UI, in
Windows' font), XAudio2 (sound), Winsock (multiplayer), WIC (PNG images and
screenshots), the Compression API (mod packs). The build uses
[nob.h](https://github.com/tsoding/nob.h) 3.10.1 (`nob.h`, MIT or public
domain), the only third-party file, used by `nob.c` alone.
