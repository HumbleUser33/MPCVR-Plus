// Global motion of a picture, from NVIDIA Optical Flow's block vectors.
//
// DLSS Super Resolution resamples its history along the vectors it is given. On a
// picture that does not move, Optical Flow's block vectors are noise -- at 960x540,
// grid 4, only 40 % of them land within a quarter of a pixel of zero
// (tools/dlssnr_probe --tflow) -- and that noise, smoothed between blocks and new
// on every picture, makes a still background shimmer like heat haze. The median of
// the blocks that can be trusted, on the other hand, is right: zero on a still
// picture, the pan on a pan. This pass measures it; cs_dlss_global_refine.hlsl then
// brings it to the fraction of a pixel a film's weave needs, which the engine's
// sub-pixel vectors fall short of, and PASS 4 of ps_dlss_stabilize.hlsl draws each
// block to it where the block lies within the noise of it.
//
// Only trusted blocks count: forward and backward flow agree, the match is good, and
// the vector stays in the picture -- the same confidence PASS 1 builds. On a still,
// heavily grained picture the flat areas, where only the grain changes, give
// coherent wrong motion, and the median of every block drifted by 0.6 to 4 pixels
// on four film frames out of six (--tsrstill). Fewer trusted blocks than minShare of
// them, and there is no global motion to speak of: zero.
//
// One group. The median of each component is exact to the engine's 1/32 of a flow
// pixel: a histogram of whole flow pixels finds the one that holds the median, then
// a histogram of that pixel in 1/32 steps finds the median itself. A last histogram,
// of each trusted block's distance to the median, gives the median of those
// distances: how noisy the vectors of this picture are.

Texture2D<int2>     flow     : register(t0);   // R16G16_SINT, 1/32 flow pixel, current -> previous
Texture2D<int2>     backward : register(t1);   // previous -> current
Texture2D<uint>     cost     : register(t2);   // R8_UINT, higher is worse
RWTexture2D<float4> result   : register(u0);   // 1x1: global motion x, y and the median distance to it,
                                               // flow pixels; w: the number of trusted blocks

cbuffer CS_DLSS_GLOBAL_MOTION : register(b0)
{
	float  grid;          // flow pixels per vector
	float  consistency;   // forward/backward mismatch (flow pixels) where trust starts to fall
	float  costLow;       // matching cost where trust starts to fall
	float  costHigh;      // and where it is gone
	float2 flowSize;      // flow frame size, pixels
	float  useBackward;   // 0 when the engine gave no backward flow
	float  useCost;       // 0 when it gave no cost
	float  minShare;      // the share of trusted blocks below which there is no global motion
	float3 reserved;
};

#define THREADS 1024
#define RANGE   256              // whole flow pixels in [-RANGE, RANGE); what lies beyond counts in the end bins
#define COARSE  (2 * RANGE)
#define STEPS   32               // 1/32 steps in a flow pixel
#define SPREAD  256              // distances in 1/32 steps up to 8 flow pixels; the last bin takes the rest

groupshared uint coarse[2][COARSE];
groupshared uint fine[2][STEPS];
groupshared uint spread[SPREAD];
groupshared uint trustedCount;
groupshared int  medianBin[2];
groupshared uint rankInBin[2];   // the median's rank inside its bin, from 1
groupshared int  median[2];      // 1/32 flow pixel

int CoarseBin(int value)
{
	return clamp((value >> 5) + RANGE, 0, COARSE - 1);   // >> 5 floors, negative values included
}

// PASS 1's confidence for the block, at least one half, and a vector that stays in the picture.
bool Trusted(int2 b, int2 raw, int2 last)
{
	const float2 v = (float2)raw / 32.0;
	const float2 q = ((float2)b + 0.5) * grid + v;
	if (any(q < 0.0) || any(q > flowSize)) {
		return false;
	}
	float confidence = 1.0;
	if (useBackward > 0.5) {
		const int2 bb = clamp((int2)floor(q / grid), int2(0, 0), last);
		const float2 back = (float2)backward.Load(int3(bb, 0)) / 32.0;
		confidence *= 1.0 - smoothstep(consistency, 2.0 * consistency, length(v + back));
	}
	if (useCost > 0.5) {
		confidence *= 1.0 - smoothstep(costLow, costHigh, (float)cost.Load(int3(b, 0)));
	}
	return confidence >= 0.5;
}

[numthreads(THREADS, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
	uint w, h;
	flow.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const uint blocks = w * h;

	for (uint c = tid; c < COARSE; c += THREADS) {
		coarse[0][c] = 0;
		coarse[1][c] = 0;
	}
	if (tid < STEPS) {
		fine[0][tid] = 0;
		fine[1][tid] = 0;
	}
	for (uint s = tid; s < SPREAD; s += THREADS) {
		spread[s] = 0;
	}
	if (tid == 0) {
		trustedCount = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint i = tid; i < blocks; i += THREADS) {
		const int2 b = int2(i % w, i / w);
		const int2 v = flow.Load(int3(b, 0));
		if (Trusted(b, v, last)) {
			InterlockedAdd(trustedCount, 1);
			InterlockedAdd(coarse[0][CoarseBin(v.x)], 1);
			InterlockedAdd(coarse[1][CoarseBin(v.y)], 1);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// Every thread goes through every barrier: too few trusted blocks are only
	// decided at the end.
	const uint n = trustedCount;
	const uint rank = (n + 1) / 2;   // the lower median, from 1

	if (tid < 2) {
		uint below = 0;
		int bin = 0;
		while (bin < COARSE - 1 && below + coarse[tid][bin] < rank) {
			below += coarse[tid][bin];
			bin++;
		}
		medianBin[tid] = bin;
		rankInBin[tid] = rank - below;
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint j = tid; j < blocks; j += THREADS) {
		const int2 b = int2(j % w, j / w);
		const int2 v = flow.Load(int3(b, 0));
		if (Trusted(b, v, last)) {
			if (CoarseBin(v.x) == medianBin[0]) {
				InterlockedAdd(fine[0][v.x & (STEPS - 1)], 1);
			}
			if (CoarseBin(v.y) == medianBin[1]) {
				InterlockedAdd(fine[1][v.y & (STEPS - 1)], 1);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (tid < 2) {
		uint below = 0;
		int f = 0;
		while (f < STEPS - 1 && below + fine[tid][f] < rankInBin[tid]) {
			below += fine[tid][f];
			f++;
		}
		median[tid] = (medianBin[tid] - RANGE) * STEPS + f;
	}
	GroupMemoryBarrierWithGroupSync();

	const float2 g = float2(median[0], median[1]);
	for (uint k = tid; k < blocks; k += THREADS) {
		const int2 b = int2(k % w, k / w);
		const int2 v = flow.Load(int3(b, 0));
		if (Trusted(b, v, last)) {
			InterlockedAdd(spread[min((uint)round(length((float2)v - g)), SPREAD - 1)], 1);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (tid == 0) {
		uint below = 0;
		uint d = 0;
		while (d < SPREAD - 1 && below + spread[d] < rank) {
			below += spread[d];
			d++;
		}
		const bool enough = n >= max(64u, (uint)(minShare * blocks));
		result[uint2(0, 0)] = enough ? float4(g / STEPS, (float)d / STEPS, (float)n) : float4(0.0, 0.0, 0.0, (float)n);
	}
}
