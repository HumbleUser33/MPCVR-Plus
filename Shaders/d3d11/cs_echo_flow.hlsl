// SPDX-License-Identifier: MIT
// From Echo-Storm/ls-addon-manager, revision 8d3eac351d42480326290b0f8965127a630b44f2
// (engine/flow_estimator.cpp), Copyright (c) 2026 Echo-Storm and andreiday; the licence is
// Source/FrameGen/echo/EchoStorm-LICENSE.txt.
//
// EchoStorm's motion estimator, the passes CEchoFlow (Source/FrameGen/echo/EchoFlow.cpp)
// runs on the Direct3D 11 device. What is changed from the original:
// - one file, a pass for each PASS (0 Luma, 1 Down, 2 Global, 3 Search, 4 Median, 5 Pixel),
//   compiled with the filter (Shaders/compile_shaders.cmd) where EchoStorm compiles them
//   when it starts;
// - the statistics it kept in u7 are left out: the renderer reads none of them;
// - the search compares blocks by their plain difference only. The "shape" cost
//   (meanWeight, gradWeight) is a variant EchoStorm itself does not use by default.
//
// Each frame: its brightness, read at 1 to 4 times the analysis size, and at halved sizes
// down to about 64 pixels wide (a pyramid, kept for the next frame). Then, from the
// smallest size up to half size, every 4x4 block finds where its 8x8 neighbourhood was in
// the frame before: the guess from the size below (and its neighbours) is tried first,
// then the positions around the best of them, with a small cost for straying from the
// guess so flat areas follow their surroundings instead of noise. At half size the answer
// is refined to a fraction of a pixel. A 3x3 vector median removes the odd wrong block.
// Last, at the analysis size, every pixel picks, from its own block's vector, the three
// nearest blocks' vectors and "not moving", the one under which its 3x3 surroundings
// match best. The vectors point from a pixel of this frame to where it was in the frame
// before, in pixels of the analysis size; beside them, a mask of where they cannot be
// trusted (0 trusted .. 1 not).

SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 size; uint2 grid; uint2 coarse; uint radius; uint flags; float lambda; float bias; float stability; float strayCap; float fastMotion; float meanWeight; float gradWeight; uint pad2; };

#if PASS == 0
// the frame's brightness at the analysis size. An HDR frame (radius: its encoding,
// lambda: the SDR white in nits) in its SDR view first, which the matching's thresholds
// are made for
#include "echo_hdr.hlsli"
Texture2D<float4> tFrame : register(t0);
RWTexture2D<float> uLuma : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    if (pad2 <= 1u) { uLuma[id.xy] = dot(ToSdr(tFrame.Load(int3(id.xy, 0)).rgb, radius, lambda), float3(0.299, 0.587, 0.114)); return; }
    // the frame read at pad2 times this size: the mean of each pad2 x pad2 block
    uint fw, fh; tFrame.GetDimensions(fw, fh);
    float s = 0;
    for (uint j = 0; j < pad2; ++j) for (uint i = 0; i < pad2; ++i)
        s += dot(ToSdr(tFrame.Load(int3(min(id.xy * pad2 + uint2(i, j), uint2(fw, fh) - 1), 0)).rgb, radius, lambda), float3(0.299, 0.587, 0.114));
    uLuma[id.xy] = s / float(pad2 * pad2);
}

#elif PASS == 1
// half size: the average of each 2x2 (grid holds the larger size)
Texture2D<float> tSrc : register(t0);
RWTexture2D<float> uDst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const int2 p = int2(id.xy) * 2, last = int2(grid) - 1;
    uDst[id.xy] = 0.25 * (tSrc.Load(int3(min(p, last), 0)) + tSrc.Load(int3(min(p + int2(1, 0), last), 0)) +
                          tSrc.Load(int3(min(p + int2(0, 1), last), 0)) + tSrc.Load(int3(min(p + int2(1, 1), last), 0)));
}

#elif PASS == 2
// The whole picture's shift (a camera turn): for every shift within +-radius at the smallest size, the average difference between this frame
// and the frame before over half the pixels they share (a checkerboard); a shift that leaves them sharing too little costs the most. The
// search's first size takes the cheapest as a guess beside "not moving", so a turn faster than that size's own search is still followed.
Texture2D<float> tCur : register(t0);
Texture2D<float> tPrev : register(t1);
RWTexture2D<float> uCost : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int n = 2 * int(radius) + 1;
    if (int(id.x) >= n || int(id.y) >= n) return;
    const int2 v = int2(id.xy) - int(radius), lo = max(-v, int2(0, 0)), hi = min(int2(size), int2(size) - v);
    float s = 0; uint count = 0;
    [loop] for (int y = lo.y; y < hi.y; ++y) {
        [loop] for (int x = lo.x + ((lo.x + y) & 1); x < hi.x; x += 2) { s += abs(tCur.Load(int3(x, y, 0)) - tPrev.Load(int3(x + v.x, y + v.y, 0))); ++count; }
    }
    uCost[id.xy] = count * 8 >= size.x * size.y ? s / count : 1e3;   // at least a quarter of the picture shared
}

#elif PASS == 3
// One size of the search: every 4x4 block, where its 8x8 surroundings were in the frame before. flags: 1 a coarser grid exists, 2 this is
// the finest size (a fraction of a pixel), 4 the whole picture's shift is known (the smallest size; coarse.x: its radius).
// Vectors in this size's pixels.
Texture2D<float> tCur : register(t0);
Texture2D<float> tPrev : register(t1);
Texture2D<float2> tCoarse : register(t2);
Texture2D<float> tGlobal : register(t3);
RWTexture2D<float2> uGrid : register(u0);
static const float kExact = 0.0005, kShallow = 0.002;   // an exact match (brightness 0..1, per pixel); the least rise either side to refine on
static float cw[64];
static int2 origin;
// The cost of straying from the coarser size's guess, capped at strayCap pixels (of this size): it settles near-ties (flat areas follow their
// surroundings), but never holds a block to a guess that is plainly wrong for it.
float Stray(float2 d) { return lambda * min(length(d), strayCap); }
float Sad(int2 v) {   // all 64 pixels of the 8x8 (the final answer and its fraction of a pixel)
    const int2 last = int2(size) - 1;
    float s = 0;
    [loop] for (int j = 0; j < 8; ++j) {
        [unroll] for (int i = 0; i < 8; ++i) s += abs(cw[j * 8 + i] - tPrev.Load(int3(clamp(origin + int2(i, j) + v, int2(0, 0), last), 0)));
    }
    return s * (1.0 / 64.0);
}
float SadHalf(int2 v) {   // half of them, as a checkerboard (the search: half the reads, with no stripe the pattern could hide in)
    const int2 last = int2(size) - 1;
    float s = 0;
    [loop] for (int j = 0; j < 8; ++j) {
        [unroll] for (int i = (j & 1); i < 8; i += 2) s += abs(cw[j * 8 + i] - tPrev.Load(int3(clamp(origin + int2(i, j) + v, int2(0, 0), last), 0)));
    }
    return s * (1.0 / 32.0);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= grid.x || id.y >= grid.y) return;
    const int2 last = int2(size) - 1;
    origin = int2(id.xy) * 4 - 2;
    [unroll] for (int j = 0; j < 8; ++j) [unroll] for (int i = 0; i < 8; ++i) cw[j * 8 + i] = tCur.Load(int3(clamp(origin + int2(i, j), int2(0, 0), last), 0));

    float2 predicted = float2(0, 0);
    int2 best = int2(0, 0);
    float bestCost = SadHalf(best);
    if (flags & 1) {   // the coarser size's answers for this block and its neighbours, doubled
        const int2 c = min(int2((id.xy * 2 + 1) >> 2), int2(coarse) - 1);
        predicted = tCoarse.Load(int3(c, 0)) * 2.0;
        bestCost += Stray(predicted);   // "not moving" strays from the guess too
        const int2 seeds[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll] for (int s = 0; s < 5; ++s) {
            const int2 v = int2(round(tCoarse.Load(int3(clamp(c + seeds[s], int2(0, 0), int2(coarse) - 1), 0)) * 2.0));
            const float cost = SadHalf(v) + Stray(float2(v) - predicted);
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
    } else if (flags & 4) {   // the whole picture's shift, the guess (a block moving with the camera needs no search of its own)
        const int gr = int(coarse.x), n = 2 * gr + 1;
        // "not moving" unless a shift is clearly better (a still picture of a repeating pattern matches as well at many shifts)
        const float still = tGlobal.Load(int3(gr, gr, 0));
        float g = still * 0.9 - 0.0005; int2 shift = int2(0, 0);
        [loop] for (int j = 0; j < n; ++j) [loop] for (int i = 0; i < n; ++i) { const float c = tGlobal.Load(int3(i, j, 0)); if (c < g) { g = c; shift = int2(i, j) - gr; } }
        if (any(shift != int2(0, 0))) {
            predicted = float2(shift);
            bestCost += Stray(predicted);
            const float cost = SadHalf(shift);
            if (cost < bestCost) { bestCost = cost; best = shift; }
        }
    }
    const int2 center = best;
    const int r = int(radius);
    const float seedCost = bestCost - Stray(float2(best) - predicted);
    if (seedCost > kExact) {   // a guess that already matches exactly (a still area, mostly) needs no search around it
        [loop] for (int dy = -r; dy <= r; ++dy) {
            [loop] for (int dx = -r; dx <= r; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int2 v = center + int2(dx, dy);
                const float cost = SadHalf(v) + Stray(float2(v) - predicted);
                if (cost < bestCost) { bestCost = cost; best = v; }
            }
        }
    }
    float2 result = float2(best);
    if (flags & 2) {
        // A fraction of a pixel: the costs of summed differences rise in a V around the true position, so two lines of equal slope through
        // the costs either side (not a parabola, which pulls toward whole pixels). Not where the match is already exact (a still picture
        // must give exactly zero), nor where the V is too shallow.
        const float c0 = Sad(best), xm = Sad(best - int2(1, 0)), xp = Sad(best + int2(1, 0)), ym = Sad(best - int2(0, 1)), yp = Sad(best + int2(0, 1));
        const float sx = max(xm, xp) - c0, sy = max(ym, yp) - c0;
        if (c0 > kExact && sx > kShallow) result.x += clamp(0.5 * (xm - xp) / sx, -0.5, 0.5);
        if (c0 > kExact && sy > kShallow) result.y += clamp(0.5 * (ym - yp) / sy, -0.5, 0.5);
    }
    uGrid[id.xy] = result;
}

#elif PASS == 4
// the 3x3 vector median: the neighbour closest (in sum) to all the others
Texture2D<float2> tGrid : register(t0);
RWTexture2D<float2> uOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    float2 v[9];
    [unroll] for (int k = 0; k < 9; ++k) v[k] = tGrid.Load(int3(clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(size) - 1), 0));
    float bestSum = 1e30; float2 best = v[4];
    [unroll] for (int a = 0; a < 9; ++a) {
        float s = 0;
        [unroll] for (int b = 0; b < 9; ++b) s += abs(v[a].x - v[b].x) + abs(v[a].y - v[b].y);
        if (s < bestSum) { bestSum = s; best = v[a]; }
    }
    uOut[id.xy] = best;
}

#elif PASS == 5
// Every pixel at the analysis size: "not moving", its own block's vector and the three nearest blocks'; the one under which its 3x3 surroundings
// match best. "Not moving" wins a tie (a caption or text that stays put must not slide), and another block's vector must beat the own block's by
// `bias`. flags: 1 there is a frame before (else zero). The distrust mask rises from 0 to 1 as even the best match's difference goes from
// kTrusted to kUntrusted (brightness 0..1, averaged over the 3x3).
// stability (0..1) moves that judgement from the pixel's 3x3 to the average brightness of its 5x5 surroundings (and widens the thresholds):
// shimmer moves light around inside the surroundings and leaves their average about the same, while something newly uncovered changes it.
Texture2D<float> tCur : register(t0);
Texture2D<float> tPrev : register(t1);
Texture2D<float2> tGrid : register(t2);
RWTexture2D<float2> uMotion : register(u0);
RWTexture2D<float> uDistrust : register(u1);
static const float kTrusted = 0.03, kUntrusted = 0.10;
void Pixel(uint2 id) {
    if (!(flags & 1)) { uMotion[id.xy] = float2(0, 0); uDistrust[id.xy] = 0; return; }
    const int2 p = int2(id.xy), last = int2(size) - 1, gl = int2(grid) - 1;
    const int2 cell = min(p >> 3, gl);
    const int2 q = int2((p.x & 7) < 4 ? -1 : 1, (p.y & 7) < 4 ? -1 : 1);
    float2 cand[5];
    cand[0] = float2(0, 0);
    cand[1] = tGrid.Load(int3(cell, 0)) * 2.0;
    cand[2] = tGrid.Load(int3(clamp(cell + int2(q.x, 0), int2(0, 0), gl), 0)) * 2.0;
    cand[3] = tGrid.Load(int3(clamp(cell + int2(0, q.y), int2(0, 0), gl), 0)) * 2.0;
    cand[4] = tGrid.Load(int3(clamp(cell + q, int2(0, 0), gl), 0)) * 2.0;
    float c[9];
    [unroll] for (int k = 0; k < 9; ++k) c[k] = tCur.Load(int3(clamp(p + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), last), 0));
    const float2 inv = 1.0 / float2(size);
    float bestCost = 1e30; float2 best = cand[0]; float bestPrev = 0;
    [unroll] for (int n = 0; n < 5; ++n) {
        // a vector already tried (neighbours moving alike, which is most of a moving picture; or the own block not moving) is not tried again
        if (n == 1 && all(abs(cand[1]) < 0.05)) continue;
        if (n >= 2 && (all(abs(cand[n] - cand[1]) < 0.05) || all(abs(cand[n] - cand[max(n - 1, 1)]) < 0.05) || all(abs(cand[n]) < 0.05))) continue;
        float s = n <= 1 ? 0.0 : bias, sp = 0;
        [unroll] for (int k = 0; k < 9; ++k) {
            const float prev = tPrev.SampleLevel(sLinear, (float2(p + int2(k % 3 - 1, k / 3 - 1)) + 0.5 + cand[n]) * inv, 0);
            s += abs(c[k] - prev) * (1.0 / 9.0); sp += prev;
        }
        if (s < bestCost) { bestCost = s; best = cand[n]; bestPrev = sp; }
    }
    uMotion[id.xy] = best;
    float cost = bestCost;
    if (stability > 0.001) {   // the 5x5 around: the 3x3 plus four bilinear taps, each the mean of a 2x2 in a corner
        float sc = 0;
        [unroll] for (int k1 = 0; k1 < 9; ++k1) sc += c[k1];
        float tc = 0, tp = 0;
        [unroll] for (int k2 = 0; k2 < 4; ++k2) {
            const float2 at = float2(p) + 0.5 + float2(k2 & 1 ? 1.5 : -1.5, k2 & 2 ? 1.5 : -1.5);
            tc += tCur.SampleLevel(sLinear, at * inv, 0); tp += tPrev.SampleLevel(sLinear, (at + best) * inv, 0);
        }
        const float around = abs((sc + 4.0 * tc) - (bestPrev + 4.0 * tp)) * (1.0 / 25.0);
        cost = lerp(bestCost, around, stability) / (1.0 + stability);
    }
    float distrust = saturate((cost - kTrusted) / (kUntrusted - kTrusted));
    // A thin line that moves (a wire swaying, a branch): a ridge one pixel wide, brighter or darker than both its neighbours across it.
    if (stability > 0.001 && dot(best, best) > 0.0625) {
        const float ridgeX = min(c[4] - max(c[3], c[5]), 1.0) , valleyX = min(min(c[3], c[5]) - c[4], 1.0);
        const float ridgeY = min(c[4] - max(c[1], c[7]), 1.0), valleyY = min(min(c[1], c[7]) - c[4], 1.0);
        const float thin = max(max(ridgeX, valleyX), max(ridgeY, valleyY));
        distrust = max(distrust, saturate((thin - 0.04) / 0.08));
    }
    // Fast motion (fastMotion > 0: from that many pixels, fully at twice it)
    if (fastMotion > 0.0) distrust = max(distrust, saturate((length(best) - fastMotion) / fastMotion));
    uDistrust[id.xy] = distrust;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x < size.x && id.y < size.y) Pixel(id.xy);
}
#endif
