// The frame the cadence slips' scene cut detector compares (Source/Cadence/SceneCut.cpp).
//
// The detector is cs_dlss_scene_cut.hlsl's, which DLSS Super Resolution runs on the flow
// frames NVIDIA Optical Flow is given: the luma of the picture DLSS works on, a box of
// `factor` + 2 `reach` pixels each way averaged into each flow pixel (PASS 0 of
// ps_dlss_stabilize.hlsl), R8. This makes the same frame from the picture as it is drawn,
// any card, for the slips that wait for a cut: so that the thresholds chosen for the flow
// frames hold here as well. An HDR10 picture is seen as EchoStorm's passes see it, in its
// SDR view (echo_hdr.hlsli), where the flow frames of an SDR film would put it.

#include "echo_hdr.hlsli"

Texture2D<float4>          picture : register(t0);   // the video area as drawn
RWTexture2D<unorm float>   frame   : register(u0);   // R8_UNORM

cbuffer CS_SCENE_CUT_LUMA : register(b0)
{
	uint  factor;     // picture pixels per frame pixel: 1 to 4
	uint  reach;      // picture pixels the box reaches beyond its frame pixel on each side
	uint  encoding;   // 0 SDR, 2 HDR10 (PQ)
	float white;      // the SDR white an HDR picture is seen under, nits
};

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint fw, fh;
	frame.GetDimensions(fw, fh);
	if (id.x >= fw || id.y >= fh) {
		return;
	}
	uint w, h;
	picture.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const int f = clamp((int)factor, 1, 4);
	const int r = clamp((int)reach, 0, 4);
	const int n = f + 2 * r;
	const int2 base = int2(id.xy) * f - r;
	float3 c = 0;
	[loop] for (int y = 0; y < n; y++) {
		[loop] for (int x = 0; x < n; x++) {
			c += ToSdr(picture.Load(int3(clamp(base + int2(x, y), int2(0, 0), last), 0)).rgb, encoding, white);
		}
	}
	c /= (float)(n * n);
	frame[id.xy] = dot(saturate(c), kLuma);
}
