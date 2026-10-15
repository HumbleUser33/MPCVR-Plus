// --tnrscale: DLSS 5 NR at a working size below the picture's, its change carried back.
//
// Included by harness.cpp after stab_suite.inl. The network costs in proportion to the
// pixels it is given, and at 3840x2160 it takes longer than a frame of film lasts. The
// renderer's CDlssCompose (Source/DLSS/DlssCompose.cpp, shaders embedded the way the
// filter embeds them) reduces the picture, the network and the renderer's stabilizer run
// on that, and the change they made is carried back onto the picture at its own size.
//
// Measured against the network and the stabilizer run at the full size, on the same
// pictures with the same grain, frame for frame: how far each way of carrying the change
// back lands from it (PSNR), how much of the full-size change it keeps, how much the
// result changes from picture to picture, and what each stage costs on the GPU. The
// points of comparison are the picture left as it is and the network's output enlarged
// alone. Settings are the filter's defaults: intensity 1.50, tone 0.30, structure 0.50,
// skin 0.90, auto mask, history kept, stabilizer at full strength on Optical Flow.
//
// --timage picks the picture; give it a 3840x2160 film frame (upscale_refs\4K1.png).

#include "DLSS/DlssCompose.h"

namespace temporal {

struct NRScaleVariant {
	const char* name;
	CDlssCompose::Method method;
	CDlssCompose::Tuning tuning;
	const wchar_t* png;   // the 75 % crop saved under this name, or nullptr
};

static const NRScaleVariant s_NRScaleVariants[] = {
	{ "output enlarged",    CDlssCompose::Method::Plain,      { 0.05f, 0.001f,  2 }, L"plain"      },
	{ "difference",         CDlssCompose::Method::Difference, { 0.05f, 0.001f,  2 }, L"difference" },
	{ "joint s0.02",        CDlssCompose::Method::Joint,      { 0.02f, 0.001f,  2 }, nullptr       },
	{ "joint s0.05",        CDlssCompose::Method::Joint,      { 0.05f, 0.001f,  2 }, L"joint"      },
	{ "joint s0.10",        CDlssCompose::Method::Joint,      { 0.10f, 0.001f,  2 }, nullptr       },
	{ "guided r2 e1e-3",    CDlssCompose::Method::Guided,     { 0.05f, 0.001f,  2 }, L"guided"     },
	{ "guided r2 e1e-4",    CDlssCompose::Method::Guided,     { 0.05f, 0.0001f, 2 }, nullptr       },
	{ "guided r4 e1e-3",    CDlssCompose::Method::Guided,     { 0.05f, 0.001f,  4 }, nullptr       },
};

struct NRScaleSequence {
	const char* name;
	int vx, vy;   // content motion, whole pixels per frame: no resampling of the picture
};

// What one row of the table adds up over the measured frames.
struct NRScaleStats {
	double psnr = 0, kept = 0, effect = 0, flick = 0, lfFlick = 0;
	int count = 0, flickCount = 0;
	std::vector<float> prevY, prevLF;

	void Reset() { *this = NRScaleStats{}; }
};

// The frame t of a sequence: the picture moved by whole pixels, clamped at the edges,
// with fresh grain quantised to 8 bits.
static void NRScaleFrame(const std::vector<float>& picture, int W, int H, const NRScaleSequence& s, int t,
                         float grain, std::vector<float>& rgba)
{
	rgba.resize(picture.size());
	const int ox = 24 - s.vx * t, oy = 24 - s.vy * t;
	Rng r(0x5EEDu + (uint32_t)t * 7919u);
	for (int y = 0; y < H; y++) {
		const int sy = std::clamp(y + oy, 0, H - 1);
		for (int x = 0; x < W; x++) {
			const int sx = std::clamp(x + ox, 0, W - 1);
			const float* p = &picture[4 * ((size_t)sy * W + sx)];
			float* o = &rgba[4 * ((size_t)y * W + x)];
			const float n = r.Gauss() * grain;
			for (int k = 0; k < 3; k++) {
				o[k] = std::round(std::clamp(p[k] + n, 0.0f, 1.0f) * 255) / 255;
			}
			o[3] = 1.0f;
		}
	}
}

// Luma, and luma at half resolution blurred (sigma about 8 picture pixels).
static void NRScaleLuma(const std::vector<float>& rgba, int W, int H, std::vector<float>& y, std::vector<float>& lf)
{
	y.resize((size_t)W * H);
	for (size_t p = 0; p < y.size(); p++) {
		y[p] = Luma(&rgba[4 * p]);
	}
	std::vector<float> half;
	LumaHalfFromFloat(rgba, W, H, half);
	Blur(half, W / 2, H / 2, 4, lf);
}

// One measured frame of a row: out against the full-size reference ref, on input in.
static void NRScaleMeasure(NRScaleStats& st, const std::vector<float>& in, const std::vector<float>& ref,
                           const std::vector<float>& out, int W, int H, const NRScaleSequence& s)
{
	double se = 0, dot = 0, norm = 0, effOut = 0, effRef = 0;
	for (size_t i = 0; i < out.size(); i += 4) {
		for (int k = 0; k < 3; k++) {
			const double e = (double)out[i + k] - ref[i + k];
			const double co = (double)out[i + k] - in[i + k];
			const double cr = (double)ref[i + k] - in[i + k];
			se += e * e;
			dot += co * cr;
			norm += cr * cr;
			effOut += std::abs(co);
			effRef += std::abs(cr);
		}
	}
	const double mse = se / (3.0 * (out.size() / 4));
	st.psnr += mse > 0 ? 10 * std::log10(1.0 / mse) : 99.0;
	st.kept += norm > 0 ? dot / norm : 0;
	st.effect += effRef > 0 ? effOut / effRef : 0;
	st.count++;

	std::vector<float> y, lf;
	NRScaleLuma(out, W, H, y, lf);
	if (!st.prevY.empty()) {
		const int margin = 32 + 2 * std::max(std::abs(s.vx), std::abs(s.vy));
		st.flick += WarpDiff(y, st.prevY, W, H, (float)s.vx, (float)s.vy, margin, 0, W);
		st.lfFlick += WarpDiff(lf, st.prevLF, W / 2, H / 2, s.vx * 0.5f, s.vy * 0.5f, margin / 2, 0, W / 2);
		st.flickCount++;
	}
	st.prevY.swap(y);
	st.prevLF.swap(lf);
}

static void NRScaleRow(const char* name, const NRScaleStats& st, bool vsReference)
{
	if (!st.count) {
		Out("  %-20s no usable frames\n", name);
		return;
	}
	const double n = st.count, f = std::max(st.flickCount, 1);
	if (vsReference) {
		Out("  %-20s %8.2f %7.0f%% %7.0f%% %9.5f %9.5f\n", name, st.psnr / n, 100 * st.kept / n, 100 * st.effect / n,
		    st.flick / f, st.lfFlick / f);
	} else {
		Out("  %-20s %8s %8s %8s %9.5f %9.5f\n", name, "-", "-", "-", st.flick / f, st.lfFlick / f);
	}
}

// Waits for every Direct3D 11 command queued so far.
static void NRScaleFinish(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	CComPtr<ID3D11Query> q;
	const D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
		ctx->End(q);
		BOOL done = FALSE;
		while (ctx->GetData(q, &done, sizeof(done), 0) == S_FALSE) {
			Sleep(0);
		}
	}
}

static int RunNRScale(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, const wchar_t* imagePath)
{
	const int W = 3840, H = 2160;
	const int frames = 20, warm = 12;
	const float grain = 1.2f / 255;
	const float diffGain = 8.0f;
	static const int qualityScales[] = { 80, 75, 70, 60, 50 };
	static const int costScales[] = { 100, 90, 80, 75, 70, 60, 50 };
	static const NRScaleSequence seqs[] = {
		{ "still picture, fresh grain every frame", 0, 0 },
		{ "pan (2, 1) px/frame",                    2, 1 },
	};

	Head("DLSS 5 NR at a working size, its change carried back");
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
		std::vector<float> picture;
		std::string error;
		const bool pictureOk = factory && LoadPicture(factory, imagePath, W, H, picture, error);
		Check(pictureOk, "picture decoded at 3840x2160");
		if (!pictureOk) {
			printf("  %S: %s\n", imagePath, error.c_str());
			rc = 1;
		}

		CStabilizerPasses passes;   // the vertex shader, input layout and samplers the renderer has
		if (!rc) {
			const bool passesOk = passes.Init(dev, error);
			Check(passesOk, "vertex shader and samplers");
			if (!passesOk) {
				printf("  %s\n", error.c_str());
				rc = 1;
			}
		}

		CDlssNR::Params params;
		params.iStyle = 0;
		params.iPreset = 0;
		params.fIntensity = 1.50f;
		params.fLocalTone = 0.30f;
		params.fLocalStructure = 0.50f;
		params.fSkinStructure = 0.90f;
		params.bUseAutoMask = true;
		params.bNoHistory = false;

		CDlssStabilizer stabilizer;
		CDlssCompose compose[4];   // one per method, indexed by CDlssCompose::Method
		Tex2D_t texFull, texIn, texOut;
		CComPtr<ID3D11RenderTargetView> inTarget;
		CComPtr<ID3D11Texture2D> stage;

		// The network and the stabilizer at workW x workH; the reduction and the
		// compositions for every method from W x H when that is smaller.
		const auto Setup = [&](UINT fullW, UINT fullH, UINT workW, UINT workH) -> bool {
			dlss.ReleaseFeature();
			dlss.SetGuides(CDlssNR::Guides{});
			stabilizer.Release();
			for (auto& c : compose) {
				c.Release();
			}
			inTarget.Release();
			if (!MakeSharedPair(dev, texIn, texOut, workW, workH)
					|| FAILED(dev->CreateRenderTargetView(texIn.pTexture, nullptr, &inTarget))
					|| FAILED(texFull.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, fullW, fullH, Tex2D_DefaultShaderRTarget))) {
				Out("  textures at %ux%u from %ux%u failed\n", workW, workH, fullW, fullH);
				return false;
			}
			if (FAILED(stabilizer.Create(dev, ctx, workW, workH, CDlssStabilizer::Motion::OpticalFlow,
			                             passes.InputLayout(), passes.VertexShader(), passes.SamplerPoint(), passes.SamplerLinear()))
					|| stabilizer.ActiveMotion() != CDlssStabilizer::Motion::OpticalFlow) {
				Out("  the stabilizer at %ux%u did not start on Optical Flow: %S\n", workW, workH, stabilizer.GetStatusLine().c_str());
				return false;
			}
			if (workW != fullW || workH != fullH) {
				for (int m = 0; m < 4; m++) {
					if (FAILED(compose[m].Create(dev, fullW, fullH, workW, workH, (CDlssCompose::Method)m,
					                             passes.InputLayout(), passes.VertexShader()))) {
						Out("  composition %d at %ux%u from %ux%u failed\n", m, workW, workH, fullW, fullH);
						return false;
					}
				}
			}
			if (!dlss.CreateFeature(texIn.pTexture, texOut.pTexture, workW, workH, params)) {
				Out("  the network at %ux%u could not start: %S\n", workW, workH, dlss.GetStatusLine().c_str());
				return false;
			}
			dlss.RequestReset();
			return true;
		};

		// One picture already in texFull through the chain, as DlssNRPass runs it.
		const auto Network = [&](bool reduce) -> bool {
			if (reduce) {
				compose[0].Reduce(ctx, texFull.pShaderResource, inTarget);
			} else {
				ctx->CopyResource(texIn.pTexture, texFull.pTexture);
			}
			stabilizer.PrepareMotion(ctx, texIn.pShaderResource);
			ID3D11ShaderResourceView* nullSRVs[4] = {};
			ctx->PSSetShaderResources(0, (UINT)std::size(nullSRVs), nullSRVs);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);
			if (!dlss.Evaluate(params)) {
				return false;
			}
			stabilizer.Stabilize(ctx, texIn.pShaderResource, texOut.pShaderResource, 1.0f, true);
			return true;
		};

		if (!rc) {
			D3D11_TEXTURE2D_DESC sd = CreateTex2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_Default);
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			Check(SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage)), "readback staging 3840x2160");
			if (!stage) {
				rc = 1;
			}
		}

		if (!rc) {
			g_report = fopen("temporal_results_nrscale.txt", "w");
			Out("\nDLSS 5 NR at a working size -- %S at %dx%d, %d frames per run, the last %d measured.\n",
			    imagePath, W, H, frames, frames - warm);
			Out("The filter's default settings; grain %.1f/255 fresh every frame. Each row is the network and the stabilizer at\n",
			    grain * 255);
			Out("that share of the size, reduced with Lanczos, and the change carried back onto the picture by CDlssCompose.\n");
			Out("PSNR = against the network and the stabilizer at the full size, frame for frame, RGB. kept = the change from the\n");
			Out("input projected on the full-size change (100%% = all of it, in the same direction). effect = mean |change| as a\n");
			Out("share of the full-size one. flick = mean |Y(t) - Y(t-1)| after the true motion; LF = the same on luma blurred\n");
			Out("to sigma ~8 px.\n");

			std::vector<float> rgba, out, inY, inLF;
			std::vector<HALF> half(4 * (size_t)W * H);
			std::vector<std::vector<float>> reference(frames - warm);
			const Rect crop = { W / 2 - 480, H / 2 - 270, W / 2 + 480, H / 2 + 270 };

			for (size_t si = 0; si < std::size(seqs) && !rc; si++) {
				const NRScaleSequence& s = seqs[si];
				Out("\n%s\n", s.name);
				Out("  %-20s %8s %8s %8s %9s %9s\n", "", "PSNR", "kept", "effect", "flick", "LF flick");

				// The full size first: the reference every other row is measured against.
				NRScaleStats inputStats, refStats;
				if (!Setup(W, H, W, H)) {
					g_failures++;
					break;
				}
				int failed = 0;
				for (int t = 0; t < frames; t++) {
					NRScaleFrame(picture, W, H, s, t, grain, rgba);
					DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
					ctx->UpdateSubresource(texFull.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);
					if (!Network(false)) {
						failed++;
						continue;
					}
					if (t < warm) {
						continue;
					}
					ctx->CopyResource(stage, stabilizer.GetResult()->pTexture);
					if (!ReadRgbaHalf(ctx, stage, W, H, reference[t - warm])) {
						failed++;
						continue;
					}
					NRScaleMeasure(inputStats, rgba, reference[t - warm], rgba, W, H, s);
					NRScaleMeasure(refStats, rgba, reference[t - warm], reference[t - warm], W, H, s);
				}
				if (failed || refStats.count != frames - warm) {
					Out("  the full-size run lost %d frames: %S\n", failed, dlss.GetStatusLine().c_str());
					g_failures++;
					break;
				}
				NRScaleRow("(input)", inputStats, true);
				NRScaleRow("(100 %)", refStats, false);
				if (si == 0) {
					SavePng(factory, L"nrscale_input.png", rgba, W, crop, 1.0f);
					SavePng(factory, L"nrscale_full.png", reference.back(), W, crop, 1.0f);
					std::vector<float> diff(rgba.size());
					for (size_t i = 0; i < diff.size(); i++) {
						diff[i] = std::abs(reference.back()[i] - rgba[i]);
					}
					SavePng(factory, L"nrscale_full_diff.png", diff, W, crop, diffGain);
				}

				for (const int scale : qualityScales) {
					UINT ww = 0, wh = 0;
					CDlssCompose::WorkingSize(W, H, scale, ww, wh);
					Out("  %d %% (%ux%u)\n", scale, ww, wh);
					if (!Setup(W, H, ww, wh)) {
						g_failures++;
						continue;
					}
					std::vector<NRScaleStats> stats(std::size(s_NRScaleVariants));
					failed = 0;
					for (int t = 0; t < frames; t++) {
						NRScaleFrame(picture, W, H, s, t, grain, rgba);
						DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
						ctx->UpdateSubresource(texFull.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);
						if (!Network(true)) {
							failed++;
							continue;
						}
						if (t < warm) {
							continue;
						}
						const std::vector<float>& ref = reference[t - warm];
						for (size_t vi = 0; vi < std::size(s_NRScaleVariants); vi++) {
							const NRScaleVariant& v = s_NRScaleVariants[vi];
							CDlssCompose& c = compose[(int)v.method];
							c.Compose(ctx, texFull.pShaderResource, texIn.pShaderResource,
							          stabilizer.GetResult()->pShaderResource, v.tuning);
							ctx->CopyResource(stage, c.GetResult()->pTexture);
							if (!ReadRgbaHalf(ctx, stage, W, H, out)) {
								failed++;
								continue;
							}
							NRScaleMeasure(stats[vi], rgba, ref, out, W, H, s);
							if (si == 0 && scale == 75 && t == frames - 1 && v.png) {
								std::vector<float> diff(out.size());
								for (size_t i = 0; i < diff.size(); i++) {
									diff[i] = std::abs(out[i] - rgba[i]);
								}
								SavePng(factory, std::format(L"nrscale_75_{}.png", v.png).c_str(), out, W, crop, 1.0f);
								SavePng(factory, std::format(L"nrscale_75_{}_diff.png", v.png).c_str(), diff, W, crop, diffGain);
							}
						}
					}
					if (failed) {
						Out("    %d frames lost: %S\n", failed, dlss.GetStatusLine().c_str());
						g_failures++;
					}
					for (size_t vi = 0; vi < std::size(s_NRScaleVariants); vi++) {
						NRScaleRow(std::format("  {}", s_NRScaleVariants[vi].name).c_str(), stats[vi], true);
					}
				}
			}

			// ---- cost --------------------------------------------------------------
			// The network's own time is the filter's measure of it (record + GPU wait); the
			// other stages are Direct3D 11 timestamps. chain = the wall time of a whole
			// picture: reduction, motion, network, stabilizer and the difference method.
			static const struct { int w, h; } sizes[] = { { 3840, 2160 }, { 3840, 1600 } };
			Out("\nCost, ms per picture on this GPU (the still picture, %d pictures each)\n", 16);
			Out("  %-30s %7s %7s %7s %7s %7s %7s %7s %7s\n",
			    "", "reduce", "motion", "network", "stab", "diff", "joint", "guided", "chain");
			for (const auto& size : sizes) {
				const UINT fw = size.w, fh = size.h;
				std::vector<float> framePicture;
				if (!LoadPicture(factory, imagePath, (int)fw, (int)fh, framePicture, error)) {
					Out("  %ux%u: %s\n", fw, fh, error.c_str());
					g_failures++;
					continue;
				}
				const NRScaleSequence still = { "", 0, 0 };
				NRScaleFrame(framePicture, (int)fw, (int)fh, still, 0, grain, rgba);
				std::vector<HALF> frameHalf(rgba.size());
				DirectX::PackedVector::XMConvertFloatToHalfStream(frameHalf.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());

				for (const int scale : costScales) {
					UINT ww = 0, wh = 0;
					CDlssCompose::WorkingSize(fw, fh, scale, ww, wh);
					if (!Setup(fw, fh, ww, wh)) {
						g_failures++;
						continue;
					}
					ctx->UpdateSubresource(texFull.pTexture, 0, nullptr, frameHalf.data(), 4 * sizeof(HALF) * fw, 0);
					const bool reduce = (ww != fw || wh != fh);
					for (int i = 0; i < 4; i++) {   // the stabilizer's history and the network's settle
						Network(reduce);
					}

					const int runs = 16;
					double network = 0, chain = 0;
					int ok = 0;
					LARGE_INTEGER qpf = {};
					QueryPerformanceFrequency(&qpf);
					for (int i = 0; i < runs; i++) {
						NRScaleFinish(dev, ctx);
						LARGE_INTEGER t0 = {}, t1 = {};
						QueryPerformanceCounter(&t0);
						if (!Network(reduce)) {
							continue;
						}
						if (reduce) {
							compose[(int)CDlssCompose::Method::Difference].Compose(ctx, texFull.pShaderResource, texIn.pShaderResource,
								stabilizer.GetResult()->pShaderResource);
						}
						NRScaleFinish(dev, ctx);
						QueryPerformanceCounter(&t1);
						network += dlss.LastTiming().recordMs + dlss.LastTiming().gpuWaitMs;
						chain += double(t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart;
						ok++;
					}
					if (!ok) {
						Out("  %u x %u at %d %%: the network failed: %S\n", fw, fh, scale, dlss.GetStatusLine().c_str());
						g_failures++;
						continue;
					}

					const double reduceMs = reduce ? GpuMs(dev, ctx, 16, [&] { compose[0].Reduce(ctx, texFull.pShaderResource, inTarget); }) : 0;
					const double motionMs = GpuMs(dev, ctx, 8, [&] { stabilizer.PrepareMotion(ctx, texIn.pShaderResource); });
					const double stabMs = GpuMs(dev, ctx, 16, [&] {
						stabilizer.Stabilize(ctx, texIn.pShaderResource, texOut.pShaderResource, 1.0f, false);
					});
					double composeMs[4] = {};
					if (reduce) {
						for (int m = 1; m < 4; m++) {
							composeMs[m] = GpuMs(dev, ctx, 16, [&] {
								compose[m].Compose(ctx, texFull.pShaderResource, texIn.pShaderResource, stabilizer.GetResult()->pShaderResource);
							});
						}
					}
					const std::string label = std::format("{}x{} {} % ({}x{})", fw, fh, scale, ww, wh);
					if (reduce) {
						Out("  %-30s %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f\n", label.c_str(),
						    reduceMs, motionMs, network / ok, stabMs, composeMs[1], composeMs[2], composeMs[3], chain / ok);
					} else {
						Out("  %-30s %7s %7.2f %7.2f %7.2f %7s %7s %7s %7.2f\n", label.c_str(),
						    "-", motionMs, network / ok, stabMs, "-", "-", "-", chain / ok);
					}
				}
			}

			Out("\nPictures: the centre %dx%d of the input, of the full-size result and of the 75 %% compositions\n",
			    crop.x1 - crop.x0, crop.y1 - crop.y0);
			Out("(nrscale_*.png), with their changes from the input amplified %.0fx (*_diff.png).\n", diffGain);
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_nrscale.txt\n");
		}

		dlss.ReleaseFeature();
		dlss.SetGuides(CDlssNR::Guides{});
		stabilizer.Release();
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
