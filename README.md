# GoCube

Voxel sandbox for Windows, in [C3](https://c3-lang.org) with Direct3D 11.

Everything in it is a block: there are no other items, no creatures and no
enemies. Survival: mining by hand, drops, health, the day and night,
trees coming up on grass by themselves, flowing water and lava; sound;
creative; multiplayer (one player hosts, others join); a title menu and pause
menu. A sky with a sun, moon, stars and drifting clouds; moonlit nights (the
moon lights the land and casts the block shadows, and goes through its
phases: a new moon's night is very dark); an orange-red haze at sunset; fog,
block shadows, light from lamps and a little of the player's own, dropped
blocks and a first-person arm in your skin.
A lonely, uncanny mood: wind out in the open that sometimes stops dead,
rare strange sounds underground and your footsteps coming back late, a
block breaking far off with nobody there, fog that closes in for a while,
some nights a small light far off that's gone when you come near, and (once
there is some in the "core:music" sound set) now and then a piece of music
between long silences.
Breaking, placing and stepping on a block plays the recording of its material
(stone, wood, grass, gravel, sand, snow, dirt, glass), chosen from what the
block is (glass, plants, soft, falling, woody or hard; `src/audio/block_sound.c3`),
pitched a little differently each time and from the block (a material with
several recordings plays them as recorded instead); a block's "sound" names
another sound set.
Landing makes the sound of the block landed on, even from a little jump.
Sounds are .flac (lossless, about half the size or less; the game reads
nothing else, with its own decoder, `src/audio/flac.c3`). The recordings are
cut from long takes kept in `refs/` (not in git): `tools/cut_takes.py` cuts
a take into single hits, `tools/make_loop.py` makes the wind loop seamless,
`tools/to_flac.py` turns the .wav files into .flac; `tools/make_mood_sounds.py`
synthesizes some of the cave sounds.
To check it without listening: `--record=DIR` saves frames and everything
heard, and `tools/review_audio.py DIR/audio.wav` draws it.
The terrain comes from 3D noise: hills, cliffs, overhangs, seas with sand and
gravel beaches, caves and lava deep down, huge dark hollows and ravines,
rare floating islands and perfectly flat patches, and here and there an
empty structure nobody explains (a pillar, a dirt hut, a shaft to a tunnel
that goes nowhere, a broken wall) or something just wrong (sand hanging in
the air, a perfect cube of stone, a tree with no leaves); deep down, tunnels
dead straight for a hundred blocks. Players wear skins in the
usual box-figure layout (`%APPDATA%\GoCube\skin.png`, 64x64 or 64x32, a
PNG with see-through parts), and everyone sees each player's. The game's
content is data (`content/content.json`: blocks, trees,
sounds; with its textures and sounds); its rules are game code. Every
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
    c3c benchmark                # noise, terrain, light and shadow timings (src/bench)
    c3c run -- --world=Test      # build, then play (arguments after --)

The shaders (`shaders\`, HLSL) are embedded in the game and compiled when it
starts, by Windows' own shader compiler (`d3dcompiler_47.dll`), so there's no
build step for them. Floats are `strict` (no fused multiply-add, no
reordering), so worlds generate the same everywhere.

The tests (`test\`, `@test` functions, by module) cover world generation
(the fingerprint of seed 777's world, the generator's hash), the block table
and atlas, JSON, saving, the player, survival and creative, commands,
multiplayer (a host and a client in one process: joining, block changes,
survival, chat, the host's checks), sound (mixing, blocks' made
sounds, and a sound played on the device), the UI's controls, and rendering on WARP (a frame, the world
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
| Left button | Break the block aimed at (hold it in survival) |
| Right button | Place the held block |
| 1-9, wheel | Choose a hotbar slot |
| Middle button | Creative: take the block aimed at |
| E | Inventory (creative: every block to take from) |
| Q | Throw what you hold (Ctrl+Q: the whole stack) |
| T, / | Chat (/ starts a command); Enter sends, Esc closes, up and down for what you sent, Tab completes a command |
| Esc | Pause menu (settings; save and quit) |

Survival without tools, weapons, armour or smelting, and only one recipe
(planks from a log): everything is mined by hand (hardness x 0.375 s) and
drops itself (or what it gives: stone gives cobblestone), nothing
that hurts (no fall damage, lava doesn't burn, no drowning; falling out of
the world brings you back at the world spawn; only /kill takes
health), a day that
lasts a real day by default, no beds, chests, doors, ladders, stairs or cacti, falling sand,
trees sprouting on grass, decaying leaves. One kind of grass and of wood (oak), no
saplings, no small plants (flowers, tall grass), no farming and no food: health
comes back by itself. The world is saved every minute and
when you quit.

Creative uses the same inventory as survival (a new creative player starts
with it empty): take blocks from the inventory screen's palette,
pick up what lies around; placing never uses anything up, and nothing hurts.

Settings (the title menu's Settings button, or the pause menu's): view distance,
mouse sensitivity, frame rate limit, vsync, smooth lighting, a retro screen
filter (vignette, film grain, vivid colours), sun shadows,
render scale, volumes, the clock and the keys. Changes show at once; they're kept in
`%APPDATA%\GoCube\settings.json`, with your name and the last address you joined.

Playing together: in the title menu, tick "Open the world I play to others" and
play a world (others join it on port 24680: TCP, and UDP too if it gets through,
for smoother movement), or type the host's address and
"Join their world". The host's world is the world: it checks and does what
joined players do, keeps their inventories and health (saved in its world, by
name), and runs the dropped blocks and time. Everyone plays the same game, so any copy
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
| `--drop=ID,...` | Drop these blocks in front of the player (trying things out) |
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
| `/give <player> <block> [count]` | Blocks into a player's inventory (`planks` means `core:planks`; `@s` is you) |
| `/time [set day\|noon\|night\|midnight\|0..1]` | The time of day |
| `/gamemode [survival\|creative]` | The world's game mode, for everyone (inventories stay as they are) |
| `/kill [player]` | Ends a survival player's life |
| `/spawnpoint` | The world's spawn where you stand |
| `/setblock <x y z> <block>`, `/fill <x1 y1 z1> <x2 y2 z2> <block>` | Change blocks (a fill: 32768 at most) |
| `/kick <player> [reason]`, `/op <player>`, `/deop <player>` | Players |

## Layout
All the code is in `src\`, a module per folder (tests in `test\`, the
same way):
- `voxel/`: chunks, noise, world generation, light, meshing, chunk files.
- `content/`: `content.json`, block ids and variants, the texture atlas (most
  block faces are painted from a colour and noise; the few drawn images are
  PNG files, read by our own decoder), blocks' models as held and dropped,
  skins and the generator's tables.
- `world/`: chunk streaming on worker threads, editing, the raycast,
  collision; the world folder (`world.json`, `chunks\`).
- `game/`: the player (movement, breaking, placing), the block rules (shaped
  blocks' variants, support), survival (inventory, health,
  drops), players' figures, the day, commands, the first-person
  hand, and the base game's rules (`rules.c3`: mining time, drops, fall
  damage, the recipe; `core.c3`: plants, fluids).
- `data/`: the game's data value and JSON (our own).
- `net/`: multiplayer: the session (the host's checks, what travels between
  host and clients), the message format, and the transport (TCP, and UDP for
  positions, on the same port; Winsock only).
- `render/`, with `shaders\` (HLSL): Direct3D 11, called through vtables
  generated from the Windows SDK's headers; the terrain, sky, dropped
  blocks and players' figures.
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
