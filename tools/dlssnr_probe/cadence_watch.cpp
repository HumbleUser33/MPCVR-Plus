// cadence_watch: what the screen shows, update by update, while a player plays.
//
// The renderer's own trace says on which refresh DXGI put each Present. It cannot say
// what the picture held -- a frame the decoder gave twice, one shown out of order, a
// temporal pass that showed an old picture -- and it says nothing at all about a
// renderer that keeps no trace, such as upstream's. The desktop duplication hands out
// every image Windows puts on the screen with the time it went there, so this reads,
// for each one: when, a signature of the film's picture in a grid of small tiles, and
// where the statistics' green marker is, which moves one pixel for every picture the
// renderer draws.
//
//    cadence_watch --out file.csv [--seconds 60] [--delay 0] [--region l,t,r,b] [--shot file.bmp]
//
// --region is the part of the screen, in fractions, the tiles are spread over: keep it
// clear of the statistics and the subtitles. --shot keeps the top left quarter of the
// first image, where the statistics are. The analysis is cadence_report.py's.

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <atlbase.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static double HalfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
	double v = 0;
	if (exp == 0) {
		v = std::ldexp((double)man, -24);
	} else if (exp != 31) {
		v = std::ldexp(1024.0 + man, (int)exp - 25);
	}
	return sign ? -v : v;
}

struct Pixels {
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	const BYTE* data = nullptr;
	UINT pitch = 0;
	// Red, green and blue at x, y, in the desktop's own scale: scRGB on an HDR desktop
	// (1.0 is SDR white), 0..1 otherwise.
	void At(int x, int y, double rgb[3]) const
	{
		const BYTE* row = data + (size_t)y * pitch;
		if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
			const uint16_t* p = (const uint16_t*)row + (size_t)x * 4;
			rgb[0] = HalfToFloat(p[0]); rgb[1] = HalfToFloat(p[1]); rgb[2] = HalfToFloat(p[2]);
		} else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
			const uint32_t v = ((const uint32_t*)row)[x];
			rgb[0] = (v & 1023) / 1023.0; rgb[1] = ((v >> 10) & 1023) / 1023.0; rgb[2] = ((v >> 20) & 1023) / 1023.0;
		} else {
			const BYTE* p = row + (size_t)x * 4;
			rgb[0] = p[2] / 255.0; rgb[1] = p[1] / 255.0; rgb[2] = p[0] / 255.0;
		}
	}
};

static bool IsMarkerGreen(const double rgb[3], double floor)
{
	return rgb[1] > floor && rgb[1] > 1.6 * rgb[0] && rgb[1] > 1.6 * rgb[2];
}

int wmain(int argc, wchar_t** argv)
{
	std::wstring out, shot;
	double seconds = 60, delay = 0;
	double rl = 0.45, rt = 0.25, rr = 0.92, rb = 0.72;
	for (int i = 1; i < argc; i++) {
		const std::wstring a = argv[i];
		if (a == L"--out" && i + 1 < argc) out = argv[++i];
		else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
		else if (a == L"--delay" && i + 1 < argc) delay = _wtof(argv[++i]);
		else if (a == L"--shot" && i + 1 < argc) shot = argv[++i];
		else if (a == L"--region" && i + 1 < argc) swscanf_s(argv[++i], L"%lf,%lf,%lf,%lf", &rl, &rt, &rr, &rb);
	}
	if (out.empty()) {
		wprintf(L"cadence_watch --out file.csv [--seconds 60] [--delay 0] [--region l,t,r,b] [--shot file.bmp]\n");
		return 2;
	}

	using PFN_SetDpi = BOOL (WINAPI*)(HANDLE);
	if (auto set = (PFN_SetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) {
		set((HANDLE)-4); // per monitor, v2: DuplicateOutput1 refuses anything else
	}

	CComPtr<ID3D11Device> pDevice;
	CComPtr<ID3D11DeviceContext> pContext;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL level = {};
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &pDevice, &level, &pContext))) {
		wprintf(L"no device\n");
		return 1;
	}
	CComQIPtr<IDXGIDevice> pDXGIDevice(pDevice.p);
	CComPtr<IDXGIAdapter> pAdapter;
	pDXGIDevice->GetAdapter(&pAdapter);
	CComPtr<IDXGIOutput> pOutput;
	if (FAILED(pAdapter->EnumOutputs(0, &pOutput))) {
		wprintf(L"no output\n");
		return 1;
	}
	DXGI_OUTPUT_DESC odesc = {};
	pOutput->GetDesc(&odesc);
	DEVMODEW mode = { sizeof(mode) };
	EnumDisplaySettingsW(odesc.DeviceName, ENUM_CURRENT_SETTINGS, &mode);

	CComPtr<IDXGIOutputDuplication> pDup;
	HRESULT hr = E_FAIL;
	if (CComQIPtr<IDXGIOutput5> pOutput5 = pOutput.p) {
		const DXGI_FORMAT wanted[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
		hr = pOutput5->DuplicateOutput1(pDevice, 0, 3, wanted, &pDup);
	}
	if (!pDup) {
		CComQIPtr<IDXGIOutput1> pOutput1(pOutput.p);
		hr = pOutput1 ? pOutput1->DuplicateOutput(pDevice, &pDup) : E_NOINTERFACE;
	}
	if (!pDup) {
		wprintf(L"the desktop cannot be duplicated: 0x%08x\n", (unsigned)hr);
		return 1;
	}
	DXGI_OUTDUPL_DESC ddesc = {};
	pDup->GetDesc(&ddesc);
	const int W = (int)ddesc.ModeDesc.Width, H = (int)ddesc.ModeDesc.Height;

	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);

	if (delay > 0) {
		Sleep((DWORD)(delay * 1000));
	}

	// The film's tiles: a grid of 8 x 8 squares of 16 pixels over the region.
	constexpr int kGrid = 8, kTile = 16, kRing = 3;
	const int gx0 = (int)(rl * W), gy0 = (int)(rt * H), gx1 = (int)(rr * W), gy1 = (int)(rb * H);
	std::vector<std::pair<int, int>> tiles;
	for (int j = 0; j < kGrid; j++) {
		for (int i = 0; i < kGrid; i++) {
			tiles.push_back({ gx0 + (gx1 - gx0 - kTile) * i / (kGrid - 1), gy0 + (gy1 - gy0 - kTile) * j / (kGrid - 1) });
		}
	}

	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	CComPtr<ID3D11Texture2D> pTiles[kRing], pStrip[kRing];
	int markerTop = -1, stripW = std::min(W, 2600);
	constexpr int kStripH = 4;

	struct Pending {
		bool used = false;
		long long qpc = 0, acquired = 0;
		UINT accumulated = 0;
	} pending[kRing];

	FILE* f = nullptr;
	if (_wfopen_s(&f, out.c_str(), L"w") || !f) {
		wprintf(L"cannot write %s\n", out.c_str());
		return 1;
	}
	fprintf(f, "# cadence_watch, %lld ticks per second, display %dx%d %u Hz, region %.2f,%.2f,%.2f,%.2f\n",
		freq.QuadPart, W, H, mode.dmDisplayFrequency, rl, rt, rr, rb);

	std::vector<float> prev[6];
	int nPrev = 0;
	long long rows = 0, misses = 0;
	QueryPerformanceCounter(&now);
	const long long tEnd = now.QuadPart + (long long)(seconds * freq.QuadPart);
	int slot = 0;

	const auto Emit = [&](int k) {
		Pending& p = pending[k];
		if (!p.used) {
			return;
		}
		p.used = false;
		std::vector<float> sig(kGrid * kGrid * kTile * kTile);
		double tileMean[kGrid * kGrid] = {};
		D3D11_MAPPED_SUBRESOURCE m = {};
		if (SUCCEEDED(pContext->Map(pTiles[k], 0, D3D11_MAP_READ, 0, &m))) {
			Pixels px{ format, (const BYTE*)m.pData, m.RowPitch };
			for (int t = 0; t < kGrid * kGrid; t++) {
				const int ox = (t % kGrid) * kTile, oy = (t / kGrid) * kTile;
				double sum = 0;
				for (int y = 0; y < kTile; y++) {
					for (int x = 0; x < kTile; x++) {
						double rgb[3];
						px.At(ox + x, oy + y, rgb);
						const float v = (float)(0.25 * rgb[0] + 0.5 * rgb[1] + 0.25 * rgb[2]);
						sig[(size_t)t * kTile * kTile + y * kTile + x] = v;
						sum += v;
					}
				}
				tileMean[t] = sum / (kTile * kTile);
			}
			pContext->Unmap(pTiles[k], 0);
		}
		int markerX = -1;
		if (pStrip[k] && SUCCEEDED(pContext->Map(pStrip[k], 0, D3D11_MAP_READ, 0, &m))) {
			Pixels px{ format, (const BYTE*)m.pData, m.RowPitch };
			// The marker is the brightest green run in the strip: its middle.
			double floor = 0;
			for (int x = 0; x < stripW; x++) {
				double rgb[3];
				px.At(x, 1, rgb);
				if (rgb[1] > 1.6 * rgb[0] && rgb[1] > 1.6 * rgb[2]) floor = std::max(floor, rgb[1]);
			}
			floor *= 0.6;
			int best = 0, bestStart = -1, run = 0;
			for (int x = 0; x <= stripW; x++) {
				double rgb[3] = {};
				if (x < stripW) px.At(x, 1, rgb);
				if (x < stripW && floor > 0 && IsMarkerGreen(rgb, floor)) {
					run++;
				} else {
					if (run >= 3 && run <= 8 && run > best) {
						best = run;
						bestStart = x - run;
					}
					run = 0;
				}
			}
			if (bestStart >= 0) {
				markerX = bestStart;
			}
			pContext->Unmap(pStrip[k], 0);
		}
		// How far this image is from those before it, per pixel.
		double d[6] = { -1, -1, -1, -1, -1, -1 };
		for (int b = 0; b < std::min(nPrev, 6); b++) {
			const std::vector<float>& q = prev[b];
			double s = 0;
			for (size_t i = 0; i < sig.size(); i++) s += std::fabs(sig[i] - q[i]);
			d[b] = s / sig.size();
		}
		for (int b = 5; b > 0; b--) prev[b] = std::move(prev[b - 1]);
		prev[0] = std::move(sig);
		nPrev++;
		double mean = 0;
		for (double v : tileMean) mean += v;
		mean /= kGrid * kGrid;
		fprintf(f, "%lld,%lld,%u,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.5f\n", p.qpc, p.acquired, p.accumulated, markerX,
			d[0], d[1], d[2], d[3], d[4], d[5], mean);
		rows++;
	};

	fprintf(f, "# qpc,acquired,accumulated,marker,d1,d2,d3,d4,d5,d6,mean\n");
	for (;;) {
		QueryPerformanceCounter(&now);
		if (now.QuadPart >= tEnd) {
			break;
		}
		DXGI_OUTDUPL_FRAME_INFO info = {};
		CComPtr<IDXGIResource> pRes;
		hr = pDup->AcquireNextFrame(100, &info, &pRes);
		if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
			continue;
		}
		if (hr == DXGI_ERROR_ACCESS_LOST) {
			// A mode change or a fullscreen switch: duplicate again.
			pDup.Release();
			fprintf(f, "# access lost\n");
			for (int tries = 0; tries < 50 && !pDup; tries++) {
				Sleep(100);
				if (CComQIPtr<IDXGIOutput5> pOutput5 = pOutput.p) {
					const DXGI_FORMAT wanted[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
					pOutput5->DuplicateOutput1(pDevice, 0, 3, wanted, &pDup);
				}
			}
			if (!pDup) {
				break;
			}
			DEVMODEW changed = { sizeof(changed) };
			EnumDisplaySettingsW(odesc.DeviceName, ENUM_CURRENT_SETTINGS, &changed);
			fprintf(f, "# display now %u Hz\n", changed.dmDisplayFrequency);
			continue;
		}
		if (FAILED(hr)) {
			fprintf(f, "# AcquireNextFrame 0x%08x\n", (unsigned)hr);
			break;
		}
		LARGE_INTEGER acquired;
		QueryPerformanceCounter(&acquired);
		if (info.LastPresentTime.QuadPart == 0) {
			pDup->ReleaseFrame(); // only the pointer moved
			continue;
		}
		CComQIPtr<ID3D11Texture2D> pDesk(pRes.p);
		D3D11_TEXTURE2D_DESC desc = {};
		pDesk->GetDesc(&desc);
		if (desc.Format != format || !pTiles[0]) {
			format = desc.Format;
			for (int k = 0; k < kRing; k++) {
				pTiles[k].Release();
				pStrip[k].Release();
				D3D11_TEXTURE2D_DESC t = desc;
				t.Width = kGrid * kTile; t.Height = kGrid * kTile;
				t.MipLevels = 1; t.ArraySize = 1; t.SampleDesc = { 1, 0 };
				t.Usage = D3D11_USAGE_STAGING; t.BindFlags = 0; t.MiscFlags = 0; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				pDevice->CreateTexture2D(&t, nullptr, &pTiles[k]);
				pending[k].used = false;
			}
			markerTop = -1;
		}
		if (markerTop < 0) {
			// Where the marker runs: a block 5 pixels wide and 10 high, bright green, in
			// the top left of the screen, found once on a whole copy of the desktop.
			D3D11_TEXTURE2D_DESC t = desc;
			t.MipLevels = 1; t.ArraySize = 1; t.SampleDesc = { 1, 0 };
			t.Usage = D3D11_USAGE_STAGING; t.BindFlags = 0; t.MiscFlags = 0; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			CComPtr<ID3D11Texture2D> pFull;
			D3D11_MAPPED_SUBRESOURCE m = {};
			if (SUCCEEDED(pDevice->CreateTexture2D(&t, nullptr, &pFull))) {
				pContext->CopyResource(pFull, pDesk);
				if (SUCCEEDED(pContext->Map(pFull, 0, D3D11_MAP_READ, 0, &m))) {
					Pixels px{ format, (const BYTE*)m.pData, m.RowPitch };
					double brightest = 0;
					for (int y = 0; y < H * 3 / 4; y += 2) {
						for (int x = 0; x < W / 2; x += 2) {
							double rgb[3];
							px.At(x, y, rgb);
							if (rgb[1] > 1.6 * rgb[0] && rgb[1] > 1.6 * rgb[2]) brightest = std::max(brightest, rgb[1]);
						}
					}
					const double floor = brightest * 0.6;
					for (int y = 0; y + 10 < H * 3 / 4 && markerTop < 0 && brightest > 0; y++) {
						for (int x = 0; x + 5 < W / 2; x++) {
							bool solid = true;
							for (int yy = 0; yy < 10 && solid; yy++) {
								for (int xx = 0; xx < 5 && solid; xx++) {
									double rgb[3];
									px.At(x + xx, y + yy, rgb);
									solid = IsMarkerGreen(rgb, floor);
								}
							}
							if (solid) {
								markerTop = y;
								fprintf(f, "# marker found at %d,%d\n", x, y);
								break;
							}
						}
					}
					if (!shot.empty()) {
						// The top left of the screen, where the statistics are, at half size, for a person to read.
						const int sw = W / 4, sh = H * 3 / 8, rowBytes = (sw * 3 + 3) & ~3;
						std::vector<BYTE> img((size_t)rowBytes * sh);
						for (int y = 0; y < sh; y++) {
							for (int x = 0; x < sw; x++) {
								double rgb[3];
								px.At(x * 2, y * 2, rgb);
								BYTE* d = &img[(size_t)(sh - 1 - y) * rowBytes + x * 3];
								for (int c = 0; c < 3; c++) {
									const double v = std::clamp(rgb[2 - c], 0.0, 1.0);
									d[c] = (BYTE)std::lround(255.0 * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055));
								}
							}
						}
						FILE* b = nullptr;
						if (!_wfopen_s(&b, shot.c_str(), L"wb") && b) {
							BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + img.size()), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
							BITMAPINFOHEADER ih = { sizeof(ih), sw, sh, 1, 24, BI_RGB, (DWORD)img.size() };
							fwrite(&fh, sizeof(fh), 1, b); fwrite(&ih, sizeof(ih), 1, b); fwrite(img.data(), 1, img.size(), b);
							fclose(b);
						}
					}
					pContext->Unmap(pFull, 0);
				}
			}
			if (markerTop < 0) {
				markerTop = H; // not on the screen: no strip
				fprintf(f, "# no marker found\n");
			} else {
				for (int k = 0; k < kRing; k++) {
					D3D11_TEXTURE2D_DESC s = t;
					s.Width = stripW; s.Height = kStripH;
					pDevice->CreateTexture2D(&s, nullptr, &pStrip[k]);
				}
			}
		}

		// The slot about to be reused is read first: the GPU has had two images' time.
		Emit(slot);
		for (int t = 0; t < kGrid * kGrid; t++) {
			const D3D11_BOX box = { (UINT)tiles[t].first, (UINT)tiles[t].second, 0,
				(UINT)tiles[t].first + kTile, (UINT)tiles[t].second + kTile, 1 };
			pContext->CopySubresourceRegion(pTiles[slot], 0, (t % kGrid) * kTile, (t / kGrid) * kTile, 0, pDesk, 0, &box);
		}
		if (pStrip[slot] && markerTop + 3 + kStripH <= H) {
			const D3D11_BOX box = { 0, (UINT)markerTop + 3, 0, (UINT)stripW, (UINT)markerTop + 3 + kStripH, 1 };
			pContext->CopySubresourceRegion(pStrip[slot], 0, 0, 0, 0, pDesk, 0, &box);
		}
		pending[slot] = { true, info.LastPresentTime.QuadPart, acquired.QuadPart, info.AccumulatedFrames };
		if (info.AccumulatedFrames > 1) {
			misses += info.AccumulatedFrames - 1;
		}
		pDup->ReleaseFrame();
		slot = (slot + 1) % kRing;
	}
	for (int k = 0; k < kRing; k++) {
		Emit((slot + k) % kRing);
	}
	fclose(f);
	wprintf(L"%lld images, %lld coalesced, format %d\n", rows, misses, (int)format);
	return 0;
}
