// What each sharpener is worth, on a picture that is already the size it will
// be shown at.
//
// A sharpener restores nothing: it exaggerates. So it is measured the only way
// that means anything -- against a truth it never saw. The reference is reduced
// by two, degraded the way a source is, enlarged again by Catmull-Rom, and the
// sharpener is given that soft picture. A good one gains PSNR at its best
// strength; a bad one loses it at every strength. That is how igv settled on
// 0.5 for adaptive-sharpen, and it is what the strength sweep below shows.
//
// The columns are the upscaling suite's own (Measure): psnr and psnr-d say
// whether it restored or invented, sharp is how much of the reference's edge
// gradient came back, halo is how far it shot past what the reference really
// holds -- the ringing -- and grain is what it did to a flat grainy area, which
// is what makes or breaks a sharpener on film. halo is printed x1000, because at
// its own scale a regression would sit far below the report differ's tolerance.

namespace temporal {

struct SharpenMethod {
	const char* name;
	const wchar_t* file;
	int   pass;        // the PASS define; 0 for the RCAS wrapper, which has none
	bool  needsFsr;    // the AMD headers have to have been fetched
	bool  ps5;         // RCAS wants shader model 5
	float param2;      // how far the clamp may pull back, 0 for no clamp
};

// The one knob, the same for all of them: 1.00 is the method as its author
// wrote it, below that a part of its answer, above that more than it meant.
static const float kSweep[] = { 0.25f, 0.50f, 0.75f, 1.00f, 1.50f, 2.00f, 3.00f };

static std::vector<SharpenMethod> SharpenMethods()
{
	return {
		{ "unsharp",       L"..\\..\\Shaders\\d3d11\\ps_sharpen.hlsl", 1, false, false, 0.0f },
		{ "unsharp+clamp", L"..\\..\\Shaders\\d3d11\\ps_sharpen.hlsl", 2, false, false, 0.8f },
		{ "CAS",           L"..\\..\\Shaders\\d3d11\\ps_sharpen.hlsl", 3, false, false, 0.0f },
		{ "CAS+clamp",     L"..\\..\\Shaders\\d3d11\\ps_sharpen.hlsl", 4, false, false, 0.8f },
		{ "RCAS",          L"upscalers\\ps_fsr_rcas.hlsl",             0, true,  true,  0.0f },
	};
}

// Measure() is luma only, and both ways of applying a sharpener move the luma by
// exactly the same amount -- so what separates them is invisible to it. This is
// the colour error, which is the whole question: Cb and Cr against the
// reference's, in decibels.
static double ChromaPsnr(const std::vector<float>& outRgba, const std::vector<float>& refRgba, size_t count)
{
	double se = 0;
	size_t n = 0;
	for (size_t p = 0; p + 3 < count; p += 4) {
		const float* o = &outRgba[p];
		const float* r = &refRgba[p];
		const double yo = Luma(o), yr = Luma(r);
		if (!std::isfinite(yo) || !std::isfinite(yr)) {
			continue;
		}
		// B - Y and R - Y, which is what a sharpener that touches the colour moves.
		const double dbo = o[2] - yo, dro = o[0] - yo;
		const double dbr = r[2] - yr, drr = r[0] - yr;
		se += (dbo - dbr) * (dbo - dbr) + (dro - drr) * (dro - drr);
		n += 2;
	}
	if (!n || se <= 0) {
		return 99.0;
	}
	return 10.0 * std::log10(1.0 / (se / n));
}

// One compiled variant: a method, and whether it moves the colour too.
struct SharpenShader {
	CComPtr<ID3D11PixelShader> ps;
	bool rgb = false;
};

static bool CompileSharpen(ID3D11Device* dev, const SharpenMethod& m, bool rgb,
                           CComPtr<ID3D11PixelShader>& ps, std::string& error)
{
	const char* passText = "1";
	char passBuf[8] = {};
	if (m.pass) {
		sprintf_s(passBuf, "%d", m.pass);
		passText = passBuf;
	}
	D3D_SHADER_MACRO defines[3] = {
		{ "RGBMODE", rgb ? "1" : "0" },
		{ nullptr, nullptr },
		{ nullptr, nullptr },
	};
	if (m.pass) {
		defines[1] = { "PASS", passText };
	}

	CComPtr<ID3DBlob> code, errors;
	const HRESULT hr = D3DCompileFromFile(m.file, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
		m.ps5 ? "ps_5_0" : "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize())
		               : std::format("cannot compile (0x{:08X})", (unsigned)hr);
		return false;
	}
	return SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
}

// The block ps_sharpen.hlsl reads: the resize shaders' own six floats, so the
// renderer could drive this pass with TextureResizeShader's buffer as it stands,
// plus the two the sharpener needs.
struct SharpenConstants {
	float wh[2];
	float dxdy[2];
	float scale[2];
	float strength;
	float param2;
};

static bool MakeSharpenBuffer(ID3D11Device* dev, CComPtr<ID3D11Buffer>& cb)
{
	D3D11_BUFFER_DESC d = {};
	d.ByteWidth = sizeof(SharpenConstants);
	d.Usage = D3D11_USAGE_DYNAMIC;
	d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	return SUCCEEDED(dev->CreateBuffer(&d, nullptr, &cb));
}

static void SetSharpenConstants(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, int W, int H, float strength, float param2)
{
	const SharpenConstants c = {
		{ (float)W, (float)H }, { 1.0f / W, 1.0f / H }, { 1.0f, 1.0f }, strength, param2
	};
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
		memcpy(mr.pData, &c, sizeof(c));
		ctx->Unmap(cb, 0);
	}
}

// The soft picture in, the sharpened one out, both the same size.
static bool SharpenOnGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx, CUpscalePasses& passes,
                         ID3D11PixelShader* ps, ID3D11Buffer* cb, float strength, float param2,
                         const std::vector<float>& src, int W, int H, std::vector<float>& dst)
{
	Tex2D_t texIn;
	Target texOut;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(texIn.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShaderRTarget))
			|| !MakeTarget(dev, W, H, false, texOut, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		return false;
	}

	std::vector<HALF> half(src.size());
	DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), src.data(), sizeof(float), src.size());
	ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);

	D3D11_TEXTURE2D_DESC sd = {};
	texOut.tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}

	SetSharpenConstants(ctx, cb, W, H, strength, param2);
	passes.Apply(ctx, ps, texIn.pShaderResource, texOut.rtv, W, H, cb);
	ctx->CopyResource(stage, texOut.tex);
	return ReadRgbaHalf(ctx, stage, W, H, dst);
}

// What a sharpener changed, ready to be written out with a gain: the eye
// cannot see a two-level difference on a picture, and can see nothing else on
// this.
static void Difference(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& out)
{
	out.resize(a.size());
	for (size_t i = 0; i + 3 < a.size(); i += 4) {
		for (int k = 0; k < 3; k++) {
			out[i + k] = std::abs(a[i + k] - b[i + k]);
		}
		out[i + 3] = 1.0f;
	}
}

// A row of the report, averaged over the references.
struct SharpenRow {
	double psnr = 0, psnrDetail = 0, sharp = 0, halo = 0, grain = 0, chroma = 0;
	int n = 0;
	void Add(const UpscaleMetrics& m, double chromaPsnr)
	{
		psnr += m.psnr; psnrDetail += m.psnrDetail; sharp += m.sharp; halo += m.halo; grain += m.grain;
		chroma += chromaPsnr;
		n++;
	}
	SharpenRow Mean() const
	{
		SharpenRow r = *this;
		if (n) {
			r.psnr /= n; r.psnrDetail /= n; r.sharp /= n; r.halo /= n; r.grain /= n; r.chroma /= n;
		}
		return r;
	}
};

static void PrintSharpenHead()
{
	Out("  %-24s %8s %8s %8s %7s %9s %7s %8s\n",
		"method", "strength", "psnr", "psnr-d", "sharp", "halo*1e3", "grain", "colour");
}

static void PrintSharpenRow(const char* name, const char* strength, const SharpenRow& r)
{
	Out("  %-24s %8s %8.3f %8.3f %7.3f %9.4f %7.3f %8.3f\n",
		name, strength, r.psnr, r.psnrDetail, r.sharp, r.halo * 1000.0, r.grain, r.chroma);
}

// How long one pass costs on a 4K target, the size it would really run at.
static double SharpenCost(ID3D11Device* dev, ID3D11DeviceContext* ctx, CUpscalePasses& passes,
                          ID3D11PixelShader* ps, ID3D11Buffer* cb, float strength, float param2)
{
	const int W = 3840, H = 2160;
	Tex2D_t texIn;
	Target texOut;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(texIn.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShaderRTarget))
			|| !MakeTarget(dev, W, H, false, texOut, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		return 0;
	}
	D3D11_TEXTURE2D_DESC sd = {};
	texOut.tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return 0;
	}
	SetSharpenConstants(ctx, cb, W, H, strength, param2);

	const int warm = 32, runs = 64;
	auto sync = [&]() {
		ctx->CopyResource(stage, texOut.tex);
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
			ctx->Unmap(stage, 0);
		}
	};
	for (int i = 0; i < warm; i++) {
		passes.Apply(ctx, ps, texIn.pShaderResource, texOut.rtv, W, H, cb);
	}
	sync();

	LARGE_INTEGER freq = {}, t0 = {}, t1 = {};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (int i = 0; i < runs; i++) {
		passes.Apply(ctx, ps, texIn.pShaderResource, texOut.rtv, W, H, cb);
	}
	sync();
	QueryPerformanceCounter(&t1);
	return 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart / runs;
}

// ------------------------------------------------------------------- suite --

static int RunSharpen(ID3D11Device* dev, ID3D11DeviceContext* ctx, int maxRefs = 0)
{
	Head("Sharpening suite: what each sharpener is worth on a soft picture");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		// Check only reports; this says the same and answers as well.
		auto ok = [](bool b, const char* what) { Check(b, what); return b; };

		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");

		std::vector<std::wstring> files;
		{
			WIN32_FIND_DATAW fd = {};
			for (const wchar_t* pattern : { L"upscale_refs\\*.png", L"upscale_refs\\*.jpg", L"upscale_refs\\*.jpeg" }) {
				HANDLE h = FindFirstFileW(pattern, &fd);
				if (h != INVALID_HANDLE_VALUE) {
					do {
						files.push_back(std::wstring(L"upscale_refs\\") + fd.cFileName);
					} while (FindNextFileW(h, &fd));
					FindClose(h);
				}
			}
			std::sort(files.begin(), files.end());
			if (maxRefs > 0 && files.size() > (size_t)maxRefs) {
				files.resize((size_t)maxRefs);
			}
		}
		if (files.empty()) {
			printf("  no picture in upscale_refs\n");
			return 1;
		}

		CUpscalePasses passes;
		std::string error;
		if (!ok(passes.Init(dev, error), ("upscale passes: " + error).c_str())) {
			return 1;
		}
		// The soft picture every sharpener is given: the renderer's own default
		// enlargement, so what is measured is the sharpener and nothing else.
		int catmull = -1;
		for (int i = 0; i < passes.MethodCount(); i++) {
			if (!strcmp(passes.GetMethod(i).name, "Catmull-Rom")) {
				catmull = i;
			}
		}
		if (!ok(catmull >= 0, "Catmull-Rom")) {
			return 1;
		}

		CComPtr<ID3D11Buffer> cb;
		if (!ok(MakeSharpenBuffer(dev, cb), "constant buffer")) {
			return 1;
		}

		std::vector<SharpenMethod> methods = SharpenMethods();
		const bool bFsr = GetFileAttributesW(L"upscalers\\ffx_fsr1.h") != INVALID_FILE_ATTRIBUTES;
		std::vector<std::vector<SharpenShader>> shaders(methods.size());   // [method][0 = luma, 1 = rgb]
		for (size_t i = 0; i < methods.size(); i++) {
			if (methods[i].needsFsr && !bFsr) {
				printf("  %s: the AMD headers are not in upscalers\\, skipped\n", methods[i].name);
				continue;
			}
			for (int rgb = 0; rgb < 2; rgb++) {
				SharpenShader s;
				s.rgb = rgb != 0;
				if (!CompileSharpen(dev, methods[i], s.rgb, s.ps, error)) {
					printf("  %s (%s): %s\n", methods[i].name, rgb ? "rgb" : "luma", error.c_str());
					rc = 1;
					continue;
				}
				shaders[i].push_back(s);
			}
		}

		g_report = nullptr;
		fopen_s(&g_report, "temporal_results_sharpen.txt", "w");

		Out("Every reference reduced by two, degraded, enlarged again by Catmull-Rom, then\n");
		Out("sharpened; scored against the reference none of them saw. psnr-d is the most\n");
		Out("detailed quarter. sharp: output edge gradient over the reference's, 1.000 is the\n");
		Out("truth's own sharpness. halo: mean overshoot past the range the reference really\n");
		Out("covers along its edges, x1000, lower is better. grain: fine detail left in flat\n");
		Out("areas, output over reference -- above 1 the sharpener is amplifying the grain.\n");

		// Load the references once.
		std::vector<UpscaleRef> refs;
		for (const std::wstring& f : files) {
			UpscaleRef r;
			if (LoadReference(factory, f.c_str(), r, error)) {
				r.name = f.substr(f.find_last_of(L'\\') + 1);
				refs.push_back(std::move(r));
			} else {
				printf("  %ls: %s\n", f.c_str(), error.c_str());
			}
		}
		if (!ok(!refs.empty(), "references")) {
			return 1;
		}
		printf("  %zu reference(s)\n", refs.size());

		static const Degradation degradations[] = {
			{ "clean",      0.0f,   0.0f  },
			{ "grain",      0.006f, 0.0f  },
			{ "compressed", 0.0f,   0.72f },
		};

		// The soft pictures, one per reference per degradation, made once.
		std::vector<std::vector<std::vector<float>>> soft(std::size(degradations));
		for (size_t d = 0; d < std::size(degradations); d++) {
			soft[d].resize(refs.size());
			for (size_t r = 0; r < refs.size(); r++) {
				const UpscaleRef& ref = refs[r];
				std::vector<float> reduced;
				DownscaleBox(ref.rgba, ref.W, ref.H, 2, reduced);
				if (degradations[d].grain > 0) {
					AddGrain(reduced, ref.W / 2, ref.H / 2, degradations[d].grain, 1234u + (uint32_t)r);
				}
				if (degradations[d].jpeg > 0) {
					JpegRoundTrip(factory, reduced, ref.W / 2, ref.H / 2, degradations[d].jpeg, error);
				}
				if (!ResizeOnGpu(dev, ctx, passes, passes.GetMethod(catmull), reduced, ref.W / 2, ref.H / 2,
						ref.W, ref.H, soft[d][r])) {
					printf("  cannot enlarge %ls\n", refs[r].name.c_str());
					rc = 1;
				}
			}
		}

		// ---- the sweep: where each method peaks, on the clean picture, luma only.
		Out("\nStrength sweep, clean source, luma only -- where each method peaks:\n\n");
		PrintSharpenHead();
		{
			SharpenRow none;
			for (size_t r = 0; r < refs.size(); r++) {
				none.Add(Measure(soft[0][r], refs[r].rgba, refs[r].W, refs[r].H),
					ChromaPsnr(soft[0][r], refs[r].rgba, refs[r].rgba.size()));
			}
			PrintSharpenRow("no sharpening", "-", none.Mean());
		}

		std::vector<float> best(methods.size(), 0.0f);
		for (size_t i = 0; i < methods.size(); i++) {
			if (shaders[i].empty()) {
				continue;
			}
			double bestScore = -1e9;
			for (const float s : kSweep) {
				SharpenRow row;
				for (size_t r = 0; r < refs.size(); r++) {
					std::vector<float> out;
					if (SharpenOnGpu(dev, ctx, passes, shaders[i][0].ps, cb, s, methods[i].param2,
							soft[0][r], refs[r].W, refs[r].H, out)) {
						row.Add(Measure(out, refs[r].rgba, refs[r].W, refs[r].H),
							ChromaPsnr(out, refs[r].rgba, refs[r].rgba.size()));
					}
				}
				const SharpenRow mean = row.Mean();
				char label[32] = {};
				sprintf_s(label, "%.2f", s);
				PrintSharpenRow(methods[i].name, label, mean);
				if (mean.psnrDetail > bestScore) {
					bestScore = mean.psnrDetail;
					best[i] = s;
				}
			}
			Out("\n");
		}

		// ---- the comparison: each at its best strength, both ways, all three sources.
		Out("Each at the strength that peaked above, luma against colour, on three sources:\n");
		for (size_t d = 0; d < std::size(degradations); d++) {
			Out("\n  --- %s ---\n", degradations[d].name);
			PrintSharpenHead();
			SharpenRow none;
			for (size_t r = 0; r < refs.size(); r++) {
				none.Add(Measure(soft[d][r], refs[r].rgba, refs[r].W, refs[r].H),
					ChromaPsnr(soft[d][r], refs[r].rgba, refs[r].rgba.size()));
			}
			PrintSharpenRow("no sharpening", "-", none.Mean());

			for (size_t i = 0; i < methods.size(); i++) {
				for (const SharpenShader& sh : shaders[i]) {
					SharpenRow row;
					for (size_t r = 0; r < refs.size(); r++) {
						std::vector<float> out;
						if (SharpenOnGpu(dev, ctx, passes, sh.ps, cb, best[i], methods[i].param2,
								soft[d][r], refs[r].W, refs[r].H, out)) {
							row.Add(Measure(out, refs[r].rgba, refs[r].W, refs[r].H),
							ChromaPsnr(out, refs[r].rgba, refs[r].rgba.size()));
						}
					}
					char name[64] = {};
					sprintf_s(name, "%s %s", methods[i].name, sh.rgb ? "(colour too)" : "(luma)");
					char label[32] = {};
					sprintf_s(label, "%.2f", best[i]);
					PrintSharpenRow(name, label, row.Mean());
				}
			}
		}

		// ---- what one pass costs at 4K.
		Out("\nOne pass over a 3840x2160 target:\n\n");
		for (size_t i = 0; i < methods.size(); i++) {
			if (!shaders[i].empty()) {
				Out("  %-24s %6.2f ms\n", methods[i].name,
					SharpenCost(dev, ctx, passes, shaders[i][0].ps, cb, best[i], methods[i].param2));
			}
		}

		// ---- the pictures, for the grid. Two real frames, not the line-art
		// test: the most detailed window of each, where a sharpener shows both
		// what it buys and what it costs. Only the luma form is drawn -- the
		// colour form is within a third of a decibel of it and looks the same.
		{
			std::vector<size_t> picked;
			for (size_t r = 0; r < refs.size() && picked.size() < 2; r++) {
				if (refs[r].W >= 1900 && refs[r].name.find(L"BrownFox") == std::wstring::npos) {
					picked.push_back(r);
				}
			}
			for (size_t k = 0; k < picked.size(); k++) {
				const size_t r = picked[k];
				const int W = refs[r].W, H = refs[r].H;
				const POINT at = FindDetailWindow(refs[r].rgba, W, H, 320, 180);
				// Rect is two corners, not a size: the window is half-open.
				const int cx = std::clamp((int)at.x, 0, W - 320);
				const int cy = std::clamp((int)at.y, 0, H - 180);
				const Rect crop = { cx, cy, cx + 320, cy + 180 };
				wchar_t path[MAX_PATH] = {};

				swprintf_s(path, L"sharpen_%zu_00_reference.png", k);
				SavePng(factory, path, refs[r].rgba, W, crop, 1.0f);
				swprintf_s(path, L"sharpen_%zu_01_no sharpening.png", k);
				SavePng(factory, path, soft[0][r], W, crop, 1.0f);

				int n = 2;
				for (size_t i = 0; i < methods.size(); i++) {
					if (shaders[i].empty()) {
						continue;
					}
					// Each at the strength that peaked, and the plain unsharp mask
					// once more at nearly three times it, which is what a slider
					// pushed too far looks like.
					const float amounts[2] = { best[i], (i == 0) ? 2.0f : -1.0f };
					for (const float a : amounts) {
						if (a < 0) {
							continue;
						}
						std::vector<float> out;
						if (SharpenOnGpu(dev, ctx, passes, shaders[i][0].ps, cb, a, methods[i].param2,
								soft[0][r], W, H, out)) {
							swprintf_s(path, L"sharpen_%zu_%02d_%hs %.2f.png", k, n, methods[i].name, a);
							SavePng(factory, path, out, W, crop, 1.0f);
							std::vector<float> diff;
							Difference(out, soft[0][r], diff);
							swprintf_s(path, L"sharpendiff_%zu_%02d_%hs %.2f.png", k, n, methods[i].name, a);
							SavePng(factory, path, diff, W, crop, 24.0f);
							n++;
						}
					}
				}
				printf("  crops from %ls at %ld,%ld\n", refs[r].name.c_str(), at.x, at.y);
			}
			printf("  written as sharpen_0_*.png and sharpen_1_*.png\n");
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
		}
		printf("\n  written to temporal_results_sharpen.txt\n");
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
