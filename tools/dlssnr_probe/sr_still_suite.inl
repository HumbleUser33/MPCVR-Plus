// --tsrstill: DLSS Super Resolution on a picture that holds still and is grained
// anew on every frame, as old film is: how much a still background shimmers, and
// how wide the band along the frame's edges where the grain stays. Then an object
// crossing that background, a slow pan and a pan, so that nothing done for the still
// picture costs the moving ones.
//
// Included by harness.cpp after sr_suite.inl, whose uploads and readbacks it uses,
// and upscale_suite.inl, for the references, the reduction, the grain and Measure.
//
// Each 4K reference is windowed and halved into the source, which DLSS enlarges
// twice: a 1080p film on a 4K screen. Every row sees the same frames:
//   0-39   still, measured on 25-39 once DLSS has settled
//   40-59  an object, 256 x 256 reference pixels cut from the same picture, crossing
//          the still background at 2.5 x 1.5 source pixels a frame; measured 55-59
//   60-79  a slow pan, 0.5 x 0.5 source pixels a frame; measured 75-79
//   80-99  a pan, 3.5 x 1.5 source pixels a frame, --tsrq's; measured 95-99
// On the output's luma, x1000:
//   wob    what moves on the still picture at the scale of structures: the mean change
//          of the 4 x 4 block means from one frame to the next -- the heat haze
//   flick  the same for what is left once the block means are taken out: the grain
//   resid  the grain left on frame 39: mean |output - reference| without the block means
// and mv, the RMS error of the vectors DLSS was given, in source pixels, on the still
// frames and on the pan. Each over the picture's centre and in bands by distance
// from the frame's edge, in source pixels. What moves is judged the same way, against
// the frame before at the place it came from: the object on 46-59 inside its square,
// the pan on 90-99 away from the edge it enters by.

namespace temporal {

namespace {

struct StillRow {
	const char* name;
	int motion = 0;                        // 0 exact, 1 Optical Flow, 2 none
	CDlssStabilizer::FlowSettings flow;    // Optical Flow rows
	int pad = 0;                           // guard band: source pixels mirrored on each side
	// Exact rows: vectors made worse on purpose, to see what accuracy DLSS needs.
	float noise = 0;                       // per 8 x 8 block, as Optical Flow's, source pixels (sigma)
	float jitter = 0;                      // the whole picture's, new on every frame, source pixels (sigma)
	bool blocks = false;                   // exact motion at 8 x 8 block centres, blended as PASS 3 blends
};

// Optical Flow as DlssSRPass runs it -- about 540 lines, grid 4, forward only --
// with or without the snapping to the global motion.
CDlssStabilizer::FlowSettings StillFlow(bool snap = false, float low = 0.5f, float high = 1.0f, int gate = 0,
	bool confidence = false, bool hints = true, UINT blur = 0, float deadZone = 0.0f)
{
	CDlssStabilizer::FlowSettings flow;
	flow.snapDeadZone  = deadZone;
	flow.flowBlur      = blur;
	flow.bidirectional = confidence;
	flow.cost          = confidence;
	flow.temporalHints = hints;
	flow.snap          = snap;
	flow.snapLow       = low;
	flow.snapHigh      = high;
	flow.snapGate      = gate;
	return flow;
}

constexpr int kBandEdges[] = { 0, 4, 8, 16, 32, 64 };
constexpr int kBands = (int)std::size(kBandEdges);   // the last band runs to the centre
const char* const kBandNames[kBands] = { "0-4", "4-8", "8-16", "16-32", "32-64", "centre" };

int BandAt(int x, int y, int W, int H, int pixelsPerSource)
{
	const int d = std::min({ x, W - 1 - x, y, H - 1 - y }) / pixelsPerSource;
	int b = 0;
	while (b + 1 < kBands && d >= kBandEdges[b + 1]) {
		b++;
	}
	return b;
}

struct Bands {
	double sum[kBands] = {};
	double count[kBands] = {};
	void Add(int band, double v) { sum[band] += v; count[band] += 1; }
	double At(int band) const { return count[band] ? sum[band] / count[band] : 0.0; }
	double Centre() const { return At(kBands - 1); }
	void Merge(const Bands& o) { for (int b = 0; b < kBands; b++) { sum[b] += o.sum[b]; count[b] += o.count[b]; } }
};

// What one row gave on one reference; summed over references for the final table.
struct StillResult {
	double psnrStill = 0, psnrObject = 0, psnrSlow = 0, psnrPan = 0;
	double gmStill = 0, gmPan = 0, spread = 0;   // global motion error and spread, source pixels
	double gmSlow = 0;                           // and on the slow pan
	int gmCount = 0;
	double gmBias = 0, gmJitter = 0;             // on the still frames: its mean, and how it moves around it
	// Steadiness of what moves, against the frame before at the place it came from.
	double objWob = 0, objFlick = 0, panWob = 0, panFlick = 0;
	Bands wob, flick, resid, mvStill, mvPan, mvSlow;
	double mvObjectIn = 0, mvObjectEdge = 0, nObjectIn = 0, nObjectEdge = 0;   // squared errors on the object
	double mvObjectMedian = 0, mvObjectP90 = 0;                             // inside it, source pixels
	// PASS 4's blocks by the luma deviation of their window on the flow frame (bins below
	// 0.005, 0.01, 0.02, 0.04 and above): on the still frame 39 all of them, on the
	// object's frames those inside it; how much of their own vector they keep, and how far
	// the kept ones move, source pixels.
	static constexpr int kTextureBins = 5;
	double stillBlocks[kTextureBins] = {}, stillKept[kTextureBins] = {}, stillMotion[kTextureBins] = {};
	double objectBlocks[kTextureBins] = {}, objectKept[kTextureBins] = {};
	int n = 0;
	void Add(const StillResult& o) {
		psnrStill += o.psnrStill; psnrObject += o.psnrObject; psnrSlow += o.psnrSlow; psnrPan += o.psnrPan;
		gmStill += o.gmStill; gmPan += o.gmPan; spread += o.spread; gmCount += o.gmCount; gmSlow += o.gmSlow;
		gmBias += o.gmBias; gmJitter += o.gmJitter;
		objWob += o.objWob; objFlick += o.objFlick; panWob += o.panWob; panFlick += o.panFlick;
		mvObjectIn += o.mvObjectIn; mvObjectEdge += o.mvObjectEdge; nObjectIn += o.nObjectIn; nObjectEdge += o.nObjectEdge;
		mvObjectMedian += o.mvObjectMedian; mvObjectP90 += o.mvObjectP90;
		for (int b = 0; b < kTextureBins; b++) {
			stillBlocks[b] += o.stillBlocks[b]; stillKept[b] += o.stillKept[b]; stillMotion[b] += o.stillMotion[b];
			objectBlocks[b] += o.objectBlocks[b]; objectKept[b] += o.objectKept[b];
		}
		wob.Merge(o.wob); flick.Merge(o.flick); resid.Merge(o.resid); mvStill.Merge(o.mvStill); mvPan.Merge(o.mvPan); mvSlow.Merge(o.mvSlow);
		n++;
	}
};

// Luma and its 4 x 4 block means. W and H are multiples of 4.
void LumaBlocks(const std::vector<float>& rgba, int W, int H, std::vector<float>& y, std::vector<float>& blocks)
{
	y.resize((size_t)W * H);
	for (size_t i = 0; i < y.size(); i++) {
		y[i] = Luma(&rgba[4 * i]);
	}
	const int bw = W / 4, bh = H / 4;
	blocks.assign((size_t)bw * bh, 0.0f);
	for (int by = 0; by < bh; by++) {
		for (int bx = 0; bx < bw; bx++) {
			float s = 0;
			for (int j = 0; j < 4; j++) {
				for (int i = 0; i < 4; i++) {
					s += y[(size_t)(4 * by + j) * W + 4 * bx + i];
				}
			}
			blocks[(size_t)by * bw + bx] = s / 16.0f;
		}
	}
}

// How steady something moving is: luma of frame t over a rectangle, against frame
// t - 1 at the place its content came from (dx, dy further), as 4 x 4 block means
// (wob) and what is left once they are taken out (flick). Sums, and their counts.
struct Steadiness {
	double wob = 0, flick = 0, blocks = 0, pixels = 0;
	void Add(const std::vector<float>& cur, const std::vector<float>& prev, int W, int x0, int y0, int x1, int y1, int dx, int dy)
	{
		for (int by = y0; by + 4 <= y1; by += 4) {
			for (int bx = x0; bx + 4 <= x1; bx += 4) {
				float mc = 0, mp = 0;
				for (int j = 0; j < 4; j++) {
					for (int i = 0; i < 4; i++) {
						mc += cur[(size_t)(by + j) * W + bx + i];
						mp += prev[(size_t)(by + j + dy) * W + bx + i + dx];
					}
				}
				mc /= 16.0f;
				mp /= 16.0f;
				wob += 1000.0 * std::abs(mc - mp);
				blocks += 1;
				for (int j = 0; j < 4; j++) {
					for (int i = 0; i < 4; i++) {
						const float c = cur[(size_t)(by + j) * W + bx + i] - mc;
						const float q = prev[(size_t)(by + j + dy) * W + bx + i + dx] - mp;
						flick += 1000.0 * std::abs(c - q);
						pixels += 1;
					}
				}
			}
		}
	}
	double Wob() const { return blocks ? wob / blocks : 0.0; }
	double Flick() const { return pixels ? flick / pixels : 0.0; }
};

// Luma PSNR over a rectangle of two pictures of width W.
double LumaPsnr(const std::vector<float>& a, const std::vector<float>& b, int W, int x0, int y0, int x1, int y1)
{
	double se = 0;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			const size_t i = 4 * ((size_t)y * W + x);
			const double d = Luma(&a[i]) - Luma(&b[i]);
			se += d * d;
		}
	}
	const double mse = se / ((double)(x1 - x0) * (y1 - y0));
	return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0;
}

bool ReadMotion(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, std::vector<float>& out, int& W, int& H)
{
	D3D11_TEXTURE2D_DESC sd = {};
	tex->GetDesc(&sd);
	W = (int)sd.Width;
	H = (int)sd.Height;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	out.resize(2 * (size_t)W * H);
	for (int y = 0; y < H; y++) {
		DirectX::PackedVector::XMConvertHalfToFloatStream(&out[2 * (size_t)y * W], sizeof(float),
			(const HALF*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * y), sizeof(HALF), 2 * (size_t)W);
	}
	ctx->Unmap(stage, 0);
	return true;
}

bool ReadGlobalMotion(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, float value[4])
{
	D3D11_TEXTURE2D_DESC sd = {};
	tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	memcpy(value, mr.pData, 4 * sizeof(float));
	ctx->Unmap(stage, 0);
	return true;
}

bool ReadRaw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int bytesPerPixel,
	std::vector<BYTE>& out, int& W, int& H)
{
	D3D11_TEXTURE2D_DESC sd = {};
	tex->GetDesc(&sd);
	W = (int)sd.Width;
	H = (int)sd.Height;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	out.resize((size_t)W * H * bytesPerPixel);
	for (int y = 0; y < H; y++) {
		memcpy(&out[(size_t)y * W * bytesPerPixel], (const BYTE*)mr.pData + (size_t)mr.RowPitch * y, (size_t)W * bytesPerPixel);
	}
	ctx->Unmap(stage, 0);
	return true;
}

// Source pixel -> the one its mirror copies, and the sign the motion takes there.
int Mirror(int x, int n, float& sign)
{
	sign = 1.0f;
	if (x < 0) {
		sign = -1.0f;
		return std::min(-x - 1, n - 1);
	}
	if (x >= n) {
		sign = -1.0f;
		return std::max(2 * n - x - 1, 0);
	}
	return x;
}

} // namespace

// PASS 4's blocks by texture: their share, how much of their own vector they keep, how far.
static void PrintTextureBins(const StillResult& r, const char* name)
{
	double still = 0, object = 0;
	for (int b = 0; b < StillResult::kTextureBins; b++) {
		still += r.stillBlocks[b];
		object += r.objectBlocks[b];
	}
	if (!still) {
		return;
	}
	Out("  %-26s by luma deviation <.005 <.01 <.02 <.04 more: still blocks", name);
	for (int b = 0; b < StillResult::kTextureBins; b++) {
		Out(" %4.1f%%", 100.0 * r.stillBlocks[b] / still);
	}
	Out(", kept");
	for (int b = 0; b < StillResult::kTextureBins; b++) {
		Out(" %4.1f%%", r.stillBlocks[b] ? 100.0 * r.stillKept[b] / r.stillBlocks[b] : 0.0);
	}
	Out(", by");
	for (int b = 0; b < StillResult::kTextureBins; b++) {
		Out(" %4.2f", r.stillKept[b] ? r.stillMotion[b] / r.stillKept[b] : 0.0);
	}
	if (object) {
		Out(" px; object blocks");
		for (int b = 0; b < StillResult::kTextureBins; b++) {
			Out(" %4.1f%%", 100.0 * r.objectBlocks[b] / object);
		}
		Out(", kept");
		for (int b = 0; b < StillResult::kTextureBins; b++) {
			Out(" %4.1f%%", r.objectBlocks[b] ? 100.0 * r.objectKept[b] / r.objectBlocks[b] : 0.0);
		}
	}
	Out("\n");
}

static int RunSRStill(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* dllPath, int maxRefs)
{
	Head("DLSS Super Resolution on still, grained pictures: shimmer and edges");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");

		std::vector<std::wstring> files;
		{
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileW(L"upscale_refs\\*.png", &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do {
					files.push_back(std::wstring(L"upscale_refs\\") + fd.cFileName);
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			std::sort(files.begin(), files.end());
		}
		if (files.empty()) {
			printf("  put 4K film frames in upscale_refs first\n");
			rc = 1;
		}

		std::string error;
		CStabilizerPasses stabPasses;
		if (!rc && (!factory || !stabPasses.Init(dev, error))) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}

		CDlssSR sr;
		if (!rc && !sr.Init(dev, dllPath)) {
			printf("%S", sr.GetInfoBlock().c_str());
			rc = 1;
		}

		// Exact motion, the raw vectors of 1.2, the snapping to the global motion alone
		// (the first version of this work), the median global motion with its 0.4-pixel
		// dead zone (until 2.0), and what the filter runs: the median refined on the flow
		// frames, which --tsrclip found a film's weave needs. Then finer flow.
		auto median = []() {
			CDlssStabilizer::FlowSettings flow = CDlssStabilizer::ForDlssSR();
			flow.globalRefine = 0;
			flow.snapDeadZone = 0.4f;
			return flow;
		};
		auto finer = []() {
			CDlssStabilizer::FlowSettings flow = CDlssStabilizer::ForDlssSR();
			flow.flowFactor = 1;
			flow.flowBlur = 2;
			return flow;
		};
		const std::vector<StillRow> rows = {
			{ "exact motion",               0 },
			{ "raw vectors (1.2)",          1, StillFlow() },
			{ "snapped only",               1, StillFlow(true, 1.0f, 2.0f, 2, true, true, 0, 0.4f) },
			{ "median (until 2.0)",         1, median() },
			{ "filter now",                 1, CDlssStabilizer::ForDlssSR() },
			{ "flow /1, blur 2",            1, finer() },
		};

		// Grained anew on every frame; the last also compressed after, so that its blocks
		// change with the grain, as a low-rate film's do.
		static const Degradation degradations[] = {
			{ "grain 0.006",                0.006f, 0.0f },
			{ "old film grain 0.02",        0.02f,  0.0f },
			{ "grain 0.006, compressed",    0.006f, 0.6f },
		};

		const int kFrames = 100;
		auto stepAt = [](int t, int& dx, int& dy) {   // reference pixels the window moved between t - 1 and t
			dx = dy = 0;
			if (t > 60 && t <= 80) { dx = 1; dy = 1; }
			if (t > 80) { dx = 7; dy = 3; }
		};
		auto offsetAt = [&](int t, int& ox, int& oy) {
			ox = oy = 0;
			for (int u = 1; u <= t; u++) {
				int dx = 0, dy = 0;
				stepAt(u, dx, dy);
				ox += dx;
				oy += dy;
			}
		};
		auto isStill = [](int t) { return t >= 25 && t <= 39; };
		auto isObject = [](int t) { return t >= 55 && t <= 59; };
		auto isSlow = [](int t) { return t >= 75 && t <= 79; };
		auto isPan = [](int t) { return t >= 95 && t <= 99; };
		auto isObjectRun = [](int t) { return t >= 45 && t <= 59; };   // steadiness from 46, against 45
		auto isPanRun = [](int t) { return t >= 89 && t <= 99; };      // from 90, against 89
		const int kPatch = 256, kObjectStart = 40, kObjectEnd = 60, kObjectDx = 5, kObjectDy = 3;

		std::map<std::string, StillResult> totals[std::size(degradations)];

		if (!rc) {
			g_report = fopen("temporal_results_srstill.txt", "w");
			Out("\nDLSS Super Resolution (%S) on still pictures grained anew on every frame, then an object,\n", sr.GetDllPath().c_str());
			Out("a slow pan and a pan. Source: a 4K window halved; DLSS enlarges it twice (1080p to 4K).\n");
			Out("wob: 4x4 block means changing between still frames; flick: the grain changing; resid: the grain\n");
			Out("left on frame 39 (all luma, x1000). mv: RMS error of the vectors DLSS got, source pixels.\n");
			Out("gm: the global motion's error, and spread: the blocks' median distance to it, source pixels.\n");
			Out("psnr on the still frame 39, on the object (55-59, inside its square), the slow pan (75-79), the pan (95-99).\n");
		}

		int refsDone = 0;
		for (size_t fi = 0; !rc && fi < files.size() && (maxRefs <= 0 || refsDone < maxRefs); fi++) {
			UpscaleRef ref;
			if (!LoadReference(factory, files[fi].c_str(), ref, error) || ref.W < 3840) {
				continue;   // true 4K frames only: the scale is a 4K screen's, and the pans need room
			}
			ref.name = files[fi].substr(files[fi].find_last_of(L'\\') + 1);
			refsDone++;
			int maxX = 0, maxY = 0;
			offsetAt(kFrames - 1, maxX, maxY);
			const int cw = (ref.W - maxX) & ~7, ch = (ref.H - maxY) & ~7;   // output size; multiples of 8
			const int sw = cw / 2, sh = ch / 2;
			const int patchX0 = (cw / 3) & ~1, patchY0 = (ch / 3) & ~1;     // where the object starts, reference pixels
			const int cutX = ref.W / 2 - kPatch / 2, cutY = ref.H / 2 - kPatch / 2;   // where it is cut from
			Out("\n%S -- window %dx%d, source %dx%d\n", ref.name.c_str(), cw, ch, sw, sh);
			printf("  %S\n", ref.name.c_str());

			// Each pixel's band, once: at the output's scale and at the source's.
			std::vector<uint8_t> bandOut((size_t)cw * ch), bandSrc((size_t)sw * sh);
			for (int yy = 0; yy < ch; yy++) {
				for (int xx = 0; xx < cw; xx++) {
					bandOut[(size_t)yy * cw + xx] = (uint8_t)BandAt(xx, yy, cw, ch, 2);
				}
			}
			for (int yy = 0; yy < sh; yy++) {
				for (int xx = 0; xx < sw; xx++) {
					bandSrc[(size_t)yy * sw + xx] = (uint8_t)BandAt(xx, yy, sw, sh, 1);
				}
			}

			auto objectAt = [&](int t, int& px, int& py) {
				px = patchX0 + kObjectDx * (t - kObjectStart);
				py = patchY0 + kObjectDy * (t - kObjectStart);
				return t >= kObjectStart && t < kObjectEnd;
			};
			// The picture of frame t at the reference's scale: the window, and the object on it.
			std::vector<float> crop(4 * (size_t)cw * ch);
			auto cropAt = [&](int t) {
				int ox = 0, oy = 0;
				offsetAt(t, ox, oy);
				for (int y = 0; y < ch; y++) {
					std::copy_n(&ref.rgba[4 * ((size_t)(oy + y) * ref.W + ox)], 4 * (size_t)cw, &crop[4 * (size_t)y * cw]);
				}
				int px = 0, py = 0;
				if (objectAt(t, px, py)) {
					for (int y = 0; y < kPatch; y++) {
						std::copy_n(&ref.rgba[4 * ((size_t)(cutY + y) * ref.W + cutX)], 4 * (size_t)kPatch,
							&crop[4 * ((size_t)(py + y) * cw + px)]);
					}
				}
			};
			// Where the content at source pixel (x, y) of frame t was one frame earlier.
			auto truth = [&](int t, int x, int y, float& mx, float& my) {
				mx = my = 0.0f;
				int px = 0, py = 0;
				if (t > kObjectStart && objectAt(t, px, py)) {
					const int rx = 2 * x + 1, ry = 2 * y + 1;
					if (rx >= px && rx < px + kPatch && ry >= py && ry < py + kPatch) {
						mx = -0.5f * kObjectDx;
						my = -0.5f * kObjectDy;
						return;
					}
				}
				int dx = 0, dy = 0;
				stepAt(t, dx, dy);
				mx = 0.5f * dx;
				my = 0.5f * dy;
			};

			for (size_t di = 0; di < std::size(degradations); di++) {
				const Degradation& deg = degradations[di];
				Out("\n  %s\n", deg.name);
				Out("  %-26s %7s %6s %6s %6s %7s | %7s %7s %7s %7s | %6s %6s %6s %6s %6s\n", "method",
				    "psnr", "wob", "flick", "resid", "mv", "object", "slow", "pan", "mv pan", "gm", "gm pan", "spread", "bias", "jitter");

				for (const StillRow& row : rows) {
					const int p = row.pad;
					const int iw = sw + 2 * p, ih = sh + 2 * p;   // what DLSS gets
					bool ok = sr.CreateFeature(iw, ih, 2 * iw, 2 * ih, NGX_DLSS_PRESET_Default);
					sr.RequestReset();

					Tex2D_t exactMotion;
					CDlssStabilizer flow;
					if (ok && row.motion == 0) {
						ok = SUCCEEDED(exactMotion.Create(dev, DXGI_FORMAT_R16G16_FLOAT, iw, ih, Tex2D_DefaultShaderRTarget));
					}
					if (ok && row.motion == 1) {
						ok = SUCCEEDED(flow.Create(dev, ctx, iw, ih,
							stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear(),
							true, row.flow))
							&& flow.HasFlow();
						if (!ok) {
							Out("  %-26s Optical Flow: %S\n", row.name, flow.GetStatusLine().c_str());
						}
					}

					StillResult r;
					std::vector<float> source, padded, out, outCrop, y, blocks, yPrev, blocksPrev, yRef, blocksRef, motion;
					std::vector<HALF> motionHalf;
					double psnrObject = 0, psnrSlow = 0, psnrPan = 0;
					Steadiness objectSteady, panSteady;
					std::vector<float> yNow, yLast;
					int tLast = -10;
					double gmSumX = 0, gmSumY = 0, gmSumSq = 0;
					std::vector<float> objectErrors;   // inside the object, every measured frame
					std::vector<float> rawX, rawY;      // Optical Flow's own blocks inside the object, source pixels
					std::vector<float> objectKeep;      // PASS 4's keep on those blocks
					int nObject = 0, nSlow = 0, nPan = 0;

					for (int t = 0; ok && t < kFrames; t++) {
						cropAt(t);
						DownscaleBox(crop, cw, ch, 2, source);
						AddGrain(source, sw, sh, deg.grain, 0x57111000u + (uint32_t)t);
						if (deg.jpeg > 0 && !JpegRoundTrip(factory, source, sw, sh, deg.jpeg, error)) {
							ok = false;
							break;
						}

						// The guard band: the source mirrored into it on every side.
						const std::vector<float>* pInput = &source;
						if (p) {
							padded.resize(4 * (size_t)iw * ih);
							for (int yy = 0; yy < ih; yy++) {
								float sy = 1;
								const int ys = Mirror(yy - p, sh, sy);
								for (int xx = 0; xx < iw; xx++) {
									float sx = 1;
									const int xs = Mirror(xx - p, sw, sx);
									std::copy_n(&source[4 * ((size_t)ys * sw + xs)], 4, &padded[4 * ((size_t)yy * iw + xx)]);
								}
							}
							pInput = &padded;
						}
						UploadRgba(ctx, sr.GetInput()->pTexture, *pInput, iw);

						ID3D11Texture2D* pMotion = nullptr;
						if (row.motion == 0) {
							// Calibration: noise per 8 x 8 block, blended between block centres as
							// PASS 1 blends Optical Flow's, and one offset for the whole picture.
							const int nbw = sw / 8 + 2, nbh = sh / 8 + 2;
							std::vector<float> blockNoise;
							float jx = 0, jy = 0;
							if (row.noise > 0 || row.jitter > 0) {
								Rng rng(0x0B5E0000u + (uint32_t)t);
								blockNoise.resize(2 * (size_t)nbw * nbh);
								for (float& v : blockNoise) {
									v = rng.Gauss() * row.noise;
								}
								jx = rng.Gauss() * row.jitter;
								jy = rng.Gauss() * row.jitter;
							}
							motion.resize(2 * (size_t)iw * ih);
							for (int yy = 0; yy < ih; yy++) {
								float sy = 1;
								const int ys = Mirror(yy - p, sh, sy);
								for (int xx = 0; xx < iw; xx++) {
									float sx = 1;
									const int xs = Mirror(xx - p, sw, sx);
									float mx = 0, my = 0;
									truth(t, xs, ys, mx, my);
									if (row.blocks) {
										// The truth at the four block centres around, blended.
										const float bx = (xs + 0.5f - 4.0f) / 8.0f, by = (ys + 0.5f - 4.0f) / 8.0f;
										const int b0x = (int)std::floor(bx), b0y = (int)std::floor(by);
										const float tx = bx - b0x, ty = by - b0y;
										mx = my = 0;
										for (int j = 0; j < 2; j++) {
											for (int i = 0; i < 2; i++) {
												const int cx = std::clamp(8 * (b0x + i) + 4, 0, sw - 1), cy = std::clamp(8 * (b0y + j) + 4, 0, sh - 1);
												float ux = 0, uy = 0;
												truth(t, cx, cy, ux, uy);
												const float w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
												mx += w * ux;
												my += w * uy;
											}
										}
									}
									if (!blockNoise.empty()) {
										const float bx = (xs + 0.5f - 4.0f) / 8.0f, by = (ys + 0.5f - 4.0f) / 8.0f;
										const int b0x = std::clamp((int)std::floor(bx), 0, nbw - 2), b0y = std::clamp((int)std::floor(by), 0, nbh - 2);
										const float tx = std::clamp(bx - b0x, 0.0f, 1.0f), ty = std::clamp(by - b0y, 0.0f, 1.0f);
										for (int k = 0; k < 2; k++) {
											const float a = blockNoise[2 * ((size_t)b0y * nbw + b0x) + k], b = blockNoise[2 * ((size_t)b0y * nbw + b0x + 1) + k];
											const float c = blockNoise[2 * ((size_t)(b0y + 1) * nbw + b0x) + k], d = blockNoise[2 * ((size_t)(b0y + 1) * nbw + b0x + 1) + k];
											const float v = (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
											(k ? my : mx) += v;
										}
										mx += jx;
										my += jy;
									}
									motion[2 * ((size_t)yy * iw + xx)] = sx * mx;
									motion[2 * ((size_t)yy * iw + xx) + 1] = sy * my;
								}
							}
							motionHalf.resize(motion.size());
							DirectX::PackedVector::XMConvertFloatToHalfStream(motionHalf.data(), sizeof(HALF), motion.data(), sizeof(float), motion.size());
							ctx->UpdateSubresource(exactMotion.pTexture, 0, nullptr, motionHalf.data(), 2 * sizeof(HALF) * iw, 0);
							pMotion = exactMotion.pTexture;
						} else if (row.motion == 1) {
							flow.PrepareMotion(ctx, sr.GetInput()->pShaderResource);
							pMotion = flow.GetMotionVectors();
						}
						ok = sr.Evaluate(pMotion, 41.7f);
						if (!ok) {
							break;
						}

						const bool measured = isStill(t) || t == 24 || isObject(t) || isSlow(t) || isPan(t)
							|| isObjectRun(t) || isPanRun(t);
						if (!measured) {
							continue;
						}
						ok = ReadTexture(dev, ctx, sr.GetOutput()->pTexture, out);
						if (!ok) {
							break;
						}
						// The picture without the guard band.
						outCrop.resize(4 * (size_t)cw * ch);
						for (int yy = 0; yy < ch; yy++) {
							std::copy_n(&out[4 * ((size_t)(yy + 2 * p) * 2 * iw + 2 * p)], 4 * (size_t)cw, &outCrop[4 * (size_t)yy * cw]);
						}

						// Optical Flow's own blocks inside the object, before anything is done to them.
						if (isObject(t) && row.motion == 1) {
							const CDlssOpticalFlow& of = flow.GetFlow();
							std::vector<BYTE> raw;
							int fw = 0, fh = 0;
							D3D11_TEXTURE2D_DESC sd = {};
							of.ForwardFlowTexture()->GetDesc(&sd);
							fw = (int)sd.Width;
							fh = (int)sd.Height;
							sd.Usage = D3D11_USAGE_STAGING;
							sd.BindFlags = 0;
							sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
							sd.MiscFlags = 0;
							CComPtr<ID3D11Texture2D> stage;
							D3D11_MAPPED_SUBRESOURCE mr = {};
							std::vector<int> objectBlocks;
							if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
								ctx->CopyResource(stage, of.ForwardFlowTexture());
								if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
									int opx = 0, opy = 0;
									objectAt(t, opx, opy);
									const float perBlock = (float)flow.GetFlowFactor() * of.GridSize();   // source pixels per block
									for (int by = 0; by < fh; by++) {
										for (int bx = 0; bx < fw; bx++) {
											const float cx = (bx + 0.5f) * perBlock + p * 0.0f, cy = (by + 0.5f) * perBlock;
											const float ox = opx / 2.0f + 8.0f, oy = opy / 2.0f + 8.0f, os = kPatch / 2.0f - 16.0f;
											if (cx >= ox && cx < ox + os && cy >= oy && cy < oy + os) {
												const int16_t* v = (const int16_t*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * by) + 2 * bx;
												rawX.push_back(v[0] / 32.0f * flow.GetFlowFactor());
												rawY.push_back(v[1] / 32.0f * flow.GetFlowFactor());
												objectBlocks.push_back(by * fw + bx);
											}
										}
									}
									ctx->Unmap(stage, 0);
								}
							}
							// And how much of their own vector PASS 4 kept.
							if (flow.GetBlockField() && !objectBlocks.empty()) {
								D3D11_TEXTURE2D_DESC fd = {};
								flow.GetBlockField()->GetDesc(&fd);
								fd.Usage = D3D11_USAGE_STAGING;
								fd.BindFlags = 0;
								fd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
								fd.MiscFlags = 0;
								CComPtr<ID3D11Texture2D> fieldStage;
								D3D11_MAPPED_SUBRESOURCE fm = {};
								if (SUCCEEDED(dev->CreateTexture2D(&fd, nullptr, &fieldStage))) {
									ctx->CopyResource(fieldStage, flow.GetBlockField());
									if (SUCCEEDED(ctx->Map(fieldStage, 0, D3D11_MAP_READ, 0, &fm))) {
										for (const int i : objectBlocks) {
											const float* f = (const float*)((const BYTE*)fm.pData + (size_t)fm.RowPitch * (i / fw)) + 4 * (i % fw);
											objectKeep.push_back(f[2]);
										}
										ctx->Unmap(fieldStage, 0);
									}
								}
							}
						}

						// PASS 4's blocks by texture, on the still frame 39 and inside the object.
						if (row.motion == 1 && (t == 39 || isObject(t)) && flow.GetBlockField()) {
							const CDlssOpticalFlow& of = flow.GetFlow();
							std::vector<BYTE> fieldRaw, nowRaw;
							int ow = 0, oh = 0, pw = 0, ph = 0;
							if (ReadRaw(dev, ctx, flow.GetBlockField(), 16, fieldRaw, ow, oh)
								&& ReadRaw(dev, ctx, of.CurrentFrameTexture(), 1, nowRaw, pw, ph)) {
								const float* field = (const float*)fieldRaw.data();
								const int grid = (int)of.GridSize();
								const float perBlock = (float)flow.GetFlowFactor() * grid;   // source pixels per block
								int opx = 0, opy = 0;
								const bool inObject = isObject(t) && objectAt(t, opx, opy);
								for (int by = 0; by < oh; by++) {
									for (int bx = 0; bx < ow; bx++) {
										if (inObject) {
											const float cx = (bx + 0.5f) * perBlock, cy = (by + 0.5f) * perBlock;
											const float ox = opx / 2.0f + 8.0f, oy = opy / 2.0f + 8.0f, os = kPatch / 2.0f - 16.0f;
											if (!(cx >= ox && cx < ox + os && cy >= oy && cy < oy + os)) {
												continue;
											}
										}
										const int x0 = (int)((bx + 0.5f) * grid) - 4, y0 = (int)((by + 0.5f) * grid) - 4;
										double m = 0, q = 0;
										for (int y = 0; y < 8; y++) {
											for (int x = 0; x < 8; x++) {
												const double a = nowRaw[(size_t)std::clamp(y0 + y, 0, ph - 1) * pw + std::clamp(x0 + x, 0, pw - 1)] / 255.0;
												m += a;
												q += a * a;
											}
										}
										m /= 64.0;
										const double deviation = std::sqrt(std::max(q / 64.0 - m * m, 0.0));
										const int bin = deviation < 0.005 ? 0 : deviation < 0.01 ? 1 : deviation < 0.02 ? 2 : deviation < 0.04 ? 3 : 4;
										const float* f = &field[4 * ((size_t)by * ow + bx)];
										if (inObject) {
											r.objectBlocks[bin] += 1;
											r.objectKept[bin] += f[2];
										} else {
											r.stillBlocks[bin] += 1;
											r.stillKept[bin] += f[2];
											r.stillMotion[bin] += f[2] * std::hypot(f[0], f[1]) * flow.GetFlowFactor();
										}
									}
								}
							}
						}

						// The vectors DLSS got on the object: inside it, a block and more from its
						// edge, and along the edge, a block on either side.
						if (isObject(t)) {
							int mw = 0, mh = 0;
							motion.clear();
							if (pMotion) {
								ok = ReadMotion(dev, ctx, pMotion, motion, mw, mh);
							}
							int opx = 0, opy = 0;
							objectAt(t, opx, opy);
							const int ox0 = opx / 2, oy0 = opy / 2, osz = kPatch / 2;   // source pixels
							for (int yy = std::max(0, oy0 - 8); ok && yy < std::min(sh, oy0 + osz + 8); yy++) {
								for (int xx = std::max(0, ox0 - 8); xx < std::min(sw, ox0 + osz + 8); xx++) {
									const int inside = std::min({ xx - ox0, ox0 + osz - 1 - xx, yy - oy0, oy0 + osz - 1 - yy });
									float tx = 0, ty = 0;
									truth(t, xx, yy, tx, ty);
									float gx = 0, gy = 0;
									if (!motion.empty()) {
										gx = motion[2 * ((size_t)(yy + p) * mw + xx + p)];
										gy = motion[2 * ((size_t)(yy + p) * mw + xx + p) + 1];
									}
									const double e2 = (gx - tx) * (gx - tx) + (gy - ty) * (gy - ty);
									if (inside >= 8) {
										r.mvObjectIn += e2;
										r.nObjectIn += 1;
										objectErrors.push_back((float)std::sqrt(e2));
									} else {
										r.mvObjectEdge += e2;
										r.nObjectEdge += 1;
									}
								}
							}
						}

						// The vectors DLSS got, against the truth, in the picture proper.
						if (isStill(t) || isSlow(t) || isPan(t)) {
							int mw = 0, mh = 0;
							motion.clear();
							if (pMotion) {
								ok = ReadMotion(dev, ctx, pMotion, motion, mw, mh);
							}
							for (int yy = 0; ok && yy < sh; yy++) {
								for (int xx = 0; xx < sw; xx++) {
									float tx = 0, ty = 0;
									truth(t, xx, yy, tx, ty);
									float gx = 0, gy = 0;
									if (!motion.empty()) {
										gx = motion[2 * ((size_t)(yy + p) * mw + xx + p)];
										gy = motion[2 * ((size_t)(yy + p) * mw + xx + p) + 1];
									}
									const double e2 = (gx - tx) * (gx - tx) + (gy - ty) * (gy - ty);
									(isStill(t) ? r.mvStill : isSlow(t) ? r.mvSlow : r.mvPan).Add(bandSrc[(size_t)yy * sw + xx], e2);
								}
							}
							if (row.motion == 1 && flow.GetGlobalMotion()) {
								float gm[4] = {};
								if (ReadGlobalMotion(dev, ctx, flow.GetGlobalMotion(), gm)) {
									const float f = (float)flow.GetFlowFactor();
									float tx = 0, ty = 0;
									truth(t, sw / 2 + sw / 4, sh / 2 + sh / 4, tx, ty);   // off the object's path
									const double err = std::hypot(gm[0] * f - tx, gm[1] * f - ty);
									if (isStill(t)) {
										r.gmStill += err;
										r.spread += gm[2] * f;
										r.gmCount++;
										gmSumX += gm[0] * f;
										gmSumY += gm[1] * f;
										gmSumSq += (double)gm[0] * f * gm[0] * f + (double)gm[1] * f * gm[1] * f;
									} else if (isSlow(t)) {
										r.gmSlow += err / 5.0;
									} else {
										r.gmPan += err / 5.0;
									}
								}
							}
						}

						if (t == 24 || isStill(t)) {
							LumaBlocks(outCrop, cw, ch, y, blocks);
							if (isStill(t) && !yPrev.empty()) {
								const int bw = cw / 4;
								for (int by = 0; by < ch / 4; by++) {
									for (int bx = 0; bx < bw; bx++) {
										const size_t bi = (size_t)by * bw + bx;
										r.wob.Add(bandOut[(size_t)(4 * by + 2) * cw + 4 * bx + 2], 1000.0 * std::abs(blocks[bi] - blocksPrev[bi]));
										for (int j = 0; j < 4; j++) {
											for (int i = 0; i < 4; i++) {
												const int px = 4 * bx + i, py = 4 * by + j;
												const size_t pi = (size_t)py * cw + px;
												const double hp = (y[pi] - blocks[bi]) - (yPrev[pi] - blocksPrev[bi]);
												r.flick.Add(bandOut[pi], 1000.0 * std::abs(hp));
											}
										}
									}
								}
							}
							std::swap(y, yPrev);
							std::swap(blocks, blocksPrev);
							if (t == 39) {
								// The grain left, and the still picture's psnr.
								r.psnrStill = LumaPsnr(outCrop, crop, cw, 0, 0, cw, ch);
								LumaBlocks(crop, cw, ch, yRef, blocksRef);
								const int bw = cw / 4;
								for (int py = 0; py < ch; py++) {
									for (int px = 0; px < cw; px++) {
										const size_t pi = (size_t)py * cw + px, bi = (size_t)(py / 4) * bw + px / 4;
										const double d = (yPrev[pi] - blocksPrev[bi]) - (yRef[pi] - blocksRef[bi]);
										r.resid.Add(bandOut[pi], 1000.0 * std::abs(d));
									}
								}
							}
						}

						if (isObject(t)) {
							int px = 0, py = 0;
							objectAt(t, px, py);
							psnrObject += LumaPsnr(outCrop, crop, cw, px, py, px + kPatch, py + kPatch);
							nObject++;
						}

						// What moves, against the frame before: the object moved kObjectDx x kObjectDy
						// output pixels, the pan's content 7 x 3 the other way.
						if (isObjectRun(t) || isPanRun(t)) {
							yNow.resize((size_t)cw * ch);
							for (size_t i = 0; i < yNow.size(); i++) {
								yNow[i] = Luma(&outCrop[4 * i]);
							}
							if (tLast == t - 1 && isObjectRun(t) && t > 45) {
								int px = 0, py = 0;
								objectAt(t, px, py);
								const int m = 16;   // away from the edges it uncovers and covers
								objectSteady.Add(yNow, yLast, cw, px + m, py + m, px + kPatch - m, py + kPatch - m, -kObjectDx, -kObjectDy);
							}
							if (tLast == t - 1 && isPanRun(t) && t > 89) {
								int dx = 0, dy = 0;
								stepAt(t, dx, dy);
								panSteady.Add(yNow, yLast, cw, 64, 64, cw - 64 - dx, ch - 64 - dy, dx, dy);
							}
							std::swap(yNow, yLast);
							tLast = t;
						}
						if (isSlow(t) || isPan(t)) {
							const double psnr = LumaPsnr(outCrop, crop, cw, 0, 0, cw, ch);
							if (isSlow(t)) {
								psnrSlow += psnr;
								nSlow++;
							} else {
								psnrPan += psnr;
								nPan++;
							}
						}
					}

					if (!ok || !nObject || !nSlow || !nPan) {
						Out("  %-26s failed -- %S\n", row.name, sr.GetStatusLine().c_str());
						g_failures++;
						continue;
					}
					r.psnrObject = psnrObject / nObject;
					if (!objectErrors.empty()) {
						auto percentile = [&](double q) {
							const size_t k = std::min(objectErrors.size() - 1, (size_t)(q * objectErrors.size()));
							std::nth_element(objectErrors.begin(), objectErrors.begin() + k, objectErrors.end());
							return (double)objectErrors[k];
						};
						r.mvObjectMedian = percentile(0.5);
						r.mvObjectP90 = percentile(0.9);
					}
					r.objWob = objectSteady.Wob();
					r.objFlick = objectSteady.Flick();
					r.panWob = panSteady.Wob();
					r.panFlick = panSteady.Flick();
					r.psnrSlow = psnrSlow / nSlow;
					r.psnrPan = psnrPan / nPan;
					if (r.gmCount) {
						const double mx = gmSumX / r.gmCount, my = gmSumY / r.gmCount;
						r.gmBias = std::hypot(mx, my);
						r.gmJitter = std::sqrt(std::max(0.0, gmSumSq / r.gmCount - mx * mx - my * my));
						r.gmStill /= r.gmCount;
						r.spread /= r.gmCount;
						r.gmCount = 1;
					}
					auto rms = [](const Bands& b) { return std::sqrt(b.Centre()); };
					Out("  %-26s %7.3f %6.3f %6.2f %6.2f %7.3f | %7.3f %7.3f %7.3f %7.3f | %6.3f %6.3f %6.3f %6.3f %6.3f\n", row.name,
					    r.psnrStill, r.wob.Centre(), r.flick.Centre(), r.resid.Centre(), rms(r.mvStill),
					    r.psnrObject, r.psnrSlow, r.psnrPan, rms(r.mvPan),
					    r.gmCount ? r.gmStill : 0.0, r.gmCount ? r.gmPan : 0.0, r.gmCount ? r.spread : 0.0,
					    r.gmCount ? r.gmBias : 0.0, r.gmCount ? r.gmJitter : 0.0);
					if (!rawX.empty()) {
						// The truth is (-2.5, -1.5): the share near it, near zero, and elsewhere.
						size_t nearTruth = 0, nearZero = 0;
						double sx = 0, sy = 0;
						for (size_t i = 0; i < rawX.size(); i++) {
							sx += rawX[i];
							sy += rawY[i];
							nearTruth += std::hypot(rawX[i] + 2.5f, rawY[i] + 1.5f) < 0.5f;
							nearZero += std::hypot(rawX[i], rawY[i]) < 0.5f;
						}
						std::vector<float> mxs = rawX, mys = rawY;
						std::nth_element(mxs.begin(), mxs.begin() + mxs.size() / 2, mxs.end());
						std::nth_element(mys.begin(), mys.begin() + mys.size() / 2, mys.end());
						double keepSum = 0;
						size_t keepFull = 0, keepNone = 0;
						for (const float k : objectKeep) {
							keepSum += k;
							keepFull += k > 0.9f;
							keepNone += k < 0.1f;
						}
						Out("  %-26s engine on the object: mean (%+.2f, %+.2f), median (%+.2f, %+.2f), %.0f %% within 0.5 px of the truth, %.0f %% near zero\n", "",
						    sx / rawX.size(), sy / rawX.size(), mxs[mxs.size() / 2], mys[mys.size() / 2],
						    100.0 * nearTruth / rawX.size(), 100.0 * nearZero / rawX.size());
						if (!objectKeep.empty()) {
							Out("  %-26s kept on the object: mean %.2f, %.0f %% whole, %.0f %% none\n", "",
							    keepSum / objectKeep.size(), 100.0 * keepFull / objectKeep.size(), 100.0 * keepNone / objectKeep.size());
						}
					}
					Out("  %-26s moving: object wob %6.3f flick %6.3f, pan wob %6.3f flick %6.3f, object mv in %6.3f edge %6.3f, slow pan mv %6.3f gm %6.3f\n", "",
					    r.objWob, r.objFlick, r.panWob, r.panFlick,
					    r.nObjectIn ? std::sqrt(r.mvObjectIn / r.nObjectIn) : 0.0, r.nObjectEdge ? std::sqrt(r.mvObjectEdge / r.nObjectEdge) : 0.0,
					    std::sqrt(r.mvSlow.Centre()), r.gmSlow);
					PrintTextureBins(r, "");
					totals[di][row.name].Add(r);
				}
			}
		}

		// The mean over the references, and the edges band by band.
		for (size_t di = 0; !rc && di < std::size(degradations); di++) {
			Out("\n=== mean over %d reference(s), %s\n", refsDone, degradations[di].name);
			Out("  %-26s %7s %6s %6s %6s %7s | %7s %7s %7s %7s | %6s %6s %6s %6s %6s\n", "method",
			    "psnr", "wob", "flick", "resid", "mv", "object", "slow", "pan", "mv pan", "gm", "gm pan", "spread", "bias", "jitter");
			for (const StillRow& row : rows) {
				const auto it = totals[di].find(row.name);
				if (it == totals[di].end() || !it->second.n) {
					continue;
				}
				const StillResult& s = it->second;
				const double n = s.n;
				Out("  %-26s %7.3f %6.3f %6.2f %6.2f %7.3f | %7.3f %7.3f %7.3f %7.3f | %6.3f %6.3f %6.3f %6.3f %6.3f\n", row.name,
				    s.psnrStill / n, s.wob.Centre(), s.flick.Centre(), s.resid.Centre(), std::sqrt(s.mvStill.Centre()),
				    s.psnrObject / n, s.psnrSlow / n, s.psnrPan / n, std::sqrt(s.mvPan.Centre()),
				    s.gmCount ? s.gmStill / s.gmCount : 0.0, s.gmCount ? s.gmPan / s.gmCount : 0.0, s.gmCount ? s.spread / s.gmCount : 0.0,
				    s.gmCount ? s.gmBias / s.gmCount : 0.0, s.gmCount ? s.gmJitter / s.gmCount : 0.0);
			}
			Out("\n  what moves, against the frame before at the place it came from (x1000)\n");
			Out("  %-26s %10s %10s %10s %10s %10s %10s %10s %10s %10s %10s\n", "", "object wob", "flick", "pan wob", "flick", "mv object", "median", "p90", "mv edge",
			    "mv slow", "gm slow");
			for (const StillRow& row : rows) {
				const auto it = totals[di].find(row.name);
				if (it == totals[di].end() || !it->second.n) {
					continue;
				}
				const StillResult& s = it->second;
				Out("  %-26s %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f\n", row.name,
				    s.objWob / s.n, s.objFlick / s.n, s.panWob / s.n, s.panFlick / s.n,
				    s.nObjectIn ? std::sqrt(s.mvObjectIn / s.nObjectIn) : 0.0, s.mvObjectMedian / s.n, s.mvObjectP90 / s.n,
				    s.nObjectEdge ? std::sqrt(s.mvObjectEdge / s.nObjectEdge) : 0.0, std::sqrt(s.mvSlow.Centre()), s.gmSlow / s.n);
			}
			Out("\n");
			for (const StillRow& row : rows) {
				const auto it = totals[di].find(row.name);
				if (it != totals[di].end() && it->second.n) {
					PrintTextureBins(it->second, row.name);
				}
			}
			const struct { const char* title; const Bands StillResult::* bands; bool root; } profiles[] = {
				{ "resid, the grain left, by distance from the edge (source pixels)", &StillResult::resid, false },
				{ "flick, the grain changing, by distance from the edge", &StillResult::flick, false },
				{ "wob, the shimmer, by distance from the edge", &StillResult::wob, false },
				{ "mv, the vectors' RMS error on the still frames, by distance from the edge", &StillResult::mvStill, true },
				{ "mv pan, the same on the pan", &StillResult::mvPan, true },
			};
			for (const auto& profile : profiles) {
				Out("\n  %s\n  %-26s", profile.title, "");
				for (int b = 0; b < kBands; b++) {
					Out(" %7s", kBandNames[b]);
				}
				Out("\n");
				for (const StillRow& row : rows) {
					const auto it = totals[di].find(row.name);
					if (it == totals[di].end() || !it->second.n) {
						continue;
					}
					Out("  %-26s", row.name);
					const Bands& bands = it->second.*profile.bands;
					for (int b = 0; b < kBands; b++) {
						Out(" %7.3f", profile.root ? std::sqrt(bands.At(b)) : bands.At(b));
					}
					Out("\n");
				}
			}
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_srstill.txt\n");
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

// --tsrport: the filter's vectors for DLSS SR -- cs_dlss_global_motion.hlsl and
// cs_dlss_global_refine.hlsl, then PASS 4 and PASS 3 of ps_dlss_stabilize.hlsl, as
// CDlssStabilizer::ForDlssSR runs them -- against the same computed here, on the CPU,
// from the engine's own output of the same frames: the median and its spread must
// match exactly, the refined global motion to float precision, the vectors to the
// precision of their RG16F texture. On a still, grained pair of frames, a panned one
// and a zoomed one.

namespace {

float SmoothstepF(float e0, float e1, float x)
{
	const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

int LowerMedian(std::vector<int> v)
{
	const size_t k = (v.size() + 1) / 2 - 1;
	std::nth_element(v.begin(), v.begin() + k, v.end());
	return v[k];
}

// cs_dlss_global_refine.hlsl on the CPU, from the same flow frames (R8) and forward
// flow (1/32 flow pixel): Gauss-Newton steps from g, flow pixels, each over the pixels
// whose block vector lies within `near` of the estimate, the 16 x 16 groups whose mean
// squared difference passes `robust` times the median group's left out. The sums are in
// double where the GPU's are in float, so the two agree to a little more than float
// precision, and a group that sits on the limit may fall on either side of it.
void RefineGlobalCpu(const BYTE* now, const BYTE* last, int w, int h, const int16_t* fwd, int fw, int fh, int grid,
                     int steps, float near_, float robust, float g[2])
{
	const int gw = (w + 15) / 16, gh = (h + 15) / 16;
	auto lastAt = [&](float x, float y) {
		const float fx = x - 0.5f, fy = y - 0.5f;
		const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
		const float tx = fx - ix, ty = fy - iy;
		auto at = [&](int xx, int yy) { return last[(size_t)std::clamp(yy, 0, h - 1) * w + std::clamp(xx, 0, w - 1)] / 255.0f; };
		const float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
		const float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
		return top + (bottom - top) * ty;
	};
	for (int step = 0; step < steps; step++) {
		std::vector<double> A(4 * (size_t)gw * gh, 0.0), B(3 * (size_t)gw * gh, 0.0);
		for (int y = 2; y <= h - 3; y++) {
			for (int x = 2; x <= w - 3; x++) {
				const int bx = std::min(x / grid, fw - 1), by = std::min(y / grid, fh - 1);
				const float ox = fwd[2 * ((size_t)by * fw + bx)] / 32.0f, oy = fwd[2 * ((size_t)by * fw + bx) + 1] / 32.0f;
				const float qx = x + 0.5f + g[0], qy = y + 0.5f + g[1];
				if (std::hypot(ox - g[0], oy - g[1]) > near_ || qx < 2.0f || qy < 2.0f || qx > w - 2.0f || qy > h - 2.0f) {
					continue;
				}
				const double gx = (lastAt(qx + 1, qy) - lastAt(qx - 1, qy)) * 0.5, gy = (lastAt(qx, qy + 1) - lastAt(qx, qy - 1)) * 0.5;
				const double e = now[(size_t)y * w + x] / 255.0f - lastAt(qx, qy);
				const size_t k = (size_t)(y / 16) * gw + x / 16;
				A[4 * k] += gx * gx;
				A[4 * k + 1] += gx * gy;
				A[4 * k + 2] += gy * gy;
				A[4 * k + 3] += 1.0;
				B[3 * k] += gx * e;
				B[3 * k + 1] += gy * e;
				B[3 * k + 2] += e * e;
			}
		}
		// The median group, by the shader's histogram: 6.4 bins an octave from 2^-40.
		double limit = 3.402823e38;
		if (robust > 0.0f) {
			std::vector<int> hist(256, 0);
			int counted = 0;
			for (size_t k = 0; k < (size_t)gw * gh; k++) {
				if (A[4 * k + 3] >= 64.0 && A[4 * k] + A[4 * k + 2] > 1e-6 * A[4 * k + 3]) {
					const double r = B[3 * k + 2] / A[4 * k + 3];
					hist[std::clamp((int)((std::log2(std::max(r, 1e-12)) + 40.0) * 6.4), 0, 255)]++;
					counted++;
				}
			}
			if (counted) {
				const int rank = (counted + 1) / 2;
				int below = 0, bin = 0;
				while (bin < 255 && below + hist[bin] < rank) {
					below += hist[bin];
					bin++;
				}
				limit = robust * std::exp2((bin + 0.5) / 6.4 - 40.0);
			}
		}
		double a[4] = {}, b[2] = {};
		for (size_t k = 0; k < (size_t)gw * gh; k++) {
			if (A[4 * k + 3] > 0.0 && B[3 * k + 2] < limit * A[4 * k + 3]) {
				for (int i = 0; i < 4; i++) {
					a[i] += A[4 * k + i];
				}
				b[0] += B[3 * k];
				b[1] += B[3 * k + 1];
			}
		}
		const double det = a[0] * a[2] - a[1] * a[1];
		const double tr = a[0] + a[2];
		const double minEigen = 0.5 * (tr - std::sqrt(std::max(tr * tr - 4.0 * det, 0.0)));
		if (a[3] >= 2000.0 && minEigen > 1e-6 * a[3]) {
			double sx = (a[2] * b[0] - a[1] * b[1]) / det, sy = (a[0] * b[1] - a[1] * b[0]) / det;
			const double len = std::hypot(sx, sy);
			if (len > 0.5) {
				sx *= 0.5 / len;
				sy *= 0.5 / len;
			}
			g[0] += (float)sx;
			g[1] += (float)sy;
		}
	}
}

} // namespace

static int RunSRPort(ID3D11Device* dev, ID3D11DeviceContext* ctx, int maxRefs)
{
	Head("DLSS SR vectors: the filter's global motion and snapping against the CPU");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
		std::string error;
		CStabilizerPasses stabPasses;
		if (!factory || !stabPasses.Init(dev, error)) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}

		std::vector<std::wstring> files;
		WIN32_FIND_DATAW fd = {};
		HANDLE h = FindFirstFileW(L"upscale_refs\\*.png", &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				files.push_back(std::wstring(L"upscale_refs\\") + fd.cFileName);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		std::sort(files.begin(), files.end());

		// What the filter runs, and the same with both smoothings on, so that PASS 4 is
		// checked whatever the settings. The second keeps the median as it comes out, and
		// the dead zone the filter had until 2.0, so that the median is checked exactly
		// and a global motion taken to zero is checked too.
		CDlssStabilizer::FlowSettings smoothed = CDlssStabilizer::ForDlssSR();
		smoothed.vectorRadius = 2;
		smoothed.vectorSigma = 0.75f;
		smoothed.vectorTemporal = 0.5f;
		smoothed.vectorShift = 0;
		smoothed.vectorRefine = 0;
		smoothed.globalRefine = 0;
		smoothed.snapDeadZone = 0.4f;
		const CDlssStabilizer::FlowSettings settingsList[] = { CDlssStabilizer::ForDlssSR(), smoothed };
		int refsDone = 0;
		for (size_t fi = 0; !rc && fi < files.size() && refsDone < (maxRefs > 0 ? maxRefs : 2); fi++) {
			UpscaleRef ref;
			if (!LoadReference(factory, files[fi].c_str(), ref, error) || ref.W < 3840) {
				continue;
			}
			refsDone++;
			const int cw = (ref.W - 16) & ~7, ch = (ref.H - 16) & ~7;
			const int sw = cw / 2, sh = ch / 2;
			printf("  %S, %dx%d\n", files[fi].substr(files[fi].find_last_of(L'\\') + 1).c_str(), sw, sh);

			for (const CDlssStabilizer::FlowSettings& settings : settingsList) {
			printf("   settings: radius %d, sigma %.2f, temporal %.2f, shift %d, refine %d; global motion refined %d, robust %.1f,"
			       " dead zone %.1f\n", settings.vectorRadius, settings.vectorSigma, settings.vectorTemporal, settings.vectorShift,
			       settings.vectorRefine, settings.globalRefine, settings.globalRobust, settings.snapDeadZone);
			Tex2D_t input;
			CDlssStabilizer flow;
			const bool made = SUCCEEDED(input.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, sw, sh, Tex2D_DefaultShaderRTarget))
				&& SUCCEEDED(flow.Create(dev, ctx, sw, sh,
					stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear(),
					true, settings))
				&& flow.HasFlow() && flow.GetGlobalMotion();
			Check(made, "motion-only Optical Flow with the snapping");
			if (!made) {
				continue;
			}

			std::vector<float> crop(4 * (size_t)cw * ch), source;
			auto frame = [&](int ox, int oy, float zoom, uint32_t seed) {
				if (zoom == 1.0f) {
					for (int y = 0; y < ch; y++) {
						std::copy_n(&ref.rgba[4 * ((size_t)(oy + y) * ref.W + ox)], 4 * (size_t)cw, &crop[4 * (size_t)y * cw]);
					}
				} else {
					// Zoomed about the window's centre: every block moves its own way.
					const float cx = ox + cw * 0.5f, cy = oy + ch * 0.5f;
					for (int y = 0; y < ch; y++) {
						for (int x = 0; x < cw; x++) {
							const float sx = cx + (x + 0.5f - cw * 0.5f) / zoom - 0.5f, sy = cy + (y + 0.5f - ch * 0.5f) / zoom - 0.5f;
							const int ix = std::clamp((int)std::floor(sx), 0, ref.W - 2), iy = std::clamp((int)std::floor(sy), 0, ref.H - 2);
							const float tx = std::clamp(sx - ix, 0.0f, 1.0f), ty = std::clamp(sy - iy, 0.0f, 1.0f);
							for (int k = 0; k < 4; k++) {
								const float p00 = ref.rgba[4 * ((size_t)iy * ref.W + ix) + k], p10 = ref.rgba[4 * ((size_t)iy * ref.W + ix + 1) + k];
								const float p01 = ref.rgba[4 * ((size_t)(iy + 1) * ref.W + ix) + k], p11 = ref.rgba[4 * ((size_t)(iy + 1) * ref.W + ix + 1) + k];
								const float top = p00 + (p10 - p00) * tx, bottom = p01 + (p11 - p01) * tx;
								crop[4 * ((size_t)y * cw + x) + k] = top + (bottom - top) * ty;
							}
						}
					}
				}
				DownscaleBox(crop, cw, ch, 2, source);
				AddGrain(source, sw, sh, 0.02f, seed);
				UploadRgba(ctx, input.pTexture, source, sw);
				flow.PrepareMotion(ctx, input.pShaderResource);
			};

			const struct { const char* name; int dx, dy; float zoom; } cases[] = {
				{ "still, grained",  0, 0, 1.0f },
				{ "pan 3.5 x 1.5",   7, 3, 1.0f },
				{ "zoom 3 %",        0, 0, 1.03f },
			};
			for (const auto& c : cases) {
				flow.Reset();
				std::vector<float> lastField;   // the GPU's PASS 4 for the frame before
				for (int t = 0; t < 3; t++) {
					frame(c.dx * t, c.dy * t, std::pow(c.zoom, (float)t), 0x90470001u + (uint32_t)t);
					if (t == 0) {
						continue;   // no flow before a second frame
					}

					const CDlssOpticalFlow& of = flow.GetFlow();
					std::vector<BYTE> fwdRaw, bwdRaw, costRaw, gmRaw, motionRaw, fieldRaw, nowRaw, lastRaw;
					int fw = 0, fh = 0, bw = 0, bh = 0, cwd = 0, chd = 0, gw = 0, gh = 0, mw = 0, mh = 0, ow = 0, oh = 0;
					int pw = 0, ph = 0, qw = 0, qh = 0;
					bool ok = ReadRaw(dev, ctx, of.ForwardFlowTexture(), 4, fwdRaw, fw, fh)
						&& ReadRaw(dev, ctx, of.BackwardFlowTexture(), 4, bwdRaw, bw, bh)
						&& ReadRaw(dev, ctx, of.ForwardCostTexture(), 1, costRaw, cwd, chd)
						&& ReadRaw(dev, ctx, flow.GetGlobalMotion(), 16, gmRaw, gw, gh)
						&& ReadRaw(dev, ctx, flow.GetMotionVectors(), 4, motionRaw, mw, mh)
						&& flow.GetBlockField() && ReadRaw(dev, ctx, flow.GetBlockField(), 16, fieldRaw, ow, oh)
						&& ow == fw && oh == fh
						&& ReadRaw(dev, ctx, of.CurrentFrameTexture(), 1, nowRaw, pw, ph)
						&& ReadRaw(dev, ctx, of.PreviousFrameTexture(), 1, lastRaw, qw, qh);
					Check(ok, "read the engine's output and the filter's");
					if (!ok) {
						break;
					}
					const int16_t* fwd = (const int16_t*)fwdRaw.data();
					const int16_t* bwd = (const int16_t*)bwdRaw.data();
					const float* gpuField = (const float*)fieldRaw.data();
					float gpu[4] = {};
					memcpy(gpu, gmRaw.data(), sizeof(gpu));

					// The global motion: the lower median of each component over the trusted
					// blocks, then of their distances to it; none with too few of them.
					const float scale[2] = { (float)sw / of.Width(), (float)sh / of.Height() };
					const float grid = (float)of.GridSize();
					const float flowSize[2] = { (float)of.Width(), (float)of.Height() };
					auto outside = [&](float qx, float qy) { return qx < 0 || qy < 0 || qx > flowSize[0] || qy > flowSize[1]; };
					auto vectorAt = [&](int bx, int by, float& vx, float& vy) {
						vx = fwd[2 * ((size_t)by * fw + bx)] / 32.0f;
						vy = fwd[2 * ((size_t)by * fw + bx) + 1] / 32.0f;
					};
					// PASS 4's Confidence: 0 for a vector leaving the picture.
					auto confidenceOf = [&](int bx, int by, float vx, float vy) {
						const float qx = (bx + 0.5f) * grid + vx, qy = (by + 0.5f) * grid + vy;
						if (outside(qx, qy)) {
							return 0.0f;
						}
						const int bbx = std::clamp((int)std::floor(qx / grid), 0, fw - 1);
						const int bby = std::clamp((int)std::floor(qy / grid), 0, fh - 1);
						const float backX = bwd[2 * ((size_t)bby * bw + bbx)] / 32.0f, backY = bwd[2 * ((size_t)bby * bw + bbx) + 1] / 32.0f;
						float confidence = 1.0f - SmoothstepF(1.0f, 2.0f, std::hypot(vx + backX, vy + backY));
						confidence *= 1.0f - SmoothstepF(48.0f, 160.0f, (float)costRaw[(size_t)by * cwd + bx]);
						return confidence;
					};
					std::vector<int> xs, ys, ds;
					for (int by = 0; by < fh; by++) {
						for (int bx = 0; bx < fw; bx++) {
							float vx = 0, vy = 0;
							vectorAt(bx, by, vx, vy);
							if (confidenceOf(bx, by, vx, vy) >= 0.5f) {
								xs.push_back(fwd[2 * ((size_t)by * fw + bx)]);
								ys.push_back(fwd[2 * ((size_t)by * fw + bx) + 1]);
							}
						}
					}
					const size_t n = xs.size();
					const bool enough = n >= std::max<size_t>(64, (size_t)(0.05f * fw * fh));
					int mx = 0, my = 0, md = 0;
					if (n) {
						mx = LowerMedian(xs);
						my = LowerMedian(ys);
						for (size_t i = 0; i < n; i++) {
							const float dx = (float)xs[i] - mx, dy = (float)ys[i] - my;
							ds.push_back(std::min((int)std::lround(std::sqrt(dx * dx + dy * dy)), 255));
						}
						md = LowerMedian(ds);
					}
					float g[2] = { enough ? mx / 32.0f : 0.0f, enough ? my / 32.0f : 0.0f };
					const float spreadCpu = enough ? md / 32.0f : 0.0f;
					if (settings.globalRefine > 0) {
						// Then refined (cs_dlss_global_refine.hlsl): to float precision, the median's
						// spread and count exactly, which the refinement leaves alone.
						const float median[2] = { g[0], g[1] };
						RefineGlobalCpu(nowRaw.data(), lastRaw.data(), pw, ph, fwd, fw, fh, (int)grid, settings.globalRefine,
						                settings.globalNear, settings.globalRobust, g);
						const float apart = std::hypot(gpu[0] - g[0], gpu[1] - g[1]);
						printf("    %-16s frame %d: global motion GPU (%+.5f, %+.5f) spread %.5f of %.0f trusted, CPU median (%+.5f, %+.5f)"
						       " refined (%+.5f, %+.5f) spread %.5f of %zu, %.6f flow px apart\n",
						       c.name, t, gpu[0], gpu[1], gpu[2], gpu[3], median[0], median[1], g[0], g[1], spreadCpu, n, apart);
						Check(apart <= 1e-3f && gpu[2] == spreadCpu && gpu[3] == (float)n, "the global motion, refined, to float precision");
						g[0] = gpu[0];   // PASS 4 from the GPU's own, so that a rare decision taken
						g[1] = gpu[1];   // on the other side of a threshold stays local
					} else {
						const bool gmSame = gpu[0] == g[0] && gpu[1] == g[1] && gpu[2] == spreadCpu && gpu[3] == (float)n;
						printf("    %-16s frame %d: global motion GPU (%+.5f, %+.5f) spread %.5f of %.0f trusted, CPU (%+.5f, %+.5f) spread %.5f of %zu, %zu blocks\n",
						       c.name, t, gpu[0], gpu[1], gpu[2], gpu[3], g[0], g[1], spreadCpu, n, (size_t)fw * fh);
						Check(gmSame, "the global motion, exactly");
					}

					// PASS 4: each block's own vector, cleaned, and how much of it it keeps.
					float gs[2] = { g[0], g[1] };
					if (std::hypot(gs[0] * scale[0], gs[1] * scale[1]) < settings.snapDeadZone) {
						gs[0] = gs[1] = 0.0f;
					}
					std::vector<float> field(4 * (size_t)fw * fh);
					for (int by = 0; by < fh; by++) {
						for (int bx = 0; bx < fw; bx++) {
							float vx = 0, vy = 0;
							vectorAt(bx, by, vx, vy);
							const float cx = (bx + 0.5f) * grid, cy = (by + 0.5f) * grid;
							float keep = 0.0f;
							if (!(outside(cx + vx, cy + vy) && !outside(cx + gs[0], cy + gs[1]))) {
								keep = SmoothstepF(settings.snapLow, settings.snapHigh, std::hypot((vx - gs[0]) * scale[0], (vy - gs[1]) * scale[1]))
									* confidenceOf(bx, by, vx, vy);
							}
							float ox = vx, oy = vy;
							if (settings.vectorShift > 0) {
								// Mean shift over the window's trusted blocks, kept by the share of
								// the trust that agrees and by how much of it there is.
								keep = 0.0f;
								const int r = std::min(settings.vectorRadius, 3);
								std::vector<float> wx, wy, wt;
								float totalTrust = 0.0f;
								for (int j = -r; j <= r; j++) {
									for (int i = -r; i <= r; i++) {
										const int nx = bx + i, ny = by + j;
										if (nx < 0 || ny < 0 || nx >= fw || ny >= fh) {
											continue;   // beyond the grid: no block
										}
										float ux = 0, uy = 0;
										vectorAt(nx, ny, ux, uy);
										wx.push_back(ux);
										wy.push_back(uy);
										wt.push_back(confidenceOf(nx, ny, ux, uy));
										totalTrust += wt.back();
									}
								}
								float weights = 0.0f;
								for (int step = 0; step < settings.vectorShift; step++) {
									float sx = 0, sy = 0;
									weights = 0.0f;
									for (size_t m = 0; m < wx.size(); m++) {
										const float d = std::hypot((wx[m] - ox) * scale[0], (wy[m] - oy) * scale[1]) / settings.vectorSigma;
										const float w = wt[m] * std::exp(-d * d);
										sx += w * wx[m];
										sy += w * wy[m];
										weights += w;
									}
									if (weights > 1e-3f) {
										ox = sx / weights;
										oy = sy / weights;
									}
								}
								if (!(outside(cx + ox, cy + oy) && !outside(cx + gs[0], cy + gs[1]))) {
									keep = SmoothstepF(settings.snapLow, settings.snapHigh, std::hypot((ox - gs[0]) * scale[0], (oy - gs[1]) * scale[1]))
										* SmoothstepF(settings.vectorSupportLow, settings.vectorSupportHigh, weights / std::max(totalTrust, 1e-4f))
										* SmoothstepF(0.5f * settings.vectorEvidence, settings.vectorEvidence, weights);
								}
								// The kept vector searched again on the flow frames, where they have texture.
								if (keep > 0.0f && settings.vectorRefine > 0) {
									auto now = [&](int x, int y) { return nowRaw[(size_t)std::clamp(y, 0, ph - 1) * pw + std::clamp(x, 0, pw - 1)] / 255.0f; };
									auto lastAt = [&](float x, float y) {
										const float fx = x - 0.5f, fy = y - 0.5f;
										const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
										const float tx = fx - ix, ty = fy - iy;
										auto at = [&](int xx, int yy) { return lastRaw[(size_t)std::clamp(yy, 0, qh - 1) * qw + std::clamp(xx, 0, qw - 1)] / 255.0f; };
										const float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
										const float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
										return top + (bottom - top) * ty;
									};
									const int size = 2 * (int)settings.vectorReach;
									const int originX = (int)((bx + 0.5f) * grid) - (int)settings.vectorReach;
									const int originY = (int)((by + 0.5f) * grid) - (int)settings.vectorReach;
									auto mismatch = [&](float mvx, float mvy) {
										float sum = 0.0f;
										for (int y = 0; y < size; y++) {
											for (int x = 0; x < size; x++) {
												const int px = std::clamp(originX + x, 0, pw - 1), py = std::clamp(originY + y, 0, ph - 1);
												const float e = now(px, py) - lastAt(px + 0.5f + mvx, py + 0.5f + mvy);
												sum += e * e;
											}
										}
										return sum;
									};
									float mean = 0.0f, square = 0.0f;
									for (int y = 0; y < size; y++) {
										for (int x = 0; x < size; x++) {
											const float a = now(originX + x, originY + y);
											mean += a;
											square += a * a;
										}
									}
									const float count = (float)(size * size);
									mean /= count;
									if (std::sqrt(std::max(square / count - mean * mean, 0.0f)) >= settings.vectorTexture) {
										float bestX = ox, bestY = oy;
										float bestCost = mismatch(bestX, bestY);
										float step = 0.5f;
										for (int level = 0; level < settings.vectorRefine; level++) {
											const float centreX = bestX, centreY = bestY;
											for (int j = -1; j <= 1; j++) {
												for (int i = -1; i <= 1; i++) {
													if (i == 0 && j == 0) {
														continue;
													}
													const float candidateX = centreX + i * step, candidateY = centreY + j * step;
													const float cost = mismatch(candidateX, candidateY);
													if (cost < bestCost) {
														bestX = candidateX;
														bestY = candidateY;
														bestCost = cost;
													}
												}
											}
											step *= 0.5f;
										}
										ox = bestX;
										oy = bestY;
									}
								}
								if (keep > 0.0f && !lastField.empty() && settings.vectorTemporal < 1.0f) {
									const int px = std::clamp((int)std::floor((cx + ox) / grid), 0, fw - 1);
									const int py = std::clamp((int)std::floor((cy + oy) / grid), 0, fh - 1);
									const float* previous = &lastField[4 * ((size_t)py * fw + px)];
									if (previous[2] > 0.5f && std::hypot((previous[0] - ox) * scale[0], (previous[1] - oy) * scale[1]) < settings.snapHigh) {
										ox = previous[0] + (ox - previous[0]) * settings.vectorTemporal;
										oy = previous[1] + (oy - previous[1]) * settings.vectorTemporal;
									}
								}
							} else if (keep > 0.0f) {
								float sx = 0, sy = 0, weights = 0;
								for (int j = -settings.vectorRadius; j <= settings.vectorRadius; j++) {
									for (int i = -settings.vectorRadius; i <= settings.vectorRadius; i++) {
										const int nx = std::clamp(bx + i, 0, fw - 1), ny = std::clamp(by + j, 0, fh - 1);
										float ux = 0, uy = 0;
										vectorAt(nx, ny, ux, uy);
										const float d = std::hypot((ux - vx) * scale[0], (uy - vy) * scale[1]) / settings.vectorSigma;
										const float w = confidenceOf(nx, ny, ux, uy) * std::exp(-d * d);
										sx += w * ux;
										sy += w * uy;
										weights += w;
									}
								}
								if (weights > 0.0f) {
									ox = sx / weights;
									oy = sy / weights;
								}
								if (!lastField.empty() && settings.vectorTemporal < 1.0f) {
									const int px = std::clamp((int)std::floor((cx + ox) / grid), 0, fw - 1);
									const int py = std::clamp((int)std::floor((cy + oy) / grid), 0, fh - 1);
									const float* previous = &lastField[4 * ((size_t)py * fw + px)];
									if (previous[2] > 0.5f && std::hypot((previous[0] - ox) * scale[0], (previous[1] - oy) * scale[1]) < settings.snapHigh) {
										ox = previous[0] + (ox - previous[0]) * settings.vectorTemporal;
										oy = previous[1] + (oy - previous[1]) * settings.vectorTemporal;
									}
								}
							}
							float* f = &field[4 * ((size_t)by * fw + bx)];
							f[0] = ox;
							f[1] = oy;
							f[2] = keep;
							f[3] = 1.0f;
						}
					}
					size_t fieldOff = 0;
					double fieldWorst = 0;
					int shown = 0;
					for (size_t i = 0; i < (size_t)fw * fh; i++) {
						bool apart = false;
						for (int k = 0; k < 3; k++) {
							const double e = std::abs((double)gpuField[4 * i + k] - field[4 * i + k]);
							fieldWorst = std::max(fieldWorst, e);
							fieldOff += e > 1e-3 + 1e-4 * std::abs(field[4 * i + k]);
							apart |= e > 1e-3 + 1e-4 * std::abs(field[4 * i + k]);
						}
						if (apart && shown < 6) {
							shown++;
							float vx = 0, vy = 0;
							vectorAt((int)(i % fw), (int)(i / fw), vx, vy);
							printf("      block (%d, %d) raw (%+.3f, %+.3f): GPU (%+.4f, %+.4f, keep %.4f), CPU (%+.4f, %+.4f, keep %.4f)\n",
							       (int)(i % fw), (int)(i / fw), vx, vy, gpuField[4 * i], gpuField[4 * i + 1], gpuField[4 * i + 2],
							       field[4 * i], field[4 * i + 1], field[4 * i + 2]);
						}
					}
					printf("    %-16s frame %d: blocks: largest difference %.6f, %zu of %zu values apart\n",
					       c.name, t, fieldWorst, fieldOff, 3 * (size_t)fw * fh);
					Check(fieldOff <= (3 * (size_t)fw * fh) / 1000, "the blocks, to float precision");

					// PASS 3: the blocks blended into the working size, from the GPU's own blocks
					// so that a rare decision taken on the other side of a threshold stays local.
					double worst = 0;
					size_t beyond = 0;
					const HALF* motion = (const HALF*)motionRaw.data();
					for (int py = 0; py < mh; py++) {
						for (int px = 0; px < mw; px++) {
							const float qx = (px + 0.5f) / scale[0], qy = (py + 0.5f) / scale[1];
							const float bfx = qx / grid - 0.5f, bfy = qy / grid - 0.5f;
							const int b0x = (int)std::floor(bfx), b0y = (int)std::floor(bfy);
							const float tx = bfx - b0x, ty = bfy - b0y;
							float f[2] = {};
							for (int j = 0; j < 2; j++) {
								for (int i = 0; i < 2; i++) {
									const int bx = std::clamp(b0x + i, 0, fw - 1), by = std::clamp(b0y + j, 0, fh - 1);
									const float w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
									const float* block = &gpuField[4 * ((size_t)by * fw + bx)];
									f[0] += w * (gs[0] + (block[0] - gs[0]) * block[2]);
									f[1] += w * (gs[1] + (block[1] - gs[1]) * block[2]);
								}
							}
							for (int k = 0; k < 2; k++) {
								const float cpu = f[k] * scale[k];
								const float gpuValue = DirectX::PackedVector::XMConvertHalfToFloat(motion[2 * ((size_t)py * mw + px) + k]);
								const double e = std::abs((double)gpuValue - cpu);
								worst = std::max(worst, e);
								beyond += e > 0.01 + 0.002 * std::abs(cpu);
							}
						}
					}
					printf("    %-16s frame %d: vectors: largest difference %.5f px, %zu of %zu beyond the RG16F precision\n",
					       c.name, t, worst, beyond, 2 * (size_t)mw * mh);
					Check(beyond <= (2 * (size_t)mw * mh) / 10000, "the vectors, to the texture's precision");
					lastField.assign(gpuField, gpuField + 4 * (size_t)fw * fh);   // the GPU's own history, as PASS 4 reads it
				}
			}
			}

			if (refsDone == 1) {
				// What the vectors cost where PASS 4 has the most to do: two pictures in turn,
				// the second the first zoomed by 3 %, so that every block away from the centre
				// moves on its own and has its vector searched again; against a still picture
				// and a pan, where every block takes the global motion.
				std::vector<float> resampled(4 * (size_t)cw * ch);
				auto make = [&](float zoom, int ox, int oy, uint32_t seed, Tex2D_t& target) {
					const float cx = ox + cw * 0.5f, cy = oy + ch * 0.5f;
					for (int y = 0; y < ch; y++) {
						for (int x = 0; x < cw; x++) {
							const float sx = cx + (x + 0.5f - cw * 0.5f) / zoom - 0.5f, sy = cy + (y + 0.5f - ch * 0.5f) / zoom - 0.5f;
							const int ix = std::clamp((int)std::floor(sx), 0, ref.W - 2), iy = std::clamp((int)std::floor(sy), 0, ref.H - 2);
							const float tx = std::clamp(sx - ix, 0.0f, 1.0f), ty = std::clamp(sy - iy, 0.0f, 1.0f);
							for (int k = 0; k < 4; k++) {
								const float p00 = ref.rgba[4 * ((size_t)iy * ref.W + ix) + k], p10 = ref.rgba[4 * ((size_t)iy * ref.W + ix + 1) + k];
								const float p01 = ref.rgba[4 * ((size_t)(iy + 1) * ref.W + ix) + k], p11 = ref.rgba[4 * ((size_t)(iy + 1) * ref.W + ix + 1) + k];
								const float top = p00 + (p10 - p00) * tx, bottom = p01 + (p11 - p01) * tx;
								resampled[4 * ((size_t)y * cw + x) + k] = top + (bottom - top) * ty;
							}
						}
					}
					std::vector<float> reduced;
					DownscaleBox(resampled, cw, ch, 2, reduced);
					AddGrain(reduced, sw, sh, 0.02f, seed);
					return SUCCEEDED(target.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, sw, sh, Tex2D_DefaultShaderRTarget))
						&& (UploadRgba(ctx, target.pTexture, reduced, sw), true);
				};

				CDlssStabilizer::FlowSettings tested = CDlssStabilizer::ForDlssSR();   // the snapping alone, as first tried
				tested.flowBlur = 0;
				tested.snapLow = 1.0f;
				tested.snapHigh = 2.0f;
				tested.vectorRadius = 0;
				tested.vectorShift = 0;
				tested.vectorRefine = 0;
				tested.vectorTemporal = 1.0f;
				CDlssStabilizer::FlowSettings unsearched = CDlssStabilizer::ForDlssSR();
				unsearched.vectorRefine = 0;
				const struct { const char* name; CDlssStabilizer::FlowSettings settings; } variants[] = {
					{ "snapping only", tested },
					{ "no search", unsearched },
					{ "filter now", CDlssStabilizer::ForDlssSR() },
				};
				const struct { const char* name; float zoom; int dx, dy; } motions[] = {
					{ "still, grained", 1.0f,  0, 0 },
					{ "pan 3.5 x 1.5",  1.0f,  7, 3 },
					{ "zoom 3 %",       1.03f, 0, 0 },
				};
				printf("   GPU time of the vectors (Optical Flow both ways, then the passes), ms per picture, and blocks keeping their own:\n");
				{
					// A second of work first, for the GPU's clocks to rise.
					Tex2D_t first, second;
					CDlssStabilizer warm;
					if (make(1.0f, 0, 0, 0x71E00001u, first) && make(1.0f, 7, 3, 0x71E00002u, second)
						&& SUCCEEDED(warm.Create(dev, ctx, sw, sh,
							stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear(),
							true, CDlssStabilizer::ForDlssSR()))) {
						GpuMs(dev, ctx, 200, [&] {
							warm.PrepareMotion(ctx, first.pShaderResource);
							warm.PrepareMotion(ctx, second.pShaderResource);
						});
					}
				}
				for (const auto& m : motions) {
					Tex2D_t first, second;
					if (!make(1.0f, 0, 0, 0x71E00001u, first) || !make(m.zoom, m.dx, m.dy, 0x71E00002u, second)) {
						Check(false, "the timing pictures");
						break;
					}
					printf("    %-16s", m.name);
					for (const auto& v : variants) {
						CDlssStabilizer timed;
						if (FAILED(timed.Create(dev, ctx, sw, sh,
								stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear(),
								true, v.settings))) {
							printf("  %s: not made", v.name);
							continue;
						}
						const double ms = GpuMs(dev, ctx, 20, [&] {
							timed.PrepareMotion(ctx, first.pShaderResource);
							timed.PrepareMotion(ctx, second.pShaderResource);
						}) / 2.0;
						std::vector<BYTE> fieldRaw;
						int ow = 0, oh = 0;
						double kept = 0;
						if (timed.GetBlockField() && ReadRaw(dev, ctx, timed.GetBlockField(), 16, fieldRaw, ow, oh)) {
							const float* field = (const float*)fieldRaw.data();
							for (size_t i = 0; i < (size_t)ow * oh; i++) {
								kept += field[4 * i + 2] > 0.0f;
							}
							kept /= std::max((size_t)ow * oh, (size_t)1);
						}
						printf("  %s %.2f (%.0f %%)", v.name, ms, 100.0 * kept);
					}
					printf("\n");
				}
			}
		}
		if (!rc && !refsDone) {
			printf("  put 4K film frames in upscale_refs first\n");
			rc = 1;
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
