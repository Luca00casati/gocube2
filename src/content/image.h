#pragma once
// Block and item images: the few drawn ones are PPM files (binary P6 or text
// P3, 8-bit), and pure magenta (#ff00ff) in them is see-through (PPM has no
// alpha); every other face is painted here from a colour and some noise, as a
// mod's block definition describes it (its `textures` face given as a table).

#include "data/data.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gc {

// An HTML colour: "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa" ('#' optional).
bool parse_html_color(std::string s, uint8_t out[4]);

// An image as 8-bit RGBA, width x height.
struct Image {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

// Reads a PPM: false if it isn't one (or can't be read).
bool parse_ppm(std::string_view bytes, Image &out);
bool read_ppm(const std::filesystem::path &path, Image &out);

// A face painted from `spec`, `size` x `size` texels (a power of two, 8 or
// more): a grid of "pixels" (default 16 across) of the colour "color", each one
// lighter or darker ("noise": how much, in "shades" steps; "blotch": how much
// it follows larger patches, "blotch_size" pixels across), or one of the
// "colors" ramp instead. On top, in this order (each a table, or a list of
// them): "cells" (stones: "size", "edge" colour, "colors" to pick from),
// "stripes" ("color", "every", "width", "offset", "across" for rows,
// "wobble", "broken", "seams" colour between boards), "rings" (a log's end:
// "color", "every", "border" colour, "border_width"), "band" (a grassy top:
// "color", "depth", "ragged") and "spots" (ore: "color", "light", "dark",
// "count", "size", "shape" = "dash" for short lines). The same spec always
// paints the same pixels, and the face tiles without seams. Unreadable
// values fall back to defaults; a bad colour is magenta.
Image paint_face(const Data &spec, int size);

} // namespace gc
