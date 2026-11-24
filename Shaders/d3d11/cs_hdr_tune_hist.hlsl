// The HDR curve's tune: what one picture of the film looks like, as the curve would receive it.
//
// Two histograms over the PQ signal of a PQ BT.2020 picture, 1024 bins each, bin i standing
// for the PQ value i / 1023: the pixel's light (BT.2020 luminance, from the decoded channels,
// encoded again) and its brightest channel. The light is what the curve works on; the
// brightest channel is what a cap clips. PQ bins are close to even steps of what the eye sees,
// so one histogram serves the shadows and the highlights alike. Source/HdrCurveTune.h turns
// them into percentiles. cs_hdr_hist.hlsl, the measured peak's, is the model; its buffers serve
// three other users and are not shared.

#include "../convert/st2084.hlsl"

#define HIST_BINS 1024
#define GROUP_SIZE 16 // 16x16 threads, each covering 2x2 pixels = 32x32 pixels per group

Texture2D<float4> tex : register(t0);
RWStructuredBuffer<uint> hist : register(u0); // [0, 1024) the light, [1024, 2048) the brightest channel

cbuffer TuneConstants : register(b0)
{
	uint2 rectOrigin; // top-left corner of the video rectangle inside the texture
	uint2 rectSize;
};

groupshared uint gsLight[HIST_BINS];
groupshared uint gsTop[HIST_BINS];

static const float3 kLum2020 = float3(0.2627, 0.6780, 0.0593);

uint Bin(float pq)
{
	return min((uint)(saturate(pq) * (HIST_BINS - 1) + 0.5f), HIST_BINS - 1);
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
	for (uint i = groupIndex; i < HIST_BINS; i += GROUP_SIZE * GROUP_SIZE) {
		gsLight[i] = 0;
		gsTop[i] = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	const uint2 origin = groupId.xy * (GROUP_SIZE * 2) + threadId.xy * 2;

	[unroll]
	for (uint dy = 0; dy < 2; dy++) {
		[unroll]
		for (uint dx = 0; dx < 2; dx++) {
			const uint2 p = origin + uint2(dx, dy);
			if (p.x < rectSize.x && p.y < rectSize.y) {
				const float3 pq = saturate(tex.Load(int3(rectOrigin + p, 0)).rgb);
				const float3 nits = ST2084ToLinear(float4(pq, 0), 10000.0f).rgb;
				const float light = LinearToST2084(float4(dot(nits, kLum2020), 0, 0, 0), 10000.0f).x;
				InterlockedAdd(gsLight[Bin(light)], 1);
				InterlockedAdd(gsTop[Bin(max(pq.r, max(pq.g, pq.b)))], 1);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint j = groupIndex; j < HIST_BINS; j += GROUP_SIZE * GROUP_SIZE) {
		if (gsLight[j] != 0) {
			InterlockedAdd(hist[j], gsLight[j]);
		}
		if (gsTop[j] != 0) {
			InterlockedAdd(hist[HIST_BINS + j], gsTop[j]);
		}
	}
}
