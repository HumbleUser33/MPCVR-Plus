// --tdownscale: what each downscaler keeps and what it lets through, measured on
// the shaders that ship.
//
// A downscaler has two jobs that pull against each other. It must keep the detail
// that still fits in the smaller picture, and it must throw away the detail that no
// longer does -- because whatever it fails to throw away does not disappear, it
// folds back into the picture as moire and shimmer. Nothing else about it matters
// much, and a comparison against another resizer cannot separate the two.
//
// So the pictures here are built rather than photographed, and their right answers
// are known in closed form:
//
//   keeps   a sinusoid below the output's Nyquist. The correct result is the same
//           sinusoid, so what comes out says directly how much of it survived. 1.000
//           is perfect; below that is softening, above it is a kernel adding contrast
//           it was not given, which is the beginning of ringing.
//   folds   a sinusoid above the output's Nyquist. It cannot be represented at the
//           smaller size at all, so the correct result is flat: anything left is
//           aliasing, and it will crawl when the picture moves.
//   halo    a step edge. How far past the range the source actually covers the
//           result reaches.
//
// No reference kernel is involved anywhere, which matters: scoring downscalers
// against a long windowed sinc would rank them by how much they resemble it, and
// that is a different question from how well they work.

namespace temporal {

namespace {

struct DownScore {
	const char* name;
	double keep[3];     // amplitude kept at 0.125, 0.250, 0.375 cycles per output pixel
	double flatness;    // the worst of |keep - 1|
	double fold[3];     // amplitude surviving from 0.60, 0.75, 0.90
	double alias;       // the worst of those
	double halo;        // overshoot past the source's own range, at a step
	double error;       // flatness + alias: everything it got wrong, in one number
	double ms;
};

// A sinusoid along x at f cycles per output pixel, written at the source size.
// Texel n covers [n, n+1) and is sampled at its middle, which is what the
// convolution assumes when it works out its taps.
void SineSource(std::vector<float>& rgba, int w, int h, double fOut, double ratio)
{
	rgba.assign((size_t)w * h * 4, 1.0f);
	const double fSrc = fOut / ratio;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const double v = 0.5 + 0.4 * cos(2.0 * 3.14159265358979323846 * fSrc * (x + 0.5));
			for (int i = 0; i < 3; i++) {
				rgba[((size_t)y * w + x) * 4 + i] = (float)v;
			}
		}
	}
}

// Hard edges along x, far enough apart that each settles before the next.
void StepSource(std::vector<float>& rgba, int w, int h)
{
	rgba.assign((size_t)w * h * 4, 1.0f);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const float v = ((x / 64) & 1) ? 0.9f : 0.1f;
			for (int i = 0; i < 3; i++) {
				rgba[((size_t)y * w + x) * 4 + i] = v;
			}
		}
	}
}

// How much of a sinusoid of this frequency is in the row, by projection: the two
// quadratures, which is immune to whatever phase the resampling landed on.
double AmplitudeAt(const std::vector<float>& rgba, int w, int h, double fOut)
{
	const double k = 2.0 * 3.14159265358979323846 * fOut;
	double total = 0.0;
	int rows = 0;
	// The edges of the picture are clamped rather than continued, so the first and
	// last few output pixels are not the signal and are left out.
	const int skip = 8;
	for (int y = h / 4; y < h * 3 / 4; y += 7) {
		double re = 0.0, im = 0.0;
		int n = 0;
		for (int x = skip; x < w - skip; x++) {
			const double v = rgba[((size_t)y * w + x) * 4];
			re += v * cos(k * (x + 0.5));
			im += v * sin(k * (x + 0.5));
			n++;
		}
		total += 2.0 * sqrt(re * re + im * im) / n;
		rows++;
	}
	return rows ? total / rows : 0.0;
}

double OvershootAt(const std::vector<float>& rgba, int w, int h, double lo, double hi)
{
	double worst = 0.0;
	const int skip = 8;
	for (int y = h / 4; y < h * 3 / 4; y += 7) {
		for (int x = skip; x < w - skip; x++) {
			const double v = rgba[((size_t)y * w + x) * 4];
			worst = std::max(worst, std::max(v - hi, lo - v));
		}
	}
	return worst;
}

} // namespace

static int RunDownscaleQuality(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	printf("\n=== What each downscaler keeps and what it lets fold (--tdownscale) ===\n");

	CUpscalePasses passes;
	std::string error;
	if (!passes.Init(dev, error)) {
		printf("  the resize passes could not be set up: %s\n", error.c_str());
		return 1;
	}
	if (passes.ShippedCount() == 0) {
		printf("  no compiled downscaler found in _bin\\shaders: build the shaders first\n");
		return 1;
	}

	const int sw = 3840, sh = 240, dw = 1920, dh = 120;   // 4K reduced by two, the usual case
	const double ratio = (double)sw / dw;
	printf("  %dx%d reduced to %dx%d, the frequencies in cycles per output pixel\n\n", sw, sh, dw, dh);

	Target texIn, texOut;
	CComPtr<ID3D11Texture2D> stage;
	if (!MakeTarget(dev, sw, sh, false, texIn, DXGI_FORMAT_R16G16B16A16_FLOAT)
			|| !MakeTarget(dev, dw, dh, false, texOut, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		printf("  textures failed\n");
		return 1;
	}
	D3D11_TEXTURE2D_DESC sd = {};
	texOut.tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		printf("  staging failed\n");
		return 1;
	}

	const auto run = [&](const CUpscalePasses::Method& m, const std::vector<float>& src,
	                     std::vector<float>& out) -> bool {
		std::vector<HALF> half(src.size());
		DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), src.data(), sizeof(float), src.size());
		ctx->UpdateSubresource(texIn.tex, 0, nullptr, half.data(), 4 * sizeof(HALF) * sw, 0);
		if (!passes.Resize(dev, ctx, m, texIn.srv, sw, sh, texOut.rtv, dw, dh)) {
			return false;
		}
		ctx->CopyResource(stage, texOut.tex);
		return ReadRgbaHalf(ctx, stage, dw, dh, out);
	};

	static const double kKeep[3] = { 0.125, 0.250, 0.375 };
	static const double kFold[3] = { 0.600, 0.750, 0.900 };

	std::vector<DownScore> scores;
	for (int i = 0; i < passes.ShippedCount(); i++) {
		const CUpscalePasses::Method& m = passes.Shipped(i);
		DownScore s = {};
		s.name = m.name;
		bool ok = true;

		std::vector<float> src, out;
		for (int f = 0; f < 3 && ok; f++) {
			SineSource(src, sw, sh, kKeep[f], ratio);
			ok = run(m, src, out);
			// 0.4 was the amplitude written in.
			s.keep[f] = ok ? AmplitudeAt(out, dw, dh, kKeep[f]) / 0.4 : 0.0;
			s.flatness = std::max(s.flatness, fabs(s.keep[f] - 1.0));
		}
		for (int f = 0; f < 3 && ok; f++) {
			SineSource(src, sw, sh, kFold[f], ratio);
			ok = run(m, src, out);
			// Above the output's Nyquist it comes back folded to 1 - f.
			s.fold[f] = ok ? AmplitudeAt(out, dw, dh, 1.0 - kFold[f]) / 0.4 : 0.0;
			s.alias = std::max(s.alias, s.fold[f]);
		}
		if (ok) {
			StepSource(src, sw, sh);
			ok = run(m, src, out);
			s.halo = ok ? OvershootAt(out, dw, dh, 0.1, 0.9) : 0.0;
		}
		if (!ok) {
			printf("  %-14s could not be run\n", m.name);
			continue;
		}

		// The time, once the shaders are warm.
		const DWORD t0 = GetTickCount();
		int frames = 0;
		SineSource(src, sw, sh, 0.25, ratio);
		for (; GetTickCount() - t0 < 300; frames++) {
			run(m, src, out);
		}
		s.ms = frames ? (double)(GetTickCount() - t0) / frames : 0.0;

		// Every one of the six is an error against an answer that is known: what the
		// picture should still have and does not, and what it should not have and
		// does. Root mean square of the lot, which weighs nothing by hand. A kernel
		// that hands back more contrast than it was given is as wrong as one that
		// loses some, and says so here instead of being rewarded for looking sharper.
		double sum = 0.0;
		for (int f = 0; f < 3; f++) {
			sum += (s.keep[f] - 1.0) * (s.keep[f] - 1.0) + s.fold[f] * s.fold[f];
		}
		s.error = sqrt(sum / 6.0);
		scores.push_back(s);
	}

	std::sort(scores.begin(), scores.end(),
		[](const DownScore& a, const DownScore& b) { return a.error < b.error; });

	printf("  %-14s %6s %6s %6s %8s | %6s %6s %6s %7s | %6s %7s\n",
		"", "0.125", "0.250", "0.375", "flatness", "0.60", "0.75", "0.90", "alias", "halo", "error");
	for (const auto& s : scores) {
		printf("  %-14s %6.3f %6.3f %6.3f %8.3f | %6.3f %6.3f %6.3f %7.3f | %6.3f %7.3f\n",
			s.name, s.keep[0], s.keep[1], s.keep[2], s.flatness,
			s.fold[0], s.fold[1], s.fold[2], s.alias, s.halo, s.error);
	}

	printf("\n  best first, by the root mean square of all six errors -- what the picture\n"
	       "  should still have and does not, and what it should not have and does:\n");
	for (size_t i = 0; i < scores.size(); i++) {
		printf("   %zu. %-14s %.4f   worst in band %+.3f, worst folded %.3f, halo %.3f%s\n",
			i + 1, scores[i].name, scores[i].error,
			scores[i].keep[2] - 1.0, scores[i].alias, scores[i].halo,
			scores[i].halo > 0.030 ? "   rings" : "");
	}
	printf("\n  cost at 4K to 1080p, per frame: ");
	for (const auto& s : scores) {
		printf("%s %.2f ms  ", s.name, s.ms);
	}
	printf("\n\n=== Result ===\n  0 check(s) failed\n");
	return 0;
}

} // namespace temporal
