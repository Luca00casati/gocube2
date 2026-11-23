// Mixing the dye vat's dye from the gems thrown in (each gem item's "dye"
// colour). Red, green and blue (any colour that isn't a grey) choose the hue,
// mixed like light and as bright as it goes (red + green is yellow, 2 red + 1
// green orange); greys (white, black) shade it, by their share of all the
// gems (red + white is pink, blue + black navy; white and black alone make
// greys). Only the counts matter, not the order. (src/game/dye.*)
module gocube::game::dye;
import gocube::voxel, std::math;

extern fn double cbrt(double x) @cname("cbrt") @private;

struct Pigment
{
	Dye color; // (its rgb; the alpha is ignored)
	int count;
}

// The dye they make; NO_DYE for none.
fn Dye mix_dye(Pigment[] pigments)
{
	double[3] hue;
	double grey = 0;
	int colours = 0;
	int greys = 0;
	foreach (p : pigments)
	{
		if (p.count <= 0) continue;
		double[3] c = { (double)(p.color >> 16 & 255) / 255.0, (double)(p.color >> 8 & 255) / 255.0, (double)(p.color & 255) / 255.0 };
		double hi = math::max(c[0], c[1], c[2]);
		double lo = math::min(c[0], c[1], c[2]);
		if (hi - lo < 0.1) // a grey: it shades
		{
			grey += (c[0] + c[1] + c[2]) / 3.0 * p.count;
			greys += p.count;
		}
		else
		{
			for (int k = 0; k < 3; k++) hue[k] += c[k] * p.count;
			colours += p.count;
		}
	}
	if (colours + greys == 0) return voxel::NO_DYE;
	// The hue as bright as it goes (red + green: yellow, not a dark yellow).
	double top = math::max(hue[0], hue[1], hue[2]);
	int[3] out;
	for (int k = 0; k < 3; k++)
	{
		double h = colours > 0 && top > 0 ? hue[k] / top : 0.0;
		double v = (h * colours + grey) / (double)(colours + greys);
		out[k] = (int)math::round(math::clamp(v, 0.0, 1.0) * 255.0);
	}
	return voxel::make_dye(out[0], out[1], out[2]);
}

// sRGB (0xRRGGBB) -> CIELAB (D65).
fn double[3] to_lab(Dye d) @private
{
	double[3] c = { (double)(d >> 16 & 255) / 255.0, (double)(d >> 8 & 255) / 255.0, (double)(d & 255) / 255.0 };
	foreach (&v : c) *v = *v <= 0.04045 ? *v / 12.92 : math::pow((*v + 0.055) / 1.055, 2.4);
	double[3] xyz = { (0.4124 * c[0] + 0.3576 * c[1] + 0.1805 * c[2]) / 0.95047, 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2],
		(0.0193 * c[0] + 0.1192 * c[1] + 0.9505 * c[2]) / 1.08883 };
	double[3] f;
	for (int k = 0; k < 3; k++) f[k] = xyz[k] > 0.008856 ? cbrt(xyz[k]) : 7.787 * xyz[k] + 16.0 / 116.0;
	return { 116.0 * f[1] - 16.0, 500.0 * (f[0] - f[1]), 200.0 * (f[1] - f[2]) };
}

// The colour of `palette` nearest to `dye` as the eye sees it (CIELAB); the
// dye as it is when the palette is empty (or it is NO_DYE). The dye vat's
// mixes snap to the game's natural tones (rules::DYE_PALETTE).
fn Dye snap_dye(Dye dye, Dye[] palette)
{
	if (dye == voxel::NO_DYE || palette.len == 0) return dye;
	double[3] want = to_lab(dye);
	Dye best = palette[0];
	double best_d = 1e30;
	foreach (p : palette)
	{
		double[3] lab = to_lab(p);
		double d = (lab[0] - want[0]) * (lab[0] - want[0]) + (lab[1] - want[1]) * (lab[1] - want[1]) + (lab[2] - want[2]) * (lab[2] - want[2]);
		if (d < best_d)
		{
			best_d = d;
			best = p;
		}
	}
	return best | 0xFF000000;
}
