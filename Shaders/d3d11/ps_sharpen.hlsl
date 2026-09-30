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
//   PASS 5  adaptive-sharpen, ported from bacondither's mpv shader (BSD 2-clause,
//           version 2021-10-17, tuned by its author for use after a resize). It
//           weighs every one of 25 neighbours by how active its own area is, and
//           limits the result with a tanh against the nearest of the local
//           extremes -- the most careful of the five, and the dearest.
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

#if PASS == 5
    // adaptive-sharpen carries its own strength -- the curve's height -- and
    // already adds one number to the three channels, which is the luma-only form
    // by construction. Neither the blend nor the mode above applies to it.
    return float4(Sharpen(uv, centre.rgb), centre.a);
#else
    const float3 sharp = lerp(centre.rgb, Sharpen(uv, centre.rgb), strength);

#if RGBMODE
    return float4(sharp, centre.a);
#else
    // Only the luma of the answer is kept, and it is carried to the three
    // channels by the same amount: the colour difference is untouched.
    return float4(centre.rgb + (dot(sharp, kLuma) - dot(centre.rgb, kLuma)), centre.a);
#endif
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

// ------------------------------------------- PASS 5, adaptive-sharpen --
#elif PASS == 5

// Copyright (c) 2015-2021, bacondither. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the conditions of the BSD 2-clause
// licence are met; the full notice travels with the source this was taken from,
// upscalers\adaptive-sharpen.glsl. Translated to HLSL, otherwise unchanged:
// `strength` is the shader's own curve_height, whose author calls 1.0 the
// default and 0.5 the one his own measurements prefer.

#define curveslope      0.5
#define L_compr_low     0.167
#define L_compr_high    0.334
#define D_compr_low     0.250
#define D_compr_high    0.500
#define scale_lim       0.1
#define scale_cs        0.056
#define pm_p            1.0

#define sat(x)         saturate(x)
#define max4(a,b,c,d)  ( max(max(a, b), max(c, d)) )
#define soft_lim(v,s)  ( sat(abs(v/s)*(27.0 + pow(v/s, 2.0))/(27.0 + 9.0*pow(v/s, 2.0)))*s )
#define wpmean(a,b,w)  ( pow(w*pow(abs(a), pm_p) + abs(1.0-w)*pow(abs(b), pm_p), (1.0/pm_p)) )
#define dxdy(val)      ( length(fwidth(val)) )
#define CtL(RGB)       ( sqrt(dot(sat(RGB)*sat(RGB), kLuma)) )
#define b_diff(pix)    ( (blur-luma[pix])*(blur-luma[pix]) )

float3 Sharpen(float2 uv, float3 e0)
{
    // [                c22               ]
    // [           c24, c9,  c23          ]
    // [      c21, c1,  c2,  c3, c18      ]
    // [ c19, c10, c4,  c0,  c5, c11, c16 ]
    // [      c20, c6,  c7,  c8, c17      ]
    // [           c15, c12, c14          ]
    // [                c13               ]
    float3 c[25] = {
        TAP( 0, 0), TAP(-1,-1), TAP( 0,-1), TAP( 1,-1), TAP(-1, 0),
        TAP( 1, 0), TAP(-1, 1), TAP( 0, 1), TAP( 1, 1), TAP( 0,-2),
        TAP(-2, 0), TAP( 2, 0), TAP( 0, 2), TAP( 0, 3), TAP( 1, 2),
        TAP(-1, 2), TAP( 3, 0), TAP( 2, 1), TAP( 2,-1), TAP(-3, 0),
        TAP(-2, 1), TAP(-2,-1), TAP( 0,-3), TAP( 1,-2), TAP(-1,-2) };

    float e[13] = {
        dxdy(c[0]),  dxdy(c[1]),  dxdy(c[2]),  dxdy(c[3]),  dxdy(c[4]),
        dxdy(c[5]),  dxdy(c[6]),  dxdy(c[7]),  dxdy(c[8]),  dxdy(c[9]),
        dxdy(c[10]), dxdy(c[11]), dxdy(c[12]) };

    float luma[25];
    [unroll] for (int k = 0; k < 25; k++) {
        luma[k] = CtL(c[k]);
    }
    const float c0_Y = luma[0];

    // Blur, gauss 3x3
    const float blur = (2.0 * (luma[2]+luma[4]+luma[5]+luma[7]) + (luma[1]+luma[3]+luma[6]+luma[8]) + 4.0 * luma[0]) / 16.0;

    // Contrast compression, centre = 0.5
    const float c_comp = sat(0.266666681 + 0.9*exp2(blur * blur * -7.4));

    const float edge = ( 1.38*b_diff(0)
                 + 1.15*(b_diff(2) + b_diff(4) + b_diff(5) + b_diff(7))
                 + 0.92*(b_diff(1) + b_diff(3) + b_diff(6) + b_diff(8))
                 + 0.23*(b_diff(9) + b_diff(10) + b_diff(11) + b_diff(12)) ) * c_comp;

    // overshoot_ctrl is false in the original's defaults, so cs stays here.
    const float2 cs = float2(L_compr_low, D_compr_low);

    const float3 w1 = float3(0.5,           1.0, 1.41421356237);
    const float3 w2 = float3(0.86602540378, 1.0, 0.54772255751);
    const float3 dWs = lerp(w1, w2, sat(2.4*edge - 0.82));
    const float3 dW = dWs * dWs;

    const float modif_e0 = 3.0 * e[0] + 0.02/2.5;

    float weights[12] = {
        min(modif_e0/e[1],  dW.y), dW.x, min(modif_e0/e[3],  dW.y), dW.x,
        dW.x, min(modif_e0/e[6],  dW.y), dW.x, min(modif_e0/e[8],  dW.y),
        min(modif_e0/e[9],  dW.z), min(modif_e0/e[10], dW.z),
        min(modif_e0/e[11], dW.z), min(modif_e0/e[12], dW.z) };

    weights[0] = (max(max((weights[8]  + weights[9])/4.0,  weights[0]), 0.25) + weights[0])/2.0;
    weights[2] = (max(max((weights[8]  + weights[10])/4.0, weights[2]), 0.25) + weights[2])/2.0;
    weights[5] = (max(max((weights[9]  + weights[11])/4.0, weights[5]), 0.25) + weights[5])/2.0;
    weights[7] = (max(max((weights[10] + weights[11])/4.0, weights[7]), 0.25) + weights[7])/2.0;

    float lowthrsum   = 0.0;
    float weightsum   = 0.0;
    float neg_laplace = 0.0;
    [unroll] for (int pix = 0; pix < 12; pix++) {
        const float lowthr = sat((20.*4.5*c_comp*e[pix + 1] - 0.221));
        neg_laplace += luma[pix+1] * luma[pix+1] * weights[pix] * lowthr;
        weightsum   += weights[pix] * lowthr;
        lowthrsum   += lowthr / 12.0;
    }
    neg_laplace = sqrt(neg_laplace / max(weightsum, 1e-6));

    // The curve's height is this pass's own strength.
    const float sharpen_val = strength/(strength*curveslope*edge + 0.625);
    float sharpdiff = (c0_Y - neg_laplace)*(lowthrsum*sharpen_val + 0.01);

    // Local near min and max, partial sort over the 25 luma values.
    float temp;
    [unroll] for (int i1 = 0; i1 < 24; i1 += 2) {
        temp = luma[i1];
        luma[i1]   = min(luma[i1], luma[i1+1]);
        luma[i1+1] = max(temp, luma[i1+1]);
    }
    [unroll] for (int i2 = 24; i2 > 0; i2 -= 2) {
        temp = luma[0];
        luma[0]  = min(luma[0], luma[i2]);
        luma[i2] = max(temp, luma[i2]);

        temp = luma[24];
        luma[24]   = max(luma[24], luma[i2-1]);
        luma[i2-1] = min(temp, luma[i2-1]);
    }

    float min_dist = min(abs(luma[24] - c0_Y), abs(c0_Y - luma[0]));
    min_dist = min(min_dist, scale_lim*(1.0 - scale_cs) + min_dist*scale_cs);

    // Soft limited anti-ringing with tanh, wpmean to control the compression slope.
    sharpdiff = wpmean(max(sharpdiff, 0.0), soft_lim( max(sharpdiff, 0.0), min_dist ), cs.x)
              - wpmean(min(sharpdiff, 0.0), soft_lim( min(sharpdiff, 0.0), min_dist ), cs.y);

    const float sharpdiff_lim = sat(c0_Y + sharpdiff) - c0_Y;
    return c[0] + sharpdiff_lim;
}

#else
    #error "PASS must be 1..5"
#endif
