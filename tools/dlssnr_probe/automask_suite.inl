// --tautomask <folder>: what DLSSNR.UseAutoMask changes, on a film's own frames.
//
// Included by harness.cpp after srclip_suite.inl, whose readers it uses. "Auto mask" is
// a parameter of NVIDIA's DLSS 5 NR snippet that nothing documents; the page used to say
// it lets the network decide where to apply itself. Here the same frames go through the
// network twice, with it and without, as the filter runs it: its default tuning, its own
// history, and the cleaned vectors CDlssStabilizer::ForDlssNR gives it. Over the last
// frames, per pixel, on luma:
//   effect on / off   mean |output - input| with the auto mask, and without it
//   apart             mean |output with - output without|
//   more without      mean (|output without - input| - |output with - input|): positive
//                     where the network does more once the auto mask is off, that is,
//                     where the mask holds it back
// and the same by kind of pixel -- dark or bright, flat or textured, saturated, skin, still
// or moving -- so that where it acts can be told. Beside the folder, <folder>_automask\:
// the last frame, how far apart the two outputs are (x16), and where the network does
// more without the mask (red) or with it (blue), x16, all at half size. --skin N sets the
// skin structure strength (x100) in place of the filter's 0.90.

namespace temporal {

static int RunAutoMask(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, const wchar_t* folder, float skinStrength)
{
	Head("DLSS 5 NR's auto mask on a film's frames");

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
		// Enough for the network's history to settle, then a second of film measured.
		const int kFrames = 48, kFrom = 24;
		if (names.size() > (size_t)kFrames) {
			names.resize(kFrames);
		}
		int W = 0, H = 0;
		std::vector<std::vector<HALF>> frames;
		std::string error;
		for (size_t i = 0; factory && i < names.size(); i++) {
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
		Check(!rc && N == kFrames, "frames decoded");
		if (N != kFrames) {
			rc = 1;
		}

		CStabilizerPasses stabPasses;
		if (!rc && !stabPasses.Init(dev, error)) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}

		// The input's luma and colour, frame by frame.
		std::vector<std::vector<float>> lumaIn(N);
		std::vector<float> rgba;
		std::vector<float> lastRgba;
		for (int t = 0; !rc && t < N; t++) {
			rgba.resize(frames[t].size());
			DirectX::PackedVector::XMConvertHalfToFloatStream(rgba.data(), sizeof(float), frames[t].data(), sizeof(HALF), rgba.size());
			ClipLuma(rgba.data(), W, H, lumaIn[t]);
			if (t == N - 1) {
				lastRgba = rgba;
			}
		}

		// Two runs. The first keeps its outputs' luma for the measured frames; the second
		// is compared with them as it goes.
		const size_t P = (size_t)W * H;
		std::vector<std::vector<float>> lumaWith(N);
		std::vector<double> effWith(P, 0.0), effWithout(P, 0.0), apart(P, 0.0), moreWithout(P, 0.0);
		std::vector<float> outLastWith, outLastWithout;
		double flick[2] = {}, flickN = 0;
		for (int mode = 0; !rc && mode < 2; mode++) {
			const bool autoMask = (mode == 0);
			Tex2D_t nrIn, nrOut, vectors;
			// The filter's defaults (DlssParamsFromSettings of a fresh Settings_t).
			CDlssNR::Params params;
			params.iStyle = 0;   // Default
			params.iPreset = 0;
			params.fIntensity = 1.50f;
			params.fLocalTone = 0.30f;
			params.fLocalStructure = 0.50f;
			params.fSkinStructure = skinStrength;
			params.bUseAutoMask = autoMask;
			params.bNoHistory = false;
			CDlssStabilizer stabilizer;
			bool ok = MakeSharedPair(dev, nrIn, nrOut, W, H) && dlss.CreateFeature(nrIn.pTexture, nrOut.pTexture, W, H, params)
				&& SUCCEEDED(stabilizer.Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
					stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), false, CDlssStabilizer::ForDlssNR(0)))
				&& stabilizer.HasFlow()
				&& SUCCEEDED(vectors.Create(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTargetUAVShared));
			if (ok) {
				CDlssNR::Guides guides;
				guides.pMVec = vectors.pTexture;
				ok = dlss.SetGuides(guides);
			}
			std::vector<float> out, luma, lumaPrev;
			for (int t = 0; ok && t < N; t++) {
				ctx->UpdateSubresource(nrIn.pTexture, 0, nullptr, frames[t].data(), 4 * sizeof(HALF) * W, 0);
				stabilizer.PrepareMotion(ctx, nrIn.pShaderResource);
				ID3D11Texture2D* pVectors = stabilizer.GetMotionVectors();
				ok = pVectors != nullptr;
				if (!ok) {
					break;
				}
				ctx->CopyResource(vectors.pTexture, pVectors);
				ok = dlss.Evaluate(params);
				if (!ok || t < kFrom - 1) {
					continue;
				}
				ok = ReadTexture(dev, ctx, nrOut.pTexture, out);
				if (!ok) {
					break;
				}
				ClipLuma(out.data(), W, H, luma);
				if (t >= kFrom) {
					const std::vector<float>& in = lumaIn[t];
					double f = 0;
					for (size_t i = 0; i < P; i++) {
						f += std::abs(luma[i] - lumaPrev[i]);
					}
					flick[mode] += f / P;
					if (mode == 0) {
						flickN++;
						for (size_t i = 0; i < P; i++) {
							effWith[i] += std::abs(luma[i] - in[i]);
						}
						lumaWith[t] = luma;
					} else {
						const std::vector<float>& with = lumaWith[t];
						for (size_t i = 0; i < P; i++) {
							const double e = std::abs(luma[i] - in[i]);
							effWithout[i] += e;
							apart[i] += std::abs(luma[i] - with[i]);
							moreWithout[i] += e - std::abs(with[i] - in[i]);
						}
					}
					if (t == N - 1) {
						(mode == 0 ? outLastWith : outLastWithout) = out;
					}
				}
				lumaPrev = luma;
			}
			dlss.SetGuides(CDlssNR::Guides());
			dlss.ReleaseFeature();
			Check(ok, autoMask ? "the frames through DLSS 5 NR, auto mask on" : "the frames through DLSS 5 NR, auto mask off");
			if (!ok) {
				rc = 1;
			}
		}

		if (!rc) {
			const double n = N - kFrom;
			for (size_t i = 0; i < P; i++) {
				effWith[i] /= n;
				effWithout[i] /= n;
				apart[i] /= n;
				moreWithout[i] /= n;
			}

			// What each pixel is, from the input over the measured frames.
			std::vector<float> meanY(P, 0.0f), motion(P, 0.0f), texture(P, 0.0f), chroma(P, 0.0f);
			std::vector<uint8_t> skin(P, 0);
			for (int t = kFrom; t < N; t++) {
				for (size_t i = 0; i < P; i++) {
					meanY[i] += lumaIn[t][i] / (float)n;
					motion[i] += std::abs(lumaIn[t][i] - lumaIn[t - 1][i]) / (float)n;
				}
			}
			for (int y = 0; y < H; y++) {
				for (int x = 0; x < W; x++) {
					double s = 0, s2 = 0;
					int c = 0;
					for (int j = std::max(0, y - 2); j <= std::min(H - 1, y + 2); j++) {
						for (int i = std::max(0, x - 2); i <= std::min(W - 1, x + 2); i++) {
							const double v = lumaIn[N - 1][(size_t)j * W + i];
							s += v;
							s2 += v * v;
							c++;
						}
					}
					texture[(size_t)y * W + x] = (float)std::sqrt(std::max(0.0, s2 / c - (s / c) * (s / c)));
					// Colour of the last frame: BT.709 Cb and Cr, and a skin tone by the
					// usual YCbCr box (Cb 77..127, Cr 133..173 on 8 bits).
					const float* p = &lastRgba[4 * ((size_t)y * W + x)];
					const float Y = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
					const float cb = (p[2] - Y) / 1.8556f, cr = (p[0] - Y) / 1.5748f;
					chroma[(size_t)y * W + x] = std::sqrt(cb * cb + cr * cr);
					const float cb8 = 128 + 224 * cb, cr8 = 128 + 224 * cr;
					skin[(size_t)y * W + x] = (cb8 >= 77 && cb8 <= 127 && cr8 >= 133 && cr8 <= 173 && Y > 0.15f && Y < 0.9f) ? 1 : 0;
				}
			}
			std::vector<float> sortedTex(texture);
			std::sort(sortedTex.begin(), sortedTex.end());
			const float tex1 = sortedTex[P / 3], tex2 = sortedTex[2 * P / 3];

			const std::wstring name = std::wstring(folder).substr(std::wstring(folder).find_last_of(L"\\/") + 1);
			g_report = _wfopen(std::format(L"temporal_results_automask_{}.txt", name).c_str(), L"w");
			Out("\nDLSS 5 NR with and without its auto mask on %S: %d frames of %dx%d, the last %d measured.\n",
			    folder, N, W, H, N - kFrom);
			Out("The filter's defaults (intensity 1.50, local tone 0.30, structure 0.50), skin %.2f, its history, its vectors.\n", skinStrength);
			Out("Luma x1000. effect: mean |out - in|. apart: mean |with - without|. more without: mean of |without - in| - |with - in|.\n\n");
			Out("  %-30s %7s %8s %8s %8s %8s %8s\n", "pixels", "share", "with", "without", "apart", "apart/ef", "more w/o");
			struct Kind { const char* name; std::function<bool(size_t)> in; };
			const Kind kinds[] = {
				{ "all",                          [&](size_t) { return true; } },
				{ "dark (luma < 0.10)",           [&](size_t i) { return meanY[i] < 0.10f; } },
				{ "mid (0.10 to 0.40)",           [&](size_t i) { return meanY[i] >= 0.10f && meanY[i] < 0.40f; } },
				{ "light (0.40 to 0.70)",         [&](size_t i) { return meanY[i] >= 0.40f && meanY[i] < 0.70f; } },
				{ "bright (0.70 and above)",      [&](size_t i) { return meanY[i] >= 0.70f; } },
				{ "flat (lowest third texture)",  [&](size_t i) { return texture[i] < tex1; } },
				{ "middle third texture",         [&](size_t i) { return texture[i] >= tex1 && texture[i] < tex2; } },
				{ "textured (top third)",         [&](size_t i) { return texture[i] >= tex2; } },
				{ "neutral colour (< 0.03)",      [&](size_t i) { return chroma[i] < 0.03f; } },
				{ "saturated (>= 0.08)",          [&](size_t i) { return chroma[i] >= 0.08f; } },
				{ "skin tone",                    [&](size_t i) { return skin[i] != 0; } },
				{ "still (< 0.005 a frame)",      [&](size_t i) { return motion[i] < 0.005f; } },
				{ "moving (>= 0.02 a frame)",     [&](size_t i) { return motion[i] >= 0.02f; } },
			};
			for (const Kind& k : kinds) {
				double c = 0, a = 0, b = 0, d = 0, m = 0;
				for (size_t i = 0; i < P; i++) {
					if (k.in(i)) {
						c++;
						a += effWith[i];
						b += effWithout[i];
						d += apart[i];
						m += moreWithout[i];
					}
				}
				if (c < 1) {
					Out("  %-30s %6.1f%%\n", k.name, 0.0);
					continue;
				}
				Out("  %-30s %6.1f%% %8.2f %8.2f %8.2f %7.0f%% %+8.2f\n", k.name, 100.0 * c / P, 1000 * a / c, 1000 * b / c,
				    1000 * d / c, a > 0 ? 100.0 * d / a : 0.0, 1000 * m / c);
			}
			Out("\n  frame to frame change of the output, mean |Y(t) - Y(t-1)| x1000: with %.2f, without %.2f\n",
			    1000 * flick[0] / flickN, 1000 * flick[1] / flickN);

			// Where most of the difference is: the share of it held by the pixels that
			// carry the most, and by each sixteenth of the picture.
			std::vector<double> sortedApart(apart);
			std::sort(sortedApart.begin(), sortedApart.end(), std::greater<double>());
			double total = 0, top1 = 0, top10 = 0;
			for (size_t i = 0; i < P; i++) {
				total += sortedApart[i];
				if (i < P / 100) top1 += sortedApart[i];
				if (i < P / 10) top10 += sortedApart[i];
			}
			Out("  the 1 %% of pixels most apart hold %.0f %% of the difference, the 10 %% most apart %.0f %%\n",
			    100 * top1 / total, 100 * top10 / total);
			Out("  by quarter of the picture each way, apart x1000 (top row first):\n");
			for (int qy = 0; qy < 4; qy++) {
				Out("   ");
				for (int qx = 0; qx < 4; qx++) {
					double s = 0;
					int c = 0;
					for (int y = qy * H / 4; y < (qy + 1) * H / 4; y++) {
						for (int x = qx * W / 4; x < (qx + 1) * W / 4; x++) {
							s += apart[(size_t)y * W + x];
							c++;
						}
					}
					Out(" %6.2f", 1000 * s / c);
				}
				Out("\n");
			}

			// The pictures, at half size.
			const std::wstring dir = std::wstring(folder)
				+ (skinStrength == 0.90f ? std::wstring(L"_automask") : std::format(L"_automask_skin{:.0f}", skinStrength * 100));
			CreateDirectoryW(dir.c_str(), nullptr);
			const int hw = W / 2, hh = H / 2;
			auto Half = [&](auto value) {
				std::vector<float> img(4 * (size_t)hw * hh);
				for (int y = 0; y < hh; y++) {
					for (int x = 0; x < hw; x++) {
						float px[3] = {};
						for (int j = 0; j < 2; j++) {
							for (int i = 0; i < 2; i++) {
								const auto v = value((size_t)(2 * y + j) * W + 2 * x + i);
								px[0] += v[0] / 4;
								px[1] += v[1] / 4;
								px[2] += v[2] / 4;
							}
						}
						float* o = &img[4 * ((size_t)y * hw + x)];
						o[0] = px[0];
						o[1] = px[1];
						o[2] = px[2];
						o[3] = 1;
					}
				}
				return img;
			};
			const Rect all = { 0, 0, hw, hh };
			const float gain = 16.0f;
			SavePng(factory, (dir + L"\\input.png").c_str(),
				Half([&](size_t i) { return std::array<float, 3>{ lastRgba[4 * i], lastRgba[4 * i + 1], lastRgba[4 * i + 2] }; }), hw, all, 1.0f);
			SavePng(factory, (dir + L"\\with.png").c_str(),
				Half([&](size_t i) { return std::array<float, 3>{ outLastWith[4 * i], outLastWith[4 * i + 1], outLastWith[4 * i + 2] }; }), hw, all, 1.0f);
			SavePng(factory, (dir + L"\\without.png").c_str(),
				Half([&](size_t i) { return std::array<float, 3>{ outLastWithout[4 * i], outLastWithout[4 * i + 1], outLastWithout[4 * i + 2] }; }), hw, all, 1.0f);
			SavePng(factory, (dir + L"\\apart_x16.png").c_str(),
				Half([&](size_t i) { const float v = (float)apart[i] * gain; return std::array<float, 3>{ v, v, v }; }), hw, all, 1.0f);
			SavePng(factory, (dir + L"\\more_without_x16.png").c_str(),
				Half([&](size_t i) {
					const float v = (float)moreWithout[i] * gain;
					return std::array<float, 3>{ std::max(v, 0.0f), 0.0f, std::max(-v, 0.0f) };
				}), hw, all, 1.0f);
			SavePng(factory, (dir + L"\\effect_with_x16.png").c_str(),
				Half([&](size_t i) { const float v = (float)effWith[i] * gain; return std::array<float, 3>{ v, v, v }; }), hw, all, 1.0f);
			Out("  pictures in %S\n", dir.c_str());
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
