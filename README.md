# GoCube

Voxel sandbox for Windows, in C++20 with Direct3D 11, gameplay and mods in Lua.
A rewrite of the Godot version of GoCube; the plan is in the
[GoCube Native Rewrite Plan](https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy).

Status: milestone 1 (a window with Direct3D 11 and Dear ImGui).

## Requirements
- Windows 10 or 11, x64
- A GPU with Direct3D 11 at feature level 11_0 (or `--warp` to render in software)
- Visual Studio 2022 or 2026 with "Desktop development with C++" (it brings MSVC,
  CMake, Ninja and vcpkg)
- Optional: Windows' **Graphics Tools** (Settings > System > Optional features) for
  the Direct3D debug layer in debug builds

Direct3D 11, not 12: Intel disabled Direct3D 12 on 4th-generation (Haswell) graphics
such as the HD 4600, where every Direct3D 12 draw hangs the GPU; their Direct3D 11
drivers are mature, and nothing a voxel game needs is missing from Direct3D 11.

## Build and test
From a **Developer PowerShell for VS** (it sets `VCPKG_ROOT`), in this folder:

    cmake --preset windows        # first time: vcpkg builds the dependencies
    cmake --build --preset debug  # or: release
    ctest --preset debug          # or: release

Or open the folder in Visual Studio, which reads `CMakePresets.json`.
The game is `build\Debug\gocube.exe` (`build\Release\gocube.exe`).

## Options
| Option | What it does |
| --- | --- |
| `--warp` | Render on WARP, Windows' software adapter (no GPU needed) |
| `--hidden` | Don't show the window (tests) |
| `--frames=N` | Quit after N frames; exit code 1 if the debug layer reported errors or warnings |
| `--no-vsync` | Present as fast as possible |
| `--debug`, `--no-debug` | Force the Direct3D 11 debug layer on or off (default: on in debug builds) |

## Dependencies
Pinned through `vcpkg.json` (its `builtin-baseline`), linked statically:
[Dear ImGui](https://github.com/ocornut/imgui) (MIT).
