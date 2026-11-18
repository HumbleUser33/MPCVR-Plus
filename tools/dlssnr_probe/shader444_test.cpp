// Does the 4:4:4 compute shader the filter generates actually compile, for each
// source format, each chroma method and each place the chroma can be sited? And the
// conversion the shaders do themselves, which writes its chroma with the same code?
// Prints the compiler's own words if not.

#include "stdafx.h"
#include <d3d11.h>
#include <D3Dcompiler.h>
#include <cstdio>
#include "Helper.h"
#include "IVideoRenderer.h"
#include "Shaders.h"

// The GUIDs the filter's own sources name are defined once, in shader444_guids.cpp.

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxva2.lib")

int wmain()
{
	wprintf(L"The 4:4:4 chroma pass the filter generates for \"Replace VP chroma upsampling\",\n"
		L"compiled for each source format and each chroma method. A compute shader has\n"
		L"rules a pixel shader has not, and one that does not compile leaves the picture\n"
		L"to the video processor without a word.\n\n");

	const struct { const wchar_t* name; ColorFormat_t cformat; DXGI_FORMAT packed; } sources[] = {
		{ L"NV12", CF_NV12, DXGI_FORMAT_AYUV },
		{ L"P010", CF_P010, DXGI_FORMAT_Y410 },
		{ L"P016", CF_P016, DXGI_FORMAT_Y410 },
		{ L"YUY2", CF_YUY2, DXGI_FORMAT_AYUV },
		{ L"YV12", CF_YV12, DXGI_FORMAT_AYUV },
	};
	const struct { const wchar_t* name; int chroma; } methods[] = {
		{ L"Nearest",     CHROMA_Nearest },
		{ L"Bilinear",    CHROMA_Bilinear },
		{ L"Catmull-Rom", CHROMA_CatmullRom },
		{ L"RAVU-zoom",   CHROMA_RAVU },
		{ L"Jinc",        CHROMA_Jinc },
		{ L"EWA Lanczos Sharp",     CHROMA_EwaLanczosSharp },
		{ L"EWA Lanczos4 Sharpest", CHROMA_EwaLanczos4Sharpest },
		{ L"EWA Ginseng", CHROMA_EwaGinseng },
	};
	// Where the chroma sample sits decides which texels a polar kernel reads and at
	// what distance. Top-left, as UHD and HDR10 sources have it, puts one exactly on a
	// pixel: that is what Jinc, EWA Lanczos Sharp and EWA Ginseng did not compile with.
	const struct { const wchar_t* name; UINT siting; } sitings[] = {
		{ L"Chroma sited left (MPEG-2)",                   DXVA2_VideoChromaSubsampling_MPEG2 },
		{ L"Chroma sited top-left (co-sited), as UHD has", DXVA2_VideoChromaSubsampling_Cosited },
		{ L"Chroma sited in the centre (MPEG-1)",          DXVA2_VideoChromaSubsampling_MPEG1 },
	};

	DXVA2_ExtendedFormat exFmt = {};
	exFmt.SampleFormat           = DXVA2_SampleProgressiveFrame;
	exFmt.NominalRange           = DXVA2_NominalRange_16_235;
	exFmt.VideoTransferMatrix    = DXVA2_VideoTransferMatrix_BT709;
	exFmt.VideoPrimaries         = DXVA2_VideoPrimaries_BT709;
	exFmt.VideoTransferFunction  = DXVA2_VideoTransFunc_709;

	int failures = 0;
	for (const auto& siting : sitings) {
		exFmt.VideoChromaSubsampling = siting.siting;
		wprintf(L"%s:\n", siting.name);
		for (const auto& source : sources) {
			const FmtConvParams_t& params = GetFmtConvParams(source.cformat);
			for (const auto& method : methods) {
				CComPtr<ID3DBlob> code;
				const HRESULT hr = GetShaderConvertTo444(1920, 1920, 1080, params, exFmt, method.chroma, source.packed, &code);
				wprintf(L"  %-5s %-22s -> %-5s : %s\n", source.name, method.name,
					source.packed == DXGI_FORMAT_AYUV ? L"AYUV" : L"Y410",
					SUCCEEDED(hr) ? L"compiles" : std::format(L"FAILED {:#010x}", (unsigned)hr).c_str());
				failures += FAILED(hr);
			}
		}
		wprintf(L"\n");
	}

	// The same chroma code in the pixel shader that converts the picture when the
	// shaders do it all, which the Dolby Vision reshaping is part of: one that does not
	// compile falls back to a stock conversion without the chosen method.
	wprintf(L"And the conversion the shaders do themselves, the same chroma code in a pixel shader:\n");
	const RECT rect = { 0, 0, 1920, 1080 };
	for (const auto& siting : sitings) {
		exFmt.VideoChromaSubsampling = siting.siting;
		wprintf(L"%s:\n", siting.name);
		for (const auto& source : sources) {
			const FmtConvParams_t& params = GetFmtConvParams(source.cformat);
			for (const auto& method : methods) {
				CComPtr<ID3DBlob> code;
				const HRESULT hr = GetShaderConvertColor(true, 1920, 1920, 1080, rect, params, exFmt, nullptr,
					method.chroma, SHADER_CONVERT_NONE, false, false, &code);
				wprintf(L"  %-5s %-22s -> RGB   : %s\n", source.name, method.name,
					SUCCEEDED(hr) ? L"compiles" : std::format(L"FAILED {:#010x}", (unsigned)hr).c_str());
				failures += FAILED(hr);
			}
		}
		wprintf(L"\n");
	}
	wprintf(L"%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
