# Third-party code

Every library the game uses is here as source, pinned to a release and built by
`CMakeLists.txt` (statically, warnings off): nothing is downloaded when building.
Only the files the build needs were copied, unchanged, with each project's licence.

| Folder | Library | Version | From | Licence |
| --- | --- | --- | --- | --- |
| `lua/` | [Lua](https://www.lua.org) | 5.4.8 | `lua-5.4.8.tar.gz` (see `lua/README.md`) | MIT |
| `imgui/` | [Dear ImGui](https://github.com/ocornut/imgui), with the Win32 and DX11 backends | 1.93.0 | tag `v1.93.0` | MIT (`LICENSE.txt`) |
| `zlib/` | [zlib](https://github.com/madler/zlib) (mod packs) | 1.3.2 | tag `v1.3.2`, without the gz* file functions | zlib (`LICENSE`) |
| `doctest/` | [doctest](https://github.com/doctest/doctest) (tests only) | 2.5.3 | tag `v2.5.3` | MIT (`LICENSE.txt`) |

To update one: take the new release's same files from its tag, replace the
folder's copies, change the version here, and build and run the tests.
