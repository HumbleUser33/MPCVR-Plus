// --thdrtune: the HDR curve's tune (Source/HdrCurveTune.h), its measurement and its sums.
//
// The filter tunes the curve on a few pictures of the film: each measured on the GPU by
// _bin\shaders\cs_hdr_tune_hist.cso into two histograms over PQ (the light and the brightest
// channel), turned into percentiles on the CPU, and the values solved from them. Questions:
//   - the shader that ships against the same histograms made on the CPU from the same half
//     floats: the bins may only differ where float and double round a value either side of a
//     bin's edge;
//   - Measure()'s percentiles against the exact ones of the pixels the histograms were made
//     of, within half a bin;
//   - Tune() on films made up for it: one already where the targets are is left alone; a
//     4000-nit one on an 800-nit display gets BT.2390's knee and its peak lands under the cap;
//     the bounds hold (lift one EV at most and never down, the mid grey's power 0.9 to 1.1);
//     the curve stays monotonic; a film tuned once and measured again needs little more.

#include <random>
#include "HdrCurveTune.h"

namespace temporal {

namespace {

constexpr int kTuneW = 1024;
constexpr int kTuneH = 512;

// The made-up films are judged against targets of their own, so that what is checked is the
// tune and not where the masters put the targets.
constexpr HdrCurveTune::Targets kTestTargets = { 2.36, 0.46, 2.35, 0.71 };

// A picture's pixels, BT.2020 in nits: the light log-normal around `median` with a spread of
// `sigma` (in ln), `blackShare` of them black, and the colours scattered around grey so that
// the brightest channel is above the light, as it is in a film.
std::vector<double> TunePixels(double median, double sigma, double blackShare, unsigned seed, int count, double ceiling = 9000.0)
{
	std::mt19937 rng(seed);
	std::normal_distribution<double> n(0.0, 1.0);
	std::uniform_real_distribution<double> u(0.0, 1.0);
	std::vector<double> rgb((size_t)count * 3);
	for (int i = 0; i < count; i++) {
		double* p = &rgb[(size_t)i * 3];
		if (u(rng) < blackShare) {
			p[0] = p[1] = p[2] = 0.0;
			continue;
		}
		const double y = std::min(median * std::exp(sigma * n(rng)), 9000.0);
		// a colour: each channel off grey by up to a third, the light kept
		double c[3] = { 1.0 + 0.33 * (u(rng) - 0.5) * 2, 1.0 + 0.33 * (u(rng) - 0.5) * 2, 1.0 + 0.33 * (u(rng) - 0.5) * 2 };
		const double l = HdrCurve::kLumR * c[0] + HdrCurve::kLumG * c[1] + HdrCurve::kLumB * c[2];
		for (int k = 0; k < 3; k++) {
			p[k] = std::min(y * c[k] / l, ceiling);
		}
	}
	return rgb;
}

struct TuneHist {
	std::vector<unsigned> light = std::vector<unsigned>(HdrCurveTune::kBins);
	std::vector<unsigned> top = std::vector<unsigned>(HdrCurveTune::kBins);
};

int TuneBin(double pq)
{
	return std::min((int)(std::clamp(pq, 0.0, 1.0) * (HdrCurveTune::kBins - 1) + 0.5), HdrCurveTune::kBins - 1);
}

// The histograms as the shader makes them, from pixels in nits.
TuneHist HistOfNits(const std::vector<double>& rgb)
{
	TuneHist h;
	for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
		const double y = HdrCurve::kLumR * rgb[i] + HdrCurve::kLumG * rgb[i + 1] + HdrCurve::kLumB * rgb[i + 2];
		h.light[TuneBin(HdrCurveTune::NitsToPq(y))]++;
		const double top = std::max({ rgb[i], rgb[i + 1], rgb[i + 2] });
		h.top[TuneBin(HdrCurveTune::NitsToPq(top))]++;
	}
	return h;
}

HdrCurveTune::Stats StatsOf(const std::vector<double>& rgb, const HdrCurveTune::Targets& t = kTestTargets)
{
	const TuneHist h = HistOfNits(rgb);
	return HdrCurveTune::Measure(h.light.data(), h.top.data(), t);
}

// The pixels through a curve: what the film would be measured as once tuned.
std::vector<double> Through(std::vector<double> rgb, const HdrCurve::Values& v)
{
	const HdrCurve::Constants c = HdrCurve::ShaderConstants(v);
	for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
		HdrCurve::Apply(&rgb[i], c);
	}
	return rgb;
}

// A made-up film: its pictures' medians, one spread, one black share, and a few bright
// pixels at `peak` in each so that its highlights are known.
struct MadeFilm {
	std::vector<std::vector<double>> pictures;
};

MadeFilm MakeFilm(const std::vector<double>& medians, double sigma, double peak, unsigned seed)
{
	MadeFilm f;
	const int count = 64 * 1024;
	for (size_t i = 0; i < medians.size(); i++) {
		std::vector<double> p = TunePixels(medians[i], sigma, 0.08, seed + (unsigned)i, count, peak * 0.95);
		// one pixel in 500 at the film's peak, grey: above kPeakShare, so it is the peak
		for (int k = 0; k < count; k += 500) {
			p[(size_t)k * 3] = p[(size_t)k * 3 + 1] = p[(size_t)k * 3 + 2] = peak;
		}
		f.pictures.push_back(std::move(p));
	}
	return f;
}

std::vector<HdrCurveTune::Stats> StatsOfFilm(const MadeFilm& f, const HdrCurve::Values* through = nullptr)
{
	std::vector<HdrCurveTune::Stats> s;
	for (const auto& p : f.pictures) {
		s.push_back(StatsOf(through ? Through(p, *through) : p));
	}
	return s;
}

void PrintTuned(const char* name, const HdrCurveTune::Tuned& t)
{
	const HdrCurveTune::Film& f = t.film;
	printf("  %-30s %2d pictures, %d lit (%d dark, %d normal, %d bright)\n", name, f.used, f.lit, f.dark, f.normal, f.bright);
	printf("  %-30s depth %.2f -> %.2f, key %.2f -> %.2f nits, contrast %.2f -> %.2f, peak %.0f -> %.0f%s\n", "",
		f.depth, t.depthAfter, f.key, t.keyAfter, f.contrast, t.contrastAfter, f.peak, t.peakAfter, t.guarded ? ", guarded" : "");
	printf("  %-30s %S\n", "", HdrCurveTune::Describe(t.values).c_str());
}

bool LoadComputeShaderCso(ID3D11Device* dev, const wchar_t* path, CComPtr<ID3D11ComputeShader>& cs)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path, L"rb") || !f) {
		return false;
	}
	std::vector<char> code;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		code.insert(code.end(), buf, buf + n);
	}
	fclose(f);
	return !code.empty() && SUCCEEDED(dev->CreateComputeShader(code.data(), code.size(), nullptr, &cs));
}

// The shader on a picture of PQ half floats, the video rectangle inside a larger texture as
// the filter has it.
bool TuneHistOnGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, const std::vector<HALF>& pq,
                   int w, int h, RECT rect, std::vector<unsigned>& out)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const D3D11_SUBRESOURCE_DATA init = { pq.data(), (UINT)w * 4 * sizeof(HALF), 0 };
	CComPtr<ID3D11Texture2D> tex;
	CComPtr<ID3D11ShaderResourceView> srv;
	if (FAILED(dev->CreateTexture2D(&d, &init, &tex)) || FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
		return false;
	}
	const UINT bins = 2 * HdrCurveTune::kBins;
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = bins * sizeof(UINT);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = sizeof(UINT);
	CComPtr<ID3D11Buffer> buf;
	CComPtr<ID3D11UnorderedAccessView> uav;
	if (FAILED(dev->CreateBuffer(&bd, nullptr, &buf))) {
		return false;
	}
	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
	ud.Format = DXGI_FORMAT_UNKNOWN;
	ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	ud.Buffer.NumElements = bins;
	if (FAILED(dev->CreateUnorderedAccessView(buf, &ud, &uav))) {
		return false;
	}
	const UINT constants[4] = { (UINT)rect.left, (UINT)rect.top, (UINT)(rect.right - rect.left), (UINT)(rect.bottom - rect.top) };
	D3D11_BUFFER_DESC cd = {};
	cd.ByteWidth = sizeof(constants);
	cd.Usage = D3D11_USAGE_DEFAULT;
	cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	const D3D11_SUBRESOURCE_DATA cinit = { constants, 0, 0 };
	CComPtr<ID3D11Buffer> cb;
	if (FAILED(dev->CreateBuffer(&cd, &cinit, &cb))) {
		return false;
	}
	const UINT zeros[4] = {};
	ctx->ClearUnorderedAccessViewUint(uav, zeros);
	ctx->CSSetShader(cs, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &srv.p);
	ctx->CSSetConstantBuffers(0, 1, &cb.p);
	ctx->CSSetUnorderedAccessViews(0, 1, &uav.p, nullptr);
	ctx->Dispatch((constants[2] + 31) / 32, (constants[3] + 31) / 32, 1);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ctx->CSSetShaderResources(0, 1, &nullSrv);
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);

	bd.Usage = D3D11_USAGE_STAGING;
	bd.BindFlags = 0;
	bd.MiscFlags = 0;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	CComPtr<ID3D11Buffer> stage;
	if (FAILED(dev->CreateBuffer(&bd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, buf);
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
		return false;
	}
	out.assign((const unsigned*)m.pData, (const unsigned*)m.pData + bins);
	ctx->Unmap(stage, 0);
	return true;
}

} // namespace

int RunHdrTune(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	Head("The HDR curve's tune");

	// -- the shader that ships, against the CPU's bins of the same half floats ----------------
	CComPtr<ID3D11ComputeShader> cs;
	if (!LoadComputeShaderCso(dev, L"..\\..\\_bin\\shaders\\cs_hdr_tune_hist.cso", cs)) {
		printf("  _bin\\shaders\\cs_hdr_tune_hist.cso cannot be loaded: build the filter first\n");
		g_failures++;
	} else {
		// The video in a larger texture, as the filter's picture sits in its window: the
		// pixels outside it set to the brightest white, so that one read is seen at once.
		const int w = kTuneW, h = kTuneH;
		const RECT rect = { 37, 21, w - 50, h - 33 };
		std::vector<double> nits = TunePixels(20.0, 2.2, 0.1, 7, w * h);
		std::vector<HALF> pq((size_t)w * h * 4);
		TuneHist cpu;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const size_t i = (size_t)y * w + x;
				const bool inside = x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
				float e[3];
				for (int k = 0; k < 3; k++) {
					e[k] = inside ? (float)HdrCurveTune::NitsToPq(nits[i * 3 + k]) : 1.0f;
					pq[i * 4 + k] = DirectX::PackedVector::XMConvertFloatToHalf(e[k]);
					e[k] = DirectX::PackedVector::XMConvertHalfToFloat(pq[i * 4 + k]);
				}
				pq[i * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
				if (inside) {
					const double r = HdrCurveTune::PqToNits(e[0]), g = HdrCurveTune::PqToNits(e[1]), b = HdrCurveTune::PqToNits(e[2]);
					cpu.light[TuneBin(HdrCurveTune::NitsToPq(HdrCurve::kLumR * r + HdrCurve::kLumG * g + HdrCurve::kLumB * b))]++;
					cpu.top[TuneBin(std::max({ (double)e[0], (double)e[1], (double)e[2] }))]++;
				}
			}
		}
		std::vector<unsigned> gpu;
		if (!TuneHistOnGpu(dev, ctx, cs, pq, w, h, rect, gpu)) {
			printf("  the shader could not run\n");
			g_failures++;
		} else {
			const double pixels = (double)(rect.right - rect.left) * (rect.bottom - rect.top);
			for (int which = 0; which < 2; which++) {
				const unsigned* g = gpu.data() + which * HdrCurveTune::kBins;
				const std::vector<unsigned>& c = which ? cpu.top : cpu.light;
				double total = 0, moved = 0, apart = 0;
				for (int i = 0; i < HdrCurveTune::kBins; i++) {
					total += g[i];
					moved += std::abs((double)g[i] - c[i]);
				}
				// a pixel in the wrong bin counts twice in `moved`: once where it is, once where
				// it is missing; a pixel two bins off would leave a gap the cumulative sums show
				for (int i = 0, gs = 0, cs2 = 0; i < HdrCurveTune::kBins; i++) {
					gs += g[i];
					cs2 += c[i];
					apart = std::max(apart, (double)std::abs(gs - cs2));
				}
				printf("  %-10s %9.0f pixels counted of %.0f, %.0f in another bin (%.4f %%), the cumulative sums %.0f apart at most\n",
					which ? "brightest" : "light", total, pixels, moved / 2, 100.0 * moved / 2 / pixels, apart);
				Check(total == pixels, std::format("{}: every pixel of the video counted once, none outside it", which ? "brightest" : "light").c_str());
				// The light is decoded and encoded again in float on the GPU and in double here:
				// a value on a bin's edge goes either side. What the tune reads must not move.
				Check(moved / 2 <= pixels * 2e-3 && apart <= moved / 2,
					std::format("{}: at most 0.2 % of the pixels in a neighbouring bin, none further", which ? "brightest" : "light").c_str());
			}
			// The percentiles the tune reads, from the GPU's histograms and from the CPU's.
			const HdrCurveTune::Stats sg = HdrCurveTune::Measure(gpu.data(), gpu.data() + HdrCurveTune::kBins, kTestTargets);
			const HdrCurveTune::Stats sc = HdrCurveTune::Measure(cpu.light.data(), cpu.top.data(), kTestTargets);
			const double g7[] = { sg.p05, sg.p10, sg.p25, sg.p50, sg.p75, sg.p90, sg.p99, sg.peak };
			const double c7[] = { sc.p05, sc.p10, sc.p25, sc.p50, sc.p75, sc.p90, sc.p99, sc.peak };
			double worst = 0;
			for (int i = 0; i < 8; i++) {
				worst = std::max(worst, std::abs(HdrCurveTune::NitsToPq(g7[i]) - HdrCurveTune::NitsToPq(c7[i])) * (HdrCurveTune::kBins - 1));
			}
			printf("  the percentiles and the peak from the GPU's histograms within %.3f bin of the CPU's\n", worst);
			Check(worst <= 0.05, "the percentiles and the peak the tune reads: the same from the GPU's histograms as from the CPU's");
		}
	}

	// -- Measure() against the exact percentiles -------------------------------------------------
	{
		const std::vector<double> rgb = TunePixels(12.0, 2.0, 0.2, 11, 400000);
		const HdrCurveTune::Stats s = StatsOf(rgb);
		// the exact ones, of the pixels Measure() counts as lit: those in bins whose centre is lit
		std::vector<double> counted;
		for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
			const double y = HdrCurve::kLumR * rgb[i] + HdrCurve::kLumG * rgb[i + 1] + HdrCurve::kLumB * rgb[i + 2];
			if (HdrCurveTune::PqToNits(HdrCurveTune::BinPq(TuneBin(HdrCurveTune::NitsToPq(y)))) >= HdrCurveTune::kBlackNits) {
				counted.push_back(y);
			}
		}
		std::sort(counted.begin(), counted.end());
		const auto Exact = [&](double q) { return counted[std::min(counted.size() - 1, (size_t)(q * counted.size()))]; };
		const double qs[] = { 0.05, 0.10, 0.25, 0.50, 0.75, 0.90, 0.99 };
		const double got[] = { s.p05, s.p10, s.p25, s.p50, s.p75, s.p90, s.p99 };
		double worst = 0;
		for (int i = 0; i < 7; i++) {
			worst = std::max(worst, std::abs(HdrCurveTune::NitsToPq(got[i]) - HdrCurveTune::NitsToPq(Exact(qs[i]))) * (HdrCurveTune::kBins - 1));
		}
		printf("\n  Measure(): black %.3f (made 0.200), p10 %.3f, median %.2f, p90 %.1f nits; percentiles within %.2f bin of the exact ones\n",
			s.black, s.p10, s.p50, s.p90, worst);
		Check(std::abs(s.black - 0.2) < 0.01, "Measure(): the black share is the one made");
		Check(worst <= 0.5, "Measure(): every percentile within half a bin of the exact one");
	}

	// -- Tune() on films made up for it ---------------------------------------------------------
	printf("\n  Tune() on made-up films, against targets of their own: depth %.2f, key %.2f nits\n",
		kTestTargets.depth, kTestTargets.key);
	const auto Bounds = [](const char* name, const HdrCurveTune::Tuned& t) {
		const HdrCurve::Constants c = HdrCurve::ShaderConstants(t.values);
		bool monotonic = true;
		double last = -1;
		for (double nits = 0; nits <= 10000; nits += nits < 10 ? 0.01 : nits < 1000 ? 0.5 : 5) {
			const double l = HdrCurve::Luminance(nits, c);
			monotonic &= l >= last - 1e-9;
			last = l;
		}
		Check(monotonic, std::format("{}: the curve never goes down", name).c_str());
		Check(t.values.liftEv100 >= 0 && t.values.liftEv100 <= HdrCurveTune::kLiftLimit, std::format("{}: the lift within 0 .. +1 EV", name).c_str());
		Check(c.midGamma >= HdrCurveTune::kGammaMin - 0.01 && c.midGamma <= HdrCurveTune::kGammaMax + 0.01,
			std::format("{}: the mid grey's power within 0.9 .. 1.1", name).c_str());
	};

	// A picture's light log-normal: its key is its median, its depth 1.2816 sigma.
	const double kDepthSigma = 1.0 / 1.2816;
	{
		// Already where its targets are: a film whose own levels are taken as the targets, with
		// a peak the display shows.
		const MadeFilm f = MakeFilm({ 2.3, 2.0, 2.7, 2.35, 2.1, 2.6, 2.4, 2.5 }, 2.36 * kDepthSigma, 600.0, 100);
		const std::vector<HdrCurveTune::Stats> stats = StatsOfFilm(f);
		const HdrCurveTune::Film own = HdrCurveTune::Summarise(stats);
		const HdrCurveTune::Targets t0 = { own.depth, 0.46, own.key, 0.71 };
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(stats, 800, t0);
		PrintTuned("at the targets, 800 nits", t);
		Check(t.values == HdrCurve::Values{}, "a film at the targets is left as it is");
		Bounds("at the targets", t);
	}
	{
		// A master graded to 4000 nits on an 800-nit display, its shadows and key where its
		// targets are.
		const MadeFilm f = MakeFilm({ 2.3, 2.0, 2.7, 2.35, 2.1, 2.6, 2.4, 2.5 }, 2.36 * kDepthSigma, 4000.0, 200);
		const std::vector<HdrCurveTune::Stats> stats = StatsOfFilm(f);
		const HdrCurveTune::Film own = HdrCurveTune::Summarise(stats);
		const HdrCurveTune::Targets t0 = { own.depth, 0.46, own.key, 0.71 };
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(stats, 800, t0);
		PrintTuned("4000 nits on 800", t);
		Check(t.values.capNits == 800, "4000 nits on 800: the cap is the display's peak");
		Check(std::abs(t.values.startNits - 362) <= 15, "4000 nits on 800: the roll-off starts at BT.2390's knee, about 360");
		Check(t.peakAfter >= HdrCurveTune::kPeakLandLow * 800 * 0.995 && t.peakAfter <= HdrCurveTune::kPeakLandHigh * 800 * 1.005,
			"4000 nits on 800: the film's peak lands at 95 to 99 % of the cap");
		Check(!t.values.liftEv100 && t.values.midGreyNits == HdrCurve::kMidNeutral, "4000 nits on 800: shadows and mid tones left alone");
		Bounds("4000 nits on 800", t);
	}
	{
		// Sunk shadows, as RTX Video HDR left a film here: its key near the masters', its
		// shadows 0.6 deeper under it.
		const MadeFilm f = MakeFilm({ 1.3, 1.1, 1.6, 0.9, 1.4, 1.2, 1.5, 1.3, 1.0, 1.7 }, 2.95 * kDepthSigma, 500.0, 300);
		const std::vector<HdrCurveTune::Stats> before = StatsOfFilm(f);
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(before, 800, kTestTargets);
		PrintTuned("sunk shadows", t);
		Check(t.values.liftEv100 > 0, "sunk shadows: lifted");
		Check(t.values.midGreyNits == HdrCurve::kMidNeutral, "sunk shadows: a key within its band, the mid grey left alone");
		Check(!t.values.capNits && !t.values.startNits, "sunk shadows: highlights that fit the display left alone");
		Check(std::abs(t.depthAfter - kTestTargets.depth) < std::abs(t.film.depth - kTestTargets.depth),
			"sunk shadows: the depth closer to the target");
		Check(t.contrastAfter >= HdrCurveTune::kContrastKept * t.film.contrast - 1e-9, "sunk shadows: 85 % of the contrast kept");
		Bounds("sunk shadows", t);
		// Measured again through the curve it was given: closer, so what is left to do is less,
		// and in the same direction -- the tune never turns back on what it did. (The filter
		// measures before the curve, so a second press gives the same values; this is the
		// curve's effect on what it aims at.)
		const HdrCurveTune::Tuned again = HdrCurveTune::Tune(StatsOfFilm(f, &t.values), 800, kTestTargets);
		PrintTuned("sunk shadows, tuned again", again);
		Check(again.values.liftEv100 < t.values.liftEv100 && again.values.midGreyNits == HdrCurve::kMidNeutral,
			"sunk shadows, measured again through its curve: less lift wanted, and nothing turned back");
		Check(HdrCurveTune::Tune(before, 800, kTestTargets).values == t.values, "sunk shadows: the same pictures give the same values");
	}
	{
		// Dim all through: its key far under the masters', its shadows as deep as theirs.
		const MadeFilm f = MakeFilm({ 0.5, 0.45, 0.6, 0.4, 0.55, 0.5, 0.48, 0.52 }, 2.36 * kDepthSigma, 300.0, 400);
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(StatsOfFilm(f), 800, kTestTargets);
		PrintTuned("dim all through", t);
		Check(t.values.midGreyNits > HdrCurve::kMidNeutral && t.keyAfter > t.film.key, "dim all through: the mid grey raises the key");
		Check(std::abs(std::log(t.keyAfter / t.film.key)) <= HdrCurveTune::kKeyMoveLimit + 0.01, "dim all through: by half an EV at most");
		Check(!t.values.liftEv100, "dim all through: shadows as deep as the masters' not lifted");
		Bounds("dim all through", t);
	}
	{
		// Shadows sunk very deep: the lift stops at one EV.
		const MadeFilm f = MakeFilm({ 2.3, 2.0, 2.7, 2.35, 2.1, 2.6, 2.4, 2.5 }, 4.5 * kDepthSigma, 300.0, 500);
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(StatsOfFilm(f), 800, kTestTargets);
		PrintTuned("shadows very deep", t);
		Check(t.values.liftEv100 > 0, "shadows very deep: lifted");
		Bounds("shadows very deep", t);
	}
	{
		// Bright and flat, washed out: the key comes down, the shallow shadows are not touched.
		const MadeFilm f = MakeFilm({ 14, 16, 13, 15, 17, 14, 16, 15 }, 1.0 * kDepthSigma, 700.0, 600);
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(StatsOfFilm(f), 800, kTestTargets);
		PrintTuned("bright and flat", t);
		Check(!t.values.liftEv100, "bright and flat: no lift");
		Check(t.values.midGreyNits < HdrCurve::kMidNeutral && t.keyAfter < t.film.key, "bright and flat: the key comes down");
		Bounds("bright and flat", t);
	}
	{
		// Too few pictures with anything in them: nothing.
		const MadeFilm f = MakeFilm({ 10, 12, 8 }, 1.5, 300.0, 600);
		const HdrCurveTune::Tuned t = HdrCurveTune::Tune(StatsOfFilm(f), 800, kTestTargets);
		Check(!t.enough && t.values == HdrCurve::Values{}, "three pictures are not enough to tune on, and nothing changes");
	}

	printf("\n  %d check(s) failed\n", g_failures);
	return g_failures ? 1 : 0;
}

} // namespace temporal
