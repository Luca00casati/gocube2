# Lua 5.4.8

The sources of [Lua](https://www.lua.org) 5.4.8 (`src/` of `lua-5.4.8.tar.gz`,
SHA-256 `4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae`),
unchanged, without the standalone `lua.c` and `luac.c`. MIT licence: `LICENSE`.

Built by CMakeLists.txt with `gocube_lua_config.h` forced in, which fixes the
string-hash seed (`luai_makeseed`) so `pairs` runs the same way every time.
