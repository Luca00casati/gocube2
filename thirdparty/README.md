# Third-party code

Every library (and the font) the game uses is here as source, pinned to a release and built by
`CMakeLists.txt` (statically, warnings off): nothing is downloaded when building.
Only the files the build needs were copied, unchanged, with each project's licence.

| Folder | Library | Version | From | Licence |
| --- | --- | --- | --- | --- |
| `lua/` | [Lua](https://www.lua.org) | 5.4.8 | `lua-5.4.8.tar.gz` (see `lua/README.md`) | MIT |
| `doctest/` | [doctest](https://github.com/doctest/doctest) (tests only) | 2.5.3 | tag `v2.5.3` | MIT (`LICENSE.txt`) |
| `font/Share_Tech/` | [Share Tech](https://fonts.google.com/specimen/Share+Tech) (the UI's font, built into the executable) | 2012 | Google Fonts | SIL Open Font License 1.1 (`OFL.txt`) |

To update one: take the new release's same files from its tag, replace the
folder's copies, change the version here, and build and run the tests.
