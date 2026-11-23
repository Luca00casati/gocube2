// 4x4 matrices as DirectXMath has them: row-major, row vectors (a point is
// transformed as v * M; A * B does A first, then B). The models build their
// nodes this way, and the shaders read them so.
module gocube::mat;
import std::math;

struct Matrix
{
	float[16] m;
}

fn Matrix identity() => { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
fn Matrix scaling(float x, float y, float z) => { { x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1 } };
fn Matrix translation(float x, float y, float z) => { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 } };

fn Matrix rotation_x(float a)
{
	float c = math::cos(a);
	float s = math::sin(a);
	return { { 1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1 } };
}

fn Matrix rotation_y(float a)
{
	float c = math::cos(a);
	float s = math::sin(a);
	return { { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1 } };
}

fn Matrix rotation_z(float a)
{
	float c = math::cos(a);
	float s = math::sin(a);
	return { { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
}

fn Matrix Matrix.mul(&self, Matrix b) @operator(*)
{
	Matrix r;
	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++) s += self.m[i * 4 + k] * b.m[k * 4 + j];
			r.m[i * 4 + j] = s;
		}
	}
	return r;
}

// A point (w = 1), divided by the resulting w (XMVector3TransformCoord).
fn float[3] Matrix.point(&self, float[3] p)
{
	float[4] r;
	for (int j = 0; j < 4; j++) r[j] = p[0] * self.m[j] + p[1] * self.m[4 + j] + p[2] * self.m[8 + j] + self.m[12 + j];
	return { r[0] / r[3], r[1] / r[3], r[2] / r[3] };
}

// A direction (w = 0, no translation).
fn float[3] Matrix.direction(&self, float[3] p)
{
	float[3] r;
	for (int j = 0; j < 3; j++) r[j] = p[0] * self.m[j] + p[1] * self.m[4 + j] + p[2] * self.m[8 + j];
	return r;
}

fn Matrix Matrix.transpose(&self)
{
	Matrix r;
	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++) r.m[j * 4 + i] = self.m[i * 4 + j];
	}
	return r;
}

// A node's transform: scale, then Euler angles in YXZ order (Z turned
// first), then the position.
fn Matrix node(float[3] at, float[3] euler = { 0, 0, 0 }, float[3] scale = { 1, 1, 1 })
	=> scaling(scale[0], scale[1], scale[2]) * rotation_z(euler[2]) * rotation_x(euler[0]) * rotation_y(euler[1]) * translation(at[0], at[1], at[2]);

// A right-handed perspective (XMMatrixPerspectiveFovRH): depth 0 at `near`, 1 at `far`.
fn Matrix perspective_fov_rh(float fov_y, float aspect, float near, float far)
{
	float h = math::cos(fov_y * 0.5f) / math::sin(fov_y * 0.5f);
	float w = h / aspect;
	float range = far / (near - far);
	return { { w, 0, 0, 0, 0, h, 0, 0, 0, 0, range, -1, 0, 0, range * near, 0 } };
}

// Looking from `eye` along `dir` (XMMatrixLookToRH).
fn Matrix look_to_rh(float[3] eye, float[3] dir, float[3] up)
{
	float[3] z = normalize({ -dir[0], -dir[1], -dir[2] });
	float[3] x = normalize(cross(up, z));
	float[3] y = cross(z, x);
	return { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
		-dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
}

// An orthographic box (XMMatrixOrthographicOffCenterRH).
fn Matrix ortho_rh(float left, float right, float bottom, float top, float near, float far)
{
	float range = 1.0f / (near - far);
	return { { 2 / (right - left), 0, 0, 0, 0, 2 / (top - bottom), 0, 0, 0, 0, range, 0,
		(left + right) / (left - right), (top + bottom) / (bottom - top), range * near, 1 } };
}

fn float[3] cross(float[3] a, float[3] b) => { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
fn float dot(float[3] a, float[3] b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

fn float[3] normalize(float[3] v)
{
	float l = math::sqrt(dot(v, v));
	return l > 0 ? { v[0] / l, v[1] / l, v[2] / l } : v;
}

// The inverse (identity if it has none).
fn Matrix Matrix.inverse(&self)
{
	float[16] m = self.m;
	float[16] inv;
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0) return identity();
	Matrix r;
	for (int i = 0; i < 16; i++) r.m[i] = inv[i] / det;
	return r;
}
