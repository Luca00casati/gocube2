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

namespace {

// sRGB (0xRRGGBB) -> CIELAB (D65).
void to_lab(gocube::Dye d, double lab[3]) {
	double c[3] = {double(d >> 16 & 255) / 255.0, double(d >> 8 & 255) / 255.0, double(d & 255) / 255.0};
	for (double &v : c) {
		v = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
	}
	const double xyz[3] = {(0.4124 * c[0] + 0.3576 * c[1] + 0.1805 * c[2]) / 0.95047,
			0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2], (0.0193 * c[0] + 0.1192 * c[1] + 0.9505 * c[2]) / 1.08883};
	double f[3];
	for (int k = 0; k < 3; k++) {
		f[k] = xyz[k] > 0.008856 ? std::cbrt(xyz[k]) : 7.787 * xyz[k] + 16.0 / 116.0;
	}
	lab[0] = 116.0 * f[1] - 16.0;
	lab[1] = 500.0 * (f[0] - f[1]);
	lab[2] = 200.0 * (f[1] - f[2]);
}

} // namespace

gocube::Dye snap_dye(gocube::Dye dye, const std::vector<gocube::Dye> &palette) {
	if (dye == gocube::NO_DYE || palette.empty()) {
		return dye;
	}
	double want[3];
	to_lab(dye, want);
	gocube::Dye best = palette[0];
	double best_d = 1e30;
	for (const gocube::Dye p : palette) {
		double lab[3];
		to_lab(p, lab);
		const double d = (lab[0] - want[0]) * (lab[0] - want[0]) + (lab[1] - want[1]) * (lab[1] - want[1]) +
				(lab[2] - want[2]) * (lab[2] - want[2]);
		if (d < best_d) {
			best_d = d;
			best = p;
		}
	}
	return best | 0xFF000000u;
}

} // namespace gc
