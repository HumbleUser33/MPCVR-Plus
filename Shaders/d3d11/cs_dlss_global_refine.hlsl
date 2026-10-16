// The global motion of cs_dlss_global_motion.hlsl, refined on the flow frames themselves.
//
// The median of Optical Flow's trusted blocks is right to the engine's 1/32 of a flow
// pixel on a picture that holds still, but on a film that weaves -- the whole picture
// moving by 0.2 to 1 source pixel from one frame to the next, as Cyrano does -- it came
// out short of the film's own motion, and fell under the 0.4-pixel dead zone PASS 4 then
// had on 77 % of the frames (tools/dlssnr_probe --tsrclip). DLSS Super Resolution
// resampled its history to the wrong place, and a wall that only weaves shimmered more
// than the source itself.
//
// So the two flow frames are registered directly: Gauss-Newton steps on the brightness
// difference, from the median. What moves on its own must take no part, and Optical Flow's
// own vectors do not tell it well enough: actors walking a pixel or two a frame lie within
// a flow pixel of the weave, and taking them in left the estimate 0.34 source pixel from
// the best registration of the source frames on Cyrano. Their residual tells it: 16 x 16
// groups whose mean squared difference, at the current estimate, passes `robust` times the
// median group's are left out, and the same estimate comes within 0.02 pixel
// (tools/dlssnr_probe/clip_motion.py --compare). Each step is two passes:
//   PASS 1  per 16 x 16 group: the sums of g g^T and g e, g the previous frame's
//           gradient where the content came from, e the difference to it, and of e^2
//   PASS 2  one group: the median of the groups' mean e^2, among those with texture
//           (a letterbox bar has none and would drag it down), those groups under
//           `robust` times it added up, the 2 x 2 system solved, and the step taken into
//           the 1 x 1 global motion, at most `maxStep`; no step where too few pixels or
//           no texture to tell a motion by
//
// Convention as the engine's: current -> previous, flow pixels; the content at x in the
// current frame was at x + g in the previous one.

#ifndef PASS
#define PASS 1
#endif

cbuffer CS_DLSS_GLOBAL_REFINE : register(b0)
{
	float  grid;       // flow pixels per vector block
	float  near;       // how far, flow pixels, a block's own vector may lie from the estimate and still take part
	float  minCount;   // fewer pixels than this, and no step
	float  maxStep;    // the largest step, flow pixels
	float  robust;     // groups whose mean e^2 passes this times the median group's are left out; 0: none are
	float3 reserved;
};

#if PASS == 1

Texture2D<float>    frameNow     : register(t0);   // the flow frame the engine just ran on
Texture2D<float>    frameLast    : register(t1);   // and the one before it
Texture2D<float4>   globalMotion : register(t2);   // 1x1: the estimate, flow pixels
Texture2D<int2>     flow         : register(t3);   // R16G16_SINT, 1/32 flow pixel, current -> previous
RWTexture2D<float4> partialA     : register(u0);   // per group: sum gx gx, gx gy, gy gy, count
RWTexture2D<float4> partialB     : register(u1);   // per group: sum gx e, gy e, e e

groupshared float4 sumA[256];
groupshared float3 sumB[256];

float Last(float2 q, int2 last)
{
	// Bilinear at a position in pixels, centres at +0.5, from Loads.
	const float2 f = q - 0.5;
	const int2 i0 = (int2)floor(f);
	const float2 t = f - (float2)i0;
	const float a = frameLast.Load(int3(clamp(i0, int2(0, 0), last), 0));
	const float b = frameLast.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), last), 0));
	const float c = frameLast.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), last), 0));
	const float d = frameLast.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), last), 0));
	return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint tid : SV_GroupIndex)
{
	uint w, h;
	frameNow.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const float2 g = globalMotion.Load(int3(0, 0, 0)).xy;

	float4 a = 0;
	float3 b = 0;
	const int2 p = int2(id.xy);
	if (all(p >= 2) && all(p <= last - 2)) {
		uint fw, fh;
		flow.GetDimensions(fw, fh);
		const uint2 block = min((uint2)p / (uint)grid, uint2(fw, fh) - 1);
		const float2 own = (float2)flow.Load(int3(block, 0)) / 32.0;
		const float2 q = (float2)p + 0.5 + g;
		if (length(own - g) <= near && all(q >= 2.0) && all(q <= (float2)last - 1.0)) {
			const float2 grad = float2(Last(q + float2(1, 0), last) - Last(q - float2(1, 0), last),
			                           Last(q + float2(0, 1), last) - Last(q - float2(0, 1), last)) * 0.5;
			const float e = frameNow.Load(int3(p, 0)) - Last(q, last);
			a = float4(grad.x * grad.x, grad.x * grad.y, grad.y * grad.y, 1.0);
			b = float3(grad * e, e * e);
		}
	}
	sumA[tid] = a;
	sumB[tid] = b;
	GroupMemoryBarrierWithGroupSync();
	for (uint s = 128; s > 0; s >>= 1) {
		if (tid < s) {
			sumA[tid] += sumA[tid + s];
			sumB[tid] += sumB[tid + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid == 0) {
		partialA[group.xy] = sumA[0];
		partialB[group.xy] = float4(sumB[0], 0);
	}
}

#else

Texture2D<float4>   partialA     : register(t0);
Texture2D<float4>   partialB     : register(t1);
RWTexture2D<float4> globalMotion : register(u0);   // 1x1, the estimate: refined in place

#define THREADS    1024
#define BINS       256
#define BIN_OCTAVE 6.4      // bins per octave of mean e^2, from 2^-40 to 1
#define MIN_GROUP  64.0     // pixels a group needs to count
#define MIN_TEX    1e-6     // mean squared gradient a group needs to count in the median

groupshared uint   hist[BINS];
groupshared uint   counted;
groupshared float  limit;
groupshared float4 sumA[THREADS];
groupshared float2 sumB[THREADS];

int Bin(float r)
{
	return clamp((int)((log2(max(r, 1e-12)) + 40.0) * BIN_OCTAVE), 0, BINS - 1);
}

bool Counts(float4 A)
{
	return A.w >= MIN_GROUP && (A.x + A.z) > MIN_TEX * A.w;
}

[numthreads(THREADS, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
	uint w, h;
	partialA.GetDimensions(w, h);

	// The median group, by a histogram of its mean e^2 in fractions of an octave.
	if (tid < BINS) {
		hist[tid] = 0;
	}
	if (tid == 0) {
		counted = 0;
		limit = 3.402823e38;
	}
	GroupMemoryBarrierWithGroupSync();
	if (robust > 0.0) {
		for (uint i = tid; i < w * h; i += THREADS) {
			const int3 at = int3(i % w, i / w, 0);
			const float4 A = partialA.Load(at);
			if (Counts(A)) {
				InterlockedAdd(hist[Bin(partialB.Load(at).z / A.w)], 1);
				InterlockedAdd(counted, 1);
			}
		}
		GroupMemoryBarrierWithGroupSync();
		if (tid == 0 && counted > 0) {
			const uint rank = (counted + 1) / 2;
			uint below = 0;
			int k = 0;
			while (k < BINS - 1 && below + hist[k] < rank) {
				below += hist[k];
				k++;
			}
			// The bin's middle, back to mean e^2.
			limit = robust * exp2(((float)k + 0.5) / BIN_OCTAVE - 40.0);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	float4 a = 0;
	float2 b = 0;
	for (uint i = tid; i < w * h; i += THREADS) {
		const int3 at = int3(i % w, i / w, 0);
		const float4 A = partialA.Load(at);
		const float4 B = partialB.Load(at);
		if (A.w > 0.0 && B.z < limit * A.w) {
			a += A;
			b += B.xy;
		}
	}
	sumA[tid] = a;
	sumB[tid] = b;
	GroupMemoryBarrierWithGroupSync();
	for (uint s = THREADS / 2; s > 0; s >>= 1) {
		if (tid < s) {
			sumA[tid] += sumA[tid + s];
			sumB[tid] += sumB[tid + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid == 0) {
		const float4 A = sumA[0];
		const float2 B = sumB[0];
		const float det = A.x * A.z - A.y * A.y;
		// Texture enough both ways: the smaller eigenvalue of the gradients' matrix,
		// per pixel, above what 8-bit noise alone gives.
		const float tr = A.x + A.z;
		const float minEigen = 0.5 * (tr - sqrt(max(tr * tr - 4.0 * det, 0.0)));
		float4 estimate = globalMotion[uint2(0, 0)];
		if (A.w >= minCount && minEigen > 1e-6 * A.w) {
			float2 step = float2(A.z * B.x - A.y * B.y, A.x * B.y - A.y * B.x) / det;
			const float len = length(step);
			if (len > maxStep) {
				step *= maxStep / len;
			}
			estimate.xy += step;
		}
		globalMotion[uint2(0, 0)] = estimate;
	}
}

#endif
