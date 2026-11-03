// A channel's logo taken out of a decoded plane, in its box only, the rest of the plane
// already copied (cs_logo_pack.hlsl MODE 2). The analysis found, for every pixel of the
// box, how opaque the logo is there (alpha) and what it lays over the picture (laid,
// alpha times its colour), as a broadcaster lays it:
//
//   seen = (1 - alpha) * picture + laid
//
// so where alpha is low enough the picture is worked back out exactly, and where it nears
// 1 there is nothing left of it and it is filled in from round the logo; in between the
// two are mixed by their errors, the inversion's grown by 1 / (1 - a):
//
//   w = (1 - a)^2 / ((1 - a)^2 + s^2 doubt),   out = w * inverted + (1 - w) * filled
//
// doubt, measured for every pixel when the logo was fitted, is the variance of the
// inversion's error over the fill's: small inside a translucent logo, large on the edge of
// an opaque one, where the codec smears logo and picture together differently each time.
//
// Alpha below zero is the codec's ringing round the logo, which darkens the picture the
// more the darker it is against the logo: inverted, and never filled.
//
// The fill is a push-pull (Gortler et al.): the pixels round the logo averaged down a
// pyramid with [1 3 3 1] weights, the holes filled from it on the way back up, then
// `jacobi` steps of Laplace's smoothing. It takes nothing from the logo, the margin round
// it or the ringing, marked unknown in the maps when they are made. A push-pull keeps the
// picture's slow changes and none of its grain or texture, so `detail` times the fine
// detail of the picture next to the logo is added to it: at each pixel, that of the known
// pixel mirrored across the nearest edge (the mirror map), over the mean of the known
// pixels in the 5 x 5 round it.
//
// Every pixel worked is inverted with what the fit found there, the margin's too: the
// faint edge the codec smeared round the logo is the logo's, however little of it there is.
//
// A logo is often laid across the edge of a black bar. Over the bar, what was under it is
// known: the bar. The rows (and columns) of the box that are a bar's on this picture --
// their pixels round the logo dark and flat, from the box's edge inwards -- are told in
// the luma pass and kept in `state` for the chroma's. There the fill is the row's own level,
// the mean of its pixels round the logo, and the inversion is trusted as anywhere: over a
// real bar it gives the bar back. Elsewhere the fill reads nothing from them, so that the
// bar and the picture are not smeared into one another across the edge.
//
// The logo is not always on screen (advertising, a fade). The luma pass measures how
// strongly it is: the least-squares strength that explains the logo's pixels against the
// picture filled in round them. Measured where the logo stands out from the picture, it
// is kept between pictures in `state`, which the chroma pass reads; a redraw (update 0)
// takes it as it is. It is LogoScan's test of a picture without the logo, made on every
// picture, on the GPU, with nothing read back.
//
// PASS 0, one group for the whole box: the pyramid built level after level, each waiting
// for the one before, and the strength. PASS 1, a thread a pixel over the box: the detail
// mirrored into the fill, and each pixel worked written -- the work of most of the time,
// spread over the whole GPU. Source/Logo/LogoAnalysis.cpp is the same work in double
// precision, and the bench holds the two to the same result (--tlogo).
//
// CHANNELS 1: luma, maps (alpha, laid, fill); 2: the two chroma components of an
// interleaved plane, maps (alpha, Cb laid, Cr laid, fill). Alpha and laid run from
// kAlphaLeast to 1 in the maps' 16 bits (Source/Logo/LogoAnalysis.h); fill is 0 for a pixel
// left as it is, and from a half up for one filled, carrying its doubt
// (Logo::ShaderMaps).

#ifndef CHANNELS
#define CHANNELS 1
#endif
#ifndef PASS
#define PASS 0
#endif

#define THREADS 1024
#define LEVELS 16    // the pyramid's levels at most, the box's own included
#define LEAST -0.5   // Logo::kAlphaLeast
#define DECODE(v) (LEAST + (v) * (1 - LEAST))
#define BAR_OFFSET (16.0 / 255.0)   // sums about the bar's usual level, for a float's precision

#if CHANNELS == 2
#define VEC float2
#define LAID(m) DECODE((m).yz)
#define UNKNOWN(m) (m).w
#else
#define VEC float
#define LAID(m) DECODE((m).y)
#define UNKNOWN(m) (m).z
#endif
#define ALPHA(m) DECODE((m).x)

float DoubtOf(float fill)
{
	const float e = min(2 * fill - 1, 0.999999);
	return e / (1 - e);
}

Texture2D<VEC>    src  : register(t0);
Texture2D<float4> maps : register(t1);
Texture2D<uint2>  mirror : register(t2);
RWTexture2D<VEC>  dst  : register(u0);
RWStructuredBuffer<float4> pyramid : register(u1);   // value(s) in xy, weight in z
// The logo's strength, as float bits; then the bars of the picture, in luma pixels from
// the box's top, bottom, left and right (Logo::Bars, Logo::kStateBytes).
RWByteAddressBuffer state : register(u2);

cbuffer LogoConstants : register(b0)
{
	uint2 boxOrigin;    // the box in the plane
	uint2 boxSize;
	uint  levels;       // of the pyramid, below the box
	uint  jacobi;       // smoothing steps after the push-pull
	uint  update;       // 1: a new picture; 0: a redraw
	float detail;       // of the picture next to the logo, added to the fill
	float alphaMin;
	float snapLow;      // a strength measured under this is no logo
	float snapHigh;     // over this, the whole logo
	float trust;        // what the logo must stand out by for its strength to be measured
	uint  barsAt;       // the pyramid's entry of the bar's level of each row, then of each column
	uint2 barDiv;       // the plane's pixel in luma pixels
	float barDark;      // a row of a bar: its pixels round the logo darker than this on average...
	float barFlat;      // ...and spread less (std)
	// Each level's width, height and first entry in the pyramid: the box itself at 0 (two
	// layers, the fill and its smoothing's other half), then every level below it.
	uint4 level[LEVELS];
};

groupshared float gsNum[THREADS];
groupshared float gsDen[THREADS];
groupshared float gsCount[THREADS];
groupshared int gsEdge[4];   // the first and last row that is not a bar's, the first and last column

#if CHANNELS == 2
float4 Pack(VEC v, float w) { return float4(v, w, 0); }
VEC Value(float4 e) { return e.xy; }
#else
float4 Pack(VEC v, float w) { return float4(v, 0, w, 0); }
VEC Value(float4 e) { return e.x; }
#endif

static const float k4[4] = { 0.25, 0.75, 0.75, 0.25 };

// The bars the luma pass found on this picture, in this plane's pixels: top, bottom, left,
// right. A chroma row is a bar's when both luma rows it covers are.
uint4 Bars()
{
	const uint4 b = state.Load4(4);
	return uint4(b.x / barDiv.y, b.y / barDiv.y, b.z / barDiv.x, b.w / barDiv.x);
}

bool InBar(uint4 b, uint2 p)
{
	return p.y < b.x || p.y + b.y >= boxSize.y || p.x < b.z || p.x + b.w >= boxSize.x;
}

// The bar's level at one of its pixels, its row's or its column's; weight 0 where that row
// had no pixel round the logo.
float4 BarLevel(uint4 b, uint2 p)
{
	const bool inRow = p.y < b.x || p.y + b.y >= boxSize.y;
	return pyramid[barsAt + (inRow ? p.y : boxSize.y + p.x)];
}

#if PASS == 0

[numthreads(THREADS, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
	const uint n0 = boxSize.x * boxSize.y;
	if (tid == 0) {
		gsEdge[0] = (int)boxSize.y;
		gsEdge[1] = -1;
		gsEdge[2] = (int)boxSize.x;
		gsEdge[3] = -1;
	}

	// The box: the picture's pixels known, the logo's and its halo's not.
	for (uint i0 = tid; i0 < n0; i0 += THREADS) {
		const uint2 p = uint2(i0 % boxSize.x, i0 / boxSize.x);
		const float known = UNKNOWN(maps[p]) < 0.5 ? 1.0 : 0.0;
		pyramid[i0] = Pack(src[boxOrigin + p] * known, known);
	}
	AllMemoryBarrierWithGroupSync();

	// Each row's and then each column's pixels round the logo: their mean, the bar's level
	// if the line is a bar's; and, for the luma, whether it is: dark and flat. A line is
	// shared by `lanes` threads, so that a box of few rows still keeps the group busy, and
	// their sums brought together in the group's memory.
	[loop]
	for (uint side = 0; side < 2; side++) {
		const bool row = side == 0;
		const uint lines = row ? boxSize.y : boxSize.x;
		const uint len = row ? boxSize.x : boxSize.y;
		uint lanes = 1;
		[loop]
		while (lanes < 32 && lanes * 2 * lines <= THREADS) {
			lanes *= 2;
		}
		const uint which = tid / lanes, lane = tid % lanes;
		VEC s = 0;
		float s2 = 0, n = 0;
		if (which < lines) {
			[loop]
			for (uint t = lane; t < len; t += lanes) {
				const float4 e = pyramid[row ? which * boxSize.x + t : t * boxSize.x + which];
				if (e.z > 0) {
					const VEC d = Value(e) - BAR_OFFSET;
					s += d;
#if CHANNELS == 1
					s2 += d * d;
#endif
					n += 1;
				}
			}
		}
#if CHANNELS == 2
		gsNum[tid] = s.x;
		gsDen[tid] = s.y;
#else
		gsNum[tid] = s;
		gsDen[tid] = s2;
#endif
		gsCount[tid] = n;
		GroupMemoryBarrierWithGroupSync();
		if (which < lines && lane == 0) {
			VEC ls = 0;
			float ls2 = 0, ln = 0;
			for (uint k = tid; k < tid + lanes; k++) {
#if CHANNELS == 2
				ls += float2(gsNum[k], gsDen[k]);
#else
				ls += gsNum[k];
				ls2 += gsDen[k];
#endif
				ln += gsCount[k];
			}
			const VEC mean = ln > 0 ? ls / ln : 0;
			pyramid[barsAt + (row ? 0 : boxSize.y) + which] = Pack(mean + BAR_OFFSET, ln > 0 ? 1.0 : 0.0);
#if CHANNELS == 1
			const bool flat = ln >= 4 && mean + BAR_OFFSET < barDark && ls2 / ln - mean * mean < barFlat * barFlat;
			if (!flat) {
				InterlockedMin(gsEdge[row ? 0 : 2], (int)which);
				InterlockedMax(gsEdge[row ? 1 : 3], (int)which);
			}
#endif
		}
#if CHANNELS == 1
		// A box of more lines than the group has threads, which the locator never makes:
		// those past them are not taken for a bar's.
		for (uint beyond = THREADS + tid; beyond < lines; beyond += THREADS) {
			InterlockedMin(gsEdge[row ? 0 : 2], (int)beyond);
			InterlockedMax(gsEdge[row ? 1 : 3], (int)beyond);
		}
#endif
		GroupMemoryBarrierWithGroupSync();
	}
	AllMemoryBarrierWithGroupSync();
#if CHANNELS == 1
	// A bar runs from the box's edge to the first row (column) that is not one.
	const uint4 bars = uint4((uint)gsEdge[0], (uint)((int)boxSize.y - 1 - gsEdge[1]),
		(uint)gsEdge[2], (uint)((int)boxSize.x - 1 - gsEdge[3]));
	if (tid == 0) {
		state.Store4(4, bars);
	}
#else
	const uint4 bars = Bars();
#endif

	// The fill reads nothing from a bar.
	for (uint i6 = tid; i6 < n0; i6 += THREADS) {
		if (InBar(bars, uint2(i6 % boxSize.x, i6 / boxSize.x))) {
			pyramid[i6] = Pack(0, 0);
		}
	}
	DeviceMemoryBarrierWithGroupSync();

	// Pull: each level the [1 3 3 1] weighted mean of the known pixels of the one below,
	// as sure as the weight it was made of, up to 1.
	for (uint lp = 1; lp <= levels; lp++) {
		const uint2 fs = level[lp - 1].xy, cs = level[lp].xy;
		const uint fo = level[lp - 1].z, co = level[lp].z;
		for (uint i1 = tid; i1 < cs.x * cs.y; i1 += THREADS) {
			const int2 c = int2(i1 % cs.x, i1 / cs.x);
			VEC sv = 0;
			float sw = 0;
			[unroll]
			for (int dy = 0; dy < 4; dy++) {
				[unroll]
				for (int dx = 0; dx < 4; dx++) {
					const int2 f = int2(2 * c.x - 1 + dx, 2 * c.y - 1 + dy);
					if (f.x < 0 || f.y < 0 || f.x >= (int)fs.x || f.y >= (int)fs.y) {
						continue;
					}
					const float4 e = pyramid[fo + f.y * fs.x + f.x];
					const float k = k4[dx] * k4[dy] * e.z;
					sv += k * Value(e);
					sw += k;
				}
			}
			pyramid[co + i1] = Pack(sw > 0 ? sv / sw : 0, min(1.0, sw));
		}
		DeviceMemoryBarrierWithGroupSync();
	}

	// Push: the holes of each level filled from the level above, bilinearly.
	for (int lu = (int)levels - 1; lu >= 0; lu--) {
		const uint2 fs = level[lu].xy, cs = level[lu + 1].xy;
		const uint fo = level[lu].z, co = level[lu + 1].z;
		for (uint i2 = tid; i2 < fs.x * fs.y; i2 += THREADS) {
			const uint2 f = uint2(i2 % fs.x, i2 / fs.x);
			const float cxf = clamp((f.x + 0.5) / 2 - 0.5, 0.0, cs.x - 1.0);
			const float cyf = clamp((f.y + 0.5) / 2 - 0.5, 0.0, cs.y - 1.0);
			const uint cx0 = (uint)cxf, cy0 = (uint)cyf;
			const uint cx1 = min(cx0 + 1, cs.x - 1), cy1 = min(cy0 + 1, cs.y - 1);
			const float ax = cxf - cx0, ay = cyf - cy0;
			const VEC up = (1 - ay) * ((1 - ax) * Value(pyramid[co + cy0 * cs.x + cx0]) + ax * Value(pyramid[co + cy0 * cs.x + cx1]))
				+ ay * ((1 - ax) * Value(pyramid[co + cy1 * cs.x + cx0]) + ax * Value(pyramid[co + cy1 * cs.x + cx1]));
			const float4 e = pyramid[fo + i2];
			const float w = min(1.0, e.z);
			pyramid[fo + i2] = Pack(w * Value(e) + (1 - w) * up, e.z);
		}
		DeviceMemoryBarrierWithGroupSync();
	}

	// Laplace's smoothing of what was filled, in the box's two layers by turns.
	uint cur = 0;
	for (uint it = 0; it < jacobi; it++) {
		const uint nxt = cur ^ n0;
		for (uint i3 = tid; i3 < n0; i3 += THREADS) {
			const uint2 p = uint2(i3 % boxSize.x, i3 / boxSize.x);
			const float4 e = pyramid[cur + i3];
			if (e.z >= 1) {
				pyramid[nxt + i3] = e;
				continue;
			}
			VEC s = 0;
			float k = 0;
			if (p.x > 0) { s += Value(pyramid[cur + i3 - 1]); k++; }
			if (p.x + 1 < boxSize.x) { s += Value(pyramid[cur + i3 + 1]); k++; }
			if (p.y > 0) { s += Value(pyramid[cur + i3 - boxSize.x]); k++; }
			if (p.y + 1 < boxSize.y) { s += Value(pyramid[cur + i3 + boxSize.x]); k++; }
			pyramid[nxt + i3] = Pack(s / k, e.z);
		}
		DeviceMemoryBarrierWithGroupSync();
		cur = nxt;
	}

#if CHANNELS == 1
	// How strongly the logo is on screen in this picture.
	float num = 0, den = 0, count = 0;
	for (uint i4 = tid; i4 < n0; i4 += THREADS) {
		const uint2 p = uint2(i4 % boxSize.x, i4 / boxSize.x);
		const float4 m = maps[p];
		const float alpha = ALPHA(m);
		if (alpha < alphaMin) {
			continue;
		}
		// What the logo at full strength changes the pixel by.
		float fill = Value(pyramid[cur + i4]);
		if (InBar(bars, p)) {
			const float4 l = BarLevel(bars, p);
			fill = l.z > 0 ? Value(l) : fill;
		}
		const float c = LAID(m) - alpha * fill;
		num += c * (src[boxOrigin + p] - fill);
		den += c * c;
		count += 1;
	}
	gsNum[tid] = num;
	gsDen[tid] = den;
	gsCount[tid] = count;
	GroupMemoryBarrierWithGroupSync();
	[unroll]
	for (uint half = THREADS / 2; half > 0; half /= 2) {
		if (tid < half) {
			gsNum[tid] += gsNum[tid + half];
			gsDen[tid] += gsDen[tid + half];
			gsCount[tid] += gsCount[tid + half];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid == 0) {
		float s = asfloat(state.Load(0));
		if (update) {
			const float n = gsCount[0];
			const float sure = n > 0 ? sqrt(gsDen[0] / n) : 0;
			if (sure >= trust && gsDen[0] > 0) {
				const float measured = saturate(gsNum[0] / gsDen[0]);
				s = measured < snapLow ? 0 : measured > snapHigh ? 1 : measured;
			}
			state.Store(0, asuint(s));
		}
	}
#endif
}

#else

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= boxSize.x || id.y >= boxSize.y) {
		return;
	}
	const uint n0 = boxSize.x * boxSize.y;
	// The fill PASS 0 left: in the second layer after an odd number of smoothing steps.
	const uint cur = (jacobi & 1) ? n0 : 0;
	const float strength = asfloat(state.Load(0));
	const uint4 bars = Bars();

	// The logo's pixels: inverted where that can be trusted, filled in where not.
	{
		const uint2 p = id.xy;
		const uint i5 = p.y * boxSize.x + p.x;
		const float4 m = maps[p];
		if (UNKNOWN(m) < 0.5) {
			return;
		}
		VEC fill = Value(pyramid[cur + i5]);
		const uint2 q = mirror[p];
		if (InBar(bars, p)) {
			// Over a bar the fill is the bar's own level, and the inversion is trusted as
			// anywhere: over a real bar it gives the bar back, over a picture's dark flat
			// rows taken for one, the picture.
			const float4 l = BarLevel(bars, p);
			fill = l.z > 0 ? Value(l) : fill;
		} else if (detail > 0 && q.x != 0xFFFF && !InBar(bars, q)) {
			VEC sum = 0;
			float k = 0;
			[loop]
			for (int dy = -2; dy <= 2; dy++) {
				[loop]
				for (int dx = -2; dx <= 2; dx++) {
					const int2 n = int2(q) + int2(dx, dy);
					if (n.x < 0 || n.y < 0 || n.x >= (int)boxSize.x || n.y >= (int)boxSize.y || UNKNOWN(maps[n]) >= 0.5
							|| InBar(bars, uint2(n))) {
						continue;
					}
					sum += src[boxOrigin + n];
					k += 1;
				}
			}
			const VEC centre = src[boxOrigin + q];
			fill += detail * (centre - (k > 0 ? sum / k : centre));
		}
		const float alpha = ALPHA(m);
		const float a = clamp(strength * alpha, LEAST, 1.0);
		const VEC laid = strength * LAID(m);
		const VEC seen = src[boxOrigin + p];
		const VEC inv = a < 0.999 ? (seen - laid) / (1 - a) : fill;
		const float d = (1 - a) * (1 - a);
		const float w = d / (d + strength * strength * DoubtOf(UNKNOWN(m)) + 1e-12);
		dst[boxOrigin + p] = saturate(w * inv + (1 - w) * fill);
	}
}

#endif
