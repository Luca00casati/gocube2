#pragma once
// Mixing the dye vat's dye from the gems thrown in (each gem item's "dye"
// colour, so mods can add more). Red, green and blue (any colour that isn't a
// grey) choose the hue, mixed like light and as bright as it goes (red +
// green is yellow, 2 red + 1 green orange); greys (white, black) shade it,
// by their share of all the gems (red + white is pink, blue + black navy;
// white and black alone make greys). Only the counts matter, not the order.

#include "voxel/chunk.h"

#include <vector>

namespace gc {

struct Pigment {
	gocube::Dye color = gocube::NO_DYE; // (its rgb; the alpha is ignored)
	int count = 0;
};

// The dye they make; NO_DYE for none.
gocube::Dye mix_dye(const std::vector<Pigment> &pigments);

// The colour of `palette` nearest to `dye` as the eye sees it (CIELAB); the
// dye as it is when the palette is empty (or it is NO_DYE). The dye vat's
// mixes snap to the base game's natural tones (the "dye_palette" rule).
gocube::Dye snap_dye(gocube::Dye dye, const std::vector<gocube::Dye> &palette);

} // namespace gc
