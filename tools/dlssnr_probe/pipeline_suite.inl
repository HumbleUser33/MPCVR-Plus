// --tpipeline: the renderer's DLSS chain run frame after frame the way Process
// runs it -- the upload, DLSS 5 NR, the stabilizer, DLSS Super Resolution and the
// copy into the 4K target -- on a panning 1080p film frame. For each stage: the
// CPU time the render thread spends in it (DLSS 5 NR waits for the GPU on the
// CPU, the other stages only queue work), the GPU time from timestamp queries,
// and the whole frame from the upload to the GPU being done with it. Mean, p95
// and max over the frames, so spikes show.
//
// Included by harness.cpp after sr_suite.inl.

namespace temporal {

namespace {

struct Samples {
	std::vector<double> v;
	void Add(double x) { v.push_back(x); }
	double Mean() const {
		double s = 0;
		for (double x : v) s += x;
		return v.empty() ? 0 : s / v.size();
	}
	double Percentile(double p) const {
		if (v.empty()) return 0;
		std::vector<double> s = v;
		std::sort(s.begin(), s.end());
		return s[std::min(s.size() - 1, (size_t)(p * (s.size() - 1) + 0.5))];
	}
	double Max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

double QpcMs(LARGE_INTEGER a, LARGE_INTEGER b)
{
	static LARGE_INTEGER freq = {};
	if (!freq.QuadPart) {
		QueryPerformanceFrequency(&freq);
	}
	return double(b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
}

LARGE_INTEGER Now()
{
	LARGE_INTEGER t = {};
	QueryPerformanceCounter(&t);
	return t;
}

} // namespace

static int RunPipeline(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, const wchar_t* srDllPath, int frames)
{
	Head("The renderer's DLSS chain, frame by frame");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		const UINT W = 1920, H = 1080, OW = 3840, OH = 2160;
		const int kSources = 8, kWarmup = 24;

		// Eight 1080p frames of a pan over a 4K film frame, half a source pixel
		// apart, uploaded once: the per-frame upload is a GPU copy, as a decoder
		// surface is.
		CComPtr<IWICImagingFactory> factory;
		CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		// Every picture of the folder is a candidate, and the first one big enough
		// wins: the suite pans over a 4K frame, so a smaller reference -- the line-art
		// test, say -- cannot stand in for one.
		std::vector<std::wstring> candidates;
		{
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileW(L"upscale_refs\\*.png", &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do {
					candidates.push_back(std::wstring(L"upscale_refs\\") + fd.cFileName);
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
		}
		candidates.push_back(L"C:\\Windows\\Web\\Wallpaper\\ThemeB\\img24.jpg");

		UpscaleRef ref;
		std::string error;
		std::wstring file;
		for (const std::wstring& candidate : candidates) {
			if (factory && LoadReference(factory, candidate.c_str(), ref, error)
					&& ref.W >= 3840 && ref.H >= 2160) {
				file = candidate;
				break;
			}
		}
		if (file.empty()) {
			printf("  a 3840x2160 reference is needed in upscale_refs (%s)\n", error.c_str());
			rc = 1;
		}

		std::vector<Tex2D_t> sources(kSources);
		if (!rc) {
			// The reference halved, then moved 3 x 1 pixels a frame, edges clamped.
			std::vector<float> low, moved(4 * (size_t)W * H);
			DownscaleBox(ref.rgba, (int)(2 * W), (int)(2 * H), 2, low);
			for (int i = 0; i < kSources; i++) {
				for (UINT y = 0; y < H; y++) {
					const UINT sy = std::min(H - 1, y + (UINT)i);
					for (UINT x = 0; x < W; x++) {
						const UINT sx = std::min(W - 1, x + 3 * (UINT)i);
						std::copy_n(&low[4 * ((size_t)sy * W + sx)], 4, &moved[4 * ((size_t)y * W + x)]);
					}
				}
				if (FAILED(sources[i].Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShader))) {
					rc = 1;
					break;
				}
				UploadRgba(ctx, sources[i].pTexture, moved, W);
			}
		}

		CStabilizerPasses stabPasses;
		CUpscalePasses passes;
		if (!rc && (!stabPasses.Init(dev, error) || !passes.Init(dev, error))) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}
		int catmullRom = -1;
		for (int mi = 0; !rc && mi < passes.MethodCount(); mi++) {
			if (!strcmp(passes.GetMethod(mi).name, "Catmull-Rom")) {
				catmullRom = mi;
			}
		}

		CDlssSR sr;
		const bool srOk = !rc && sr.Init(dev, srDllPath) && sr.CreateFeature(W, H, OW, OH, NGX_DLSS_PRESET_Default);
		if (!rc && !srOk) {
			printf("  DLSS SR unavailable: %S\n", sr.GetStatusLine().c_str());
		}

		Target out4K;
		if (!rc && !MakeTarget(dev, OW, OH, false, out4K, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
			rc = 1;
		}

		// Timestamps around each stage, and an event query for "the GPU is done".
		const int kMarks = 5;
		CComPtr<ID3D11Query> disjoint, event, marks[kMarks];
		if (!rc) {
			D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			dev->CreateQuery(&qd, &disjoint);
			qd.Query = D3D11_QUERY_EVENT;
			dev->CreateQuery(&qd, &event);
			qd.Query = D3D11_QUERY_TIMESTAMP;
			for (auto& m : marks) {
				dev->CreateQuery(&qd, &m);
			}
		}

		struct Config {
			const char* name;
			bool nr;
			bool sr;
		};
		const Config configs[] = {
			{ "DLSS 5 NR + stabilizer + DLSS SR",                 true,  true  },
			{ "DLSS SR alone (its own Optical Flow)",            false, true  },
			{ "DLSS 5 NR + stabilizer + Catmull-Rom",            true,  false },
			{ "Catmull-Rom alone",                               false, false },
		};

		if (!rc) {
			g_report = fopen("temporal_results_pipeline.txt", "w");
			Out("\nThe renderer's DLSS chain on %S, %ux%u -> %ux%u, %d frames after %d to warm up.\n", ref.name.c_str(), W, H, OW, OH, frames, kWarmup);
			Out("CPU: time the render thread spends in each stage (DLSS 5 NR waits for the GPU there; the other stages\n");
			Out("queue work). GPU: timestamp queries (DLSS 5 NR runs on its own Direct3D 12 device and is not in them).\n");
			Out("frame: from the upload to the GPU being done with the frame. Times in ms: mean / p95 / max.\n");
		}

		for (const Config& config : configs) {
			if (rc) {
				break;
			}
			if (config.nr && !dlss.IsInitialised()) {
				Out("\n%s: no DLSS 5 NR session (--nonr)\n", config.name);
				continue;
			}
			if (config.sr && !srOk) {
				Out("\n%s: no DLSS SR\n", config.name);
				continue;
			}

			// What each configuration needs, as the renderer builds it.
			Tex2D_t nrIn, nrOut;
			CDlssNR::Params params;
			CDlssStabilizer stabilizer, flow;
			bool ok = true;
			if (config.nr) {
				ok = MakeSharedPair(dev, nrIn, nrOut, W, H) && dlss.CreateFeature(nrIn.pTexture, nrOut.pTexture, W, H, params)
					&& SUCCEEDED(stabilizer.Create(dev, ctx, W, H, CDlssStabilizer::Motion::OpticalFlow,
						stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear()));
			}
			Tex2D_t plainIn;
			if (ok && !config.nr) {
				ok = SUCCEEDED(plainIn.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShaderRTarget));
			}
			if (ok && config.sr) {
				// DLSS SR's own vectors, as DlssSRPass makes them, NR or not.
				ok = SUCCEEDED(flow.Create(dev, ctx, W, H, CDlssStabilizer::Motion::OpticalFlow,
					stabPasses.InputLayout(), stabPasses.VertexShader(), stabPasses.SamplerPoint(), stabPasses.SamplerLinear(),
					true, CDlssStabilizer::ForDlssSR()));
			}
			if (ok && config.sr) {
				sr.RequestReset();
			}
			if (!ok) {
				Out("\n%s: could not be set up\n", config.name);
				g_failures++;
				continue;
			}

			Samples cpuNR, cpuStab, cpuSR, cpuOut, cpuTotal, frameTotal, gpuStab, gpuSR, gpuOut, gpuTotal;
			for (int f = 0; f < kWarmup + frames; f++) {
				const bool measured = f >= kWarmup;
				Tex2D_t& source = sources[f % kSources];

				ctx->Begin(disjoint);
				ctx->End(marks[0]);
				const LARGE_INTEGER t0 = Now();

				// Upload and DLSS 5 NR.
				Tex2D_t* pCurrent = nullptr;
				if (config.nr) {
					ctx->CopyResource(nrIn.pTexture, source.pTexture);
					if (!dlss.Evaluate(params)) {
						ok = false;
						break;
					}
				} else {
					ctx->CopyResource(plainIn.pTexture, source.pTexture);
					pCurrent = &plainIn;
				}
				const LARGE_INTEGER t1 = Now();
				ctx->End(marks[1]);

				// The stabilizer.
				if (config.nr) {
					stabilizer.PrepareMotion(ctx, nrIn.pShaderResource);
					stabilizer.Stabilize(ctx, nrIn.pShaderResource, nrOut.pShaderResource, 1.0f, true);
					pCurrent = stabilizer.GetResult();
				}
				const LARGE_INTEGER t2 = Now();
				ctx->End(marks[2]);

				// DLSS Super Resolution, or the resize shaders.
				if (config.sr) {
					ctx->CopyResource(sr.GetInput()->pTexture, pCurrent->pTexture);
					flow.PrepareMotion(ctx, sr.GetInput()->pShaderResource);
					ID3D11Texture2D* pMotion = flow.GetMotionVectors();
					if (!sr.Evaluate(pMotion, 41.7f)) {
						ok = false;
						break;
					}
				}
				const LARGE_INTEGER t3 = Now();
				ctx->End(marks[3]);

				if (config.sr) {
					ctx->CopyResource(out4K.tex, sr.GetOutput()->pTexture);
				} else {
					passes.Resize(dev, ctx, passes.GetMethod(catmullRom), pCurrent->pShaderResource, W, H, out4K.rtv, OW, OH);
				}
				const LARGE_INTEGER t4 = Now();
				ctx->End(marks[4]);
				ctx->End(disjoint);

				// The present would go here; wait for the GPU to be done instead.
				ctx->End(event);
				BOOL done = FALSE;
				while (ctx->GetData(event, &done, sizeof(done), 0) == S_FALSE || !done) {
					YieldProcessor();
				}
				const LARGE_INTEGER t5 = Now();

				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
				UINT64 ts[kMarks] = {};
				while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
					YieldProcessor();
				}
				for (int m = 0; m < kMarks; m++) {
					while (ctx->GetData(marks[m], &ts[m], sizeof(ts[m]), 0) == S_FALSE) {
						YieldProcessor();
					}
				}

				if (measured) {
					cpuNR.Add(QpcMs(t0, t1));
					cpuStab.Add(QpcMs(t1, t2));
					cpuSR.Add(QpcMs(t2, t3));
					cpuOut.Add(QpcMs(t3, t4));
					cpuTotal.Add(QpcMs(t0, t4));
					frameTotal.Add(QpcMs(t0, t5));
					if (!dj.Disjoint && dj.Frequency) {
						auto gpuMs = [&](int a, int b) { return double(ts[b] - ts[a]) * 1000.0 / dj.Frequency; };
						gpuStab.Add(gpuMs(1, 2));
						gpuSR.Add(gpuMs(2, 3));
						gpuOut.Add(gpuMs(3, 4));
						gpuTotal.Add(gpuMs(0, 4));
					}
				}
			}

			if (!ok) {
				Out("\n%s: evaluation failed\n", config.name);
				g_failures++;
				continue;
			}

			auto line = [&](const char* what, const Samples& cpu, const Samples* gpu) {
				Out("  %-22s CPU %6.2f / %6.2f / %6.2f", what, cpu.Mean(), cpu.Percentile(0.95), cpu.Max());
				if (gpu && !gpu->v.empty()) {
					Out("   GPU %6.2f / %6.2f / %6.2f", gpu->Mean(), gpu->Percentile(0.95), gpu->Max());
				}
				Out("\n");
			};
			Out("\n%s\n", config.name);
			line(config.nr ? "upload + DLSS 5 NR" : "upload", cpuNR, nullptr);
			if (config.nr) {
				line("stabilizer", cpuStab, &gpuStab);
			}
			line(config.sr ? (config.nr ? "DLSS SR" : "Optical Flow + DLSS SR") : "(no DLSS SR)", cpuSR, &gpuSR);
			line(config.sr ? "copy to 4K" : "Catmull-Rom to 4K", cpuOut, &gpuOut);
			line("render thread total", cpuTotal, &gpuTotal);
			Out("  %-22s     %6.2f / %6.2f / %6.2f\n", "frame, upload to done", frameTotal.Mean(), frameTotal.Percentile(0.95), frameTotal.Max());

			if (config.nr) {
				dlss.ReleaseFeature();
			}
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_pipeline.txt\n");
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}


} // namespace temporal
