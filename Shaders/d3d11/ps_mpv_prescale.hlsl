// The passes around the mpv prescalers (Shaders/mpv), which enlarge one plane:
// the renderer hands them that plane and puts the colour back itself.
//
//   PASS 1  luma of the picture, for the luma prescalers, as grey: FSRCNNX, RAVU-zoom
//           and ArtCNN read red, and Anime4K, written for mpv's RGB picture, reads
//           all three and leaves a grey picture grey
//   PASS 2  colour: the picture enlarged by Catmull-Rom (t0) with its luma
//           replaced by the prescaler's (t1), both at the same size. The
//           difference goes to R, G and B alike, which moves luma and leaves
//           B - Y and R - Y as they were, whatever weights the luma has.
//   PASS 3  one channel of a chroma plane (the constant picks it) into red, for
//           a prescaler on Cb and on Cr
//   PASS 4  pass 2 with anti-ringing: before the luma goes in, it is held to the
//           range the source plane (t2) really covers around that point, which is
//           what libplacebo does inside its own kernels. A network invents an edge
//           steeper than the one it was given; this takes back the overshoot.
//   PASS 5  anti-ringing alone, on a plane a prescaler enlarged (t0) against the
//           plane it was given (t1): for chroma, which has no colour to put back.
//
// They all draw through the renderer's quad and read with its point sampler,
// output and input sizes matching texel for texel, except where a pass reads the
// smaller plane it started from.

#ifndef PASS
#define PASS 1
#endif

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
SamplerState samp : register(s0);

cbuffer PS_MPV_PRESCALE : register(b0)
{
	float4 param; // pass 3: 1 for the channel to take, 0 for the others
	              // pass 5: where the prescaler read the plane, its shift
};

// How far a value may be pulled back towards what the source really holds:
// libplacebo settles on 0.8, and the doom9 chroma test shows what it buys.
#define ANTIRING_STRENGTH 0.8

// The range the four source texels around this point cover. tex is the plane the
// prescaler was given, pos the point in it, in normalized coordinates.
void SourceRange(Texture2D tex, float2 pos, out float lo, out float hi)
{
	float2 size;
	tex.GetDimensions(size.x, size.y);
	const float2 step = 1.0 / size;
	const float2 corner = (floor(pos * size - 0.5) + 0.5) * step;
	const float a = tex.SampleLevel(samp, corner, 0).r;
	const float b = tex.SampleLevel(samp, corner + float2(step.x, 0), 0).r;
	const float c = tex.SampleLevel(samp, corner + float2(0, step.y), 0).r;
	const float d = tex.SampleLevel(samp, corner + step, 0).r;
	lo = min(min(a, b), min(c, d));
	hi = max(max(a, b), max(c, d));
}

// BT.709 weights, those of the video the networks were trained on.
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_Target
{
#if PASS == 1
	float3 rgb = tex0.Sample(samp, input.Tex).rgb;
	float y = dot(rgb, kLuma);
	return float4(y, y, y, 1);
#elif PASS == 2
	float4 color = tex0.Sample(samp, input.Tex);
	float y = tex1.Sample(samp, input.Tex).r;
	color.rgb += y - dot(color.rgb, kLuma);
	return color;
#elif PASS == 3
	return float4(dot(tex0.Sample(samp, input.Tex), param), 0, 0, 1);
#elif PASS == 4
	float4 color = tex0.Sample(samp, input.Tex);
	float y = tex1.Sample(samp, input.Tex).r;
	float lo, hi;
	SourceRange(tex2, input.Tex, lo, hi);
	y += ANTIRING_STRENGTH * (clamp(y, lo, hi) - y);
	color.rgb += y - dot(color.rgb, kLuma);
	return color;
#else
	float value = tex0.Sample(samp, input.Tex).r;
	float lo, hi;
	SourceRange(tex1, input.Tex + param.xy, lo, hi);
	value += ANTIRING_STRENGTH * (clamp(value, lo, hi) - value);
	return float4(value, 0, 0, 1);
#endif
}
