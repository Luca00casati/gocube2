#include "game/dye.h"

#include <algorithm>
#include <cmath>

namespace gc {

gocube::Dye mix_dye(const std::vector<Pigment> &pigments) {
	double hue[3] = {0, 0, 0}, grey = 0;
	int colours = 0, greys = 0;
	for (const Pigment &p : pigments) {
		if (p.count <= 0) {
			continue;
		}
		const double c[3] = {double(p.color >> 16 & 255) / 255.0, double(p.color >> 8 & 255) / 255.0,
				double(p.color & 255) / 255.0};
		const double hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
		if (hi - lo < 0.1) { // a grey: it shades
			grey += (c[0] + c[1] + c[2]) / 3.0 * p.count;
			greys += p.count;
		} else {
			for (int k = 0; k < 3; k++) {
				hue[k] += c[k] * p.count;
			}
			colours += p.count;
		}
	}
	if (colours + greys == 0) {
		return gocube::NO_DYE;
	}
	// The hue as bright as it goes (red + green: yellow, not a dark yellow).
	const double top = std::max({hue[0], hue[1], hue[2]});
	double out[3];
	for (int k = 0; k < 3; k++) {
		const double h = colours > 0 && top > 0 ? hue[k] / top : 0.0;
		out[k] = (h * colours + grey) / double(colours + greys);
	}
	auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
	return gocube::make_dye(byte(out[0]), byte(out[1]), byte(out[2]));
}

} // namespace gc
