// DLSS 5 NR at a working size smaller than the picture, and its change carried back.
//
// The network costs in proportion to the pixels it is given: 62 ms at 3840x2160 on an
// RTX 4060, longer than a frame of film lasts. So the picture is reduced, the network
// runs on that, and what it changed is carried back onto the picture at its own size:
// the source keeps its own fine detail and grain, and the network's change reaches it
// down to the detail the working size holds. Three ways of carrying it back, measured
// against the network run at the full size (tools/dlssnr_probe --tnrscale). The filter
// runs PASS 0 with the reduction, and embeds those three; the bench embeds them all:
//
//   PASS 0  the change, out - in at the working size, enlarged by Catmull-Rom and added
//           to the picture; with plain set, the network's output enlarged alone, which
//           is what reducing and enlarging would give without any composition
//   PASS 1  the same change enlarged by joint bilateral upsampling: each of the 4x4
//           working pixels around counts as much as it resembles the picture's own
//           pixel, so the change keeps to the picture's edges and does not spill
//   PASS 2-6  the guided filter (He, Sun and Tang): a linear model out = a * in + b,
//           per channel, fitted over a box of the working picture, then a and b
//           enlarged and applied to the picture itself
//     PASS 2  box, across: I, P, I*I and I*P (MRT, four targets)
//     PASS 3  box, down, then a and b (MRT, two targets)
//     PASS 4  box across a and b (MRT, two targets)
//     PASS 5  box down a and b (MRT, two targets)
//     PASS 6  the picture: a and b enlarged bilinearly, a * picture + b
//   PASS 7  the reduction, across: Lanczos, as the Downscaling list's best measured
//   PASS 8  the reduction, down
//
// Every texture is read with Load at integer positions, clamped at the edges: the
// sizes are known and no sampler state has a say in the result.

#ifndef PASS
    #define PASS 0
#endif

#define FILTER 4
#include "../resize/convolution_filters.hlsl"

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
Texture2D tex3 : register(t3);

cbuffer PS_CONSTANTS : register(b0)
{
    float2 fullSize;    // the picture, pixels
    float2 workSize;    // what the network works at
    float  sigma;       // PASS 1: how far apart two colours may be and still count
    float  epsilon;     // PASS 3: the guided filter's regularisation
    float  radius;      // PASS 2-5: the box, in working pixels each side
    float  plain;       // PASS 0: the network's output enlarged, nothing composed
    float2 srcSize;     // PASS 7, 8: the texture being reduced
    float2 dstSize;     // and the size it is reduced to
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

float4 At(Texture2D t, int2 p, float2 size)
{
    return t.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0));
}

// Catmull-Rom weights of the taps at -1, 0, 1 and 2 for a position t between 0 and 1.
float4 CatmullRom(float t)
{
    const float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t,
                   1.5 * t3 - 2.5 * t2 + 1.0,
                  -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                   0.5 * t3 - 0.5 * t2);
}

#if (PASS == 0 || PASS == 1)

// tex0 the picture, tex1 the network's input, tex2 its output.
float4 main(PS_INPUT input) : SV_Target
{
    const int2 p = int2(input.Pos.xy);
    const float2 pw = input.Pos.xy * (workSize / fullSize) - 0.5;   // in working pixels
    const int2 base = int2(floor(pw));
    const float2 f = pw - base;
    const float4 picture = At(tex0, p, fullSize);

#if (PASS == 0)
    const float4 wx = CatmullRom(f.x);
    const float4 wy = CatmullRom(f.y);
    float3 sum = 0;
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int i = 0; i < 4; i++) {
            const int2 q = base + int2(i - 1, j - 1);
            const float3 out_ = At(tex2, q, workSize).rgb;
            const float3 in_ = At(tex1, q, workSize).rgb;
            sum += wx[i] * wy[j] * (plain > 0.5 ? out_ : out_ - in_);
        }
    }
    return float4(plain > 0.5 ? sum : picture.rgb + sum, picture.a);
#else
    // A Gaussian over distance, of about a working pixel, times a Gaussian over how far
    // the working pixel's colour is from the picture's own.
    const float inv2s2 = 0.5 / (sigma * sigma);
    float3 sum = 0;
    float  wsum = 0;
    float3 near = 0;   // the spatial weights alone, where no neighbour resembles the pixel
    float  nsum = 0;
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int i = 0; i < 4; i++) {
            const int2 q = base + int2(i - 1, j - 1);
            const float2 d = float2(i - 1, j - 1) - f;
            const float ws = exp(-dot(d, d) / (2.0 * 0.75 * 0.75));
            const float3 in_ = At(tex1, q, workSize).rgb;
            const float3 change = At(tex2, q, workSize).rgb - in_;
            const float3 c = picture.rgb - in_;
            const float w = ws * exp(-dot(c, c) * inv2s2);
            sum += w * change;
            wsum += w;
            near += ws * change;
            nsum += ws;
        }
    }
    const float3 change = wsum > 1e-6 ? sum / wsum : near / nsum;
    return float4(picture.rgb + change, picture.a);
#endif
}

#elif (PASS == 2)

struct PS_OUTPUT4
{
    float4 i  : SV_Target0;
    float4 p  : SV_Target1;
    float4 ii : SV_Target2;
    float4 ip : SV_Target3;
};

// tex1 the network's input (I), tex2 its output (P), at the working size.
PS_OUTPUT4 main(PS_INPUT input)
{
    const int2 q = int2(input.Pos.xy);
    const int r = (int)radius;
    PS_OUTPUT4 o = (PS_OUTPUT4)0;
    for (int k = -r; k <= r; k++) {
        const float3 I = At(tex1, q + int2(k, 0), workSize).rgb;
        const float3 P = At(tex2, q + int2(k, 0), workSize).rgb;
        o.i.rgb += I;
        o.p.rgb += P;
        o.ii.rgb += I * I;
        o.ip.rgb += I * P;
    }
    return o;
}

#elif (PASS == 3)

struct PS_OUTPUT2
{
    float4 a : SV_Target0;
    float4 b : SV_Target1;
};

// tex0..tex3 PASS 2's four sums across.
PS_OUTPUT2 main(PS_INPUT input)
{
    const int2 q = int2(input.Pos.xy);
    const int r = (int)radius;
    float3 sI = 0, sP = 0, sII = 0, sIP = 0;
    for (int k = -r; k <= r; k++) {
        const int2 at = q + int2(0, k);
        sI  += At(tex0, at, workSize).rgb;
        sP  += At(tex1, at, workSize).rgb;
        sII += At(tex2, at, workSize).rgb;
        sIP += At(tex3, at, workSize).rgb;
    }
    const float n = (2 * r + 1) * (2 * r + 1);
    const float3 mI = sI / n, mP = sP / n;
    const float3 varI = sII / n - mI * mI;
    const float3 covIP = sIP / n - mI * mP;
    PS_OUTPUT2 o;
    o.a = float4(covIP / (max(varI, 0.0) + epsilon), 0);
    o.b = float4(mP - o.a.rgb * mI, 0);
    return o;
}

#elif (PASS == 4 || PASS == 5)

struct PS_OUTPUT2
{
    float4 a : SV_Target0;
    float4 b : SV_Target1;
};

// tex0 a, tex1 b: the box across (PASS 4) or down (PASS 5), as a mean.
PS_OUTPUT2 main(PS_INPUT input)
{
    const int2 q = int2(input.Pos.xy);
    const int r = (int)radius;
#if (PASS == 4)
    const int2 step = int2(1, 0);
#else
    const int2 step = int2(0, 1);
#endif
    PS_OUTPUT2 o = (PS_OUTPUT2)0;
    for (int k = -r; k <= r; k++) {
        o.a.rgb += At(tex0, q + k * step, workSize).rgb;
        o.b.rgb += At(tex1, q + k * step, workSize).rgb;
    }
    o.a /= (2 * r + 1);
    o.b /= (2 * r + 1);
    return o;
}

#elif (PASS == 6)

// tex0 the picture, tex1 the mean a, tex2 the mean b.
float4 main(PS_INPUT input) : SV_Target
{
    const int2 p = int2(input.Pos.xy);
    const float2 pw = input.Pos.xy * (workSize / fullSize) - 0.5;
    const int2 base = int2(floor(pw));
    const float2 f = pw - base;
    const float3 a = lerp(lerp(At(tex1, base, workSize).rgb, At(tex1, base + int2(1, 0), workSize).rgb, f.x),
                          lerp(At(tex1, base + int2(0, 1), workSize).rgb, At(tex1, base + int2(1, 1), workSize).rgb, f.x), f.y);
    const float3 b = lerp(lerp(At(tex2, base, workSize).rgb, At(tex2, base + int2(1, 0), workSize).rgb, f.x),
                          lerp(At(tex2, base + int2(0, 1), workSize).rgb, At(tex2, base + int2(1, 1), workSize).rgb, f.x), f.y);
    const float4 picture = At(tex0, p, fullSize);
    return float4(a * picture.rgb + b, picture.a);
}

#elif (PASS == 7 || PASS == 8)

// tex0 the texture being reduced, srcSize; this pass writes dstSize. Across, then down.
float4 main(PS_INPUT input) : SV_Target
{
    const int2 p = int2(input.Pos.xy);
#if (PASS == 7)
    const int axis = 0;
#else
    const int axis = 1;
#endif
    const float scale = srcSize[axis] / dstSize[axis];   // greater than one: a reduction
    const float pos = (p[axis] + 0.5) * scale;            // the output pixel's centre, in source pixels
    const float support = filter_support * scale;
    const int low = (int)floor(pos - support);
    const int high = (int)ceil(pos + support);
    float4 sum = 0;
    float wsum = 0;
    for (int n = low; n < high; n++) {
        const float w = filter((n + 0.5 - pos) / scale);
        int2 at = p;
        at[axis] = n;
        sum += w * At(tex0, at, srcSize);
        wsum += w;
    }
    return sum / wsum;
}

#endif
