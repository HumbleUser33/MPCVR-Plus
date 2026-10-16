// --tsr: DLSS Super Resolution through the filter's own CDlssSR, on the harness's
// Direct3D 11 device: bring-up, the scales and presets it accepts, what it costs,
// and whether it leaves the renderer's pipeline state and the DLSS 5 NR session
// alone.
//
// Included by harness.cpp after upscale_suite.inl, whose reference loading,
// reductions, metrics and resize passes it uses.

#include "DLSS/DlssSR.h"

namespace temporal {

// A picture into a texture the harness owns, as the renderer's blit would.
static bool UploadRgba(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::vector<float>& rgba, int W)
{
	std::vector<HALF> half(rgba.size());
	DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
	ctx->UpdateSubresource(tex, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);
	return true;
}

static bool ReadTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, std::vector<float>& out)
{
	D3D11_TEXTURE2D_DESC sd = {};
	tex->GetDesc(&sd);
	const int W = (int)sd.Width, H = (int)sd.Height;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);
	return ReadRgbaHalf(ctx, stage, W, H, out);
}

// GPU time of a callable, in ms per run: two warm-ups, then a batch.
template <class F>
static double GpuMs(ID3D11Device* dev, ID3D11DeviceContext* ctx, int runs, F&& work)
{
	CComPtr<ID3D11Query> disjoint, t0q, t1q;
	D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	dev->CreateQuery(&qd, &disjoint);
	qd.Query = D3D11_QUERY_TIMESTAMP;
	dev->CreateQuery(&qd, &t0q);
	dev->CreateQuery(&qd, &t1q);
	if (!disjoint || !t0q || !t1q) {
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		work();
	}
	ctx->Begin(disjoint);
	ctx->End(t0q);
	for (int i = 0; i < runs; i++) {
		work();
	}
	ctx->End(t1q);
	ctx->End(disjoint);
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
	UINT64 a = 0, b = 0;
	while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) { Sleep(0); }
	while (ctx->GetData(t0q, &a, sizeof(a), 0) == S_FALSE) { Sleep(0); }
	while (ctx->GetData(t1q, &b, sizeof(b), 0) == S_FALSE) { Sleep(0); }
	return (!dj.Disjoint && dj.Frequency) ? double(b - a) * 1000.0 / dj.Frequency / runs : -1;
}

static double MeanLuma(const std::vector<float>& rgba)
{
	double sum = 0;
	for (size_t p = 0; p + 3 < rgba.size(); p += 4) {
		sum += Luma(&rgba[p]);
	}
	return rgba.empty() ? 0 : sum / (rgba.size() / 4);
}

static int RunSR(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, const wchar_t* dllPath)
{
	Head("DLSS Super Resolution: bring-up on Direct3D 11");

	// Printed as it happens: a crash inside NGX still shows how far it got. The
	// runtime's configuration dump and per-mode preset lines are left out.
	CDlssSR::SetTrace([](const wchar_t* line) {
		for (const wchar_t* noise : { L"NGXLoadConfig", L"NGXLoadFromPath", L"NgxRayReconstruction", L"Container", L"app id is" }) {
			if (wcsstr(line, noise)) {
				return;
			}
		}
		printf("    | %S\n", line);
	});

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CDlssSR sr;
		const bool inited = sr.Init(dev, dllPath);
		Check(inited, "CDlssSR::Init");
		printf("%S", sr.GetInfoBlock().c_str());
		if (!inited) {
			rc = 2;
		}

		CComPtr<IWICImagingFactory> factory;
		UpscaleRef ref;
		std::string error;
		if (!rc) {
			Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
			std::wstring file = L"C:\\Windows\\Web\\Wallpaper\\ThemeB\\img24.jpg";
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileW(L"upscale_refs\\*.png", &fd);
			if (h != INVALID_HANDLE_VALUE) {
				file = std::wstring(L"upscale_refs\\") + fd.cFileName;
				FindClose(h);
			}
			if (!factory || !LoadReference(factory, file.c_str(), ref, error)) {
				printf("  %S: %s\n", file.c_str(), error.c_str());
				rc = 1;
				g_failures++;
			} else {
				printf("  reference %S, %dx%d\n", file.c_str(), ref.W, ref.H);
			}
		}

		CUpscalePasses passes;
		if (!rc && !passes.Init(dev, error)) {
			printf("  resize shaders: %s\n", error.c_str());
			rc = 1;
			g_failures++;
		}

		if (!rc) {
			// ---- one feature, one picture ------------------------------------
			Head("Feature at half the reference size");
			std::vector<float> low, out, lanczos;
			DownscaleBox(ref.rgba, ref.W, ref.H, 2, low);
			const int lw = ref.W / 2, lh = ref.H / 2;

			const bool created = sr.CreateFeature(lw, lh, ref.W, ref.H, NGX_DLSS_PRESET_Default);
			Check(created, std::format("CreateFeature {}x{} -> {}x{}", lw, lh, ref.W, ref.H).c_str());
			printf("  %S\n", sr.GetStatusLine().c_str());

			if (created) {
				UploadRgba(ctx, sr.GetInput()->pTexture, low, lw);
				bool evaluated = true;
				for (int i = 0; i < 8 && evaluated; i++) {
					evaluated = sr.Evaluate(nullptr, 41.7f);
				}
				Check(evaluated, "eight evaluations of a still picture");
				if (!evaluated) {
					printf("%S", sr.GetInfoBlock().c_str());
				}

				const bool read = evaluated && ReadTexture(dev, ctx, sr.GetOutput()->pTexture, out);
				const double luma = read ? MeanLuma(out) : 0;
				Check(read && luma > 0.01, std::format("the output holds a picture (mean luma {:.3f}, reference {:.3f})",
					luma, MeanLuma(ref.rgba)).c_str());

				int lanczos3 = -1;
				for (int mi = 0; mi < passes.MethodCount(); mi++) {
					if (!strcmp(passes.GetMethod(mi).name, "Lanczos3")) {
						lanczos3 = mi;
					}
				}
				if (read && lanczos3 >= 0 && ResizeOnGpu(dev, ctx, passes, passes.GetMethod(lanczos3), low, lw, lh, ref.W, ref.H, lanczos)) {
					const UpscaleMetrics ms = Measure(out, ref.rgba, ref.W, ref.H);
					const UpscaleMetrics ml = Measure(lanczos, ref.rgba, ref.W, ref.H);
					printf("  against the reference: DLSS SR psnr %.3f psnr-d %.3f ssim %.4f sharp %.3f halo %.5f\n",
					       ms.psnr, ms.psnrDetail, ms.ssim, ms.sharp, ms.halo);
					printf("                          Lanczos3 psnr %.3f psnr-d %.3f ssim %.4f sharp %.3f halo %.5f\n",
					       ml.psnr, ml.psnrDetail, ml.ssim, ml.sharp, ml.halo);
					Check(!ms.notFinite && ms.psnr > ml.psnr - 3.0, "DLSS SR is in the same league as Lanczos3");

					// The renderer's own pass must give the same picture before and after
					// an evaluation: NGX works in a state of its own.
					std::vector<float> before, after;
					ResizeOnGpu(dev, ctx, passes, passes.GetMethod(lanczos3), low, lw, lh, ref.W, ref.H, before);
					sr.Evaluate(nullptr, 41.7f);
					ResizeOnGpu(dev, ctx, passes, passes.GetMethod(lanczos3), low, lw, lh, ref.W, ref.H, after);
					double worst = 0;
					for (size_t i = 0; i < before.size() && i < after.size(); i++) {
						worst = std::max(worst, (double)std::abs(before[i] - after[i]));
					}
					Check(before.size() == after.size() && worst == 0.0, "the renderer's resize is unchanged by an evaluation");

					SaveBoard(factory, L"sr_bringup_board.png", { &ref.rgba, &lanczos, &out, nullptr }, ref.W, ref.H,
						FindDetailWindow(ref.rgba, ref.W, ref.H, std::min(960, ref.W), std::min(540, ref.H)));
					printf("  board: sr_bringup_board.png (reference, Lanczos3, DLSS SR)\n");
				}

				const double ms = GpuMs(dev, ctx, 16, [&]() { sr.Evaluate(nullptr, 41.7f); });
				printf("  GPU time per evaluation at %dx%d -> %dx%d: %.2f ms\n", lw, lh, ref.W, ref.H, ms);
			}

			// ---- presets --------------------------------------------------------
			Head("Presets at the same size");
			const struct { const char* name; unsigned value; } presets[] = {
				{ "Default", NGX_DLSS_PRESET_Default }, { "J", NGX_DLSS_PRESET_J }, { "K", NGX_DLSS_PRESET_K },
				{ "L", NGX_DLSS_PRESET_L }, { "M", NGX_DLSS_PRESET_M },
			};
			for (const auto& p : presets) {
				const bool ok = sr.CreateFeature(lw, lh, ref.W, ref.H, p.value)
					&& (UploadRgba(ctx, sr.GetInput()->pTexture, low, lw), sr.Evaluate(nullptr, 41.7f));
				double ms = -1;
				if (ok) {
					ms = GpuMs(dev, ctx, 8, [&]() { sr.Evaluate(nullptr, 41.7f); });
				}
				printf("  preset %-8s %s  %.2f ms  %S\n", p.name, ok ? "ok  " : "FAIL", ms, sr.GetStatusLine().c_str());
			}

			// ---- scales -----------------------------------------------------------
			Head("Scales");
			const struct { int iw, ih, ow, oh; } scales[] = {
				{ 1920, 1080, 3840, 2160 },   // 1080p on a 4K screen
				{ 1280,  720, 3840, 2160 },   // 720p on a 4K screen
				{ 1920,  800, 3840, 1600 },   // scope 1080p
				{ 1440, 1080, 2880, 2160 },   // 4:3 1080p
				{ 1920, 1080, 2560, 1440 },   // 1080p on a 1440p screen
				{ 1280,  720, 1920, 1080 },   // 720p on a 1080p screen
				{  960,  540, 3840, 2160 },   // x4, past Ultra Performance
				{  720,  480, 3240, 2160 },   // DVD, x4.5
				{ 1920, 1080, 1920, 1080 },   // x1
			};
			for (const auto& s : scales) {
				const bool ok = sr.CreateFeature(s.iw, s.ih, s.ow, s.oh, NGX_DLSS_PRESET_Default) && sr.Evaluate(nullptr, 41.7f);
				double ms = -1;
				if (ok) {
					ms = GpuMs(dev, ctx, 8, [&]() { sr.Evaluate(nullptr, 41.7f); });
				}
				printf("  %4dx%-4d -> %4dx%-4d %s  %6.2f ms  %S\n", s.iw, s.ih, s.ow, s.oh, ok ? "ok  " : "FAIL", ms,
				       sr.GetStatusLine().c_str());
			}

			// ---- next to DLSS 5 NR --------------------------------------------------
			Head("Next to the DLSS 5 NR session");
			if (!dlss.IsInitialised()) {
				printf("  no DLSS 5 NR session (--nonr)\n");
			} else {
				Tex2D_t nrIn, nrOut;
				CDlssNR::Params params;
				const bool nrOk = MakeSharedPair(dev, nrIn, nrOut, 1920, 1080)
					&& dlss.CreateFeature(nrIn.pTexture, nrOut.pTexture, 1920, 1080, params);
				FillInput(dev, ctx, nrIn.pTexture, 0.3f);
				const bool nrEval = nrOk && dlss.Evaluate(params);
				const bool srEval = sr.CreateFeature(1920, 1080, 3840, 2160, NGX_DLSS_PRESET_Default) && sr.Evaluate(nullptr, 41.7f);
				FillInput(dev, ctx, nrIn.pTexture, 0.6f);
				const bool nrAgain = nrOk && dlss.Evaluate(params);
				Check(nrEval && srEval && nrAgain, "NR, SR, NR again");
				dlss.ReleaseFeature();
				const bool srAfterNr = sr.Evaluate(nullptr, 41.7f);
				Check(srAfterNr, "SR after the NR feature is released");
			}

			// ---- teardown ---------------------------------------------------------
			Head("Shutdown and a second session");
			sr.Shutdown();
			Check(SUCCEEDED(dev->GetDeviceRemovedReason()), "device survived Shutdown");
			// nvngx_dlss.dll is still in the process and will call into the runtime that
			// loaded it for as long as it is (CDlssSR::LoadCore), so that runtime may not
			// have gone. Run with --nonr for this to mean something: a DLSS 5 NR session
			// holds a runtime of its own.
			Check(GetModuleHandleW(L"_nvngx.dll") != nullptr, "the NGX runtime is still loaded after Shutdown");
			const bool again = sr.Init(dev, dllPath) && sr.CreateFeature(1280, 720, 2560, 1440, NGX_DLSS_PRESET_Default)
				&& sr.Evaluate(nullptr, 41.7f);
			Check(again, "Init, CreateFeature and Evaluate again");
			if (!again) {
				printf("%S", sr.GetInfoBlock().c_str());
			}
			sr.Shutdown();
			Check(SUCCEEDED(dev->GetDeviceRemovedReason()), "device survived the second Shutdown");

			// The Direct3D 11 session ends without a device argument: the DLSS 5 NR
			// session in the same runtime must not notice.
			if (dlss.IsInitialised()) {
				Tex2D_t nrIn, nrOut;
				CDlssNR::Params params;
				const bool nrOk = MakeSharedPair(dev, nrIn, nrOut, 1920, 1080)
					&& dlss.CreateFeature(nrIn.pTexture, nrOut.pTexture, 1920, 1080, params);
				FillInput(dev, ctx, nrIn.pTexture, 0.4f);
				const bool nrEval = nrOk && dlss.Evaluate(params) && OutputIsNonZero(dev, ctx, nrOut.pTexture);
				Check(nrEval, "DLSS 5 NR still evaluates after the SR session ended");
				dlss.ReleaseFeature();
			}
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}


// ------------------------------------------------------------ quality suite --

// --tsrq: DLSS Super Resolution on moving film frames, against the renderer's
// own filters. Each reference is cropped by a window that holds still, then pans
// by 7 x 3 reference pixels a frame: 3.5 x 1.5 source pixels, so every other frame
// samples the picture half a pixel off -- what a real pan gives DLSS to work with,
// minus the jitter a game adds. The source is the crop halved, then grained or
// compressed per frame. Every method sees the same frames.
//
// DLSS runs with the exact motion (what a perfect estimator would give), with the
// renderer's Optical Flow vectors (CDlssStabilizer::ForDlssSR, the same with the median
// global motion it had until 2.0, and the raw vectors the filter gave it up to 1.2), and
// with none.

namespace {

struct SRRow {
	const char* name;
	int  filter = -1;              // a CUpscalePasses method, or -1 for DLSS
	unsigned preset = NGX_DLSS_PRESET_Default;
	int  motion = 0;               // DLSS: 0 exact, 1 Optical Flow, 2 none
	CDlssStabilizer::FlowSettings flow;   // for Optical Flow
};

// Optical Flow as the stabilizer runs it, and finer: at the source size, a vector
// per pixel or per 2x2 block, the engine's slowest search. The confidence map is
// not read by DLSS, so no backward flow and no cost.
CDlssStabilizer::FlowSettings SRFlow(UINT factor, UINT grid, NV_OF_PERF_LEVEL perf)
{
	CDlssStabilizer::FlowSettings flow;
	flow.flowFactor = factor;
	flow.gridSize = grid;
	flow.perfLevel = perf;
	flow.bidirectional = false;
	flow.cost = false;
	return flow;
}

struct SRAccum {
	double psnr = 0, psnrDetail = 0, ssim = 0, sharp = 0, halo = 0, grain = 0;
	int n = 0;
	void Add(const UpscaleMetrics& m) {
		psnr += m.psnr; psnrDetail += m.psnrDetail; ssim += m.ssim; sharp += m.sharp; halo += m.halo; grain += m.grain; n++;
	}
};

} // namespace

static int RunSRQuality(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* dllPath, int maxRefs)
{
	Head("DLSS Super Resolution against the renderer's filters, on moving film frames");

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
		CUpscalePasses passes;
		CStabilizerPasses stabPasses;
		if (!rc && (!factory || !passes.Init(dev, error) || !stabPasses.Init(dev, error))) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}

		CDlssSR sr;
		if (!rc && !sr.Init(dev, dllPath)) {
			printf("%S", sr.GetInfoBlock().c_str());
			rc = 1;
		}

		std::vector<SRRow> rows;
		if (!rc) {
			for (int mi = 0; mi < passes.MethodCount(); mi++) {
				const char* name = passes.GetMethod(mi).name;
				if (!strcmp(name, "Catmull-Rom") || !strcmp(name, "Lanczos3") || !strcmp(name, "Jinc2m")) {
					rows.push_back({ name, mi });
				}
			}
			rows.push_back({ "DLSS SR, exact motion",   -1, NGX_DLSS_PRESET_Default, 0 });
			rows.push_back({ "DLSS SR, filter's vectors", -1, NGX_DLSS_PRESET_Default, 1, CDlssStabilizer::ForDlssSR() });
			{
				// The same until 2.0: the median global motion, none under 0.4 pixel.
				CDlssStabilizer::FlowSettings median = CDlssStabilizer::ForDlssSR();
				median.globalRefine = 0;
				median.snapDeadZone = 0.4f;
				rows.push_back({ "DLSS SR, median (2.0)", -1, NGX_DLSS_PRESET_Default, 1, median });
			}
			rows.push_back({ "DLSS SR, raw vectors (1.2)", -1, NGX_DLSS_PRESET_Default, 1, SRFlow(0, 4, NV_OF_PERF_LEVEL_MEDIUM) });
			rows.push_back({ "DLSS SR, flow as stab.",  -1, NGX_DLSS_PRESET_Default, 1 });
			rows.push_back({ "DLSS SR, flow 1:1 g4 med", -1, NGX_DLSS_PRESET_Default, 1, SRFlow(1, 4, NV_OF_PERF_LEVEL_MEDIUM) });
			rows.push_back({ "DLSS SR, flow 1:1 g2 slow", -1, NGX_DLSS_PRESET_Default, 1, SRFlow(1, 2, NV_OF_PERF_LEVEL_SLOW) });
			rows.push_back({ "DLSS SR, flow 1:1 g1 slow", -1, NGX_DLSS_PRESET_Default, 1, SRFlow(1, 1, NV_OF_PERF_LEVEL_SLOW) });
			rows.push_back({ "DLSS SR, no motion",      -1, NGX_DLSS_PRESET_Default, 2 });
		}

		static const Degradation degradations[] = {
			{ "clean",      0.0f,   0.0f  },
			{ "grain",      0.006f, 0.0f  },
			{ "compressed", 0.0f,   0.72f },
		};
		const int kFrames = 40, kMotionStart = 12, kStepX = 7, kStepY = 3;
		const int kStatic[] = { 10, 11 };
		const int kMoving[] = { 37, 38, 39 };
		const int kBoardFrame = 39;
		auto offsetAt = [&](int t, int& ox, int& oy) {
			const int moved = std::max(0, t - kMotionStart + 1);
			ox = kStepX * moved;
			oy = kStepY * moved;
		};

		if (!rc) {
			g_report = fopen("temporal_results_sr.txt", "w");
			Out("\nDLSS Super Resolution (%S) against the renderer's filters on moving film frames.\n", sr.GetDllPath().c_str());
			Out("A window of each 4K reference holds still for %d frames, then pans %d x %d reference pixels a frame\n", kMotionStart, kStepX, kStepY);
			Out("(half-pixel steps at the source). The source is that window halved, then degraded per frame.\n");
			Out("still: frames 10-11, converged on a picture that does not move; moving: frames 37-39.\n");
			Out("psnr, psnr-d, ssim, sharp, halo, grain: as in temporal_results_upscale.txt. tflick: mean luma change\n");
			Out("between the two still frames, x1000 -- 0 for a filter on a clean source, the source's own grain\n");
			Out("change on the grain rows; lower than that means grain smoothed over time.\n");
		}

		int refsDone = 0;
		for (size_t fi = 0; !rc && fi < files.size() && (maxRefs <= 0 || refsDone < maxRefs); fi++) {
			UpscaleRef ref;
			if (!LoadReference(factory, files[fi].c_str(), ref, error) || ref.W < 3840) {
				continue;   // true 4K frames only: the pan needs room, and the scale has to be a 4K screen's
			}
			ref.name = files[fi].substr(files[fi].find_last_of(L'\\') + 1);
			refsDone++;
			int maxX = 0, maxY = 0;
			offsetAt(kFrames - 1, maxX, maxY);
			const int cw = (ref.W - maxX) & ~1, ch = (ref.H - maxY) & ~1;
			const int sw = cw / 2, sh = ch / 2;
			Out("\n%S -- window %dx%d, source %dx%d\n", ref.name.c_str(), cw, ch, sw, sh);
			printf("  %S\n", ref.name.c_str());

			std::vector<float> crop(4 * (size_t)cw * ch), source, out;
			auto cropAt = [&](int t) {
				int ox = 0, oy = 0;
				offsetAt(t, ox, oy);
				for (int y = 0; y < ch; y++) {
					std::copy_n(&ref.rgba[4 * ((size_t)(oy + y) * ref.W + ox)], 4 * (size_t)cw, &crop[4 * (size_t)y * cw]);
				}
			};
			auto makeFrame = [&](int t, const Degradation& deg) {
				cropAt(t);
				DownscaleBox(crop, cw, ch, 2, source);
				if (deg.grain > 0) {
					AddGrain(source, sw, sh, deg.grain, 0x5EED0000u + (uint32_t)t);
				}
				if (deg.jpeg > 0) {
					JpegRoundTrip(factory, source, sw, sh, deg.jpeg, error);
				}
			};

			for (const Degradation& deg : degradations) {
				Out("\n  %s\n", deg.name);
				Out("  %-26s %8s %8s %7s %7s %8s %6s %7s   | moving %8s %8s %7s %7s %8s\n",
				    "method", "psnr", "psnr-d", "ssim", "sharp", "halo", "grain", "tflick", "psnr", "psnr-d", "ssim", "sharp", "halo");

				std::vector<float> boardRef, boardLanczos, boardExact, boardFlow;

				for (const SRRow& row : rows) {
					SRAccum still, moving;
					std::vector<float> previousStill;
					double tflick = 0;
					bool ok = true;

					Tex2D_t exactMotion;
					CComPtr<ID3D11RenderTargetView> exactTarget;
					CDlssStabilizer flow;
					if (row.filter < 0) {
						ok = sr.CreateFeature(sw, sh, cw, ch, row.preset);
						sr.RequestReset();
						if (ok && row.motion == 0) {
							ok = SUCCEEDED(exactMotion.Create(dev, DXGI_FORMAT_R16G16_FLOAT, sw, sh, Tex2D_DefaultShaderRTarget))
								&& SUCCEEDED(dev->CreateRenderTargetView(exactMotion.pTexture, nullptr, &exactTarget));
						}
						if (ok && row.motion == 1) {
							ok = SUCCEEDED(flow.Create(dev, ctx, sw, sh,
								stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear(),
								true, row.flow))
								&& flow.HasFlow();
							if (!ok) {
								Out("  %-26s Optical Flow: %S\n", row.name, flow.GetStatusLine().c_str());
							}
						}
					}

					for (int t = 0; ok && t < kFrames; t++) {
						const bool measureStill = std::find(std::begin(kStatic), std::end(kStatic), t) != std::end(kStatic);
						const bool measureMoving = std::find(std::begin(kMoving), std::end(kMoving), t) != std::end(kMoving);
						const bool wanted = measureStill || measureMoving || t == kBoardFrame;
						if (row.filter >= 0 && !wanted) {
							continue;   // a filter has no history: only the measured frames matter
						}
						makeFrame(t, deg);

						if (row.filter >= 0) {
							ok = ResizeOnGpu(dev, ctx, passes, passes.GetMethod(row.filter), source, sw, sh, cw, ch, out);
						} else {
							UploadRgba(ctx, sr.GetInput()->pTexture, source, sw);
							ID3D11Texture2D* pMotion = nullptr;
							if (row.motion == 0) {
								int ox = 0, oy = 0, px = 0, py = 0;
								offsetAt(t, ox, oy);
								offsetAt(std::max(0, t - 1), px, py);
								// Content at x now was at x + (ox - px) / 2 one frame earlier: current -> previous.
								const FLOAT mv[4] = { (ox - px) * 0.5f, (oy - py) * 0.5f, 0, 0 };
								ctx->ClearRenderTargetView(exactTarget, mv);
								pMotion = exactMotion.pTexture;
							} else if (row.motion == 1) {
								flow.PrepareMotion(ctx, sr.GetInput()->pShaderResource);
								pMotion = flow.GetMotionVectors();
							}
							ok = sr.Evaluate(pMotion, 41.7f);
							if (ok && wanted) {
								ok = ReadTexture(dev, ctx, sr.GetOutput()->pTexture, out);
							}
						}
						if (!ok || !wanted) {
							continue;
						}

						cropAt(t);   // the reference for this frame
						if (measureStill || measureMoving) {
							const UpscaleMetrics m = Measure(out, crop, cw, ch, deg.jpeg > 0 ? 16 : 0);
							(measureStill ? still : moving).Add(m);
						}
						if (measureStill) {
							if (!previousStill.empty()) {
								double sum = 0;
								for (size_t p = 0; p + 3 < out.size(); p += 4) {
									sum += std::abs(Luma(&out[p]) - Luma(&previousStill[p]));
								}
								tflick = 1000.0 * sum / (out.size() / 4);
							}
							previousStill = out;
						}
						if (t == kBoardFrame) {
							if (boardRef.empty()) {
								boardRef = crop;
							}
							if (!strcmp(row.name, "Lanczos3")) {
								boardLanczos = out;
							} else if (row.filter < 0 && row.preset == NGX_DLSS_PRESET_Default && row.motion == 0) {
								boardExact = out;
							} else if (row.filter < 0 && row.motion == 1 && row.flow.gridSize == 1) {
								boardFlow = out;   // the finest flow
							}
						}
					}

					if (!ok || !still.n || !moving.n) {
						Out("  %-26s failed -- %S\n", row.name, row.filter < 0 ? sr.GetStatusLine().c_str() : L"resize");
						g_failures++;
						continue;
					}
					Out("  %-26s %8.3f %8.3f %7.4f %7.3f %8.5f %6.3f %7.3f   |        %8.3f %8.3f %7.4f %7.3f %8.5f\n", row.name,
					    still.psnr / still.n, still.psnrDetail / still.n, still.ssim / still.n, still.sharp / still.n,
					    still.halo / still.n, still.grain / still.n, tflick,
					    moving.psnr / moving.n, moving.psnrDetail / moving.n, moving.ssim / moving.n, moving.sharp / moving.n,
					    moving.halo / moving.n);
				}

				if (!boardRef.empty()) {
					const std::wstring board = std::format(L"sr_{}_{}_board.png", ref.name,
						std::wstring(deg.name, deg.name + strlen(deg.name)));
					SaveBoard(factory, board.c_str(), {
						&boardRef,
						boardLanczos.empty() ? nullptr : &boardLanczos,
						boardExact.empty() ? nullptr : &boardExact,
						boardFlow.empty() ? nullptr : &boardFlow }, cw, ch,
						FindDetailWindow(boardRef, cw, ch, std::min(960, cw), std::min(540, ch)));
				}
			}
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_sr.txt; boards sr_*_board.png\n");
			printf("  (reference top left, Lanczos3 top right, DLSS SR exact motion bottom left, Optical Flow bottom right)\n");
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
