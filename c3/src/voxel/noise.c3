// Gradient noise (2D/3D) and fractal sums, deterministic per seed.
// Self-contained: no external noise library. (src/voxel/noise.h)
module gocube::voxel;
import std::math;

// Integer hash of a lattice point; well mixed, cheap.
fn uint hash_point(int x, int y, int z, uint seed)
{
	uint h = seed * 0x9E3779B1;
	h ^= (uint)x * 0x85EBCA77;
	h = (h << 13) | (h >> 19);
	h ^= (uint)y * 0xC2B2AE3D;
	h = (h << 17) | (h >> 15);
	h ^= (uint)z * 0x27D4EB2F;
	h ^= h >> 16;
	h *= 0x7FEB352D;
	h ^= h >> 15;
	h *= 0x846CA68B;
	h ^= h >> 16;
	return h;
}

fn float smooth5(float t) @inline => t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);

fn float mixf(float a, float b, float t) @inline => a + (b - a) * t;

// Dot product of the offset with one of 8 unit directions picked by hash.
fn float grad2(uint h, float x, float y)
{
	const float D = 0.70710678f;
	switch (h & 7)
	{
		case 0: return x;
		case 1: return -x;
		case 2: return y;
		case 3: return -y;
		case 4: return (x + y) * D;
		case 5: return (x - y) * D;
		case 6: return (-x + y) * D;
		default: return (-x - y) * D;
	}
}

// Dot product with one of 12 directions towards cube edge midpoints.
fn float grad3(uint h, float x, float y, float z)
{
	switch (h % 12)
	{
		case 0: return x + y;
		case 1: return -x + y;
		case 2: return x - y;
		case 3: return -x - y;
		case 4: return x + z;
		case 5: return -x + z;
		case 6: return x - z;
		case 7: return -x - z;
		case 8: return y + z;
		case 9: return -y + z;
		case 10: return y - z;
		default: return -y - z;
	}
}

// Roughly in [-1, 1].
fn float noise2(float x, float y, uint seed)
{
	float fx = math::floor(x);
	float fy = math::floor(y);
	int ix = (int)fx;
	int iy = (int)fy;
	float dx = x - fx;
	float dy = y - fy;
	float n00 = grad2(hash_point(ix, iy, 0, seed), dx, dy);
	float n10 = grad2(hash_point(ix + 1, iy, 0, seed), dx - 1.0f, dy);
	float n01 = grad2(hash_point(ix, iy + 1, 0, seed), dx, dy - 1.0f);
	float n11 = grad2(hash_point(ix + 1, iy + 1, 0, seed), dx - 1.0f, dy - 1.0f);
	float u = smooth5(dx);
	float v = smooth5(dy);
	return mixf(mixf(n00, n10, u), mixf(n01, n11, u), v) * 1.4f;
}

// Roughly in [-1, 1].
fn float noise3(float x, float y, float z, uint seed)
{
	float fx = math::floor(x);
	float fy = math::floor(y);
	float fz = math::floor(z);
	int ix = (int)fx;
	int iy = (int)fy;
	int iz = (int)fz;
	float dx = x - fx;
	float dy = y - fy;
	float dz = z - fz;
	float[8] c;
	for (int i = 0; i < 8; i++)
	{
		int ox = i & 1;
		int oy = (i >> 1) & 1;
		int oz = (i >> 2) & 1;
		c[i] = grad3(hash_point(ix + ox, iy + oy, iz + oz, seed), dx - (float)ox, dy - (float)oy, dz - (float)oz);
	}
	float u = smooth5(dx);
	float v = smooth5(dy);
	float w = smooth5(dz);
	float x00 = mixf(c[0], c[1], u);
	float x10 = mixf(c[2], c[3], u);
	float x01 = mixf(c[4], c[5], u);
	float x11 = mixf(c[6], c[7], u);
	return mixf(mixf(x00, x10, v), mixf(x01, x11, v), w) * 0.95f;
}

// noise3 for many nearby points (cave noise: lattice cells span dozens of
// blocks): keeps the last cell's corner hashes. Same result as noise3.
struct Noise3Cell
{
	int ix, iy, iz;
	uint seed;
	bool valid;
	bool graded;   // g worked out from h
	uint[8] h;
	char[8] g;     // gradient per corner (h % 12)
}

const float[3][12] GRAD3 @private = {
	{ 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
	{ 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
	{ 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
};

fn float noise3_cached(float x, float y, float z, uint seed, Noise3Cell* cell)
{
	float fx = math::floor(x);
	float fy = math::floor(y);
	float fz = math::floor(z);
	int ix = (int)fx;
	int iy = (int)fy;
	int iz = (int)fz;
	if (!cell.valid || cell.ix != ix || cell.iy != iy || cell.iz != iz || cell.seed != seed)
	{
		for (int i = 0; i < 8; i++)
		{
			cell.h[i] = hash_point(ix + (i & 1), iy + ((i >> 1) & 1), iz + ((i >> 2) & 1), seed);
		}
		cell.ix = ix;
		cell.iy = iy;
		cell.iz = iz;
		cell.seed = seed;
		cell.valid = true;
		cell.graded = false;
	}
	float dx = x - fx;
	float dy = y - fy;
	float dz = z - fz;
	// grad3 without its branches: the same sums (only a zero's sign may differ).
	if (!cell.graded)
	{
		for (int i = 0; i < 8; i++) cell.g[i] = (char)(cell.h[i] % 12);
		cell.graded = true;
	}
	float[8] c;
	for (int i = 0; i < 8; i++)
	{
		float[3] g = GRAD3[cell.g[i]];
		c[i] = g[0] * (dx - (float)(i & 1)) + g[1] * (dy - (float)((i >> 1) & 1)) + g[2] * (dz - (float)((i >> 2) & 1));
	}
	float u = smooth5(dx);
	float v = smooth5(dy);
	float w = smooth5(dz);
	float x00 = mixf(c[0], c[1], u);
	float x10 = mixf(c[2], c[3], u);
	float x01 = mixf(c[4], c[5], u);
	float x11 = mixf(c[6], c[7], u);
	return mixf(mixf(x00, x10, v), mixf(x01, x11, v), w) * 0.95f;
}

// Fractal sum normalised to roughly [-1, 1].
fn float fbm2(float x, float y, uint seed, int octaves)
{
	float sum = 0.0f;
	float amp = 1.0f;
	float norm = 0.0f;
	for (int i = 0; i < octaves; i++)
	{
		sum += noise2(x, y, seed + (uint)i * 1013) * amp;
		norm += amp;
		amp *= 0.5f;
		x *= 2.0f;
		y *= 2.0f;
	}
	return sum / norm;
}

// Fractal sum of 3D noise, roughly [-1, 1].
fn float fbm3(float x, float y, float z, uint seed, int octaves)
{
	float sum = 0.0f;
	float amp = 1.0f;
	float norm = 0.0f;
	for (int i = 0; i < octaves; i++)
	{
		sum += noise3(x, y, z, seed + (uint)i * 1013) * amp;
		norm += amp;
		amp *= 0.5f;
		x *= 2.0f;
		y *= 2.0f;
		z *= 2.0f;
	}
	return sum / norm;
}
