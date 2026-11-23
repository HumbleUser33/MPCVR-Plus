// The HDR curve on a PQ BT.2020 picture, the last change to the light of every picture that
// leaves in HDR: the shadows raised below a range of nits, the mid grey moved with the peak
// held, the highlights rolled off, a ceiling. One gain for the three channels, worked out on
// the pixel's light, so a colour keeps its hue. The curve is Veyra's (GPL-3.0, commit
// c663312, shaders/PresentBlit.hlsl, HdrCurveLuminance and HdrCurve); its numbers come from
// Source/HdrCurve.h, whose Apply() is this shader on the CPU, for dlssnr_harness --thdrcurve.

#include "../convert/st2084.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

cbuffer HdrCurveConstants : register(b0)
{
	float liftGain;  // 2^EV - 1; 0 lifts nothing
	float rangeNits; // where the lift has faded out
	float midGamma;  // 1 leaves the mid grey
	float peakNits;  // what that power holds fixed
	float startNits; // 0: no roll-off
	float rollNits;
	float capNits;   // 0: no ceiling
	float padding;
};

float CurveNits(float nits)
{
	if (liftGain != 0.0 && rangeNits > 0.0) {
		const float w = saturate(1.0 - nits / rangeNits);
		nits *= 1.0 + w * w * liftGain;
	}
	if (midGamma != 1.0) {
		nits = peakNits * pow(max(nits / peakNits, 0.0), midGamma);
	}
	if (startNits > 0.0 && rollNits > 0.0 && nits > startNits) {
		const float d = nits - startNits;
		nits = startNits + d / (1.0 + d / rollNits);
	}
	if (capNits > 0.0) {
		nits = min(nits, capNits);
	}
	return nits;
}

float4 main(PS_INPUT input) : SV_Target
{
	const float4 pq = tex.Sample(samp, input.Tex);
	float3 rgb = ST2084ToLinear(float4(saturate(pq.rgb), 0.0), 10000.0).rgb;

	// Black stays black, untouched by the round trip through light.
	const float y = dot(rgb, float3(0.2627, 0.6780, 0.0593));
	if (y <= 1e-6) {
		return pq;
	}
	rgb *= CurveNits(y) / y;
	if (capNits > 0.0) {
		const float top = max(rgb.r, max(rgb.g, rgb.b));
		if (top > capNits) {
			rgb *= capNits / top;
		}
	}

	return float4(LinearToST2084(float4(rgb, 0.0), 10000.0).rgb, pq.a);
}
