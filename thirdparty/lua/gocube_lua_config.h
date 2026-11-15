/* Forced into every Lua source file (CMakeLists.txt). */
#pragma once

/* A fixed string-hash seed instead of one from the clock and memory addresses:
   mods' tables are then walked by pairs() in the same order on every run and
   every machine, so what a mod registers depends only on its files. */
#define luai_makeseed(L) 0x5eed1e55u
