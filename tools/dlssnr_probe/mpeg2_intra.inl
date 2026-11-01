// The 8-point DCT and an MPEG-2 intra block coder, for the artifact removal's tests:
// dlssnr_harness --tclean (clean_suite.inl) and playback_test --clean degrade a picture
// the same way with it. Double precision, orthonormal.

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// --------------------------------------------------------------------- DCT --

struct Dct8 {
	double m[8][8];   // m[u][n]
	Dct8()
	{
		for (int u = 0; u < 8; u++) {
			for (int n = 0; n < 8; n++) {
				m[u][n] = (u ? 0.5 : std::sqrt(0.125)) * std::cos((2 * n + 1) * u * 3.14159265358979323846 / 16.0);
			}
		}
	}
	void Forward(const double in[64], double out[64]) const
	{
		double t[64];
		for (int y = 0; y < 8; y++) {
			for (int u = 0; u < 8; u++) {
				double s = 0;
				for (int n = 0; n < 8; n++) {
					s += m[u][n] * in[y * 8 + n];
				}
				t[y * 8 + u] = s;
			}
		}
		for (int v = 0; v < 8; v++) {
			for (int u = 0; u < 8; u++) {
				double s = 0;
				for (int n = 0; n < 8; n++) {
					s += m[v][n] * t[n * 8 + u];
				}
				out[v * 8 + u] = s;
			}
		}
	}
	void Inverse(const double in[64], double out[64]) const
	{
		double t[64];
		for (int v = 0; v < 8; v++) {
			for (int x = 0; x < 8; x++) {
				double s = 0;
				for (int u = 0; u < 8; u++) {
					s += m[u][x] * in[v * 8 + u];
				}
				t[v * 8 + x] = s;
			}
		}
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				double s = 0;
				for (int v = 0; v < 8; v++) {
					s += m[v][y] * t[v * 8 + x];
				}
				out[y * 8 + x] = s;
			}
		}
	}
};

static const Dct8& Dct()
{
	static const Dct8 d;
	return d;
}

// MPEG-2's default intra quantizer matrix, row by row.
static const int kIntraMatrix[64] = {
	 8, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83,
};

// Every 8 x 8 block of the plane, its grid moved by (ox, oy), coded as an MPEG-2
// intra block with quantiser_scale_code q (linear scale: quantiser_scale 2q) and
// decoded again, written back at `levels` + 1 code values (255: 8 bits, 1023: 10). An AC
// coefficient's step is W x 2q / 16 and the DC's is 8 (intra_dc_precision 8), both in
// 8-bit units whatever the depth written.
static void Mpeg2Intra(std::vector<float>& plane, int W, int H, int q, int ox, int oy, int levels = 255)
{
	const Dct8& d = Dct();
	std::vector<float> out(plane.size());
	for (int by = -oy; by < H; by += 8) {
		for (int bx = -ox; bx < W; bx += 8) {
			double blk[64], f[64], r[64];
			for (int j = 0; j < 8; j++) {
				for (int i = 0; i < 8; i++) {
					const int x = std::clamp(bx + i, 0, W - 1), y = std::clamp(by + j, 0, H - 1);
					blk[j * 8 + i] = plane[(size_t)y * W + x] * 255.0 - 128.0;
				}
			}
			d.Forward(blk, f);
			f[0] = std::round(f[0] / 8.0) * 8.0;
			for (int k = 1; k < 64; k++) {
				const double step = kIntraMatrix[k] * 2.0 * q / 16.0;
				f[k] = std::round(f[k] / step) * step;
			}
			d.Inverse(f, r);
			for (int j = 0; j < 8; j++) {
				for (int i = 0; i < 8; i++) {
					const int x = bx + i, y = by + j;
					if (x >= 0 && x < W && y >= 0 && y < H) {
						out[(size_t)y * W + x] = (float)(std::clamp(std::round((r[j * 8 + i] + 128.0) * levels / 255.0), 0.0, (double)levels) / levels);
					}
				}
			}
		}
	}
	plane.swap(out);
}
