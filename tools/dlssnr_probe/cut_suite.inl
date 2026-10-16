// --tsrcut <folder>: DLSS Super Resolution across the cuts of a film, a folder of PNG
// frames in order (fNNNN.png), enlarged twice.
//
// Included by harness.cpp after srclip_suite.inl, whose readers it uses. Seen in playback
// (2026-10-16): where a bright shot follows a darker one, the bright parts of the new shot
// come out darker for two or three pictures, then right. DLSS SR resamples its history
// along the vectors it is given, and nothing tells it that the picture before belongs to
// another shot: it blends the dark shot into the bright one until its history has caught
// up. A seek resets it (CDlssSR::RequestReset); a cut did not.
//
// The cuts are found in the frames themselves: the mean change of the picture's luma at
// an eighth of its size, against the clip's typical change. For each, the pixels that
// brightened across it, and those that darkened. On each picture from the cut on, the
// enlarged picture is brought back to the source's size (a 2 x 2 box) and compared with
// the source there: what it lacks on the brightened pixels is the dark trail, what it has
// in excess on the darkened ones the bright one, each against the same measure a second
// into the shot, where the history is the shot's own. Luma x1000.
//
// Rows: the filter as 2.1 had it, with no cut detection; the filter as it is, whose
// vectors leave the picture where cs_dlss_scene_cut.hlsl calls a cut; then, on 2.1's
// vectors and at the cuts the bench found, DLSS reset on the cut picture (the answer a
// player cannot give without waiting for the GPU) and every vector pointing out of the
// picture there (what the detector does, put where it should be); and DLSS with no
// history at all, reset on every picture. With --tsrcrops the most changed 256 x 256
// source window is saved around the first cut, for every row.
//
// The filter's own row also reads, on every picture, what the detector said: the
// pictures it called a cut against those the bench found anywhere in the clip, a miss
// or a false cut being a failure, and the most its measures reached elsewhere. A clip
// with no cut runs that row alone, for its false cuts. The bench finds a flash as two
// cuts a picture apart, its start and its end, and so does the detector.
//
// --tsrcutnr: DLSS 5 NR across the same cuts instead, with the vectors the filter gives
// it (CDlssStabilizer::ForDlssNR) and the stabilizer after it, as the renderer runs them:
// the trail on what is shown and on the network's own output, at the source's size. Rows:
// the vectors as 2.1 gave them, the same with the cut detection, DLSS 5 NR reset on the
// cut, its vectors out of the picture there, and no vectors at all.
//
// --tcutscan <film> [--cutfrom <s>] [--cutfor <s>]: what the detector says of every
// picture of a stretch of a film, minutes of it, with no DLSS and no player. ffmpeg
// decodes the stretch into a pipe at 16 bits a channel, as the clips' PNGs are, and the
// filter's own CDlssStabilizer (ForDlssSR) measures each picture, read back at once. Beside
// its four measures, ffmpeg's scene score of the same picture (the select filter's
// "scene", a second decode of the same stretch) and the bench's own change of luma, for
// the detector to be compared with on cuts it has never seen. A line a picture in
// cutscan_<film>.csv; the cuts called, and ffmpeg's that were not, in the report.

namespace temporal {

namespace {

struct CutRow {
	const char* name;
	const wchar_t* tag;
	int  cutWay;          // 0 nothing, 1 Reset on the cut, 2 vectors out of the picture on the cut, 3 Reset every picture
	bool detect;          // FlowSettings::cutDetect
};

} // namespace

static int RunSRCut(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* folder, const wchar_t* dllPath, bool saveCrops,
                    CDlssNR* nr)
{
	Head(nr ? "DLSS 5 NR across a film's cuts" : "DLSS Super Resolution across a film's cuts");

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
		Check(!rc && N > 40, "frames decoded");
		if (rc || N <= 40) {
			rc = 1;
		}

		// Each frame's luma at the source's size, and the cuts.
		std::vector<std::vector<float>> luma(N);
		std::vector<int> cuts, anyCuts;   // measured, and anywhere in the clip
		const int kSettle = 24, kAfter = 12, kSteadyFrom = 8;
		if (!rc) {
			std::vector<float> rgba;
			for (int t = 0; t < N; t++) {
				rgba.resize(frames[t].size());
				DirectX::PackedVector::XMConvertHalfToFloatStream(rgba.data(), sizeof(float), frames[t].data(), sizeof(HALF), rgba.size());
				ClipLuma(rgba.data(), W, H, luma[t]);
			}
			std::vector<double> change(N, 0.0);
			for (int t = 1; t < N; t++) {
				double s = 0;
				long long n = 0;
				for (int y = 0; y < H; y += 8) {
					for (int x = 0; x < W; x += 8) {
						s += std::abs(luma[t][(size_t)y * W + x] - luma[t - 1][(size_t)y * W + x]);
						n++;
					}
				}
				change[t] = s / n;
			}
			std::vector<double> sorted(change.begin() + 1, change.end());
			std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
			const double typical = sorted[sorted.size() / 2];
			for (int t = 1; t < N; t++) {
				if (change[t] > std::max(6.0 * typical, 0.04)) {
					anyCuts.push_back(t);
					if (t >= kSettle && t + kAfter < N) {
						cuts.push_back(t);
					}
				}
			}
			const std::wstring name = std::wstring(folder).substr(std::wstring(folder).find_last_of(L"\\/") + 1);
			g_report = _wfopen(std::format(L"temporal_results_{}cut_{}.txt", nr ? L"nr" : L"sr", name).c_str(), L"w");
			if (nr) {
				Out("\nDLSS 5 NR on %S: %d frames of %dx%d.\n", folder, N, W, H);
			} else {
				Out("\nDLSS Super Resolution on %S: %d frames of %dx%d, enlarged twice.\n", folder, N, W, H);
			}
			Out("Typical change from one frame to the next: %.4f of luma; cuts, where it passes six times that:", typical);
			for (int c : anyCuts) {
				Out(" %d (%.3f)%s", c, change[c], std::find(cuts.begin(), cuts.end(), c) == cuts.end() ? " not measured" : "");
			}
			if (anyCuts.empty()) {
				Out(" none");
			}
			Out(nr ? "\nOn each picture from the cut: the trail, what the picture DLSS 5 NR's chain gives\n"
			       : "\nOn each picture from the cut: the trail, what the enlarged picture brought back to the source's size\n");
			Out("lacks on the pixels that brightened across the cut (dark) or has in excess on those that darkened\n");
			Out("(bright), against the same %d to %d pictures into the shot; luma x1000.\n", kSteadyFrom, kAfter - 1);
		}

		CDlssSR sr;
		CStabilizerPasses stabPasses;
		if (!rc && (!stabPasses.Init(dev, error) || (!nr && !sr.Init(dev, dllPath)))) {
			printf("  %s\n%S", error.c_str(), sr.GetInfoBlock().c_str());
			rc = 1;
		}

		// The cuts' masks, and the window to crop around the first.
		struct CutMasks { std::vector<uint8_t> bright, dark; size_t nBright = 0, nDark = 0; };
		std::vector<CutMasks> masks(cuts.size());
		Rect crop = { 0, 0, 0, 0 };
		for (size_t ci = 0; !rc && ci < cuts.size(); ci++) {
			const int c = cuts[ci];
			CutMasks& m = masks[ci];
			m.bright.assign((size_t)W * H, 0);
			m.dark.assign((size_t)W * H, 0);
			for (size_t i = 0; i < (size_t)W * H; i++) {
				const float d = luma[c][i] - luma[c - 1][i];
				if (d > 0.08f) {
					m.bright[i] = 1;
					m.nBright++;
				} else if (d < -0.08f) {
					m.dark[i] = 1;
					m.nDark++;
				}
			}
			if (ci == 0) {
				const int win = 256;
				double best = -1;
				for (int y0 = 0; y0 + win <= H; y0 += 32) {
					for (int x0 = 0; x0 + win <= W; x0 += 32) {
						double s = 0;
						for (int y = y0; y < y0 + win; y += 4) {
							for (int x = x0; x < x0 + win; x += 4) {
								s += m.bright[(size_t)y * W + x] + m.dark[(size_t)y * W + x];
							}
						}
						if (s > best) {
							best = s;
							crop = { 2 * x0, 2 * y0, 2 * (x0 + win), 2 * (y0 + win) };
						}
					}
				}
			}
		}

		// The trails of one row, a line for each cut and each way, less the level the shot
		// settles at.
		auto printTrails = [&](const char* name, const std::vector<std::vector<double>>& darkTrail,
		                       const std::vector<std::vector<double>>& brightTrail) {
			for (size_t ci = 0; ci < cuts.size(); ci++) {
				for (int way = 0; way < 2; way++) {
					const auto& trail = way == 0 ? darkTrail[ci] : brightTrail[ci];
					const size_t n = way == 0 ? masks[ci].nBright : masks[ci].nDark;
					double steady = 0;
					for (int k = kSteadyFrom; k < kAfter; k++) {
						steady += trail[k] / (kAfter - kSteadyFrom);
					}
					double worst = 0;
					Out("  %-28s %5d %5.1f%%", ci == 0 && way == 0 ? name : "", cuts[ci], 100.0 * n / ((double)W * H));
					for (int k = 0; k < 6; k++) {
						Out(" %5.1f", trail[k] - steady);
						worst = std::max(worst, trail[k] - steady);
					}
					Out(" %7.1f  %s\n", worst, way == 0 ? "dark trail on what brightened" : "bright trail on what darkened");
				}
			}
		};
		// The cuts a picture belongs to, and how far into each: -1 the picture before. A
		// flash is two cuts a picture apart, its start and its end, and its pictures count
		// for both.
		auto cutsAt = [&](int t) {
			std::vector<std::pair<int, int>> at;
			for (size_t i = 0; i < cuts.size(); i++) {
				if (t >= cuts[i] - 1 && t < cuts[i] + kAfter) {
					at.emplace_back((int)i, t - cuts[i]);
				}
			}
			return at;
		};

		struct NRCutRow { const char* name; int cutWay; bool detect; bool vectors; };
		static const NRCutRow nrRows[] = {
			{ "NR, vectors (2.1)",          0, false, true  },
			{ "NR, vectors, cut detection", 0, true,  true  },
			{ "NR, reset on the cut",       1, false, true  },
			{ "NR, vectors out on the cut", 2, false, true  },
			{ "NR, no vectors",             0, false, false },
		};
		if (!rc && nr && !cuts.empty()) {
			Out("\n  %-28s %5s %6s", "", "cut", "pixels");
			for (int k = 0; k < 6; k++) {
				Out("   +%d", k);
			}
			Out("   worst\n");
		}
		for (const NRCutRow& row : nrRows) {
			if (rc || !nr || cuts.empty()) {
				break;
			}
			Tex2D_t nrIn, nrOut, vectors, away;
			CComPtr<ID3D11RenderTargetView> awayTarget;
			CDlssNR::Params params;
			params.bNoHistory = false;   // as DlssParamsFromSettings
			CDlssStabilizer stabilizer;
			CDlssStabilizer::FlowSettings settings = CDlssStabilizer::ForDlssNR(0);
			settings.cutDetect = row.detect;
			bool ok = MakeSharedPair(dev, nrIn, nrOut, W, H) && nr->CreateFeature(nrIn.pTexture, nrOut.pTexture, W, H, params)
				&& SUCCEEDED(stabilizer.Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
					stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), false, settings))
				&& stabilizer.HasFlow();
			if (ok && row.vectors) {
				// The network takes its guides through a shared texture: the vectors are copied in.
				ok = SUCCEEDED(vectors.Create(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTargetUAVShared))
					&& SUCCEEDED(away.Create(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTarget))
					&& SUCCEEDED(dev->CreateRenderTargetView(away.pTexture, nullptr, &awayTarget));
				if (ok) {
					CDlssNR::Guides guides;
					guides.pMVec = vectors.pTexture;
					ok = nr->SetGuides(guides);
				}
			}
			nr->RequestReset();
			// [0] what is shown, after the stabilizer; [1] the network's own output.
			std::vector<std::vector<double>> darkTrail[2], brightTrail[2];
			for (int which = 0; which < 2; which++) {
				darkTrail[which].assign(cuts.size(), std::vector<double>(kAfter, 0.0));
				brightTrail[which].assign(cuts.size(), std::vector<double>(kAfter, 0.0));
			}
			std::vector<float> outs[2];
			for (int t = 0; ok && t < N; t++) {
				ctx->UpdateSubresource(nrIn.pTexture, 0, nullptr, frames[t].data(), 4 * sizeof(HALF) * W, 0);
				stabilizer.PrepareMotion(ctx, nrIn.pShaderResource);
				const bool onCut = std::find(cuts.begin(), cuts.end(), t) != cuts.end();
				if (row.vectors) {
					ID3D11Texture2D* pSource = stabilizer.GetMotionVectors();
					if (row.cutWay == 2 && onCut) {
						const FLOAT out[4] = { 2.0f * W, 2.0f * H, 0, 0 };
						ctx->ClearRenderTargetView(awayTarget, out);
						pSource = away.pTexture;
					}
					ok = pSource != nullptr;
					if (!ok) {
						break;
					}
					ctx->CopyResource(vectors.pTexture, pSource);
				}
				if (row.cutWay == 1 && onCut) {
					nr->RequestReset();
				}
				ok = nr->Evaluate(params);
				if (!ok) {
					break;
				}
				stabilizer.Stabilize(ctx, nrIn.pShaderResource, nrOut.pShaderResource, 1.0f, true);
				const auto at = cutsAt(t);
				if (std::none_of(at.begin(), at.end(), [](const auto& a) { return a.second >= 0; })) {
					continue;
				}
				ok = ReadTexture(dev, ctx, stabilizer.GetResult()->pTexture, outs[0]) && ReadTexture(dev, ctx, nrOut.pTexture, outs[1]);
				if (!ok) {
					break;
				}
				for (const auto& [ci, k] : at) {
					if (k < 0) {
						continue;
					}
					const CutMasks& m = masks[ci];
					for (int which = 0; which < 2; which++) {
						double sb = 0, sd = 0;
						for (size_t i = 0; i < (size_t)W * H; i++) {
							if (m.bright[i]) {
								sb += luma[t][i] - Luma(&outs[which][4 * i]);
							} else if (m.dark[i]) {
								sd += Luma(&outs[which][4 * i]) - luma[t][i];
							}
						}
						darkTrail[which][ci][k] = m.nBright ? 1000.0 * sb / m.nBright : 0.0;
						brightTrail[which][ci][k] = m.nDark ? 1000.0 * sd / m.nDark : 0.0;
					}
				}
			}
			nr->SetGuides(CDlssNR::Guides());
			nr->ReleaseFeature();
			if (!ok) {
				Out("  %-28s failed: %S\n", row.name, nr->GetStatusLine().c_str());
				g_failures++;
				continue;
			}
			printTrails(row.name, darkTrail[0], brightTrail[0]);
			printTrails("  the network alone", darkTrail[1], brightTrail[1]);
		}

		static const CutRow rows[] = {
			{ "no cut detection (2.1)",     L"2.1",    0, false },
			{ "filter now",                 L"now",    0, true  },
			{ "reset on the cut",           L"reset",  1, false },
			{ "vectors out on the cut",     L"out",    2, false },
			{ "no history (reset always)",  L"always", 3, false },
		};
		if (!rc && !nr && !cuts.empty()) {
			Out("\n  %-28s %5s %6s", "", "cut", "pixels");
			for (int k = 0; k < 6; k++) {
				Out("   +%d", k);
			}
			Out("   worst\n");
		}
		for (const CutRow& row : rows) {
			if (rc || nr) {
				break;
			}
			if (cuts.empty() && !row.detect) {
				continue;
			}
			bool ok = sr.CreateFeature(W, H, 2 * W, 2 * H, NGX_DLSS_PRESET_Default);
			sr.RequestReset();
			CDlssStabilizer flow;
			CDlssStabilizer::FlowSettings settings = CDlssStabilizer::ForDlssSR();
			settings.cutDetect = row.detect;
			ok = ok && SUCCEEDED(flow.Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
				stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), true, settings)) && flow.HasFlow()
				&& (!row.detect || flow.GetCutState());
			// What the detector said of each picture, read back on every one.
			CComPtr<ID3D11Buffer> cutStaging;
			if (ok && row.detect) {
				D3D11_BUFFER_DESC desc = {};
				desc.ByteWidth      = 16 * sizeof(float);
				desc.Usage          = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ok = SUCCEEDED(dev->CreateBuffer(&desc, nullptr, &cutStaging));
			}
			std::vector<int> called;
			std::vector<std::array<float, 16>> said(N, std::array<float, 16>{});
			Tex2D_t outMotion;
			CComPtr<ID3D11RenderTargetView> outTarget;
			ok = ok && SUCCEEDED(outMotion.Create(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTarget))
				&& SUCCEEDED(dev->CreateRenderTargetView(outMotion.pTexture, nullptr, &outTarget));
			if (!ok) {
				Out("  %-28s could not start: %S %S\n", row.name, sr.GetStatusLine().c_str(), flow.GetStatusLine().c_str());
				g_failures++;
				continue;
			}
			std::wstring cropDir;
			if (saveCrops) {
				cropDir = std::wstring(folder) + L"_crops";
				CreateDirectoryW(cropDir.c_str(), nullptr);
				cropDir += std::wstring(L"\\") + row.tag;
				CreateDirectoryW(cropDir.c_str(), nullptr);
			}

			// trail[cut][k]: dark trail and bright trail, k pictures from the cut.
			std::vector<std::vector<double>> darkTrail(cuts.size(), std::vector<double>(kAfter, 0.0));
			std::vector<std::vector<double>> brightTrail(cuts.size(), std::vector<double>(kAfter, 0.0));
			std::vector<float> out, down((size_t)W * H);
			for (int t = 0; ok && t < N; t++) {
				ctx->UpdateSubresource(sr.GetInput()->pTexture, 0, nullptr, frames[t].data(), 4 * sizeof(HALF) * W, 0);
				flow.PrepareMotion(ctx, sr.GetInput()->pShaderResource);
				if (cutStaging) {
					ctx->CopyResource(cutStaging, flow.GetCutState());
					D3D11_MAPPED_SUBRESOURCE mr = {};
					if (SUCCEEDED(ctx->Map(cutStaging, 0, D3D11_MAP_READ, 0, &mr))) {
						memcpy(said[t].data(), mr.pData, sizeof(said[t]));
						ctx->Unmap(cutStaging, 0);
					}
					if (said[t][0] > 0.5f) {
						called.push_back(t);
					}
				}
				const bool onCut = std::find(cuts.begin(), cuts.end(), t) != cuts.end();
				ID3D11Texture2D* pMotion = flow.GetMotionVectors();
				if ((row.cutWay == 1 && onCut) || row.cutWay == 3) {
					sr.RequestReset();
				}
				if (row.cutWay == 2 && onCut) {
					// Current -> previous, source pixels: twice the picture away, so that every
					// pixel's history lies outside it.
					const FLOAT away[4] = { 2.0f * W, 2.0f * H, 0, 0 };
					ctx->ClearRenderTargetView(outTarget, away);
					pMotion = outMotion.pTexture;
				}
				ok = sr.Evaluate(pMotion, 1000.0f / 24.0f);
				if (!ok) {
					break;
				}
				// Only the pictures measured, or cropped, are read back.
				const auto at = cutsAt(t);
				if (at.empty()) {
					continue;
				}
				ok = ReadTexture(dev, ctx, sr.GetOutput()->pTexture, out);
				if (!ok) {
					break;
				}
				if (!cropDir.empty() && at[0].first == 0 && at[0].second <= 5) {
					SavePng(factory, std::format(L"{}\\f{:04}.png", cropDir, t).c_str(), out, 2 * W, crop, 1.0f);
				}
				if (std::none_of(at.begin(), at.end(), [](const auto& a) { return a.second >= 0; })) {
					continue;
				}
				for (int y = 0; y < H; y++) {
					for (int x = 0; x < W; x++) {
						double s = 0;
						for (int j = 0; j < 2; j++) {
							for (int i = 0; i < 2; i++) {
								s += Luma(&out[4 * ((size_t)(2 * y + j) * 2 * W + 2 * x + i)]);
							}
						}
						down[(size_t)y * W + x] = (float)(s / 4);
					}
				}
				for (const auto& [ci, k] : at) {
					if (k < 0) {
						continue;
					}
					const CutMasks& m = masks[ci];
					double sb = 0, sd = 0;
					for (size_t i = 0; i < (size_t)W * H; i++) {
						if (m.bright[i]) {
							sb += luma[t][i] - down[i];
						} else if (m.dark[i]) {
							sd += down[i] - luma[t][i];
						}
					}
					darkTrail[ci][k] = m.nBright ? 1000.0 * sb / m.nBright : 0.0;
					brightTrail[ci][k] = m.nDark ? 1000.0 * sd / m.nDark : 0.0;
				}
			}
			if (!ok) {
				Out("  %-28s failed: %S\n", row.name, sr.GetStatusLine().c_str());
				g_failures++;
				continue;
			}
			printTrails(row.name, darkTrail, brightTrail);
			if (row.detect) {
				// The detector against the bench's cuts. It calls none before it has three
				// pictures' residuals: a cut that early is not counted as missed.
				std::vector<int> missed, falseCuts;
				for (int c : anyCuts) {
					if (c >= 4 && std::find(called.begin(), called.end(), c) == called.end()) {
						missed.push_back(c);
					}
				}
				for (int c : called) {
					if (std::find(anyCuts.begin(), anyCuts.end(), c) == anyCuts.end()) {
						falseCuts.push_back(c);
					}
				}
				Out("  %-28s detector called a cut on:", cuts.empty() ? row.name : "");
				for (int c : called) {
					Out(" %d (%.2f, x%.0f, %.4f, %.2f)", c, said[c][1], said[c][3], said[c][2], said[c][14]);
				}
				if (called.empty()) {
					Out(" none");
				}
				Out("\n");
				// Elsewhere: the most each measure reached, and the picture that came
				// closest to all four at once, by the least of its four against their limits
				// (CDlssStabilizer's kCut...).
				float most[3] = {}, closest[4] = {}, leastCorr = 1.0f;
				int closestAt = -1;
				double closestShare = -1;
				for (int t = 0; t < N; t++) {
					if (std::find(anyCuts.begin(), anyCuts.end(), t) != anyCuts.end()) {
						continue;
					}
					for (int m = 0; m < 3; m++) {
						most[m] = std::max(most[m], said[t][1 + m]);
					}
					if (t > 0) {
						leastCorr = std::min(leastCorr, said[t][14]);
					}
					const double base = std::min(said[t][1] / 0.20, said[t][2] / 0.001);
					const double share = std::min(base, std::max(std::min(said[t][1] / 0.25, said[t][3] / 30.0),
						std::min(said[t][3] / 3.0, (1.0 - said[t][14]) / (1.0 - 0.25))));
					if (share > closestShare) {
						closestShare = share;
						closestAt = t;
						closest[0] = said[t][1];
						closest[1] = said[t][3];
						closest[2] = said[t][2];
						closest[3] = said[t][14];
					}
				}
				Out("  %-28s elsewhere at most: histogram %.2f, ratio x%.1f, residual %.4f; correlation at least %.2f;"
				    " closest to a cut: %d (%.2f, x%.1f, %.4f, %.2f)\n",
					"", most[0], most[2], most[1], leastCorr, closestAt, closest[0], closest[1], closest[2], closest[3]);
				Out("  %-28s missed %zu, false %zu\n", "", missed.size(), falseCuts.size());
				if (!missed.empty() || !falseCuts.empty()) {
					g_failures++;
				}
			}
		}
		if (!rc && !nr && !cuts.empty()) {
			Out("  crop, output pixels: %d,%d to %d,%d\n", crop.x0, crop.y0, crop.x1, crop.y1);
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

// A command's output, read as it comes; null when it would not start.
static FILE* OpenPipe(const std::wstring& command)
{
	// cmd.exe takes the outer quotes off a command line that starts with one.
	return _wpopen((L"\"" + command + L"\"").c_str(), L"rb");
}

static int RunCutScan(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* film, double from, double seconds)
{
	Head("DLSS SR's cut detection over a stretch of a film");

	const std::wstring path(film);
	const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
	const std::wstring range = std::format(L"-ss {:.3f} -t {:.3f}", from, seconds);

	// The stream's size and rate.
	int W0 = 0, H0 = 0;
	double fps = 0;
	if (FILE* probe = OpenPipe(std::format(L"ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 \"{}\"", path))) {
		char line[256] = {};
		if (fgets(line, sizeof(line), probe)) {
			int num = 0, den = 1;
			if (sscanf_s(line, "%d,%d,%d/%d", &W0, &H0, &num, &den) == 4 && den > 0) {
				fps = (double)num / den;
			}
		}
		_pclose(probe);
	}
	// Multiples of 8, as the clips: the flow divides the picture.
	const int W = W0 & ~7, H = H0 & ~7;
	Check(W >= 64 && H >= 64 && fps > 0, "the film's size and rate (ffprobe)");
	if (W < 64 || H < 64 || fps <= 0) {
		return 1;
	}

	// ffmpeg's scene score of every picture, from a decode of its own.
	std::vector<double> scene;
	{
		const wchar_t* scores = L"cutscan_scene.txt";
		DeleteFileW(scores);
		if (FILE* run = OpenPipe(std::format(L"ffmpeg -v error {} -i \"{}\" -an -sn -fps_mode passthrough -vf \"select='gte(scene,0)',metadata=print:file={}\" -f null - 2>&1", range, path, scores))) {
			char line[512];
			while (fgets(line, sizeof(line), run)) {
			}
			_pclose(run);
		}
		FILE* in = nullptr;
		if (!_wfopen_s(&in, scores, L"r") && in) {
			char line[512];
			while (fgets(line, sizeof(line), in)) {
				if (!strncmp(line, "frame:", 6)) {
					scene.push_back(-1);
				} else if (!scene.empty() && !strncmp(line, "lavfi.scene_score=", 18)) {
					scene.back() = atof(line + 18);
				}
			}
			fclose(in);
		}
		DeleteFileW(scores);
	}

	CStabilizerPasses stabPasses;
	std::string error;
	if (!stabPasses.Init(dev, error)) {
		printf("  %s\n", error.c_str());
		return 1;
	}
	CDlssStabilizer flow;
	Tex2D_t input;
	CComPtr<ID3D11Buffer> staging;
	bool ok = SUCCEEDED(input.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_DefaultShader))
		&& SUCCEEDED(flow.Create(dev, ctx, W, H, stabPasses.InputLayout(), stabPasses.VertexShader(),
			stabPasses.SamplerPoint(), stabPasses.SamplerLinear(), true, CDlssStabilizer::ForDlssSR()))
		&& flow.HasFlow() && flow.GetCutState();
	if (ok) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth      = 16 * sizeof(float);
		desc.Usage          = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ok = SUCCEEDED(dev->CreateBuffer(&desc, nullptr, &staging));
	}
	Check(ok, "the filter's motion for DLSS SR, with its cut detection");
	if (!ok) {
		return 1;
	}

	FILE* frames = OpenPipe(std::format(L"ffmpeg -v error {} -i \"{}\" -an -sn -fps_mode passthrough -f rawvideo -pix_fmt rgba64le -", range, path));
	Check(frames != nullptr, "ffmpeg's pipe");
	if (!frames) {
		return 1;
	}
	g_report = _wfopen(std::format(L"temporal_results_cutscan_{}.txt", name).c_str(), L"w");
	FILE* csv = _wfopen(std::format(L"cutscan_{}.csv", name).c_str(), L"w");
	if (csv) {
		fprintf(csv, "frame,seconds,ffmpeg_scene,change,cut,histogram,ratio,residual,correlation,correlated\n");
	}

	std::vector<HALF> toHalf(65536);
	for (int v = 0; v < 65536; v++) {
		toHalf[v] = DirectX::PackedVector::XMConvertFloatToHalf(v / 65535.0f);
	}
	std::vector<uint16_t> raw(4 * (size_t)W0 * H0);
	std::vector<HALF> half(4 * (size_t)W * H);
	std::vector<float> grid, lastGrid;
	struct Called { int frame; float hist, ratio, resid, corr; };
	std::vector<Called> called;
	float most[3] = {}, leastCorr = 1.0f;
	int n = 0, measured = 0;
	for (; fread(raw.data(), 8 * (size_t)W0 * H0, 1, frames) == 1; n++) {
		for (int y = 0; y < H; y++) {
			const uint16_t* src = &raw[4 * (size_t)y * W0];
			HALF* dst = &half[4 * (size_t)y * W];
			for (int i = 0; i < 4 * W; i++) {
				dst[i] = (i % 4 == 3) ? toHalf[65535] : toHalf[src[i]];
			}
		}
		// The bench's change: the mean difference of luma at an eighth of the size.
		grid.clear();
		for (int y = 0; y < H; y += 8) {
			for (int x = 0; x < W; x += 8) {
				const uint16_t* q = &raw[4 * ((size_t)y * W0 + x)];
				const float rgb[3] = { q[0] / 65535.0f, q[1] / 65535.0f, q[2] / 65535.0f };
				grid.push_back(Luma(rgb));
			}
		}
		double change = 0;
		if (lastGrid.size() == grid.size()) {
			for (size_t i = 0; i < grid.size(); i++) {
				change += std::abs(grid[i] - lastGrid[i]);
			}
			change /= grid.size();
		}
		std::swap(grid, lastGrid);

		ctx->UpdateSubresource(input.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);
		flow.PrepareMotion(ctx, input.pShaderResource);
		float state[16];
		std::fill(std::begin(state), std::end(state), -1.0f);
		if (flow.CutMeasured()) {
			ctx->CopyResource(staging, flow.GetCutState());
			D3D11_MAPPED_SUBRESOURCE mr = {};
			if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mr))) {
				memcpy(state, mr.pData, sizeof(state));
				ctx->Unmap(staging, 0);
				measured++;
			}
		}
		const double ff = n < (int)scene.size() ? scene[n] : -1;
		if (csv) {
			fprintf(csv, "%d,%.3f,%.4f,%.4f,%d,%.4f,%.2f,%.6f,%.4f,%.3f\n", n, from + n / fps, ff, change,
				state[0] < 0 ? -1 : state[0] > 0.5f ? 1 : 0, state[1], state[3], state[2], state[14], state[15]);
		}
		if (state[0] > 0.5f) {
			called.push_back({ n, state[1], state[3], state[2], state[14] });
		} else if (state[0] >= 0) {
			most[0] = std::max(most[0], state[1]);
			most[1] = std::max(most[1], state[3]);
			most[2] = std::max(most[2], state[2]);
			leastCorr = std::min(leastCorr, state[14]);
		}
	}
	_pclose(frames);
	if (csv) {
		fclose(csv);
	}

	Out("\n%S from %.1f s for %.1f s: %d pictures of %dx%d at %.3f a second, %d measured; ffmpeg scored %zu.\n",
		film, from, seconds, n, W, H, fps, measured, scene.size());
	Out("Called a cut (histogram, ratio, residual, correlation), with ffmpeg's scene score:\n");
	for (const Called& c : called) {
		Out("  %6d  %9.3f s  %.2f  x%-7.0f %.4f  %5.2f   ffmpeg %.2f\n", c.frame, from + c.frame / fps, c.hist, c.ratio, c.resid,
			c.corr, c.frame < (int)scene.size() ? scene[c.frame] : -1.0);
	}
	Out("Not called, at most: histogram %.2f, ratio x%.1f, residual %.4f; correlation at least %.2f\n",
		most[0], most[1], most[2], leastCorr);
	Out("ffmpeg's scene score past 0.3 on a picture not called:\n");
	int notCalled = 0;
	for (int i = 0; i < (int)scene.size() && i < n; i++) {
		if (scene[i] > 0.3 && std::none_of(called.begin(), called.end(), [i](const Called& c) { return c.frame == i; })) {
			Out("  %6d  %9.3f s  ffmpeg %.2f\n", i, from + i / fps, scene[i]);
			notCalled++;
		}
	}
	Out("  %d picture(s)\n", notCalled);
	Check(n > 0 && measured > 0, "pictures measured");
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
	}
	return (n > 0 && measured > 0 && !g_failures) ? 0 : 1;
}

} // namespace temporal
