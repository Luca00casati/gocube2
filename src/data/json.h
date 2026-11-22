#pragma once
// Data files and data as text: JSON (RFC 8259), read and written by our own
// code. Mods' manifests (mod.json) and content files, a world's world.json,
// the settings, what travels over the network and across the modding API
// (mods/gocube_api.h) are all JSON.
//
// In memory it's a Data value (data/data.h): JSON's objects keep their keys in
// the order they're written; a number without a fraction or exponent that
// fits is an integer, any other a float. Reading is strict JSON (no comments,
// no trailing commas), nested at most MAX_DATA_DEPTH deep, whatever the input.

#include "data/data.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace gc {

constexpr int MAX_DATA_DEPTH = 32;

// Reads `text` (named `name` in errors). False and `error` if it isn't JSON.
bool parse_json(std::string_view text, const std::string &name, Data &out, std::string &error);
bool read_json_file(const std::filesystem::path &path, Data &out, std::string &error);

// `data` as JSON: objects' keys sorted, one entry a line where it doesn't fit
// on one, a line break at the end. Throws std::invalid_argument for nan or
// inf. `compact`: on one line, no spaces (for the network and the modding API).
std::string write_json(const Data &data, bool compact = false);
// Writes it next to `path` and moves it over (never half a file). False on failure.
bool save_json_file(const std::filesystem::path &path, const Data &data);

} // namespace gc
