// Debanding on a decoded plane, before anything converts or resizes the picture.
//
// A codec that is short of bits flattens a slow gradient -- a sky, a wall, a fade --
// into steps of one or a few code levels, and the eye follows their edges. This is
// libplacebo's debanding (src/shaders/sampling.c, pl_shader_deband, v7.351.0), the
// one mpv and FFmpeg's libplacebo filter run, written again for Direct3D:
//
//   for each iteration i, four samples at a random distance up to i x radius, a
//   quarter turn apart round the pixel, averaged; the pixel takes the average if it
//   is within threshold / i of it. A step one level high sits under the threshold and
//   is smoothed away; an edge in the picture does not, and is kept.
//   Then grain, at most `grain`, fading out towards `neutral` (black for luma, the
//   middle for chroma) so that black stays black.
//
// Its random numbers are libplacebo's own pcg3d, seeded by the pixel and the picture,
// so the same picture always gets the same grain: a redraw does not shimmer. Its
// samples are taken as libplacebo takes them, the nearest texel, clamped at the edge.
//
// threshold and grain are in thousandths of the plane's range, as libplacebo's, so a
// level means the same on 8 and on 10 bits; radius is in the plane's own pixels.
// CHANNELS 1: one plane; 2: the two chroma components of an interleaved plane.
// quantStep 0 writes the result as it is; otherwise it is dithered to that step, for
// a target that keeps 8 or 10 bits.
//
// libplacebo is free software under the GNU Lesser General Public License, version
// 2.1 or later; this translation of its function is offered under the GPL, version 3
// or later, as the rest of the renderer, which the LGPL allows.

#ifndef CHANNELS
#define CHANNELS 1
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
	uint   seed;
	uint   iterations;
	float  threshold;     // 1/1000 of the range
	float  radius;        // pixels
	float  grain;         // 1/1000 of the range
	float  quantStep;
	float2 neutral;
	uint   mode;          // deblock's
	uint   pad;
};

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

// pcg3d (http://jcgt.org/published/0009/03/02/), as libplacebo's sh_prng.
float3 Rand(inout uint3 s)
{
	s = 1664525u * s + 1013904223u;
	s.x += s.y * s.z;
	s.y += s.z * s.x;
	s.z += s.x * s.y;
	s ^= s >> 16u;
	s.x += s.y * s.z;
	s.y += s.z * s.x;
	s.z += s.x * s.y;
	return float3(s) * (1.0 / 4294967295.0);
}

VEC Get(int2 p, float2 d)
{
	const int2 q = clamp(int2(floor(float2(p) + 0.5 + d)), 0, int2(planeSize) - 1);
	return src.Load(int3(q, 0));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const uint2 p = id.xy;
	if (any(p >= planeSize)) {
		return;
	}

	uint3 state = uint3(p, seed);
	VEC res = src.Load(int3(p, 0));
	const float t = threshold / 1000.0;

	[loop] for (uint i = 1; i <= iterations; i++) {
		float2 d = Rand(state).xy * float2(i * radius, 6.28318530717958647692);
		d = d.x * float2(cos(d.y), sin(d.y));
		VEC avg = Get(p, float2(+d.x, +d.y));
		avg += Get(p, float2(-d.x, +d.y));
		avg += Get(p, float2(-d.x, -d.y));
		avg += Get(p, float2(+d.x, -d.y));
		avg *= 0.25;
		const VEC diff = abs(res - avg);
		res = (diff > t / i) ? res : avg;
	}

	if (grain > 0) {
#if CHANNELS == 2
		const VEC strength = min(abs(res - neutral), grain / 1000.0);
		res += strength * (Rand(state).xy - 0.5);
#else
		const VEC strength = min(abs(res - neutral.x), grain / 1000.0);
		res += strength * (Rand(state).x - 0.5);
#endif
	}

	if (quantStep > 0) {
		const float dd = (kDither[(p.y & 7) * 8 + (p.x & 7)] + 0.5) / 64.0;
		res = floor(res / quantStep + dd) * quantStep;
	}
	dst[p] = saturate(res);
}
