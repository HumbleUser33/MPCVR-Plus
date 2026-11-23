// --thdrcurve: the HDR curve (Source/HdrCurve.h), the shader that ships against its sum,
// and the sum against Veyra's, which it is taken from.
//
// The filter draws the curve with _bin\shaders\ps_hdr_curve.cso on a PQ BT.2020 picture:
// decoded to nits, one gain for the three channels from the pixel's light, a ceiling on
// the brightest channel, encoded again. Four questions, each a number in the report:
//   - the shader against HdrCurve::Apply() in double, on PQ ramps and random colours, in
//     steps of the half float it writes ("to the last half float", as the other ports);
//   - the shader with nothing to do, against its input, in codes of a 10-bit output: the
//     round trip through light must not move a picture the curve leaves alone;
//   - what the curve is supposed to keep: nothing above the shadows' range moved by the
//     lift, no channel above the cap, every pixel's hue;
//   - the sum against Veyra's own (include/veyra/engine/HdrOutputTuning.h resolveHdrTuning,
//     src/engine/VideoPresenter.cpp and shaders/PresentBlit.hlsl HdrCurveLuminance at commit
//     c663312), written out again below as Veyra has it, for every preset, strength and
//     display peak.

#include "HdrCurve.h"

namespace temporal {

namespace {

constexpr int kCurveW = 1024;
constexpr int kCurveH = 64;

double PqToNits(double pq)
{
	const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	const double p = std::pow(std::clamp(pq, 0.0, 1.0), 1.0 / m2);
	return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double NitsToPq(double nits)
{
	const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
	return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float CurveHalf(float v)
{
	return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(v));
}

// The spacing of half floats at v: what one step of the shader's output is worth there.
double HalfStep(double v)
{
	v = std::abs(v);
	if (v < 6.103515625e-05) {
		return 5.960464477539063e-08;   // the subnormals' spacing
	}
	return std::ldexp(1.0, (int)std::floor(std::log2(v)) - 10);
}

// The picture: grey ramps over the whole PQ range, then random colours, dark ones and
// saturated ones among them. PQ values held as the half floats the filter's textures hold.
std::vector<float> CurvePicture()
{
	std::vector<float> rgba((size_t)kCurveW * kCurveH * 4);
	uint32_t seed = 12345;
	const auto next = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) / 16777216.0;
	};
	for (int y = 0; y < kCurveH; y++) {
		for (int x = 0; x < kCurveW; x++) {
			float* p = &rgba[((size_t)y * kCurveW + x) * 4];
			if (y < 16) {
				p[0] = p[1] = p[2] = (float)x / (kCurveW - 1);
			} else {
				const double level = next();
				const double sat = next();
				for (int c = 0; c < 3; c++) {
					p[c] = (float)(level * (1.0 - sat * next()));
				}
			}
			for (int c = 0; c < 3; c++) {
				p[c] = CurveHalf(p[c]);
			}
			p[3] = 1.0f;
		}
	}
	return rgba;
}

// What the filter's shader should give for one pixel, in double: black left alone, else
// HdrCurve::Apply() on its light.
void CurveExpected(const float* in, const HdrCurve::Constants& c, double out[3])
{
	double rgb[3];
	for (int i = 0; i < 3; i++) {
		rgb[i] = PqToNits(in[i]);
	}
	const double y = HdrCurve::kLumR * rgb[0] + HdrCurve::kLumG * rgb[1] + HdrCurve::kLumB * rgb[2];
	if (y <= 1e-6) {
		for (int i = 0; i < 3; i++) {
			out[i] = in[i];
		}
		return;
	}
	HdrCurve::Apply(rgb, c);
	for (int i = 0; i < 3; i++) {
		out[i] = NitsToPq(rgb[i]);
	}
}

bool CurveOnGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx, CUpscalePasses& passes, ID3D11PixelShader* ps,
                const std::vector<float>& picture, const HdrCurve::Constants& c, std::vector<float>& out)
{
	std::vector<HALF> half(picture.size());
	DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), picture.data(), sizeof(float), picture.size());
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = kCurveW;
	d.Height = kCurveH;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const D3D11_SUBRESOURCE_DATA init = { half.data(), kCurveW * 4 * sizeof(HALF), 0 };
	CComPtr<ID3D11Texture2D> tex;
	CComPtr<ID3D11ShaderResourceView> srv;
	if (FAILED(dev->CreateTexture2D(&d, &init, &tex)) || FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
		return false;
	}
	Target target;
	if (!MakeTarget(dev, kCurveW, kCurveH, false, target, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		return false;
	}
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(HdrCurve::Constants);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	const D3D11_SUBRESOURCE_DATA cbInit = { &c, 0, 0 };
	CComPtr<ID3D11Buffer> cb;
	if (FAILED(dev->CreateBuffer(&bd, &cbInit, &cb))) {
		return false;
	}
	passes.Apply(ctx, ps, srv, target.rtv, kCurveW, kCurveH, cb);

	D3D11_TEXTURE2D_DESC sd = {};
	target.tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, target.tex);
	return ReadRgbaHalf(ctx, stage, kCurveW, kCurveH, out);
}

// ---- Veyra's, as it is written there ------------------------------------------------

struct VeyraCurve {   // HdrCurveSettings, the fields the curve uses
	bool enabled;
	int shadowLiftEv100;
	unsigned shadowRangeNits, midGrayNits, highlightStartNits, rollOffPercent, peakCapNits;
};

// resolveHdrTuning(preset, strength, displayPeak): the display's peak scales the highlight
// numbers, then the strength takes every value towards its neutral one.
VeyraCurve VeyraResolve(const HdrCurve::Values& preset, unsigned strengthPercent, unsigned displayPeakNits)
{
	VeyraCurve curve = { true, preset.liftEv100, (unsigned)preset.rangeNits, (unsigned)preset.midGreyNits,
		(unsigned)preset.startNits, (unsigned)preset.rollOffPercent, (unsigned)preset.capNits };
	if (displayPeakNits > 0) {
		const double scale = double(displayPeakNits) / 1000.0;
		const auto scaleNits = [scale](unsigned v, unsigned lo, unsigned hi) {
			if (v == 0) return 0u;
			const auto scaled = unsigned(std::lround(double(v) * scale));
			return std::clamp(scaled, lo, hi);
		};
		curve.highlightStartNits = scaleNits(curve.highlightStartNits, 100u, 2000u);
		curve.peakCapNits = scaleNits(curve.peakCapNits, 100u, 4000u);
	}
	const double k = double(std::min(200u, strengthPercent)) / 100.0;
	if (k <= 0.001) {
		curve.enabled = false;
		return curve;
	}
	if (k == 1.0) {
		return curve;
	}
	const auto blend = [k](int base, int neutral) {
		return int(std::lround(double(neutral) + double(base - neutral) * k));
	};
	curve.shadowLiftEv100 = std::clamp(blend(curve.shadowLiftEv100, 0), -200, 200);
	curve.shadowRangeNits = static_cast<unsigned>(std::clamp(int(std::lround(double(curve.shadowRangeNits) * k)), 1, 100));
	curve.midGrayNits = static_cast<unsigned>(std::clamp(blend(int(curve.midGrayNits), 203), 100, 500));
	curve.rollOffPercent = static_cast<unsigned>(std::clamp(blend(int(curve.rollOffPercent), 100), 10, 200));
	if (curve.highlightStartNits) {
		curve.highlightStartNits = static_cast<unsigned>(
			std::clamp(int(std::lround(double(curve.highlightStartNits) / k)), 100, 2000));
	}
	return curve;
}

// VideoPresenter's constants and PresentBlit's HdrCurveLuminance, without Dolby Vision's
// per-picture peak, in double.
double VeyraLuminance(double nits, const VeyraCurve& v)
{
	if (!v.enabled) {
		return nits;
	}
	const double c0x = v.shadowLiftEv100 / 100.0, c0y = v.shadowRangeNits, c0z = v.midGrayNits, c0w = v.highlightStartNits;
	const double peak = v.peakCapNits ? double(v.peakCapNits) : 1000.0;
	const double c1x = std::max(1.0, peak - double(v.highlightStartNits)) * double(v.rollOffPercent) / 100.0;
	const double c1y = v.peakCapNits;
	if (c0x != 0.0 && c0y > 0.0) {
		const double w = std::clamp(1.0 - nits / c0y, 0.0, 1.0);
		nits *= 1.0 + w * w * (std::exp2(c0x) - 1.0);
	}
	if (c0z > 0.0 && std::abs(c0z - 203.0) > 0.5) {
		const double pk = c1y > 0.0 ? c1y : 1000.0;
		if (pk > c0z + 1.0) {
			const double g = std::log(c0z / pk) / std::log(203.0 / pk);
			nits = pk * std::pow(std::max(nits / pk, 0.0), g);
		}
	}
	if (c0w > 0.0 && c1x > 0.0 && nits > c0w) nits = c0w + (nits - c0w) / (1.0 + (nits - c0w) / c1x);
	if (c1y > 0.0) nits = std::min(nits, c1y);
	return nits;
}

struct CurveCase {
	const char* name;
	int preset;
	HdrCurve::Values custom;
	int displayNits;
	int strength;
};

} // namespace

int RunHdrCurve(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	Head("The HDR curve");

	CUpscalePasses passes;
	std::string error;
	if (!passes.Init(dev, error)) {
		printf("  passes: %s\n", error.c_str());
		g_failures++;
		return 1;
	}
	CComPtr<ID3D11PixelShader> ps;
	if (!LoadPixelShaderObject(dev, L"..\\..\\_bin\\shaders\\ps_hdr_curve.cso", ps)) {
		printf("  _bin\\shaders\\ps_hdr_curve.cso cannot be loaded: build the filter first\n");
		g_failures++;
		return 1;
	}
	const std::vector<float> picture = CurvePicture();

	// -- the shader against the sum, and with nothing to do against its input ----------
	const CurveCase cases[] = {
		{ "Dark lift, 1000 nits",            HdrCurve::DarkLift,       {}, 1000, 100 },
		{ "Highlight guard, 1000 nits",      HdrCurve::HighlightGuard, {}, 1000, 100 },
		{ "Highlight guard, 600 nits",       HdrCurve::HighlightGuard, {},  600, 100 },
		{ "Dark and bright, 1000 nits",      HdrCurve::DarkAndBright,  {}, 1000, 100 },
		{ "Dark and bright, 50 %",           HdrCurve::DarkAndBright,  {}, 1000,  50 },
		{ "Dark and bright, 200 %",          HdrCurve::DarkAndBright,  {}, 1000, 200 },
		{ "Bright room, 1000 nits",          HdrCurve::BrightRoom,     {}, 1000, 100 },
		{ "Dark room, 1000 nits",            HdrCurve::DarkRoom,       {}, 1000, 100 },
		{ "Punch, 2000 nits",                HdrCurve::Punch,          {}, 2000, 100 },
		{ "Custom, darkened shadows",        HdrCurve::Custom, { -200, 100, 203,    0, 100,    0 }, 1000, 100 },
		{ "Custom, mid grey 500, cap 4000",  HdrCurve::Custom, {    0,  10, 500,    0, 100, 4000 }, 1000, 100 },
		{ "Custom, mid grey 100, soft 10 %", HdrCurve::Custom, {    0,  10, 100,  200,  10,    0 }, 1000, 100 },
	};
	printf("\n  The shader against HdrCurve::Apply(), in steps of the half float it writes\n");
	printf("  %-34s %10s %10s\n", "", "max steps", "over 1");
	for (const CurveCase& t : cases) {
		const HdrCurve::Constants c = HdrCurve::ShaderConstants(HdrCurve::Applied(t.preset, t.custom, t.displayNits, t.strength));
		std::vector<float> out;
		if (!CurveOnGpu(dev, ctx, passes, ps, picture, c, out)) {
			printf("  %-34s could not run\n", t.name);
			g_failures++;
			continue;
		}
		double worst = 0;
		int over = 0;
		for (size_t i = 0; i < (size_t)kCurveW * kCurveH; i++) {
			double expected[3];
			CurveExpected(&picture[i * 4], c, expected);
			for (int k = 0; k < 3; k++) {
				const double steps = std::abs(out[i * 4 + k] - expected[k]) / HalfStep(expected[k]);
				worst = std::max(worst, steps);
				over += steps > 1.0;
			}
		}
		printf("  %-34s %10.2f %10d\n", t.name, worst, over);
		Check(worst <= 3.0, std::format("{}: within 3 half-float steps of the sum", t.name).c_str());
	}

	{
		// The filter does not draw the step with nothing to do; drawn anyway, the round trip
		// through light must give the picture back to its last half float.
		std::vector<float> out;
		const HdrCurve::Constants neutral = HdrCurve::ShaderConstants({});
		double worst = 0;
		if (CurveOnGpu(dev, ctx, passes, ps, picture, neutral, out)) {
			for (size_t i = 0; i < picture.size(); i++) {
				if (i % 4 != 3) {
					worst = std::max(worst, std::abs(out[i] - picture[i]) / HalfStep(picture[i]));
				}
			}
			printf("\n  Nothing to do: the picture moved by at most %.2f half-float steps\n", worst);
			Check(worst <= 1.0, "nothing to do moves the picture by no more than its rounding");
		} else {
			g_failures++;
		}
	}

	// -- what the curve keeps ---------------------------------------------------------
	{
		printf("\n  What it keeps, on the shader's own output\n");
		// The lift alone: nothing at or above its range moves; below it, nothing darkens.
		const HdrCurve::Values liftOnly = { 50, 35, HdrCurve::kMidNeutral, 0, HdrCurve::kRollDef, 0 };
		const HdrCurve::Constants cl = HdrCurve::ShaderConstants(liftOnly);
		// Measured in steps of the half float the picture is held in: one is the rounding of
		// the round trip through light, and anything more is the curve's doing.
		std::vector<float> out;
		double movedAbove = 0;
		int darkened = 0;
		if (CurveOnGpu(dev, ctx, passes, ps, picture, cl, out)) {
			for (size_t i = 0; i < (size_t)kCurveW * kCurveH; i++) {
				const float* in = &picture[i * 4];
				double yIn = 0;
				for (int k = 0; k < 3; k++) {
					const double w = k == 0 ? HdrCurve::kLumR : k == 1 ? HdrCurve::kLumG : HdrCurve::kLumB;
					yIn += w * PqToNits(in[k]);
				}
				for (int k = 0; k < 3; k++) {
					const double steps = (out[i * 4 + k] - in[k]) / HalfStep(in[k]);
					if (yIn >= liftOnly.rangeNits) {
						movedAbove = std::max(movedAbove, std::abs(steps));
					} else if (steps < -1.0) {
						darkened++;
					}
				}
			}
		}
		printf("  %-34s %10.2f\n", "lift: moved above its range, steps", movedAbove);
		printf("  %-34s %10d\n", "lift: darkened below it", darkened);
		Check(movedAbove <= 1.0, "the lift leaves everything at or above its range alone");
		Check(darkened == 0, "the lift darkens nothing");

		// The cap: no channel above it; and every pixel's hue, as the sum-normalized colour.
		const HdrCurve::Constants cg = HdrCurve::ShaderConstants(HdrCurve::Applied(HdrCurve::DarkAndBright, {}, 1000, 100));
		double overCap = 0, hue = 0;
		if (CurveOnGpu(dev, ctx, passes, ps, picture, cg, out)) {
			for (size_t i = 0; i < (size_t)kCurveW * kCurveH; i++) {
				double a[3], b[3], sa = 0, sb = 0;
				for (int k = 0; k < 3; k++) {
					a[k] = PqToNits(picture[i * 4 + k]);
					b[k] = PqToNits(out[i * 4 + k]);
					sa += a[k];
					sb += b[k];
					overCap = std::max(overCap, b[k] / cg.capNits - 1.0);
				}
				if (sa > 1.0 && sb > 1.0) {
					for (int k = 0; k < 3; k++) {
						hue = std::max(hue, std::abs(a[k] / sa - b[k] / sb));
					}
				}
			}
		}
		printf("  %-34s %10.5f\n", "cap: brightest channel over it", std::max(overCap, 0.0));
		printf("  %-34s %10.5f\n", "hue: colour moved, of its share", hue);
		Check(overCap <= 2e-3, "no channel above the cap");
		Check(hue <= 5e-3, "every pixel keeps its hue");
	}

	// -- the sum against Veyra's --------------------------------------------------------
	{
		const unsigned strengths[] = { 0, 1, 25, 50, 75, 100, 125, 150, 200 };
		const unsigned displays[] = { 0, 400, 600, 1000, 1500, 4000 };
		int resolved = 0, resolveDiffer = 0;
		double worstLight = 0;
		for (int preset = HdrCurve::DarkLift; preset < HdrCurve::PresetCount; preset++) {
			for (const unsigned s : strengths) {
				for (const unsigned d : displays) {
					const VeyraCurve v = VeyraResolve(HdrCurve::PresetValues(preset), s, d);
					const HdrCurve::Values ours = HdrCurve::Applied(preset, {}, (int)d, (int)s);
					resolved++;
					if (!v.enabled) {
						resolveDiffer += !HdrCurve::Neutral(HdrCurve::ShaderConstants(ours));
						continue;
					}
					const bool same = ours.liftEv100 == v.shadowLiftEv100 && ours.rangeNits == (int)v.shadowRangeNits
						&& ours.midGreyNits == (int)v.midGrayNits && ours.startNits == (int)v.highlightStartNits
						&& ours.rollOffPercent == (int)v.rollOffPercent && ours.capNits == (int)v.peakCapNits;
					resolveDiffer += !same;
					const HdrCurve::Constants c = HdrCurve::ShaderConstants(ours);
					for (double nits = 0.001; nits <= 10000.0; nits *= 1.05) {
						const double a = HdrCurve::Luminance(nits, c), b = VeyraLuminance(nits, v);
						worstLight = std::max(worstLight, std::abs(a - b) / std::max(b, 1e-9));
					}
				}
			}
		}
		printf("\n  Against Veyra's own sums (resolveHdrTuning, HdrCurveLuminance)\n");
		printf("  %-34s %10d\n", "presets x strengths x displays", resolved);
		printf("  %-34s %10d\n", "resolved to other values", resolveDiffer);
		printf("  %-34s %10.2e\n", "light, largest relative difference", worstLight);
		Check(resolveDiffer == 0, "every preset, strength and display resolves to Veyra's values");
		Check(worstLight <= 1e-5, "the curve gives Veyra's light, to float precision");
	}

	return g_failures ? 1 : 0;
}

} // namespace temporal
