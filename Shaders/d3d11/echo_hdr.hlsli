// SPDX-License-Identifier: MIT
// From Echo-Storm/ls-addon-manager, revision 8d3eac351d42480326290b0f8965127a630b44f2
// (engine/hdr_hlsl.h), Copyright (c) 2026 Echo-Storm and andreiday; the licence is
// Source/FrameGen/echo/EchoStorm-LICENSE.txt. The part the motion passes use (ToSdr),
// taken as it is out of the C++ string it was kept in.
//
// HDR frames as EchoStorm's passes see them. Its matching thresholds are made for
// display-referred pictures, 0..1 and sRGB-encoded: an HDR frame is matched in its SDR
// view, light relative to the SDR white, rolled off above kKnee, then sRGB encoded.
// Encoding 0 is SDR (the identity), 1 scRGB, 2 HDR10 (PQ, Rec.2020).

static const float kKnee = 0.75;
// Above the knee a logarithmic roll-off: 1.0 is kKnee + kLogTop times the SDR white (10000 nits at 80), and kLogA makes its slope 1 at the knee.
static const float kLogA = 0.03, kLogTop = 125.0;
static const float kLogSpan = log(1.0 + kLogTop / kLogA);
static const float3x3 kRec2020To709 = { 1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187 };
float3 LinearToSrgb(float3 l) { l = saturate(l); return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }
float3 PqToNits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float3 Compress(float3 x) { const float3 over = max(x - kKnee, 0.0); return min(x, kKnee) + (1.0 - kKnee) * log(1.0 + over / kLogA) / kLogSpan; }
// encoding: 0 SDR, 1 scRGB, 2 HDR10 (PQ); white: the SDR white in nits
float3 ToLight(float3 c, uint encoding, float white) {   // an HDR frame as light, Rec.709, 1 = the SDR white
    return encoding == 1u ? c * (80.0 / white) : mul(kRec2020To709, PqToNits(c)) / white;
}
float3 ToSdr(float3 c, uint encoding, float white) {
    if (encoding == 0u) return c;
    return LinearToSrgb(Compress(max(ToLight(c, encoding, white), 0.0)));
}
// And back to PQ, for the guard after frame generation (cs_echo_guard.hlsl).
float3 NitsToPq(float3 n) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(saturate(n / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
