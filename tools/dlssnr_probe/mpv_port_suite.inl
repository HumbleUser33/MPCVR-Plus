// --tmpvport: the prescalers as the filter runs them against the harness's own runner,
// and the chroma siting the filter applies against chroma subsampled where video puts it.
//
// Included by harness.cpp after chroma_suite.inl. Source/Upscale/MpvShader.cpp is the
// filter's runner, with the passes and tables Shaders/mpv/mpv_shaders.py generated and
// compile_shaders.cmd compiled; it reads them here from _bin\shaders and Shaders\mpv
// instead of the filter's resources.
//
//   luma    FSRCNNX 8, FSRCNNX 16, RAVU-zoom, ArtCNN and Anime4K on the same luma plane
//           through both runners, given as grey as the filter gives it: the pictures
//           must agree to the last half float. Anime4K hooks MAIN in the harness's
//           translation and LUMA in the filter's, which is the same plane renamed.
//   chroma  4:2:0 made the way MPEG-2 (chroma on the even luma columns) and co-sited
//           video make it, brought back to full size by RAVU-zoom with the shift the
//           filter gives it, without it, and by Catmull-Rom at the same positions.
//           The filter's shift must score like Catmull-Rom; the missing shift must not.

#include "resource.h"
#define MPV_SHADER_FILES
#include "Upscale/MpvShaderTables.h"

namespace temporal {

namespace {

// The compiled passes and the tables from the repository, in place of the resources.
class CMpvFiles
{
public:
	::CMpvShader::DataSource Source()
	{
		return [this](UINT resid, const BYTE*& data, size_t& size) { return Get(resid, data, size); };
	}

private:
	bool Get(UINT resid, const BYTE*& data, size_t& size)
	{
		auto it = m_files.find(resid);
		if (it == m_files.end()) {
			std::wstring path;
			if (resid == IDF_VS_11_MPV_HOOK) {
				path = L"..\\..\\_bin\\shaders\\vs_mpv_hook.cso";
			} else {
				for (const auto& file : kMpvShaderFiles) {
					if (file.resid == resid) {
						path = std::wstring(L"..\\..\\") + file.path;
					}
				}
			}
			FILE* f = nullptr;
			if (path.empty() || _wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
				return false;
			}
			fseek(f, 0, SEEK_END);
			const long bytes = ftell(f);
			fseek(f, 0, SEEK_SET);
			std::vector<BYTE> content((size_t)std::max(bytes, 0L));
			const bool ok = bytes > 0 && fread(content.data(), 1, content.size(), f) == content.size();
			fclose(f);
			if (!ok) {
				return false;
			}
			it = m_files.emplace(resid, std::move(content)).first;
		}
		data = it->second.data();
		size = it->second.size();
		return true;
	}

	std::map<UINT, std::vector<BYTE>> m_files;
};

// A plane of the filter's runner, back on the CPU: red only.
bool ReadPlaneRed(ID3D11Device* dev, ID3D11DeviceContext* ctx, const ::CMpvShader::Texture& texture, std::vector<float>& out)
{
	D3D11_TEXTURE2D_DESC desc = {};
	if (!texture.pTexture) {
		return false;
	}
	texture.pTexture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	std::vector<float> rgba;
	if (FAILED(dev->CreateTexture2D(&desc, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, texture.pTexture);
	if (!ReadRgbaHalf(ctx, stage, texture.width, texture.height, rgba)) {
		return false;
	}
	out.resize((size_t)texture.width * texture.height);
	for (size_t i = 0; i < out.size(); i++) {
		out[i] = rgba[4 * i];
	}
	return true;
}

bool ReadPlaneRed(ID3D11Device* dev, ID3D11DeviceContext* ctx, const MpvTexture& texture, std::vector<float>& out)
{
	::CMpvShader::Texture wrapper;
	wrapper.pTexture = texture.tex;
	wrapper.width = texture.w;
	wrapper.height = texture.h;
	return ReadPlaneRed(dev, ctx, wrapper, out); // the wrapper gives the reference back
}

double PlanePsnr(const std::vector<float>& a, const std::vector<float>& b)
{
	if (a.size() != b.size() || a.empty()) {
		return -1;
	}
	double se = 0;
	for (size_t i = 0; i < a.size(); i++) {
		se += (double)(a[i] - b[i]) * (a[i] - b[i]);
	}
	const double mse = se / a.size();
	return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0;
}

double PlaneMaxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
	double worst = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); i++) {
		worst = std::max(worst, (double)std::abs(a[i] - b[i]));
	}
	return worst;
}

// One chroma sample at a position in texel centres: bilinear or Catmull-Rom.
float SampleChromaAt(const std::vector<float>& plane, int cw, int ch, float u, float v, bool cubic)
{
	const int ix = (int)std::floor(u), iy = (int)std::floor(v);
	const float fx = u - ix, fy = v - iy;
	auto at = [&](int px, int py) {
		return plane[(size_t)std::clamp(py, 0, ch - 1) * cw + std::clamp(px, 0, cw - 1)];
	};
	if (!cubic) {
		return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy)
		     + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
	}
	auto weights = [](float t, float w[4]) {
		const float t2 = t * t, t3 = t2 * t;
		w[0] = -0.5f * t3 + t2 - 0.5f * t;
		w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
		w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
		w[3] = 0.5f * t3 - 0.5f * t2;
	};
	float wx[4], wy[4];
	weights(fx, wx);
	weights(fy, wy);
	float acc = 0;
	for (int j = 0; j < 4; j++) {
		float row = 0;
		for (int i = 0; i < 4; i++) {
			row += wx[i] * at(ix - 1 + i, iy - 1 + j);
		}
		acc += wy[j] * row;
	}
	return acc;
}

// Where chroma sits among the luma pixels, and how a picture is subsampled to put it there.
struct Siting {
	const char* name;
	bool bHorizontalCosited; // MPEG-2 and co-sited: on the even luma columns
	bool bVerticalCosited;   // co-sited only: on the even luma rows
};

void Subsample(const std::vector<float>& full, int W, int H, const Siting& siting, std::vector<float>& out)
{
	const int cw = W / 2, ch = H / 2;
	out.assign((size_t)cw * ch, 0.0f);
	auto at = [&](int x, int y) {
		return full[(size_t)std::clamp(y, 0, H - 1) * W + std::clamp(x, 0, W - 1)];
	};
	for (int j = 0; j < ch; j++) {
		for (int i = 0; i < cw; i++) {
			float acc = 0;
			// [1/4, 1/2, 1/4] where the sample sits on a luma pixel, [1/2, 1/2] where
			// it sits between two.
			const int xs[3] = { 2 * i - 1, 2 * i, 2 * i + 1 };
			const int ys[3] = { 2 * j - 1, 2 * j, 2 * j + 1 };
			const float wxs[3] = { 0.25f, 0.5f, 0.25f };
			for (int b = 0; b < 3; b++) {
				for (int a = 0; a < 3; a++) {
					const float wx = siting.bHorizontalCosited ? wxs[a] : (a == 0 ? 0.0f : 0.5f);
					const float wy = siting.bVerticalCosited ? wxs[b] : (b == 0 ? 0.0f : 0.5f);
					const int x = siting.bHorizontalCosited ? xs[a] : 2 * i + a - 1;
					const int y = siting.bVerticalCosited ? ys[b] : 2 * j + b - 1;
					if (wx > 0 && wy > 0) {
						acc += wx * wy * at(x, y);
					}
				}
			}
			out[(size_t)j * cw + i] = acc;
		}
	}
}

} // namespace

static int RunMpvPort(ID3D11Device* dev, ID3D11DeviceContext* ctx, int maxRefs)
{
	Head("mpv prescalers: the filter's runner against the harness's");

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
			if (maxRefs > 0 && files.size() > (size_t)maxRefs) {
				files.resize((size_t)maxRefs);
			}
		}
		if (files.empty()) {
			printf("  no picture in upscale_refs\n");
			rc = 1;
		}

		// The prescalers the filter offers, in both runners. ArtCNN is the one made of
		// compute passes, so it is also what says the filter dispatches them right.
		struct Shader {
			const wchar_t* dir;            // the harness's translation
			const MpvShaderInfo* pInfo;    // the filter's tables
		};
		const Shader shaders[] = {
			{ L"FSRCNNX_x2_8-0-4-1",  &kMpvFSRCNNX8      },
			{ L"FSRCNNX_x2_16-0-4-1", &kMpvFSRCNNX16     },
			{ L"ravu-zoom-ar-r3",     &kMpvRavuZoomAR3   },
			{ L"ArtCNN_C4F16_DS",     &kMpvArtCNNC4F16DS },
			{ L"Anime4K_Upscale_CNN_x2_M", &kMpvAnime4KCNNx2M },
		};
		CMpvFiles resources;

		if (!rc) {
			g_report = fopen("temporal_results_mpvport.txt", "w");
			Out("\nThe prescalers the filter embeds, run by its own code (Source/Upscale/MpvShader.cpp) and by the\n");
			Out("harness (mpvhook.inl), on the luma of each reference reduced to half size.\n\n");
			Out("psnr: the two pictures against each other, 99 when they are identical; max: the largest difference\n");
			Out("in a plane whose values run from 0 to 1. ms: the filter's runner, GPU time of one picture.\n");
		}

		CComPtr<ID3D11Query> disjoint, tsStart, tsEnd;
		if (!rc) {
			D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			dev->CreateQuery(&qd, &disjoint);
			qd.Query = D3D11_QUERY_TIMESTAMP;
			dev->CreateQuery(&qd, &tsStart);
			dev->CreateQuery(&qd, &tsEnd);
		}

		for (const std::wstring& file : files) {
			if (rc) {
				break;
			}
			UpscaleRef ref;
			std::string error;
			ref.name = file.substr(file.find_last_of(L'\\') + 1);
			if (!LoadReference(factory, file.c_str(), ref, error)) {
				Out("\n%S: %s\n", ref.name.c_str(), error.c_str());
				g_failures++;
				continue;
			}
			const int W = ref.W & ~1, H = ref.H & ~1;
			std::vector<float> lowres;
			DownscaleBox(ref.rgba, ref.W, ref.H, 2, lowres);
			const int w = ref.W / 2, h = ref.H / 2;

			Out("\n%S -- %dx%d luma to %dx%d\n", ref.name.c_str(), w, h, W, H);
			Out("  %-24s %8s %9s %9s\n", "shader", "psnr", "max", "time");

			// The luma plane both runners take, as grey: what the filter gives them
			// (ps_mpv_prescale.hlsl, PASS 1). Anime4K reads all three channels.
			std::vector<HALF> yHalf(4 * (size_t)w * h);
			const HALF zero = DirectX::PackedVector::XMConvertFloatToHalf(0.0f);
			const HALF one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
			for (size_t i = 0; i < (size_t)w * h; i++) {
				yHalf[4 * i] = DirectX::PackedVector::XMConvertFloatToHalf(Luma(&lowres[4 * i]));
				yHalf[4 * i + 1] = yHalf[4 * i + 2] = yHalf[4 * i];
				yHalf[4 * i + 3] = one;
			}
			MpvTexture yPlane;
			if (!MakeMpvTexture(dev, w, h, yPlane, yHalf.data(), 4 * sizeof(HALF) * w)) {
				Out("  luma plane texture failed\n");
				g_failures++;
				continue;
			}

			for (const Shader& shader : shaders) {
				::CMpvShader filter;
				HRESULT hr = filter.Load(dev, *shader.pInfo, IDF_VS_11_MPV_HOOK, resources.Source());
				if (FAILED(hr)) {
					Out("  %-24S the filter's runner did not load it (%08x)\n", shader.dir, (unsigned)hr);
					g_failures++;
					continue;
				}
				::CMpvShader::Texture output;
				hr = filter.Process(ctx, yPlane.srv, w, h, W, H, 0.0f, 0.0f, output);
				double ms = 0;
				if (hr == S_OK && disjoint) {
					ctx->Begin(disjoint);
					ctx->End(tsStart);
					hr = filter.Process(ctx, yPlane.srv, w, h, W, H, 0.0f, 0.0f, output);
					ctx->End(tsEnd);
					ctx->End(disjoint);
					D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
					UINT64 t0 = 0, t1 = 0;
					while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
						Sleep(0);
					}
					while (ctx->GetData(tsStart, &t0, sizeof(t0), 0) == S_FALSE) {
						Sleep(0);
					}
					while (ctx->GetData(tsEnd, &t1, sizeof(t1), 0) == S_FALSE) {
						Sleep(0);
					}
					ms = (!dj.Disjoint && dj.Frequency) ? double(t1 - t0) * 1000.0 / dj.Frequency : 0.0;
				}
				if (hr != S_OK) {
					Out("  %-24S the filter's runner failed (%08x)\n", shader.dir, (unsigned)hr);
					g_failures++;
					continue;
				}

				temporal::CMpvShader harness;
				std::string loadError;
				if (!harness.Load(dev, std::wstring(L"upscalers\\hlsl\\") + shader.dir, loadError)) {
					Out("  %-24S the harness's runner did not load it: %s\n", shader.dir, loadError.c_str());
					g_failures++;
					continue;
				}
				// The harness runs mpv's own text, so Anime4K on MAIN there.
				const char* hook = (harness.Hooks("MAIN") && !harness.Hooks("LUMA")) ? "MAIN" : "LUMA";
				std::map<std::string, MpvTexture> planes;
				planes[hook] = yPlane;
				std::string runError;
				if (harness.Run(dev, ctx, hook, planes, W, H, runError) < 0) {
					Out("  %-24S the harness's runner failed: %s\n", shader.dir, runError.c_str());
					g_failures++;
					continue;
				}

				std::vector<float> fromFilter, fromHarness;
				if (!ReadPlaneRed(dev, ctx, output, fromFilter) || !ReadPlaneRed(dev, ctx, planes[hook], fromHarness)
						|| output.width != planes[hook].w || output.height != planes[hook].h) {
					Out("  %-24S the two results do not compare\n", shader.dir);
					g_failures++;
					continue;
				}
				const double psnr = PlanePsnr(fromFilter, fromHarness);
				const double worst = PlaneMaxDiff(fromFilter, fromHarness);
				const double megabytes = 8.0 * filter.TextureCount() * w * h / (1024 * 1024);
				Out("  %-24S %8.2f %9.6f %6.2f ms  %2u textures, %.0f MB\n", shader.dir, psnr, worst, ms,
					filter.TextureCount(), megabytes);
				// Half floats hold about three decimal digits: anything beyond that is
				// not rounding.
				if (psnr < 80.0 || worst > 0.002) {
					Out("   FAIL: the filter's runner does not give the harness's picture\n");
					g_failures++;
				}
			}

			// Chroma: what the filter's shift does to chroma that is not centred.
			std::vector<float> Y((size_t)W * H), refCb((size_t)W * H), refCr((size_t)W * H);
			for (int j = 0; j < H; j++) {
				for (int i = 0; i < W; i++) {
					const float* p = &ref.rgba[4 * ((size_t)j * ref.W + i)];
					const size_t k = (size_t)j * W + i;
					Y[k] = Luma(p);
					refCb[k] = (p[2] - Y[k]) / 1.8556f + 0.5f;
					refCr[k] = (p[0] - Y[k]) / 1.5748f + 0.5f;
				}
			}
			const int cw = W / 2, chh = H / 2;

			static const Siting sitings[] = {
				{ "MPEG-2 (left)", true,  false },
				{ "co-sited",      true,  true  },
				{ "MPEG-1 (centred)", false, false },
			};
			::CMpvShader chromaFilter;
			if (FAILED(chromaFilter.Load(dev, kMpvRavuZoomAR3, IDF_VS_11_MPV_HOOK, resources.Source()))) {
				Out("  RAVU-zoom did not load for the chroma rows\n");
				g_failures++;
				continue;
			}

			for (const Siting& siting : sitings) {
				std::vector<float> subCb, subCr;
				Subsample(refCb, W, H, siting, subCb);
				Subsample(refCr, W, H, siting, subCr);

				Out("  chroma sited %s, %dx%d to %dx%d\n", siting.name, cw, chh, W, H);
				Out("  %-24s %8s\n", "method", "psnr-c");

				// Catmull-Rom on the CPU, at the positions the siting gives.
				const float du = siting.bHorizontalCosited ? 0.0f : -0.5f;
				const float dv = siting.bVerticalCosited ? 0.0f : -0.5f;
				std::vector<float> cb((size_t)W * H), cr((size_t)W * H);
				for (int j = 0; j < H; j++) {
					for (int i = 0; i < W; i++) {
						const float u = (i + du) / 2, v = (j + dv) / 2;
						cb[(size_t)j * W + i] = SampleChromaAt(subCb, cw, chh, u, v, true);
						cr[(size_t)j * W + i] = SampleChromaAt(subCr, cw, chh, u, v, true);
					}
				}
				auto psnrOf = [&](const std::vector<float>& gotCb, const std::vector<float>& gotCr) {
					double se = 0;
					for (size_t k = 0; k < (size_t)W * H; k++) {
						se += (double)(gotCb[k] - refCb[k]) * (gotCb[k] - refCb[k])
							+ (double)(gotCr[k] - refCr[k]) * (gotCr[k] - refCr[k]);
					}
					const double mse = se / (2.0 * W * H);
					return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0;
				};
				const double psnrCatmull = psnrOf(cb, cr);
				Out("  %-24s %8.3f\n", "Catmull-Rom (CPU)", psnrCatmull);

				// RAVU-zoom through the filter's runner, with the shift the filter
				// gives it for this siting and without any.
				std::vector<HALF> cbHalf(4 * (size_t)cw * chh), crHalf(4 * (size_t)cw * chh);
				for (size_t k = 0; k < (size_t)cw * chh; k++) {
					cbHalf[4 * k] = DirectX::PackedVector::XMConvertFloatToHalf(subCb[k]);
					crHalf[4 * k] = DirectX::PackedVector::XMConvertFloatToHalf(subCr[k]);
					cbHalf[4 * k + 1] = cbHalf[4 * k + 2] = crHalf[4 * k + 1] = crHalf[4 * k + 2] = zero;
					cbHalf[4 * k + 3] = crHalf[4 * k + 3] = one;
				}
				MpvTexture cbPlane, crPlane;
				if (!MakeMpvTexture(dev, cw, chh, cbPlane, cbHalf.data(), 4 * sizeof(HALF) * cw)
						|| !MakeMpvTexture(dev, cw, chh, crPlane, crHalf.data(), 4 * sizeof(HALF) * cw)) {
					Out("  chroma plane textures failed\n");
					g_failures++;
					continue;
				}

				struct Shift { const char* name; float x, y; };
				const Shift shifts[] = {
					{ "RAVU-zoom, filter's shift", siting.bHorizontalCosited ? 0.5f / W : 0.0f, siting.bVerticalCosited ? 0.5f / H : 0.0f },
					{ "RAVU-zoom, no shift",       0.0f, 0.0f },
				};
				double psnrShift[2] = { -1, -1 };
				for (const Shift& shift : shifts) {
					::CMpvShader::Texture outCb, outCr;
					if (S_OK != chromaFilter.Process(ctx, cbPlane.srv, cw, chh, W, H, shift.x, shift.y, outCb)
							|| S_OK != chromaFilter.Process(ctx, crPlane.srv, cw, chh, W, H, shift.x, shift.y, outCr)) {
						Out("  %-24s failed\n", shift.name);
						g_failures++;
						continue;
					}
					std::vector<float> gotCb, gotCr;
					if (!ReadPlaneRed(dev, ctx, outCb, gotCb) || !ReadPlaneRed(dev, ctx, outCr, gotCr)) {
						g_failures++;
						continue;
					}
					const double psnr = psnrOf(gotCb, gotCr);
					Out("  %-24s %8.3f\n", shift.name, psnr);
					psnrShift[&shift - shifts] = psnr;
				}

				// RAVU-zoom sits about a decibel under Catmull-Rom on chroma wherever it
				// reads it (--tchroma measured that): far below means it is reading the
				// wrong place. And where the siting asks for a shift, leaving it out must
				// cost, by a quarter of a chroma pixel.
				if (psnrShift[0] > 0 && psnrShift[0] < psnrCatmull - 2.5) {
					Out("   FAIL: much further from the reference than Catmull-Rom at the same positions\n");
					g_failures++;
				}
				if (siting.bHorizontalCosited || siting.bVerticalCosited) {
					// How much it costs depends on how much chroma detail the picture
					// has; it is always a loss.
					if (psnrShift[0] < psnrShift[1] + 0.2) {
						Out("   FAIL: the shift does not put the chroma where this siting has it\n");
						g_failures++;
					}
				} else if (std::abs(psnrShift[0] - psnrShift[1]) > 0.001) {
					Out("   FAIL: centred chroma is being shifted\n");
					g_failures++;
				}
			}
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_mpvport.txt\n");
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
