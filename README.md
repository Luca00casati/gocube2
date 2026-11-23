# GoCube

Voxel sandbox for Windows, in [C3](https://c3-lang.org) with Direct3D 11.

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
PNG with see-through parts), and everyone sees each player's. The game's
content is data (`content/content.json`: blocks, items, ores, trees, the
biome, sounds; with its textures and sounds); its rules are game code. Every
data file (`content.json`, a world's `world.json`, the settings) is JSON, read
and written by our own code.

Worlds made by earlier versions open unchanged: the generator makes the same
terrain, bit for bit, and the files are the same.

## Requirements
- Windows 10 or 11, x64
- A GPU with Direct3D 11 at feature level 11_0 (or `--warp` to render in software)
- [c3c](https://github.com/c3lang/c3c) 0.8.6, the C3 compiler, and the
  Windows SDK's libraries, which it links against (installed with Visual
  Studio or its Build Tools, or the Windows SDK on its own). Nothing else: no
  build tool, no third-party libraries.
- Optional: Windows' **Graphics Tools** (Settings > System > Optional features) for
  the Direct3D debug layer (`--debug`)

Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell) graphics
such as the HD 4600, where every Direct3D 12 draw hangs the GPU; their Direct3D 11
drivers are mature, and nothing a voxel game needs is missing from Direct3D 11.

## Build and test
The project is `project.json`; in this folder:

    c3c build                    # build build\gocube.exe
    c3c test                     # build, then run the tests
    c3c run -- --world=Test      # build, then play (arguments after --)

The shaders (`shaders\`, HLSL) are embedded in the game and compiled when it
starts, by Windows' own shader compiler (`d3dcompiler_47.dll`), so there's no
build step for them. Floats are `strict` (no fused multiply-add, no
reordering), so worlds generate the same everywhere.

The tests (`test\`, `@test` functions, by module) cover world generation
(the fingerprint of seed 777's world, the generator's hash), the block table
and atlas, JSON, saving, the player, survival and creative, commands,
multiplayer (a host and a client in one process: joining, block changes,
survival, chests, chat, the host's checks), sound (mixing, and a sound played
on the device), the UI's controls, and rendering on WARP (a frame, the world
and the UI, read back from screenshots in `build\`). They run in the test
runner's leak-tracking allocator: a test that leaks fails.

The game is `build\gocube.exe`; it finds `content\` in its folder or a
folder above it.

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
render scale, volumes, the clock and the keys. Changes show at once; they're kept in
`%APPDATA%\GoCube\settings.json`, with your name and the last address you joined.

Playing together: in the title menu, tick "Open the world I play to others" and
play a world (others join it on port 24680: TCP, and UDP too if it gets through,
for smoother movement), or type the host's address and
"Join their world". The host's world is the world: it checks and does what
joined players do, keeps their inventories and health (saved in its world, by
name), and runs the creatures, items, chests and time; the night is
skipped once everyone is in bed. Everyone plays the same game, so any copy
of the same version can join any host.

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
| `--frames=N` | Quit after N frames |
| `--screenshot=FILE` | Save the last frame as a PNG (frames are counted once the world around has loaded) |
| `--render-scale=F` | Draw the world at this part of the window's resolution (0.25 to 1; stretched over it; default: the settings') |
| `--host[=PORT]` | Open the world to others (default port 24680) |
| `--join=ADDRESS[:PORT]` | Play in the world of the host at this address |
| `--name=NAME` | The name others see (default "Player"; the host keeps a survival player's things by it) |
| `--no-sound` | Play without sound (`--hidden` never plays any) |
| `--no-vsync` | Present as fast as possible |
| `--debug`, `--no-debug` | The Direct3D 11 debug layer on or off (default: off; needs Windows' Graphics Tools) |

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

## Layout
All the code is in `src\`, a module per folder (tests in `test\`, the
same way):
- `voxel/`: chunks, noise, world generation, light, meshing, chunk files.
- `content/`: `content.json`, block ids and variants, the texture atlas (most
  block faces are painted from a colour and noise; the few drawn images are
  PNG files, read by our own decoder), item models, skins and the generator's
  tables.
- `world/`: chunk streaming on worker threads, editing, the raycast,
  collision; the world folder (`world.json`, `chunks\`).
- `game/`: the player (movement, breaking, placing), the block rules (shaped
  blocks' variants, doors, beds, support), survival (inventory, health,
  drops), chests and dye vats, creatures and their models, the day, commands,
  the first-person hand, and the base game's rules (`rules.c3`: mining time,
  drops, fall damage, food, the two recipes; `core.c3`: plants, fluids, Block
  Eaters).
- `data/`: the game's data value and JSON (our own).
- `net/`: multiplayer: the session (the host's checks, what travels between
  host and clients), the message format, and the transport (TCP, and UDP for
  positions, on the same port; Winsock only).
- `render/`, with `shaders\` (HLSL): Direct3D 11, called through vtables
  generated from the Windows SDK's headers; the terrain, sky, item and
  creature drawing.
- `ui/`: the UI, our own: an immediate-mode core (`gui.c3`: windows, layout,
  buttons, text fields, sliders, drop-downs, scrolling; tested with a fake
  font) drawn with Direct3D 11 (`gui_d3d.c3`, `shaders\gui.hlsl`) in Segoe UI,
  rasterized by GDI; the inventory, container and settings screens.
- `audio/`: `.wav` files, each ear's loudness, XAudio2, the underwater filter
  (an XAPO written in C3).
- `platform/`: the window, Win32 calls, the clock, the log; `profiler.c3`:
  the frame profiler (F3, `--profile`).
- `app/`: the game itself: options, the title menu, playing, the HUD.

`content\` holds the game's content and assets.

## Conventions
- Containers that outlive a function are `gocube::util::vec::Vec{T}` and
  `gocube::util::map::Map{K, V}`: a zero value is ready and uses the heap.
  (The standard library's `List`, `HashMap` and `DString` start on the
  temporary allocator when not initialized, so what they hold is freed at the
  end of the enclosing `@pool`.) A `DString` that must last is `init(mem)`ed.
- Everything that owns memory has `free()`; `Data` owns what it holds, and
  `set()`/`push()` take values over.
- Methods can't be `@private` (it's ignored); constants from another module
  need its prefix (`voxel::CHUNK`).

## Dependencies
None but Windows' own: Direct3D 11 and DXGI, GDI (the UI's text), XAudio2
(sound), Winsock (multiplayer), user32, shell32 (the Recycle Bin), winmm (the
timer), bcrypt (random numbers) and the D3D shader compiler.
