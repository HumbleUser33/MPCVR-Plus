// Deblocking on a decoded plane, before anything converts or resizes the picture.
//
// A block codec cuts the picture into 8 x 8 blocks, takes each one's DCT and throws
// away the coefficients that fall under its quantizer. Where it throws away too much,
// the blocks show their edges and a contour rings. Both methods here do what the
// codec's own quantizer would have done once more, on blocks that do not sit on its
// grid: what the codec kept survives a second quantizing anywhere, and a block edge
// does not, because seen from a shifted block it is a step that no coefficient under
// the quantizer can make. They need neither the codec's quantizers nor its grid, so a
// cropped or rescaled source is no different from another.
//
//   MODE 0  SPP, simple post-processing (MPlayer, Michael Niedermayer; FFmpeg vf_spp):
//           the 8 x 8 DCT of every block of SHIFTS shifted grids, the coefficients under
//           the threshold dropped, the inverse transform, the grids averaged. The shift
//           table is vf_spp.c's (GPL-2+).
//   MODE 1  PP7 (MPlayer; FFmpeg vf_pp7): one 7 x 7 window centred on each pixel, its
//           4 x 4 even transform -- the part of the 7-point one that the centre sample
//           depends on -- thresholded, and only the centre rebuilt. The transform, its
//           weights and its thresholds are vf_pp7.c's (GPL-2+).
//
// Work is in 8-bit code levels of the plane (x 255), whatever its depth, and qp is
// the quantizer it assumes the codec used, as in FFmpeg's qp option: SPP keeps a
// coefficient above 2 qp - 1/8 in orthonormal units, which is FFmpeg's (16 qp - 1) on
// its DCT scaled by 8; PP7 has a threshold of its own per coefficient. The DC is
// always kept. Planes are mirrored at their edges as FFmpeg mirrors them.
//
// CHANNELS 1: one plane (luma, or a chroma plane of its own); 2: the two chroma
// components of an interleaved plane, done together.
// quantStep 0 writes the result as it is; otherwise it is dithered to that step with
// FFmpeg's 8 x 8 ordered pattern, for a target that keeps 8 or 10 bits.

#ifndef CHANNELS
#define CHANNELS 1
#endif
#ifndef MODE
#define MODE 1
#endif
#ifndef SHIFTS
#define SHIFTS 8
#endif

#if CHANNELS == 2
#define VEC float2
#else
#define VEC float
#endif

Texture2D<VEC>   src : register(t0);
RWTexture2D<VEC> dst : register(u0);

cbuffer CleanConstants : register(b0)
{
	uint2  planeSize;
	uint   seed;          // deband's
	uint   iterations;    // deband's
	float  qp;
	float  radius;        // deband's
	float  grain;         // deband's
	float  quantStep;
	float2 neutral;       // deband's
	uint   mode;          // 0 hard, 1 soft, 2 medium
	uint   pad;
};

#define TILE 16

static const uint kDither[64] = {
	 0, 48, 12, 60,  3, 51, 15, 63,
	32, 16, 44, 28, 35, 19, 47, 31,
	 8, 56,  4, 52, 11, 59,  7, 55,
	40, 24, 36, 20, 43, 27, 39, 23,
	 2, 50, 14, 62,  1, 49, 13, 61,
	34, 18, 46, 30, 33, 17, 45, 29,
	10, 58,  6, 54,  9, 57,  5, 53,
	42, 26, 38, 22, 41, 25, 37, 21,
};

// FFmpeg's mirror: -1 is 0, n is n - 1.
int Mirror(int i, int n)
{
	i = (i < 0) ? -1 - i : i;
	i = (i >= n) ? 2 * n - 1 - i : i;
	return clamp(i, 0, n - 1);
}

VEC Store(VEC v, uint2 p)
{
	if (quantStep > 0) {
		const float d = (kDither[(p.y & 7) * 8 + (p.x & 7)] + 0.5) / 64.0;
		v = floor(v / quantStep + d) * quantStep;
	}
	return saturate(v);
}

// A coefficient after the threshold.
VEC Requantize(VEC c, float t)
{
	const VEC a = abs(c);
	const VEC kept = (a > t) ? c : 0;
	if (mode == 1) {
		return (a > t) ? c - sign(c) * t : 0;
	}
	if (mode == 2) {
		return (a > 2 * t) ? c : (a > t) ? 2 * (c - sign(c) * t) : 0;
	}
	return kept;
}

#if MODE == 1

// ------------------------------------------------------------------------- PP7 --

#define SPAN (TILE + 6)
groupshared VEC window[SPAN * SPAN];

// The 7-point even transform of vf_pp7.c's dctA/dctB: four outputs from seven samples.
void Even7(VEC w0, VEC w1, VEC w2, VEC w3, VEC w4, VEC w5, VEC w6, out VEC o[4])
{
	const VEC s0 = w0 + w6, s1 = w1 + w5, s2 = w2 + w4;
	const VEC s = w3 + w3;
	const VEC t3 = s - s0, t0 = s + s0;
	const VEC ss = s2 + s1, t2 = s2 - s1;
	o[0] = t0 + ss;
	o[2] = t0 - ss;
	o[1] = 2 * t3 + t2;
	o[3] = t3 - 2 * t2;
}

// N0, N1, N0, N2 of vf_pp7.c, and the thresholds' SN0 or SN2.
static const float kN[4]  = { 4.0, 5.0, 4.0, 10.0 };
static const float kSN[4] = { 2.0, 3.16227766017, 2.0, 3.16227766017 };

[numthreads(TILE, TILE, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	const int2 origin = int2(gid.xy) * TILE - 3;
	for (uint i = gi; i < SPAN * SPAN; i += TILE * TILE) {
		const int2 q = int2(i % SPAN, i / SPAN) + origin;
		window[i] = src.Load(int3(Mirror(q.x, (int)planeSize.x), Mirror(q.y, (int)planeSize.y), 0)) * 255.0;
	}
	GroupMemoryBarrierWithGroupSync();

	const uint2 p = gid.xy * TILE + tid.xy;
	if (any(p >= planeSize)) {
		return;
	}

	// Down each of the seven columns of the window, then across.
	VEC v[7][4];
	[unroll] for (int c = 0; c < 7; c++) {
		const uint at = tid.y * SPAN + tid.x + c;
		Even7(window[at], window[at + SPAN], window[at + 2 * SPAN], window[at + 3 * SPAN],
		      window[at + 4 * SPAN], window[at + 5 * SPAN], window[at + 6 * SPAN], v[c]);
	}
	const float q = max(1.0, qp);
	VEC a = 0;
	[unroll] for (int k = 0; k < 4; k++) {        // the vertical coefficient
		VEC h[4];
		Even7(v[0][k], v[1][k], v[2][k], v[3][k], v[4][k], v[5][k], v[6][k], h);
		[unroll] for (int j = 0; j < 4; j++) {    // the horizontal one
			const float weight = 1.0 / (kN[j] * kN[k]);
			if (j == 0 && k == 0) {
				a += h[j] * weight;
			} else {
				a += Requantize(h[j], kSN[j] * kSN[k] * q * 4 - 1) * weight;
			}
		}
	}
	dst[p] = Store(a * (0.25 / 255.0), p);
}

#else

// ------------------------------------------------------------------------- SPP --

// vf_spp.c's offsets: the SHIFTS grids are entries SHIFTS .. 2 SHIFTS - 1.
static const uint2 kOffset[128] = {
	uint2(0,0), uint2(0,0),
	uint2(0,0), uint2(4,4),
	uint2(0,0), uint2(2,2), uint2(6,4), uint2(4,6),
	uint2(0,0), uint2(5,1), uint2(2,2), uint2(7,3), uint2(4,4), uint2(1,5), uint2(6,6), uint2(3,7),
	uint2(0,0), uint2(4,0), uint2(1,1), uint2(5,1), uint2(3,2), uint2(7,2), uint2(2,3), uint2(6,3),
	uint2(0,4), uint2(4,4), uint2(1,5), uint2(5,5), uint2(3,6), uint2(7,6), uint2(2,7), uint2(6,7),
	uint2(0,0), uint2(0,2), uint2(0,4), uint2(0,6), uint2(1,1), uint2(1,3), uint2(1,5), uint2(1,7),
	uint2(2,0), uint2(2,2), uint2(2,4), uint2(2,6), uint2(3,1), uint2(3,3), uint2(3,5), uint2(3,7),
	uint2(4,0), uint2(4,2), uint2(4,4), uint2(4,6), uint2(5,1), uint2(5,3), uint2(5,5), uint2(5,7),
	uint2(6,0), uint2(6,2), uint2(6,4), uint2(6,6), uint2(7,1), uint2(7,3), uint2(7,5), uint2(7,7),
	uint2(0,0), uint2(4,4), uint2(0,4), uint2(4,0), uint2(2,2), uint2(6,6), uint2(2,6), uint2(6,2),
	uint2(0,2), uint2(4,6), uint2(0,6), uint2(4,2), uint2(2,0), uint2(6,4), uint2(2,4), uint2(6,0),
	uint2(1,1), uint2(5,5), uint2(1,5), uint2(5,1), uint2(3,3), uint2(7,7), uint2(3,7), uint2(7,3),
	uint2(1,3), uint2(5,7), uint2(1,7), uint2(5,3), uint2(3,1), uint2(7,5), uint2(3,5), uint2(7,1),
	uint2(0,1), uint2(4,5), uint2(0,5), uint2(4,1), uint2(2,3), uint2(6,7), uint2(2,7), uint2(6,3),
	uint2(0,3), uint2(4,7), uint2(0,7), uint2(4,3), uint2(2,1), uint2(6,5), uint2(2,5), uint2(6,1),
	uint2(1,0), uint2(5,4), uint2(1,4), uint2(5,0), uint2(3,2), uint2(7,6), uint2(3,6), uint2(7,2),
	uint2(1,2), uint2(5,6), uint2(1,6), uint2(5,2), uint2(3,0), uint2(7,4), uint2(3,4), uint2(7,0),
};

// The orthonormal 8-point DCT-II: kDct[u * 8 + n].
static const float kDct[64] = {
	0.35355339, 0.35355339, 0.35355339, 0.35355339, 0.35355339, 0.35355339, 0.35355339, 0.35355339,
	0.49039264, 0.41573481, 0.27778512, 0.09754516,-0.09754516,-0.27778512,-0.41573481,-0.49039264,
	0.46193977, 0.19134172,-0.19134172,-0.46193977,-0.46193977,-0.19134172, 0.19134172, 0.46193977,
	0.41573481,-0.09754516,-0.49039264,-0.27778512, 0.27778512, 0.49039264, 0.09754516,-0.41573481,
	0.35355339,-0.35355339,-0.35355339, 0.35355339, 0.35355339,-0.35355339,-0.35355339, 0.35355339,
	0.27778512,-0.49039264, 0.09754516, 0.41573481,-0.41573481,-0.09754516, 0.49039264,-0.27778512,
	0.19134172,-0.46193977, 0.46193977,-0.19134172,-0.19134172, 0.46193977,-0.46193977, 0.19134172,
	0.09754516,-0.27778512, 0.41573481,-0.49039264, 0.49039264,-0.41573481, 0.27778512,-0.09754516,
};

// The tile and 8 pixels round it: every block of every grid that covers the tile. Rows
// are padded by one, so that threads reading a column each fall in a bank of their own.
#define SPAN (TILE + 16)
#define PITCH (SPAN + 1)
groupshared VEC image[SPAN * PITCH];
groupshared float basis[64];      // kDct, for the one step where each thread wants a row of its own
groupshared VEC rows[9 * 72];     // each block's rows transformed, [block][row y][u], 9 a row
groupshared VEC cols[9 * 72];     // then down the columns, thresholded and back: [block][y][u]

[numthreads(TILE, TILE, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	const int2 origin = int2(gid.xy) * TILE - 8;
	for (uint i = gi; i < SPAN * SPAN; i += TILE * TILE) {
		const int2 q = int2(i % SPAN, i / SPAN);
		image[q.y * PITCH + q.x] = src.Load(int3(Mirror(q.x + origin.x, (int)planeSize.x), Mirror(q.y + origin.y, (int)planeSize.y), 0)) * 255.0;
	}
	if (gi < 64) {
		basis[gi] = kDct[gi];
	}
	GroupMemoryBarrierWithGroupSync();

	const float t = 2 * max(qp, 0.0625) - 0.125;
	VEC acc = 0;

	[loop] for (uint s = 0; s < SHIFTS; s++) {
		const uint2 o = kOffset[SHIFTS + s];
		// A grid's blocks start where x = o (mod 8). Seen from the tile, two of them
		// cover it when that lands on its edge and three when it does not.
		const uint2 first = uint2(o.x ? o.x : 8, o.y ? o.y : 8);
		const uint2 blocks = uint2(o.x ? 3 : 2, o.y ? 3 : 2);
		const uint pairs = blocks.x * blocks.y * 8;

		// 1. Along one row of one block a thread: eight samples in, eight coefficients out.
		if (gi < pairs) {
			const uint b = gi / 8, r = gi % 8;
			const uint2 at = first + 8 * uint2(b % blocks.x, b / blocks.x) + uint2(0, r);
			VEC px[8];
			[unroll] for (uint n = 0; n < 8; n++) {
				px[n] = image[at.y * PITCH + at.x + n];
			}
			[unroll] for (uint u = 0; u < 8; u++) {
				VEC sum = 0;
				[unroll] for (uint n2 = 0; n2 < 8; n2++) {
					sum += kDct[u * 8 + n2] * px[n2];
				}
				rows[b * 72 + r * 9 + u] = sum;
			}
		}
		GroupMemoryBarrierWithGroupSync();

		// 2. Down one column of one block a thread: the coefficients, the threshold, and
		// straight back down the column.
		if (gi < pairs) {
			const uint b = gi / 8, u = gi % 8;
			VEC col[8], c[8];
			[unroll] for (uint r = 0; r < 8; r++) {
				col[r] = rows[b * 72 + r * 9 + u];
			}
			[unroll] for (uint v = 0; v < 8; v++) {
				VEC sum = 0;
				[unroll] for (uint r2 = 0; r2 < 8; r2++) {
					sum += kDct[v * 8 + r2] * col[r2];
				}
				c[v] = (v == 0 && u == 0) ? sum : Requantize(sum, t);
			}
			[unroll] for (uint y = 0; y < 8; y++) {
				VEC sum = 0;
				[unroll] for (uint v2 = 0; v2 < 8; v2++) {
					sum += kDct[v2 * 8 + y] * c[v2];
				}
				cols[b * 72 + y * 9 + u] = sum;
			}
		}
		GroupMemoryBarrierWithGroupSync();

		// 3. This pixel: back along its row of the block of this grid that holds it. The
		// next grid's first step writes rows, not cols, and its barrier comes before its
		// second step writes cols: no third barrier is needed.
		const uint2 at = tid.xy + 8 - first;
		const uint b = (at.y / 8) * blocks.x + at.x / 8;
		const uint2 n = at % 8;
		VEC value = 0;
		[unroll] for (uint u = 0; u < 8; u++) {
			value += basis[u * 8 + n.x] * cols[b * 72 + n.y * 9 + u];
		}
		acc += value;
	}

	const uint2 p = gid.xy * TILE + tid.xy;
	if (all(p < planeSize)) {
		dst[p] = Store(acc * (1.0 / (SHIFTS * 255.0)), p);
	}
}

#endif
