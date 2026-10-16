// A cut between this picture and the last, for the vectors DLSS is given.
//
// DLSS Super Resolution resamples its history along its vectors, and nothing told it
// that the picture before belonged to another shot: where a bright shot followed a dark
// one it blended the dark one in, and the bright parts of the new shot came out darker
// for two or three pictures (tools/dlssnr_probe --tsrcut: 85 and 115 thousandths of
// luma on the first picture of the two cuts seen in playback). Its Reset would do, but
// the CPU sets it before the GPU has seen the picture, and waiting for the GPU is what
// render ahead exists not to do. A vector that leaves the picture makes DLSS drop the
// history of that pixel, and PASS 3 of ps_dlss_stabilize.hlsl sends every vector out
// of it when this pass says the picture is a cut: as clean as the Reset, on the GPU.
//
// Between the two flow frames, the last moved by the global motion
// (cs_dlss_global_motion.hlsl, cs_dlss_global_refine.hlsl):
//   the L1 distance of their 64-bin luma histograms, every fourth pixel each way
//   the median 16 x 16 group's mean squared difference, the residual
//   that residual against the median of the 8 pictures before, its ratio
//   the median group's normalized correlation, which a cut takes to nothing and a
//     change of light leaves alone
// A cut is a residual past residLimit and a histogram past histLimit, and either
//   the light changing at once: the histogram past lightHist and the ratio past
//     lightRatio. Most cuts, and a flash, which DLSS dims with the pictures before it
//     as it darkens a bright shot after a dark one (--tsrcut: 42 thousandths of luma)
//   or the picture: the correlation under corrLimit, the ratio past ratioLimit. A cut
//     out of a shot that already changed a lot on every picture, a chase, a car under
//     trees, where the ratio stays low; the histogram and the ratio keep out the
//     foliage and the dust such a shot is made of, which take the correlation as low
// Chosen on 48 minutes of six films beside ffmpeg's own scene score (2026-10-16,
// tools/dlssnr_probe --tcutscan, CDlssStabilizer's kCut...).
//
//   PASS 1  per 16 x 16 group: the sums of e^2 and of the gradient's square, the pixels,
//           and about the group's means, of dn^2, dl^2 and dn dl
//   PASS 2  one group: the histograms, the medians, the ratio, and whether it is a cut
// The two, 0.06 ms a picture at 1080p on an RTX 4060 (--tsrclip, cost).
//
// A cut's own residual is not kept among the 8, and no cut is called before three
// pictures are, nor after the owner reset the state (a seek, a new session).
//
// state, R32F:
//   [0] 1 on a cut     [1] histogram distance   [2] residual   [3] its ratio
//   [4] residuals kept, up to 8   [5..12] the last 8 residuals   [13] the next slot
//   [14] the median correlation (0 when no group has texture: a cut to a flat picture
//        is still one)   [15] the share of groups it is over

#ifndef PASS
#define PASS 2
#endif

#define MIN_GROUP  64.0     // pixels a group needs to count
#define MIN_TEX    1e-6     // mean squared gradient a group needs to count for the residual
#define MIN_VAR    1e-4     // variance each frame's group needs to count for the correlation

#if PASS == 1

Texture2D<float>    frameNow     : register(t0);   // the flow frame the engine just ran on
Texture2D<float>    frameLast    : register(t1);   // and the one before it
Texture2D<float4>   globalMotion : register(t2);   // 1x1: flow pixels
RWTexture2D<float4> sumsA        : register(u0);   // per group: sum e^2, sum |gradient|^2, pixels
RWTexture2D<float4> sumsB        : register(u1);   // per group: sum dn^2, dl^2, dn dl

groupshared float4 sum0[256];
groupshared float  sum1[256];

float Last(float2 q, int2 last)
{
	// Bilinear at a position in pixels, centres at +0.5, from Loads: as cs_dlss_global_refine.hlsl.
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

	// The pixels the refinement takes, every one: no block vector gate.
	float n = 0, l = 0, inside = 0;
	float4 a = 0;
	const int2 p = int2(id.xy);
	if (all(p >= 2) && all(p <= last - 2)) {
		const float2 q = (float2)p + 0.5 + g;
		if (all(q >= 2.0) && all(q <= (float2)last - 1.0)) {
			const float2 grad = float2(Last(q + float2(1, 0), last) - Last(q - float2(1, 0), last),
			                           Last(q + float2(0, 1), last) - Last(q - float2(0, 1), last)) * 0.5;
			n = frameNow.Load(int3(p, 0));
			l = Last(q, last);
			inside = 1.0;
			a = float4((n - l) * (n - l), dot(grad, grad), 1.0, n);
		}
	}
	sum0[tid] = a;
	sum1[tid] = l * inside;
	GroupMemoryBarrierWithGroupSync();
	for (uint s = 128; s > 0; s >>= 1) {
		if (tid < s) {
			sum0[tid] += sum0[tid + s];
			sum1[tid] += sum1[tid + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	const float4 totals = sum0[0];
	const float meanN = totals.w / max(totals.z, 1.0), meanL = sum1[0] / max(totals.z, 1.0);
	GroupMemoryBarrierWithGroupSync();

	// Then about the means, so that a flat bright group loses nothing to cancellation.
	const float dn = (n - meanN) * inside, dl = (l - meanL) * inside;
	sum0[tid] = float4(dn * dn, dl * dl, dn * dl, 0);
	GroupMemoryBarrierWithGroupSync();
	for (uint s2 = 128; s2 > 0; s2 >>= 1) {
		if (tid < s2) {
			sum0[tid] += sum0[tid + s2];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid == 0) {
		sumsA[group.xy] = float4(totals.xyz, 0);
		sumsB[group.xy] = float4(sum0[0].xyz, 0);
	}
}

#else

cbuffer CS_DLSS_SCENE_CUT : register(b0)
{
	float histLimit;
	float ratioLimit;
	float residLimit;
	float corrLimit;
	float lightHist;
	float lightRatio;
	float2 reserved;
};

Texture2D<float>  frameNow  : register(t0);   // the flow frame the engine just ran on
Texture2D<float>  frameLast : register(t1);   // and the one before it
Texture2D<float4> sumsA     : register(t2);   // PASS 1
Texture2D<float4> sumsB     : register(t3);
RWBuffer<float>   state     : register(u0);

#define THREADS    1024
#define BINS       64
#define RBINS      256
#define BIN_OCTAVE 6.4      // as the refinement's: bins per octave of mean e^2, from 2^-40 to 1
#define CBINS      128      // the correlation's, over -1 to 1
#define STRIDE     4        // the histograms count one flow pixel in this many, each way

groupshared uint histNow[BINS];
groupshared uint histLast[BINS];
groupshared uint resHist[RBINS];
groupshared uint corrHist[CBINS];
groupshared uint counted;
groupshared uint correlated;

int ResBin(float r)
{
	return clamp((int)((log2(max(r, 1e-12)) + 40.0) * BIN_OCTAVE), 0, RBINS - 1);
}

int CorrBin(float c)
{
	return clamp((int)((c + 1.0) * 0.5 * CBINS), 0, CBINS - 1);
}

[numthreads(THREADS, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
	if (tid < BINS) {
		histNow[tid] = 0;
		histLast[tid] = 0;
	}
	if (tid < RBINS) {
		resHist[tid] = 0;
	}
	if (tid < CBINS) {
		corrHist[tid] = 0;
	}
	if (tid == 0) {
		counted = 0;
		correlated = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	uint w, h;
	frameNow.GetDimensions(w, h);
	const uint sw = w / STRIDE, sh = h / STRIDE;
	for (uint i = tid; i < sw * sh; i += THREADS) {
		const int3 at = int3((i % sw) * STRIDE, (i / sw) * STRIDE, 0);
		InterlockedAdd(histNow[min((uint)(frameNow.Load(at) * BINS), BINS - 1)], 1);
		InterlockedAdd(histLast[min((uint)(frameLast.Load(at) * BINS), BINS - 1)], 1);
	}
	uint gw, gh;
	sumsA.GetDimensions(gw, gh);
	for (uint j = tid; j < gw * gh; j += THREADS) {
		const int3 at = int3(j % gw, j / gw, 0);
		const float4 A = sumsA.Load(at);
		if (A.z >= MIN_GROUP && A.y > MIN_TEX * A.z) {
			InterlockedAdd(resHist[ResBin(A.x / A.z)], 1);
			InterlockedAdd(counted, 1);
		}
		const float4 B = sumsB.Load(at);
		if (A.z >= MIN_GROUP && B.x > MIN_VAR * A.z && B.y > MIN_VAR * A.z) {
			InterlockedAdd(corrHist[CorrBin(B.z * rsqrt(B.x * B.y))], 1);
			InterlockedAdd(correlated, 1);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (tid == 0) {
		uint distance = 0;
		for (uint k = 0; k < BINS; k++) {
			distance += (uint)abs((int)histNow[k] - (int)histLast[k]);
		}
		const float hist = (float)distance / max((float)(sw * sh), 1.0);

		float resid = 0.0;
		if (counted > 0) {
			const uint rank = (counted + 1) / 2;
			uint below = 0;
			int b = 0;
			while (b < RBINS - 1 && below + resHist[b] < rank) {
				below += resHist[b];
				b++;
			}
			resid = exp2(((float)b + 0.5) / BIN_OCTAVE - 40.0);
		}

		float corr = 0.0;
		if (correlated > 0) {
			const uint rank = (correlated + 1) / 2;
			uint below = 0;
			int b = 0;
			while (b < CBINS - 1 && below + corrHist[b] < rank) {
				below += corrHist[b];
				b++;
			}
			corr = ((float)b + 0.5) * 2.0 / CBINS - 1.0;
		}

		// The median of the residuals kept: a sort of up to 8.
		const uint kept = (uint)state[4];
		float sorted[8];
		for (uint m = 0; m < 8; m++) {
			sorted[m] = m < kept ? state[5 + m] : 3.402823e38;
		}
		for (uint p = 1; p < 8; p++) {
			const float v = sorted[p];
			int q = (int)p - 1;
			while (q >= 0 && sorted[q] > v) {
				sorted[q + 1] = sorted[q];
				q--;
			}
			sorted[q + 1] = v;
		}
		const float base = kept > 0 ? sorted[(kept - 1) / 2] : resid;
		const float ratio = resid / max(base, 1e-12);

		const bool light = hist > lightHist && ratio > lightRatio;
		const bool picture = corr < corrLimit && ratio > ratioLimit;
		const bool cut = kept >= 3 && counted > 0 && resid > residLimit && hist > histLimit && (light || picture);
		state[0] = cut ? 1.0 : 0.0;
		state[1] = hist;
		state[2] = resid;
		state[3] = ratio;
		if (!cut && counted > 0) {
			const uint slot = (uint)state[13] % 8;
			state[5 + slot] = resid;
			state[13] = (float)((slot + 1) % 8);
			state[4] = (float)min(kept + 1, 8u);
		}
		state[14] = corr;
		state[15] = (float)correlated / max((float)(gw * gh), 1.0);
	}
}

#endif
