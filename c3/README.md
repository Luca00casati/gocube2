# GoCube in C3

The game, ported from C++ (`../src`) to [C3](https://c3-lang.org) (c3c
0.8.6), without mod support: the base game's content is data
(`mods/core/content.json`, its textures and sounds) and its rules are game
code. Everything the C++ game does is here and playable: the title menu,
survival and creative, saves, multiplayer, the renderer, the UI, sound.
Where both must agree exactly (worlds generate the same, the block table and
atlas, chunk files) a test in each language checks the same recorded value.

    nob test           # build and run the C3 tests
    nob                # compile the shaders (fxc), build build/c3/gocube.exe
    nob run            # build and run it (options after --: see src/app/options.c3)

`nob` runs `c3c` from the path, or the one the `C3C` environment variable
names (`$env:C3C = "...\c3c.exe"`), and fxc from the path (a Developer
PowerShell). The shaders are compiled to `build/shaders/*.cso` and embedded
(`src/render/shaders.c3`). The project is `project.json`: floats are
`strict` (no fused multiply-add) so the numbers come out as the C++ build's.

Left out of the port: mods (DLLs, mod packs, downloading a host's mods), the
furnace and armour screens (the base game has neither), restarting sound when
the default device changes (it restarts when the device goes), presenting
with tearing when vsync is off, and the debug layer's message count.

## Layout

The modules follow `src/`'s folders: `gocube::voxel` is `src/voxel`, and so
on. Tests are in `test/`, by module, as `@test` functions.

## Order

Bottom up, each step tested against the C++ before what uses it: voxel,
data and content, game and world, net, then the platform, rendering, UI and
sound, and the game itself. All done; what's left is retiring the C++
(`src/`, `tests/`, the mod system and its folders) and making `nob` build
this game by default.

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

## Done

| Module | From | Checked by |
| --- | --- | --- |
| `gocube::voxel`: noise, chunks, generator, light, storage, mesher | `src/voxel/` | `test/voxel/generator_test.c3`: the generator configured by hand hashes as `tests/test_worldgen.cpp` records; `test/content/content_test.c3`: the world of seed 777 has the C++ game's fingerprint (blocks, chunk files, light) |
| `gocube::data`: the data value, JSON | `src/data/` | `test/data/json_test.c3` (`tests/test_save.cpp`'s JSON cases) |
| `gocube::content`: content.json, block ids, variants, the atlas, PNG (our own inflate), painted faces | `src/content/content.*`, `image.*` | `test/content/content_test.c3`: the block table and atlas hash as `tests/test_image.cpp` records |
| `gocube::world`: the world (threads, light, meshing jobs), saves | `src/world/` | `test/game/*`: the C++ game's survival and creative tests |
| `gocube::game`: blocks' rules, player, inventory, drops, chests and vats, creatures, vitals, day, settings, commands, the game and the base game's rules (plants, fluids, Block Eaters) | `src/game/`, `src/core_mod/` | `test/game/survival_test.c3`, `survival2_test.c3`, `commands_test.c3` |
| `gocube::net`: the transport (TCP + UDP), the wire format, sessions | `src/net/` | `test/net/transport_test.c3`, `multiplayer_test.c3` (host and client in one process) |
| `gocube::d3d11`, `renderer`, `chunk_renderer`, `shaders`, `mat`: Direct3D 11 through vtables generated from the SDK headers, the world's drawing, the shaders embedded | `src/render/` | `test/render/renderer_test.c3` (WARP: a cleared frame, the world, the UI, read back from screenshots) |
| `gocube::gui`, `gui_d3d`, `screens`, `settings_screen`: the immediate-mode UI, its Direct3D + GDI drawing, the game's screens | `src/ui/` | `test/ui/gui_test.c3` (`tests/test_gui.cpp`'s buttons, controls, text fields) |
| `gocube::sound`, `audio`: .wav, each ear's loudness, the underwater filter (an XAPO written in C3), XAudio2 | `src/audio/` | `test/audio/mixing_test.c3` (`tests/test_audio.cpp`'s numbers; a sound played on the device) |
| `gocube::window`, `win`, `profiler`, `game::hand`, `game::creature_models`, `app`: the window, Win32/GDI/user32 calls, the frame profiler, the hand and models, the game itself | `src/platform/`, `src/profiler.*`, `src/game/`, `src/main.cpp` | screenshots of the game, its menu and screens, and a host and a client in two processes |
