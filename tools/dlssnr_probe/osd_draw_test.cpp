// osd_draw_test.exe: the statistics drawn in one call a colour give the pixels they gave
// drawn two calls a line.
//
// COsdPlusText (Source/OsdPlus/OverlayText.h) draws every label in one call and each
// section's values in one call, counting on Consolas giving every printable ASCII
// character the advance of a space. This draws the same text both ways with the filter's
// own CD3D11Font, at the sizes the filter takes its font in, onto the formats it draws
// the statistics on, and requires the same bytes. The two calls a line are the code of
// a12c199, kept here as it was.

#include "stdafx.h"
#include <d3d11.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include "Helper.h"
#include "DX11Helper.h"
#include "D3DUtil/D3D11Font.h"
#include "OsdPlus/OverlayText.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

// DX11Helper.cpp names it for a texture dump this test never makes; it is in Helper.cpp,
// which the test does not need.
HRESULT SaveToBMP(BYTE*, UINT, UINT, UINT, UINT, const wchar_t*) { return E_NOTIMPL; }

static int g_failures = 0;
static int g_oldCalls = 0;

static void Check(bool ok, const char* what)
{
	printf("   %-62s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok) {
		g_failures++;
	}
}

// COsdPlusOverlay::Draw as a12c199 had it.
static HRESULT DrawTwoCallsALine(CD3D11Font& font, ID3D11RenderTargetView* target, const SIZE& size,
	POINT origin, const std::wstring& text, D3DCOLOR white, int hdrBrightness)
{
	g_oldCalls = 0;
	float y = static_cast<float>(origin.y);
	D3DCOLOR valueColor = D3DCOLOR_XRGB(128, 255, 128);
	const auto color = [&](D3DCOLOR c) {
		if (white == D3DCOLOR_XRGB(255, 255, 255)) return c;
		return TransferPQ(c, hdrBrightness == 1 ? 50 : hdrBrightness == 2 ? 30 : 100);
	};
	HRESULT hr = S_OK;
	for (size_t pos = 0; pos <= text.size();) {
		const auto end = text.find(L'\n', pos);
		const auto line = text.substr(pos, end == std::wstring::npos ? end : end - pos);
		if (line.find(L"-- Source") == 0) valueColor = D3DCOLOR_XRGB(128, 255, 128);
		else if (line.find(L"-- Pipeline") == 0) valueColor = D3DCOLOR_XRGB(128, 224, 255);
		else if (line.find(L"-- Timing") == 0) valueColor = D3DCOLOR_XRGB(255, 224, 128);
		else if (line.find(L"-- Audio") == 0) valueColor = D3DCOLOR_XRGB(224, 176, 255);
		else if (line.find(L"-- Hardware") == 0) valueColor = D3DCOLOR_XRGB(255, 192, 128);
		const auto colon = line.find(L':');
		const size_t split = colon != std::wstring::npos && colon < 24 ? colon + 1
			: !line.empty() && line.front() == L' ' ? 0 : line.size();
		const auto label = line.substr(0, split);
		SIZE extent = {};
		if (FAILED(hr = font.GetTextExtent(label.c_str(), &extent))) return hr;
		if (FAILED(hr = font.Draw2DText(target, size, static_cast<float>(origin.x), y, white, label.c_str()))) return hr;
		g_oldCalls++;
		if (split < line.size()) {
			const auto value = line.substr(split);
			if (FAILED(hr = font.Draw2DText(target, size, static_cast<float>(origin.x + extent.cx), y, color(valueColor), value.c_str()))) return hr;
			g_oldCalls++;
		}
		y += font.GetMaxCharMetric().cy;
		if (end == std::wstring::npos) break;
		pos = end + 1;
	}
	return hr;
}

// The statistics of a film played with the processor resizing and DLSS off, as the bench
// read them from the filter (osd_first.txt), with OSD Plus's lines.
static const wchar_t kStats[] =
	L"MPC VR 0.11.1.2788, Direct3D 11, Windows 11 24H2\n"
	L"Display: Panasonic-TV 3840x2160 60.000 Hz Primary windowed\n"
	L"  Color: RGB 12-bit HDR10: On\n"
	L"Graph. Adapter: NVIDIA GeForce RTX 4060 (10DE:2882)\n"
	L"-- Source ----------------------------------------------------------------------\n"
	L"Input format  : P010 1920x800\n"
	L"  Range: 16-235, Matrix: BT.709*\n"
	L"  Primaries: BT.709*, Function: BT.709*\n"
	L"  ChromaLocation: Left(MPEG-2)*\n"
	L"Frame rate    :  23.977p in,  24.009 drawn\n"
	L"Codec         : AV01\n"
	L"Video bitrate : avg 2.39 Mb/s, current 1.96 Mb/s\n"
	L"File avg      : 3.02 Mb/s (all streams + container)\n"
	L"-- Audio -----------------------------------------------------------------------\n"
	L"Audio input   : Opus, 48000 Hz, 6 ch, 32-bit, 221 kb/s\n"
	L"Audio decoded : Float PCM, 48000 Hz, 6 ch, 32-bit, 9216 kb/s\n"
	L"Audio render  : Float PCM, 48000 Hz, 6 ch, 32-bit, 9216 kb/s\n"
	L"-- Hardware --------------------------------------------------------------------\n"
	L"CPU           : 3.8% total, 1.8% player\n"
	L"GPU           : 22.6% busiest engine, decode 0.0%\n"
	L"VRAM / DRAM   : 462 MiB / 1251 MiB (player only)\n"
	L"-- Pipeline, in R16G16B16A16_FLOAT -------------------------------------- GPU ms\n"
	L"1 Processor   : D3D11 VP to R10G10B10A2_UNORM, converts chroma, resizes      9.7\n"
	L"2 Scaling     : 1920x800 -> 3840x1600 D3D11 SuperResolution*\n"
	L"3 Finish      : Adaptive-Sharpen 4, dither                                   0.9\n"
	L"  Total       : for a frame of 41.7 ms                                      10.5\n"
	L"-- Timing ----------------------------------------------------------------------\n"
	L"Clock         : Audio renderer\n"
	L"Render ahead  : asked  40 ms early, ready in  12.9 (max  13.7), 2 not in time\n"
	L"CPU (ms)      : copy   0.3, draw   0.3, wait  26.3, Present   0.1\n"
	L"Swap chain    : Flip discard, R10G10B10A2_UNORM, 3 buffers\n"
	L"Frame sync    : a refresh chosen for each picture\n"
	L"Sync offset   :   +5.2 ms\n"
	L"On screen     :   -4.1 ms, scheduled: repeated 0, dropped 0, shown late 0\n"
	L"Frames        :   452 received, skipped 0 late + 0 fields, failed 0";

// What the statistics do not hold today and could: characters outside ASCII in a value
// and in a label, a colon past the 24th column, an empty line, a value past the 400
// characters the font draws in one go, and a line that ends the text with a newline.
static std::wstring EdgeText()
{
	std::wstring s =
		L"-- Source ----\n"
		L"Bitrate       : 12.3 µs, 45° → ± «»\n"
		L"Débit         : 3.02 Mb/s\n"
		L"A label that runs past the twenty-fourth column: its value\n"
		L"\n"
		L"-- Timing ----\n"
		L"Long          : ";
	for (int i = 0; i < 460; i++) {
		s += static_cast<wchar_t>(L'!' + i % 94);
	}
	s += L"\n  indented line, all value\n";
	return s;
}

struct Target {
	CComPtr<ID3D11Texture2D> texture, staging;
	CComPtr<ID3D11RenderTargetView> view;
	SIZE size = {};
};

static bool MakeTarget(ID3D11Device* device, DXGI_FORMAT format, SIZE size, Target& t)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = size.cx;
	desc.Height = size.cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	t = {};
	t.size = size;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &t.texture))) return false;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &t.staging))) return false;
	return SUCCEEDED(device->CreateRenderTargetView(t.texture, nullptr, &t.view));
}

static std::vector<uint8_t> Read(ID3D11DeviceContext* context, Target& t, UINT bytesPerPixel)
{
	context->CopyResource(t.staging, t.texture);
	std::vector<uint8_t> bytes;
	D3D11_MAPPED_SUBRESOURCE map = {};
	if (SUCCEEDED(context->Map(t.staging, 0, D3D11_MAP_READ, 0, &map))) {
		const UINT row = t.size.cx * bytesPerPixel;
		bytes.resize(static_cast<size_t>(row) * t.size.cy);
		for (LONG y = 0; y < t.size.cy; y++) {
			memcpy(bytes.data() + static_cast<size_t>(y) * row, static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, row);
		}
		context->Unmap(t.staging, 0);
	}
	return bytes;
}

static size_t Lines(const std::wstring& text)
{
	return std::count(text.begin(), text.end(), L'\n') + 1;
}

static size_t Widest(const std::wstring& text)
{
	size_t widest = 0;
	for (size_t pos = 0; pos <= text.size();) {
		const auto end = text.find(L'\n', pos);
		widest = std::max(widest, (end == std::wstring::npos ? text.size() : end) - pos);
		if (end == std::wstring::npos) break;
		pos = end + 1;
	}
	return widest;
}

int wmain()
{
	wprintf(L"The statistics drawn in one call a colour against two calls a line, with the\n"
		L"filter's own font, at the sizes it takes its font in and on the formats it draws\n"
		L"the statistics on: the same bytes are required.\n\n");

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CComPtr<ID3D11Device> device;
	CComPtr<ID3D11DeviceContext> context;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, (UINT)std::size(levels),
		D3D11_SDK_VERSION, &device, nullptr, &context);
	if (FAILED(hr)) {
		wprintf(L"no Direct3D 11 device: 0x%08X\n1 failure(s)\n", (unsigned)hr);
		return 1;
	}

	const std::wstring stats = kStats;
	const std::wstring edge = EdgeText();
	const struct { const wchar_t* name; DXGI_FORMAT format; UINT bytes; } formats[] = {
		{ L"B8G8R8A8_UNORM",     DXGI_FORMAT_B8G8R8A8_UNORM,     4 },
		{ L"R10G10B10A2_UNORM",  DXGI_FORMAT_R10G10B10A2_UNORM,  4 },
		{ L"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, 8 },
	};
	// What the filter draws with: white on an SDR picture, white carried in PQ on an HDR
	// one, with the three OSD brightnesses that tone the section colours.
	const struct { const wchar_t* name; D3DCOLOR white; int brightness; bool hdrOnly; } colours[] = {
		{ L"SDR white",     D3DCOLOR_XRGB(255, 255, 255),                  0, false },
		{ L"PQ, 100 nits",  TransferPQ(D3DCOLOR_XRGB(255, 255, 255), 100), 0, true },
		{ L"PQ, 50 nits",   TransferPQ(D3DCOLOR_XRGB(255, 255, 255), 50),  1, true },
		{ L"PQ, 30 nits",   TransferPQ(D3DCOLOR_XRGB(255, 255, 255), 30),  2, true },
	};
	// 14 is the smallest the filter takes, MulDiv(14, dpi, 96) gives the odd ones, and the
	// box following the window gives even sizes up past 40 on a 4K screen.
	const int heights[] = { 14, 15, 17, 18, 21, 24, 28, 33, 36, 42, 48 };
	const POINT origin = { 13, 11 };

	int compared = 0, newCalls = 0, oldCalls = 0;
	for (const int h : heights) {
		CD3D11Font font;
		if (FAILED(font.InitDeviceObjects(device, context)) || FAILED(font.CreateFontBitmap(L"Consolas", h, 0))) {
			wprintf(L"Consolas %d: the font could not be made\n", h);
			g_failures++;
			continue;
		}
		SIZE space = {};
		font.GetTextExtent(L" ", &space);
		bool sameAdvance = true;
		for (wchar_t c = 0x20; c <= 0x7E; c++) {
			const wchar_t one[2] = { c, 0 };
			SIZE e = {};
			font.GetTextExtent(one, &e);
			sameAdvance &= e.cx == space.cx;
		}
		const SIZE cell = font.GetMaxCharMetric();
		printf("-- Consolas %d: a cell of %ld x %ld, a space advances %ld and a line %ld\n", h, cell.cx, cell.cy, space.cx, space.cy);
		Check(sameAdvance, "every printable ASCII character advances as a space does");

		for (const auto* text : { &stats, &edge }) {
			const SIZE size = { origin.x + 2 + static_cast<LONG>(Widest(*text) + 2) * (cell.cx + 1), origin.y + 2 + static_cast<LONG>(Lines(*text)) * (cell.cy + 1) };
			for (const auto& f : formats) {
				Target t;
				if (!MakeTarget(device, f.format, size, t)) {
					wprintf(L"   %s %ldx%ld: no render target\n", f.name, size.cx, size.cy);
					g_failures++;
					continue;
				}
				for (const auto& c : colours) {
					if (c.hdrOnly && f.format == DXGI_FORMAT_B8G8R8A8_UNORM) continue;
					const float clear[4] = { 0.05f, 0.1f, 0.2f, 0.7f };
					context->ClearRenderTargetView(t.view, clear);
					hr = DrawTwoCallsALine(font, t.view, t.size, origin, *text, c.white, c.brightness);
					const auto before = Read(context, t, f.bytes);
					context->ClearRenderTargetView(t.view, clear);
					COsdPlusText draw;
					const HRESULT hr2 = draw.Draw(font, t.view, t.size, origin, *text, c.white, c.brightness);
					const auto after = Read(context, t, f.bytes);
					size_t differ = 0, first = SIZE_MAX;
					for (size_t i = 0; i < before.size() && i < after.size(); i += f.bytes) {
						if (memcmp(&before[i], &after[i], f.bytes)) {
							if (first == SIZE_MAX) first = i / f.bytes;
							differ++;
						}
					}
					compared++;
					if (text == &stats) {
						oldCalls = g_oldCalls;
						newCalls = draw.Calls();
					}
					const bool ok = SUCCEEDED(hr) && SUCCEEDED(hr2) && !before.empty() && before.size() == after.size() && !differ;
					char what[128];
					snprintf(what, sizeof(what), "%s, %ls, %ls", text == &stats ? "statistics" : "edge cases", f.name, c.name);
					if (!ok && first != SIZE_MAX) {
						snprintf(what, sizeof(what), "%s, %ls, %ls: %zu pixels, first at %zu,%zu", text == &stats ? "statistics" : "edge cases",
							f.name, c.name, differ, first % size.cx, first / size.cx);
					}
					Check(ok, what);
				}
			}
		}
	}

	// The calls, and what they cost the render thread, on the statistics at 14 points.
	{
		CD3D11Font font;
		font.InitDeviceObjects(device, context);
		font.CreateFontBitmap(L"Consolas", 14, 0);
		Target t;
		MakeTarget(device, DXGI_FORMAT_R10G10B10A2_UNORM, { 1400, 900 }, t);
		COsdPlusText draw;
		const auto time = [&](auto&& fn) {
			for (int i = 0; i < 20; i++) fn();
			context->Flush();
			const auto start = std::chrono::steady_clock::now();
			const int n = 400;
			for (int i = 0; i < n; i++) {
				fn();
				context->Flush();
			}
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / n;
		};
		const double oldMs = time([&] { DrawTwoCallsALine(font, t.view, t.size, origin, stats, D3DCOLOR_XRGB(255, 255, 255), 0); });
		const double newMs = time([&] { draw.Draw(font, t.view, t.size, origin, stats, D3DCOLOR_XRGB(255, 255, 255), 0); });
		printf("\n-- the statistics, %zu lines\n", Lines(stats));
		printf("   calls a picture        two a line %d, one a colour %d\n", oldCalls, newCalls);
		printf("   CPU a picture          two a line %.3f ms, one a colour %.3f ms\n", oldMs, newMs);
		Check(newCalls >= 1 && newCalls <= 6, "the statistics go out in six calls at most");
	}

	printf("\n%d drawings compared\n%d failure(s)\n", compared, g_failures);
	return g_failures ? 1 : 0;
}
