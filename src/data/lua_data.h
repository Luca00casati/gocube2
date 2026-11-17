#pragma once
// Data files as Lua tables: a file is `return { ... }` (mods' mod.lua, a
// world's world.lua). Read in a Lua state of its own with nothing in it (no
// libraries, no functions: only table constructors, strings, numbers and
// booleans mean anything), as text (no bytecode), with a step and memory limit:
// a data file can't do anything but describe its data.
//
// In memory the data is a Data value (data/data.h, the game's dynamic value type):
// a table of 1..n is a list, any other table an object with string keys (an
// empty table reads back as an empty object); integers stay integers, other
// numbers floats.

#include "content/content.h"

#include <filesystem>
#include <string>
#include <string_view>

struct lua_State;

namespace gc {

// Reads `text` (named `name` in errors). False and `error` if it isn't one.
bool parse_lua_data(std::string_view text, const std::string &name, Data &out, std::string &error);
bool read_lua_data(const std::filesystem::path &path, Data &out, std::string &error);

// `data` as a Lua data file ("return {...}\n"): objects' keys sorted, one entry
// a line where it doesn't fit on one. Lists can't hold null (a hole in a Lua
// list): it throws std::invalid_argument; null object fields are left out.
std::string write_lua_data(const Data &data);
// Writes it next to `path` and moves it over (never half a file). False on failure.
bool save_lua_data(const std::filesystem::path &path, const Data &data);

// The Lua value at `idx` as data; raises a Lua error for what data can't be
// (functions, non-string keys, nesting deeper than 32). For the mods' API too.
Data lua_to_data(lua_State *L, int idx, int depth = 0);
void push_data(lua_State *L, const Data &data);

} // namespace gc
