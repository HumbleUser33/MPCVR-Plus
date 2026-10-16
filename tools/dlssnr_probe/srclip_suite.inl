// --tsrclip <folder>: DLSS Super Resolution on a real film's pictures, a folder of PNG
// frames in order (fNNNN.png, as ffmpeg writes them), enlarged twice.
//
// Included by harness.cpp after sr_still_suite.inl, whose readers it uses. --tsrstill
// grains a still picture with noise of its own making; a film brings its own grain,
// its compression's blocks, its noise -- and its weave. The wall that shimmered behind
// the actors in Cyrano (2026-10-15) is not still at all: the whole picture moves by 0.2
// to 1 source pixel from one frame to the next, 0.48 on average, and 62 % of the frames
// move more than the 0.4 pixel under which CDlssStabilizer::ForDlssSR called the global
// motion none.
//
// So nothing is measured at fixed places. The folder carries motion.txt, written by
// clip_motion.py ('fNNNN.png dx dy', the content's displacement, source pixels): each
// frame registered against the one before at the source's size, what moves on its own
// left out, the film's own global motion. Every picture is compared with the one before
// taken where its content came from, and the parts of the film that hold still once that
// motion is taken out are found the same way: 8 x 8 source blocks whose compensated mean
// hardly changes over the clip, none of whose neighbours moves on its own. Over those, on
// luma, x1000:
//   wob    the 2 x 2 source-pixel block means of that difference: structures moving
//          against the film, the shimmer
//   flick  the rest of it: the grain changing
// against the source's own; and detail, the mean luma gradient there, x1000, which
// says whether a steadier row is only a softer one. For the rows that compute vectors:
// mv err, how far the vectors DLSS got on those blocks lie from the film's motion, source
// pixels; gm err, the same for the global motion PASS 4 starts from, and dead, the share
// of frames where it fell under the dead zone and counted as none.
//
// Rows: the filter's vectors, the median global motion with the 0.4-pixel dead zone it
// had until 2.0, then without it, refined with nothing left out, refined with no block
// vector gate or three times, the film's own global motion given everywhere, vectors at
// zero, finer flow, the engine's finer grid and slower search, and DLSS's presets. --tsrclipwhich
// now,film,... runs only the rows of those tags. With --tsrcrops the crop of the most
// textured still area is saved for a few rows, every measured frame, beside the folder
// (<folder>_crops\<tag>\), to be looked at and played. Then what the refinement costs,
// the filter's motion timed in turns with the median's (the tag "cost" runs that alone).
//
// --tsrclipnr runs the frames through DLSS 5 NR and its stabilizer first, as the
// renderer does before DLSS SR, and DLSS SR's vectors are computed after DLSS 5 NR, as
// today, or on the frame before it, as one Optical Flow shared by the two would.

namespace temporal {

namespace {

struct ClipRow {
	const char* name;
	const wchar_t* tag;                     // for --tsrclipwhich, and the crops' folder
	int motion = 1;                         // 0 vectors at zero, 1 Optical Flow, 2 the film's global motion
	CDlssStabilizer::FlowSettings flow;
	unsigned preset = NGX_DLSS_PRESET_Default;
	int flowOn = 0;                         // with --tsrclipnr, 1: the vectors computed on the frame before DLSS 5 NR
};

CDlssStabilizer::FlowSettings ClipFlow(UINT factor, UINT blur, UINT grid = 4, NV_OF_PERF_LEVEL perf = NV_OF_PERF_LEVEL_MEDIUM)
{
	CDlssStabilizer::FlowSettings flow = CDlssStabilizer::ForDlssSR();
	if (factor) {
		flow.flowFactor = factor;
		flow.flowBlur = blur;
	}
	flow.gridSize = grid;
	flow.perfLevel = perf;
	return flow;
}

// The filter's vectors with another global motion: the dead zone under which it counts
// as none, the Gauss-Newton steps refining it, the residual past which a group is left
// out of them (0: none is), and how far a block's own vector may lie from it to take part.
CDlssStabilizer::FlowSettings ClipGlobal(float deadZone, int refine, float robust, float ofGate = 1.0f)
{
	CDlssStabilizer::FlowSettings flow = CDlssStabilizer::ForDlssSR();
	flow.snapDeadZone = deadZone;
	flow.globalRefine = refine;
	flow.globalRobust = robust;
	flow.globalNear = ofGate;
	return flow;
}

// A PNG at its own size, 16 bits a channel kept, as RGBA half floats.
bool LoadClipFrame(IWICImagingFactory* factory, const wchar_t* path, int& W, int& H, std::vector<HALF>& half, std::string& error)
{
	CComPtr<IWICBitmapDecoder> decoder;
	CComPtr<IWICBitmapFrameDecode> frame;
	HRESULT hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
	if (SUCCEEDED(hr)) {
		hr = decoder->GetFrame(0, &frame);
	}
	UINT sw = 0, sh = 0;
	if (SUCCEEDED(hr)) {
		hr = frame->GetSize(&sw, &sh);
	}
	CComPtr<IWICFormatConverter> converter;
	if (SUCCEEDED(hr)) {
		hr = factory->CreateFormatConverter(&converter);
	}
	if (SUCCEEDED(hr)) {
		hr = converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	}
	// Multiples of 8: the output is twice the source on each side, and the flow divides it.
	W = (int)sw & ~7;
	H = (int)sh & ~7;
	std::vector<uint16_t> words(4 * (size_t)W * H);
	if (SUCCEEDED(hr)) {
		const WICRect rect = { 0, 0, W, H };
		hr = converter->CopyPixels(&rect, 8 * (UINT)W, (UINT)(words.size() * 2), (BYTE*)words.data());
	}
	if (FAILED(hr) || W < 64 || H < 64) {
		error = std::format("cannot read the frame (0x{:08X})", (unsigned)hr);
		return false;
	}
	std::vector<float> rgba(words.size());
	for (size_t i = 0; i < words.size(); i++) {
		rgba[i] = (i % 4 == 3) ? 1.0f : words[i] / 65535.0f;
	}
	half.resize(rgba.size());
	DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
	return true;
}

void ClipLuma(const float* rgba, int W, int H, std::vector<float>& y)
{
	y.resize((size_t)W * H);
	for (size_t i = 0; i < y.size(); i++) {
		y[i] = Luma(&rgba[4 * i]);
	}
}

// What changed at (x, y) since the last picture, taken where its content came from:
// y(x) - last(x - d), bilinear. False where that place lies outside the last picture.
inline bool ClipChange(const std::vector<float>& y, const std::vector<float>& last, int W, int H, int x, int yy,
                       float dx, float dy, float& r)
{
	const float sx = x - dx, sy = yy - dy;
	if (sx < 0 || sy < 0 || sx > W - 1.001f || sy > H - 1.001f) {
		return false;
	}
	const int x0 = (int)sx, y0 = (int)sy;
	const float tx = sx - x0, ty = sy - y0;
	const float* a = &last[(size_t)y0 * W + x0];
	const float* c = a + W;
	const float prev = (a[0] * (1 - tx) + a[1] * tx) * (1 - ty) + (c[0] * (1 - tx) + c[1] * tx) * ty;
	r = y[(size_t)yy * W + x] - prev;
	return true;
}

// One picture's wob and flick against the last, over the still 8 x 8 source blocks,
// compensated by the film's motion d (source pixels). scale: output pixels per source
// pixel; the measuring blocks are 2 x 2 source pixels.
struct ClipSteadiness {
	double wob = 0, flick = 0, detail = 0;
	int n = 0, nDetail = 0;
	std::vector<float> yLast;

	void Add(const float* rgba, int W, int H, int scale, float dx, float dy, const std::vector<uint8_t>& still, int stillW)
	{
		std::vector<float> y;
		ClipLuma(rgba, W, H, y);
		{
			// How sharp the still blocks are: the mean gradient, at the source's scale.
			double g = 0;
			long long ng = 0;
			for (int yy = scale; yy < H - scale; yy++) {
				for (int xx = scale; xx < W - scale; xx++) {
					if (!still[(size_t)(yy / (8 * scale)) * stillW + xx / (8 * scale)]) {
						continue;
					}
					const size_t i = (size_t)yy * W + xx;
					const float gx = y[i + scale] - y[i - scale], gy = y[i + (size_t)scale * W] - y[i - (size_t)scale * W];
					g += std::sqrt(gx * gx + gy * gy);
					ng++;
				}
			}
			if (ng) {
				detail += 1000.0 * g / ng;
				nDetail++;
			}
		}
		if (!yLast.empty()) {
			const int b = 2 * scale;
			double w = 0, f = 0;
			long long nw = 0, nf = 0;
			float r[64];
			for (int by = 0; by < H / b; by++) {
				for (int bx = 0; bx < W / b; bx++) {
					if (!still[(size_t)(by / 4) * stillW + bx / 4]) {
						continue;
					}
					double m = 0;
					int k = 0;
					bool inside = true;
					for (int yy = by * b; yy < (by + 1) * b && inside; yy++) {
						for (int xx = bx * b; xx < (bx + 1) * b && inside; xx++) {
							inside = ClipChange(y, yLast, W, H, xx, yy, dx * scale, dy * scale, r[k]);
							m += r[k++];
						}
					}
					if (!inside) {
						continue;
					}
					m /= k;
					w += std::abs(m);
					nw++;
					for (int i = 0; i < k; i++) {
						f += std::abs(r[i] - m);
					}
					nf += k;
				}
			}
			if (nw && nf) {
				wob += 1000.0 * w / nw;
				flick += 1000.0 * f / nf;
				n++;
			}
		}
		yLast.swap(y);
	}
	double Wob() const { return n ? wob / n : 0.0; }
	double Flick() const { return n ? flick / n : 0.0; }
	double Detail() const { return nDetail ? detail / nDetail : 0.0; }
};

} // namespace

static int RunSRClip(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* folder, const wchar_t* dllPath, bool saveCrops,
                     const wchar_t* which, CDlssNR* nr)
{
	Head("DLSS Super Resolution on a real film's frames");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");

		std::vector<std::wstring> names;
		{
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileW((std::wstring(folder) + L"\\f*.png").c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do {
					names.push_back(fd.cFileName);
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			std::sort(names.begin(), names.end());
		}
		const int kWarm = 24;   // a second of film for DLSS to settle before anything is measured
		Check(names.size() > (size_t)kWarm + 8, "frames found");
		if (names.size() <= (size_t)kWarm + 8) {
			printf("  %S: put the frames there first (fNNNN.png ...)\n", folder);
			rc = 1;
		}

		// The film's own motion, frame by frame.
		std::vector<float> filmDx(names.size(), 0.0f), filmDy(names.size(), 0.0f);
		if (!rc) {
			std::map<std::wstring, std::pair<float, float>> motion;
			FILE* f = _wfopen((std::wstring(folder) + L"\\motion.txt").c_str(), L"r");
			if (f) {
				char name[64] = {};
				float dx = 0, dy = 0;
				while (fscanf(f, "%63s %f %f", name, &dx, &dy) == 3) {
					motion[std::wstring(name, name + strlen(name))] = { dx, dy };
				}
				fclose(f);
			}
			int found = 0;
			for (size_t i = 1; i < names.size(); i++) {
				const auto it = motion.find(names[i]);
				if (it != motion.end()) {
					filmDx[i] = it->second.first;
					filmDy[i] = it->second.second;
					found++;
				}
			}
			Check(found == (int)names.size() - 1, "the film's motion for every frame (motion.txt)");
			if (found != (int)names.size() - 1) {
				rc = 1;
			}
		}

		// Every frame, as the renderer would hand it to DLSS SR: RGB at the source's size.
		int W = 0, H = 0;
		std::vector<std::vector<HALF>> frames;
		std::string error;
		for (size_t i = 0; !rc && i < names.size(); i++) {
			int w = 0, h = 0;
			std::vector<HALF> half;
			if (!LoadClipFrame(factory, (std::wstring(folder) + L"\\" + names[i]).c_str(), w, h, half, error) || (i && (w != W || h != H))) {
				printf("  %S: %s\n", names[i].c_str(), error.empty() ? "another size" : error.c_str());
				rc = 1;
				break;
			}
			W = w;
			H = h;
			frames.push_back(std::move(half));
		}
		const int N = (int)frames.size();
		Check(!rc, "frames decoded");

		// Where the film holds still once its own motion is taken out: 8 x 8 source
		// blocks whose compensated mean changes under 1.5 times the typical block's and
		// never more than 3 times the typical largest change, with no moving block around.
		// Black (the bars) is left out: there is nothing to see shimmer on.
		const int sbw = W / 8, sbh = H / 8;
		std::vector<uint8_t> still((size_t)sbw * sbh, 0);
		std::vector<float> texture((size_t)sbw * sbh, 0.0f);
		ClipSteadiness sourceSteady;
		double filmMean = 0;
		if (!rc) {
			std::vector<double> meanChange((size_t)sbw * sbh, 0.0), maxChange((size_t)sbw * sbh, 0.0), level((size_t)sbw * sbh, 0.0);
			std::vector<float> rgba, y, yLast;
			for (int t = kWarm - 1; t < N; t++) {
				rgba.resize(frames[t].size());
				DirectX::PackedVector::XMConvertHalfToFloatStream(rgba.data(), sizeof(float), frames[t].data(), sizeof(HALF), rgba.size());
				ClipLuma(rgba.data(), W, H, y);
				if (t == kWarm) {
					for (int by = 0; by < sbh; by++) {
						for (int bx = 0; bx < sbw; bx++) {
							double m = 0, q = 0;
							for (int yy = 0; yy < 8; yy++) {
								for (int xx = 0; xx < 8; xx++) {
									const double v = y[(size_t)(8 * by + yy) * W + 8 * bx + xx];
									m += v;
									q += v * v;
								}
							}
							m /= 64.0;
							texture[(size_t)by * sbw + bx] = (float)std::sqrt(std::max(q / 64.0 - m * m, 0.0));
						}
					}
				}
				if (!yLast.empty()) {
					for (int by = 0; by < sbh; by++) {
						for (int bx = 0; bx < sbw; bx++) {
							const size_t k = (size_t)by * sbw + bx;
							double m = 0, lv = 0;
							int n = 0;
							for (int yy = 8 * by; yy < 8 * by + 8; yy++) {
								for (int xx = 8 * bx; xx < 8 * bx + 8; xx++) {
									float r = 0;
									if (ClipChange(y, yLast, W, H, xx, yy, filmDx[t], filmDy[t], r)) {
										m += r;
										n++;
									}
									lv += y[(size_t)yy * W + xx];
								}
							}
							const double d = n ? std::abs(m / n) : 1.0;
							meanChange[k] += d / (N - kWarm);
							maxChange[k] = std::max(maxChange[k], d);
							level[k] += lv / 64.0 / (N - kWarm);
						}
					}
				}
				yLast.swap(y);
			}
			auto median = [](std::vector<double> v) {
				if (v.empty()) {
					return 0.0;
				}
				std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
				return v[v.size() / 2];
			};
			std::vector<double> lit, litMax;
			for (size_t k = 0; k < meanChange.size(); k++) {
				if (level[k] > 0.03) {
					lit.push_back(meanChange[k]);
					litMax.push_back(maxChange[k]);
				}
			}
			const double typical = median(lit), typicalMax = median(litMax);
			std::vector<uint8_t> moving((size_t)sbw * sbh, 0), quiet((size_t)sbw * sbh, 0);
			for (size_t k = 0; k < meanChange.size(); k++) {
				if (level[k] <= 0.03) {
					continue;
				}
				if (meanChange[k] < 1.5 * typical && maxChange[k] < 3.0 * typicalMax) {
					quiet[k] = 1;
				} else {
					moving[k] = 1;
				}
			}
			int nStill = 0, nLit = 0;
			for (int by = 0; by < sbh; by++) {
				for (int bx = 0; bx < sbw; bx++) {
					const size_t k = (size_t)by * sbw + bx;
					nLit += (level[k] > 0.03);
					if (!quiet[k] || bx < 2 || by < 2 || bx >= sbw - 2 || by >= sbh - 2) {
						continue;
					}
					bool byMotion = false;
					for (int j = -1; j <= 1 && !byMotion; j++) {
						for (int i = -1; i <= 1 && !byMotion; i++) {
							byMotion = moving[(size_t)(by + j) * sbw + bx + i] != 0;
						}
					}
					still[k] = !byMotion;
					nStill += still[k];
				}
			}

			// The source's own steadiness there, the yardstick.
			for (int t = kWarm; t < N; t++) {
				rgba.resize(frames[t].size());
				DirectX::PackedVector::XMConvertHalfToFloatStream(rgba.data(), sizeof(float), frames[t].data(), sizeof(HALF), rgba.size());
				sourceSteady.Add(rgba.data(), W, H, 1, filmDx[t], filmDy[t], still, sbw);
				filmMean += std::hypot(filmDx[t], filmDy[t]) / (N - kWarm);
			}

			const std::wstring name = std::wstring(folder).substr(std::wstring(folder).find_last_of(L"\\/") + 1);
			g_report = _wfopen(std::format(L"temporal_results_srclip_{}.txt", name).c_str(), L"w");
			Out("\nDLSS Super Resolution on %S: %d frames of %dx%d, enlarged twice; the first %d to settle.\n",
			    folder, N, W, H, kWarm);
			Out("The film's own motion from frame to frame: %.3f source pixels on average.\n", filmMean);
			Out("Still once it is taken out: %d of the %d lit 8x8 source blocks (%.0f %%). Each picture against the one\n",
			    nStill, nLit, nLit ? 100.0 * nStill / nLit : 0.0);
			Out("before where its content came from: wob, the 2x2 source-pixel block means of the difference; flick, the\n");
			Out("rest of it (the grain); luma x1000 over the still blocks, and against the source's own. mv err: the vectors\n");
			Out("DLSS got there against the film's motion; gm err: the global motion PASS 4 starts from, the same; dead: the\n");
			Out("frames where it fell under the dead zone. Source pixels. OF: Optical Flow and its passes, GPU ms.\n\n");
			Out("  %-26s %7s %6s %7s %6s %7s %7s %7s %6s %7s  %s\n", "", "wob", "ratio", "flick", "ratio", "detail", "mv err", "gm err",
			    "dead", "OF ms", "DLSS");
			Out("  %-26s %7.3f %6s %7.3f %6s %7.2f\n", "(source)", sourceSteady.Wob(), "1.00", sourceSteady.Flick(), "1.00",
			    sourceSteady.Detail());
		}

		// The crop: the 256 x 256 source window with the most still, textured blocks.
		Rect crop = { 0, 0, 0, 0 };
		if (!rc) {
			const int win = 32;   // blocks
			double best = -1;
			for (int by = 0; by + win <= sbh; by += 4) {
				for (int bx = 0; bx + win <= sbw; bx += 4) {
					double s = 0;
					for (int j = 0; j < win; j++) {
						for (int i = 0; i < win; i++) {
							const size_t k = (size_t)(by + j) * sbw + bx + i;
							s += still[k] ? 0.02 + texture[k] : 0.0;
						}
					}
					if (s > best) {
						best = s;
						crop = { 16 * bx, 16 * by, 16 * (bx + win), 16 * (by + win) };
					}
				}
			}
		}

		CStabilizerPasses stabPasses;
		if (!rc && !stabPasses.Init(dev, error)) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}
		// With --tsrclipnr the frames go through DLSS 5 NR and its stabilizer first, as the
		// renderer runs them before DLSS SR, and DLSS SR enlarges what comes out. Its vectors
		// are computed there, as today, or on the frame before DLSS 5 NR: what one Optical
		// Flow shared by the two would see, since the stabilizer's runs on that frame.
		std::vector<std::vector<HALF>> nrFrames;
		// And what DLSS 5 NR itself is given: the cleaned vectors the filter gives it
		// (CDlssStabilizer::ForDlssNR); none, as until 2.0 by default; the stabilizer's raw
		// ones, what the box of that time sent. Only the first run's frames go on to DLSS SR.
		static const char* const nrVectors[3] = { "(after DLSS 5 NR)", "(NR, no vectors)", "(NR, raw vectors)" };
		for (int mode = 0; !rc && nr && mode < 3; mode++) {
			Tex2D_t nrIn, nrOut, vectors;
			CDlssNR::Params params;
			params.bNoHistory = false;   // as DlssParamsFromSettings
			CDlssStabilizer stabilizer;
			bool ok = MakeSharedPair(dev, nrIn, nrOut, W, H) && nr->CreateFeature(nrIn.pTexture, nrOut.pTexture, W, H, params)
				&& SUCCEEDED(stabilizer.Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
					stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), false,
					mode == 0 ? CDlssStabilizer::ForDlssNR(0) : CDlssStabilizer::FlowSettings()))
				&& stabilizer.HasFlow();
			if (ok && mode != 1) {
				// The network takes its guides through a shared texture: the vectors are copied in.
				ok = SUCCEEDED(vectors.Create(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTargetUAVShared));
				if (ok) {
					CDlssNR::Guides guides;
					guides.pMVec = vectors.pTexture;
					ok = nr->SetGuides(guides);
				}
			}
			ClipSteadiness nrSteady;
			std::vector<float> out;
			for (int t = 0; ok && t < N; t++) {
				ctx->UpdateSubresource(nrIn.pTexture, 0, nullptr, frames[t].data(), 4 * sizeof(HALF) * W, 0);
				stabilizer.PrepareMotion(ctx, nrIn.pShaderResource);
				if (mode != 1) {
					ID3D11Texture2D* pSource = stabilizer.GetMotionVectors();
					D3D11_TEXTURE2D_DESC desc = {};
					if (pSource) {
						pSource->GetDesc(&desc);
					}
					ok = pSource && desc.Width == (UINT)W && desc.Height == (UINT)H && desc.Format == DXGI_FORMAT_R16G16_FLOAT;
					if (!ok) {
						Out("  %s: the vectors are not %dx%d RG16F\n", nrVectors[mode], W, H);
						break;
					}
					ctx->CopyResource(vectors.pTexture, pSource);
				}
				ok = nr->Evaluate(params);
				if (ok) {
					stabilizer.Stabilize(ctx, nrIn.pShaderResource, nrOut.pShaderResource, 1.0f, true);
					ok = ReadTexture(dev, ctx, stabilizer.GetResult()->pTexture, out);
				}
				if (ok) {
					if (t >= kWarm) {
						nrSteady.Add(out.data(), W, H, 1, filmDx[t], filmDy[t], still, sbw);
					}
					if (mode == 0) {
						std::vector<HALF> half(out.size());
						DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), out.data(), sizeof(float), out.size());
						nrFrames.push_back(std::move(half));
					}
				}
			}
			nr->SetGuides(CDlssNR::Guides());
			nr->ReleaseFeature();
			Check(ok, std::format("the frames through DLSS 5 NR and its stabilizer, {}", nrVectors[mode]).c_str());
			if (ok) {
				const double sw = sourceSteady.Wob(), sf = sourceSteady.Flick();
				Out("  %-26s %7.3f %6.2f %7.3f %6.2f %7.2f\n", nrVectors[mode], nrSteady.Wob(), sw > 0 ? nrSteady.Wob() / sw : 0.0,
				    nrSteady.Flick(), sf > 0 ? nrSteady.Flick() / sf : 0.0, nrSteady.Detail());
			} else {
				rc = 1;
			}
		}

		CDlssSR sr;
		if (!rc && !sr.Init(dev, dllPath)) {
			printf("%S", sr.GetInfoBlock().c_str());
			rc = 1;
		}

		const std::vector<ClipRow> nrRows = {
			{ "NR, flow after it",          L"nrA",    1, CDlssStabilizer::ForDlssSR() },
			{ "NR, flow before it",         L"nrB",    1, CDlssStabilizer::ForDlssSR(), NGX_DLSS_PRESET_Default, 1 },
			{ "NR, median (until 2.0)",     L"nrM",    1, ClipGlobal(0.4f, 0, 0.0f) },
			{ "NR, the film's global",      L"nrF",    2 },
		};
		const std::vector<ClipRow> srRows = {
			{ "filter now",                 L"now",    1, CDlssStabilizer::ForDlssSR() },
			{ "median (until 2.0)",         L"med",    1, ClipGlobal(0.4f, 0, 0.0f) },
			{ "median, dead zone 0",        L"dz0",    1, ClipGlobal(0.0f, 0, 0.0f) },
			{ "refined, nothing left out",  L"ref",    1, ClipGlobal(0.0f, 2, 0.0f) },
			{ "refined, no vector gate",    L"robA",   1, ClipGlobal(0.0f, 2, 2.5f, 1e6f) },
			{ "refined three times",        L"rob3",   1, ClipGlobal(0.0f, 3, 2.5f) },
			{ "the film's global",          L"film",   2 },
			{ "vectors at zero",            L"zero",   0 },
			{ "flow /1, blur 1",            L"f1b1",   1, ClipFlow(1, 1) },
			{ "flow /1, blur 2",            L"f1b2",   1, ClipFlow(1, 2) },
			{ "grid 2",                     L"g2",     1, ClipFlow(0, 0, 2) },
			{ "slow search",                L"slow",   1, ClipFlow(0, 0, 4, NV_OF_PERF_LEVEL_SLOW) },
			{ "preset J",                   L"J",      1, CDlssStabilizer::ForDlssSR(), NGX_DLSS_PRESET_J },
			{ "preset K",                   L"K",      1, CDlssStabilizer::ForDlssSR(), NGX_DLSS_PRESET_K },
			{ "preset L",                   L"L",      1, CDlssStabilizer::ForDlssSR(), NGX_DLSS_PRESET_L },
			{ "preset K, median (2.0)",     L"Kmed",   1, ClipGlobal(0.4f, 0, 0.0f), NGX_DLSS_PRESET_K },
		};
		const std::vector<ClipRow>& rows = nr ? nrRows : srRows;
		const std::vector<std::vector<HALF>>& srFrames = nr ? nrFrames : frames;
		Tex2D_t beforeNR;   // the frame before DLSS 5 NR, for the rows whose vectors are computed there
		if (!rc && nr && FAILED(beforeNR.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShader))) {
			rc = 1;
		}
		// The rows to run, by tag; and those whose crops are worth saving.
		const std::wstring wanted = which && *which ? std::wstring(L",") + which + L"," : std::wstring();
		static const wchar_t* const cropped[] = { L"now", L"med", L"film", L"zero", L"K", L"Kmed" };

		for (const ClipRow& row : rows) {
			if (rc) {
				break;
			}
			if (!wanted.empty() && wanted.find(std::wstring(L",") + row.tag + L",") == std::wstring::npos) {
				continue;
			}
			bool ok = sr.CreateFeature(W, H, 2 * W, 2 * H, row.preset);
			sr.RequestReset();
			Tex2D_t uniformMotion;
			CComPtr<ID3D11RenderTargetView> uniformTarget;
			CDlssStabilizer flow;
			if (ok && (row.motion == 0 || row.motion == 2)) {
				ok = SUCCEEDED(uniformMotion.Create(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTarget))
					&& SUCCEEDED(dev->CreateRenderTargetView(uniformMotion.pTexture, nullptr, &uniformTarget));
			}
			if (ok && row.motion == 1) {
				ok = SUCCEEDED(flow.Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
					stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), true, row.flow)) && flow.HasFlow();
			}
			if (!ok) {
				Out("  %-26s could not start: %S %S\n", row.name, sr.GetStatusLine().c_str(), flow.GetStatusLine().c_str());
				g_failures++;
				continue;
			}

			std::wstring cropDir;
			if (saveCrops && std::any_of(std::begin(cropped), std::end(cropped), [&](const wchar_t* t) { return !wcscmp(t, row.tag); })) {
				cropDir = std::wstring(folder) + L"_crops";
				CreateDirectoryW(cropDir.c_str(), nullptr);
				cropDir += std::wstring(L"\\") + row.tag;
				CreateDirectoryW(cropDir.c_str(), nullptr);
			}

			ClipSteadiness steady;
			double mvSum = 0, gmSum = 0;
			long long mvN = 0;
			int gmN = 0, dead = 0;
			std::vector<float> out;
			std::vector<BYTE> raw;
			for (int t = 0; ok && t < N; t++) {
				ctx->UpdateSubresource(sr.GetInput()->pTexture, 0, nullptr, srFrames[t].data(), 4 * sizeof(HALF) * W, 0);
				if (row.flowOn == 1) {
					ctx->UpdateSubresource(beforeNR.pTexture, 0, nullptr, frames[t].data(), 4 * sizeof(HALF) * W, 0);
				}
				// Vectors are current -> previous: the content at x came from x - d.
				const float mx = -filmDx[t], my = -filmDy[t];
				ID3D11Texture2D* pMotion = nullptr;
				if (row.motion == 0 || row.motion == 2) {
					const FLOAT value[4] = { row.motion == 2 ? mx : 0.0f, row.motion == 2 ? my : 0.0f, 0, 0 };
					ctx->ClearRenderTargetView(uniformTarget, value);
					pMotion = uniformMotion.pTexture;
				} else {
					flow.PrepareMotion(ctx, row.flowOn == 1 ? beforeNR.pShaderResource : sr.GetInput()->pShaderResource);
					pMotion = flow.GetMotionVectors();
				}
				ok = sr.Evaluate(pMotion, 1000.0f / 24.0f);
				if (!ok || t < kWarm) {
					continue;
				}
				ok = ReadTexture(dev, ctx, sr.GetOutput()->pTexture, out);
				if (!ok) {
					break;
				}
				steady.Add(out.data(), 2 * W, 2 * H, 2, filmDx[t], filmDy[t], still, sbw);
				if (!cropDir.empty()) {
					SavePng(factory, std::format(L"{}\\f{:04}.png", cropDir, t).c_str(), out, 2 * W, crop, 1.0f);
				}

				// What DLSS was told about the still blocks, against the film's own motion.
				if (row.motion == 1 && pMotion) {
					int mw = 0, mh = 0;
					if (ReadRaw(dev, ctx, pMotion, 4, raw, mw, mh)) {
						const HALF* v = (const HALF*)raw.data();
						for (int yy = 0; yy < mh; yy++) {
							for (int xx = 0; xx < mw; xx++) {
								if (still[(size_t)(yy / 8) * sbw + xx / 8]) {
									const size_t i = 2 * ((size_t)yy * mw + xx);
									const float ex = DirectX::PackedVector::XMConvertHalfToFloat(v[i]) - mx;
									const float ey = DirectX::PackedVector::XMConvertHalfToFloat(v[i + 1]) - my;
									mvSum += std::sqrt(ex * ex + ey * ey);
									mvN++;
								}
							}
						}
					}
					int gw = 0, gh = 0;
					if (flow.GetGlobalMotion() && ReadRaw(dev, ctx, flow.GetGlobalMotion(), 16, raw, gw, gh)) {
						const float* g = (const float*)raw.data();
						const float gx = g[0] * flow.GetFlowFactor(), gy = g[1] * flow.GetFlowFactor();
						gmSum += std::hypot(gx - mx, gy - my);
						gmN++;
						dead += std::hypot(gx, gy) < row.flow.snapDeadZone;
					}
				}
			}
			if (!ok) {
				Out("  %-26s failed: %S\n", row.name, sr.GetStatusLine().c_str());
				g_failures++;
				continue;
			}
			const double ofMs = (row.motion == 1)
				? GpuMs(dev, ctx, 8, [&] { flow.PrepareMotion(ctx, sr.GetInput()->pShaderResource); }) : 0.0;
			const std::string mv = mvN ? std::format("{:7.3f}", mvSum / mvN) : std::string("      -");
			const std::string gm = gmN ? std::format("{:7.3f}", gmSum / gmN) : std::string("      -");
			const std::string deadShare = gmN ? std::format("{:5.0f}%", 100.0 * dead / gmN) : std::string("     -");
			const std::string of = (row.motion == 1) ? std::format("{:7.2f}", ofMs) : std::string("      -");
			const double sw = sourceSteady.Wob(), sf = sourceSteady.Flick();
			Out("  %-26s %7.3f %6.2f %7.3f %6.2f %7.2f %s %s %s %s  %S\n", row.name, steady.Wob(), sw > 0 ? steady.Wob() / sw : 0.0,
			    steady.Flick(), sf > 0 ? steady.Flick() / sf : 0.0, steady.Detail(), mv.c_str(), gm.c_str(), deadShare.c_str(),
			    of.c_str(), sr.GetStatsLine().c_str());
		}
		Out("  crop, output pixels: %d,%d to %d,%d\n", crop.x0, crop.y0, crop.x1, crop.y1);

		// What the refinement costs. OF ms above moves by most of a millisecond between
		// two rows that compute the same thing, with the GPU's clocks after each row's
		// DLSS; here the filter's motion and the robust refinement's are timed in turns,
		// on the clip's frames and nothing else, so that the clocks weigh on both alike.
		if (!rc && (wanted.empty() || wanted.find(L",cost,") != std::wstring::npos)) {
			CDlssStabilizer::FlowSettings settings[2] = { ClipGlobal(0.4f, 0, 0.0f), CDlssStabilizer::ForDlssSR() };
			CDlssStabilizer motion[2];
			// A texture of its own: DLSS's input exists only once a row has run.
			Tex2D_t input;
			bool ok = SUCCEEDED(input.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShader));
			for (int k = 0; k < 2 && ok; k++) {
				ok = SUCCEEDED(motion[k].Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
					stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), true, settings[k])) && motion[k].HasFlow();
			}
			if (ok) {
				// A new frame each time, as in play; the upload alone is timed too and taken out.
				std::vector<double> ms[3];
				for (int round = 0; round < 8; round++) {
					for (int k = 0; k < 3; k++) {
						int t = 0;
						ms[k].push_back(GpuMs(dev, ctx, 24, [&] {
							ctx->UpdateSubresource(input.pTexture, 0, nullptr, frames[t++ % N].data(), 4 * sizeof(HALF) * W, 0);
							if (k < 2) {
								motion[k].PrepareMotion(ctx, input.pShaderResource);
							}
						}));
					}
				}
				for (auto& v : ms) {
					std::sort(v.begin(), v.end());
				}
				const double upload = ms[2][ms[2].size() / 2];
				Out("  motion, GPU ms a frame, median of 8 batches of 24 in turns, the upload taken out: median (until 2.0)"
				    " %.3f, filter now %.3f\n", ms[0][ms[0].size() / 2] - upload, ms[1][ms[1].size() / 2] - upload);
			} else {
				Out("  motion, GPU ms: could not start\n");
				g_failures++;
			}
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
