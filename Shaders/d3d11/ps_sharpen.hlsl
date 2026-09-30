// Sharpening, one method per PASS, over a picture that is already the size it
// will be shown at.
//
// A sharpener restores nothing -- it exaggerates. All of the difference between
// them is in where they are allowed to: a plain unsharp mask exaggerates
// everywhere and rings along every edge, the others each hold themselves back
// somewhere.
//
//   PASS 1  unsharp mask, nothing held back. The one to beat, and the one that
//           shows what ringing looks like.
//   PASS 2  the same, then pulled back into the range the neighbours really
//           cover -- the principle of AviSynth's LimitedSharpenFaster, and the
//           same clamp ps_mpv_prescale.hlsl already uses against the networks.
//   PASS 3  AMD FidelityFX CAS: the gain falls where the local contrast is
//           already high, so it cannot overshoot what the 3x3 holds.
//   PASS 4  CAS's shape with the clamp of PASS 2 on top, which is both limits at
//           once.
//
// Each method runs at the setting its own author gives it, and `strength` is
// how much of its answer is kept: 1.0 is the method as written, below that a
// part of it, above that more than its author meant. One knob, the same meaning
// for all of them, which is what a comparison needs and what a slider should be.
//
// RGBMODE 0 moves the luma only: the correction goes to R, G and B alike, which
// leaves B - Y and R - Y exactly as they were -- the trick of
// ps_mpv_prescale.hlsl PASS 2, and the reason sharpening does not have to touch
// the colour. RGBMODE 1 sharpens the three channels on their own, which is what
// CAS does natively and what a 4:4:4 picture could afford.

#ifndef PASS
    #define PASS 1
#endif
#ifndef RGBMODE
    #define RGBMODE 0
#endif

Texture2D tex : register(t0);
SamplerState samp : register(s0);

cbuffer PS_SHARPEN : register(b0)
{
    float2 wh;       // the picture's size
    float2 dxdy;     // one texel
    float2 scale;    // 1, 1 here: this pass does not resize
    float  strength; // how much of the method's answer is kept, 0 = none
    float  param2;   // PASS 2 and 4: how far the clamp may pull back
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

// BT.709, the weights of the video this renderer is given.
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// The nine texels around this one, in the order CAS reads them:
//   a b c
//   d e f
//   g h i
#define TAP(dx, dy) tex.SampleLevel(samp, uv + float2(dx, dy) * dxdy, 0).rgb

float3 Sharpen(float2 uv, float3 e);

float4 main(PS_INPUT input) : SV_Target
{
    const float2 uv = input.Tex;
    const float4 centre = tex.SampleLevel(samp, uv, 0);
    const float3 sharp = lerp(centre.rgb, Sharpen(uv, centre.rgb), strength);

#if RGBMODE
    return float4(sharp, centre.a);
#else
    // Only the luma of the answer is kept, and it is carried to the three
    // channels by the same amount: the colour difference is untouched.
    return float4(centre.rgb + (dot(sharp, kLuma) - dot(centre.rgb, kLuma)), centre.a);
#endif
}

// The range the 3x3 around this point really covers. Nothing a sharpener
// produces may leave it: at an edge that range spans the whole step, so the step
// still gets steeper, and what it cannot do any more is shoot past it.
void Range(float2 uv, float3 e, out float3 lo, out float3 hi)
{
    const float3 a = TAP(-1, -1), b = TAP(0, -1), c = TAP(1, -1);
    const float3 d = TAP(-1,  0),                 f = TAP(1,  0);
    const float3 g = TAP(-1,  1), h = TAP(0,  1), i = TAP(1,  1);
    lo = min(min(min(a, b), min(c, d)), min(min(f, g), min(h, min(i, e))));
    hi = max(max(max(a, b), max(c, d)), max(max(f, g), max(h, max(i, e))));
}

// --------------------------------------------------------------- PASS 1, 2 --
#if PASS == 1 || PASS == 2

float3 Sharpen(float2 uv, float3 e)
{
    // A 3x3 gaussian, [1 2 1; 2 4 2; 1 2 1] / 16, and the high pass is what the
    // picture has above it. Amount 1.0, the textbook setting.
    const float3 blur =
        (TAP(-1, -1) + TAP(1, -1) + TAP(-1, 1) + TAP(1, 1)
        + 2.0 * (TAP(0, -1) + TAP(-1, 0) + TAP(1, 0) + TAP(0, 1))
        + 4.0 * e) / 16.0;

    float3 result = e + (e - blur);

#if PASS == 2
    float3 lo, hi;
    Range(uv, e, lo, hi);
    result += param2 * (clamp(result, lo, hi) - result);
#endif

    return result;
}

// ------------------------------------------------------------ PASS 3, 4, CAS --
#elif PASS == 3 || PASS == 4

float3 Sharpen(float2 uv, float3 e)
{
    const float3 a = TAP(-1, -1), b = TAP(0, -1), c = TAP(1, -1);
    const float3 d = TAP(-1,  0),                 f = TAP(1,  0);
    const float3 g = TAP(-1,  1), h = TAP(0,  1), i = TAP(1,  1);

    // AMD's own weighting: the cross counts once and the whole 3x3 once more,
    // so the diagonals weigh half. mn and mx come out on a 0..2 scale.
    float3 mn = min(min(min(d, e), f), min(b, h));
    float3 mx = max(max(max(d, e), f), max(b, h));
    mn += min(min(min(mn, a), c), min(g, i));
    mx += max(max(max(mx, a), c), max(g, i));

    // How far this pixel is from the ends of what is around it. Where the local
    // contrast is already high there is nothing to gain and amp falls to zero,
    // which is what keeps CAS from ringing.
    const float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));

    // The peak of the kernel. AMD moves it between -1/8 and -1/5 with its own
    // sharpness knob; the middle is taken here, and the one knob above does the
    // rest.
    const float3 w = amp * (-1.0 / 6.5);
    float3 result = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);

#if PASS == 4
    float3 lo, hi;
    Range(uv, e, lo, hi);
    result += param2 * (clamp(result, lo, hi) - result);
#endif

    return result;
}

#else
    #error "PASS must be 1..4"
#endif
