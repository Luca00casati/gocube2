# GoCube

Voxel sandbox for Windows, in C++20 with Direct3D 11, gameplay and mods in Lua.
A rewrite of the Godot version of GoCube; the plan is in the
[GoCube Native Rewrite Plan](https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy).

Status: milestone 2. The world is generated exactly as in the Godot version and
streamed around a flying camera.

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
Click in the window to look around with the mouse (Esc lets go). W A S D fly,
Space goes up, Shift down, Ctrl flies faster.

| Option | What it does |
| --- | --- |
| `--seed=N` | World seed (default 777) |
| `--generator=N` | World generator version: 1 (before biomes), 2, 3 (default, the newest) |
| `--view=N` | View distance in blocks (default 128) |
| `--pos=X,Y,Z`, `--look=YAW,PITCH` | Start position, and view direction in degrees |
| `--warp` | Render on WARP, Windows' software adapter (no GPU needed) |
| `--hidden` | Don't show the window (tests) |
| `--frames=N` | Quit after N frames; exit code 1 if the debug layer reported errors or warnings |
| `--screenshot=FILE` | Save the last frame as a PNG (frames are counted once the world around has loaded) |
| `--no-vsync` | Present as fast as possible |
| `--debug`, `--no-debug` | Force the Direct3D 11 debug layer on or off (default: on in debug builds) |

## Layout
- `src/voxel/`: the voxel engine (chunks, world generation, light, meshing, chunk
  files), copied unchanged from the Godot version's GDExtension (GoCube `4425f46`).
- `src/content/`: mods' JSON content, block ids, the block table, the texture atlas
  and the generator's tables, as the Godot version builds them.
- `src/world/`: chunk streaming on worker threads.
- `src/render/`, `shaders/`: Direct3D 11 and the terrain shaders (HLSL, compiled by
  FXC at build time).
- `mods/core/`: the base game's content and assets. Its `main.gd` (one block and one
  recipe registered from code) is `content/planks.json` until Lua arrives.
- `tests/`: doctest unit tests, including the generation fingerprints.

## Dependencies
Pinned through `vcpkg.json` (its `builtin-baseline`), linked statically:
[Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[nlohmann/json](https://github.com/nlohmann/json) (MIT),
[stb](https://github.com/nothings/stb) (MIT or public domain),
[doctest](https://github.com/doctest/doctest) (MIT, tests only).
