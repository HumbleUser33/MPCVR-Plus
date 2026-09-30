// Render ahead, measured in a real DirectShow graph: a synthetic film at 23.976 fps
// (or --fps) played by the renderer's own filter (_bin\Filter_x64\MpcVideoRenderer64.ax) in a
// window, on the system clock. For each configuration -- DLSS off, DLSS SR, DLSS 5
// NR with SR, render ahead off and on -- it reads the statistics the renderer
// draws (IExFilterConfig "statsText") ten times a second and reports:
//
//   sync offset  when Present returned, against the picture's time (the renderer's
//                own Sync offset); without DLSS it sits about half a refresh early
//   skipped      pictures the renderer dropped while measuring
//   late         pictures render ahead held that were ready only after their time
//
// Then it pauses, runs and stops the graph, and fails if a state change takes more
// than a second: a held picture must never block the player.
//
// The settings are handed to the filter for this run only; nothing is saved.
//
// --scalers plays a still NV12 picture through the shader video processor with each
// Upscaling and Chroma upsampling method in turn instead (Catmull-Rom, Jinc2m, the
// mpv prescalers), reports what the statistics say they did and cost, saves the
// displayed picture as scalers_<n>.bmp next to the program and compares each with
// the Catmull-Rom one: a wrong pass shows as a large difference.
//
//   playback_test.exe [--seconds 20] [--size 800x450] [--window 1280x720] [--fps 23.976] [--only N] [--scalers]
//   playback_test.exe --dlsspage N    shows the filter's DLSS page for N seconds instead
//   playback_test.exe --mainpage N    the same for the Settings page
//   playback_test.exe --chroma10 <png>  --chroma in ten bits: P010 against a Y410 reference
//   playback_test.exe --chroma <png> --interlaced   the media type says interlaced, to see
//   playback_test.exe --toggle          settings changed in full playback, as the player does
//                                     which processor then converts the picture

#include <windows.h>
#include <VersionHelpers.h>
#include <streams.h>
#include <dvdmedia.h>
#include <atlbase.h>
#include <d3d11.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <vector>
#include "IVideoRenderer.h"
#include "../../Include/ID3DVideoMemoryConfiguration.h"
#include "../../Include/FilterInterfaces.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// BaseClasses' DLL entry code comes along with the library; an executable has no
// class factories to list.
CFactoryTemplate g_Templates[] = { { L"", nullptr, nullptr, nullptr, nullptr } };
int g_cTemplates = 0;

static const CLSID CLSID_MpcVideoRenderer = { 0x71F080AA, 0x8661, 0x4093, { 0xB1, 0x5E, 0x4F, 0x69, 0x03, 0xE7, 0x7D, 0x0A } };

static REFERENCE_TIME g_frameDuration = 417083; // 23.976 fps unless --fps says otherwise
static bool g_bScalers = false; // --scalers: a still picture, in NV12
static bool g_bHardwareVP = false; // --vp: leave the video processor the formats it is set for
// --chroma: one still picture fed as 4:2:0 and as 4:4:4, at 1:1, to see what each
// chroma upsampler rebuilds -- the hardware video processor included.
static bool g_bChroma = false;
static bool g_bChroma444 = false;      // this run feeds AYUV, the reference
static bool g_bToggle = false;         // --toggle: settings changed while the film plays
static bool g_bToggleHdr = false;      // --toggle --hdr: with HDR passthrough, the case that freezes
static bool g_bToggleThread = false;   // --toggle --thread: applied from another thread, as the player's page does
static bool g_bToggleSwitch = false;   // --toggle --switch: the processor's extras as the chroma moves to the shaders and back
static const wchar_t* g_filmFile = nullptr;  // --file <path>: a real film, split and decoded as the player does
// What to ask of the processor on the film path. Sent through SetSettings rather than
// left to the registry, so the same request reaches any build of the filter: the one
// MPC-HC ships does not read the registry at all, and Settings_t now carries upstream's
// fields first and this fork's after them, so the layout it reads is the one it expects.
static int g_filmRtxHdr = -1;     // --rtxhdr 0|1
static int g_filmSuperRes = -1;   // --superres 0..4
static int g_filmPrepass = -1;    // --prepass 0|1
static int g_filmHdrOut = -1;     // --hdrout 0|1
static int g_filmVPScale = -1;    // --vpscale 0|1
static int g_filmStats = -1;      // --stats 0|1
// --asis: leave the settings exactly as the registry has them. The only way to drive a
// renderer this bench did not build: Settings_t has grown fields since, so SetSettings
// on the stock filter writes a layout it does not know and it ignores the lot. Both
// renderers read the same key, so the registry is the common ground.
static bool g_bAsIs = false;
static bool g_bToggleGpu = false;      // --toggle --gpu: the pictures arrive as D3D11 textures, as from a hardware decoder
static double g_filmSeek = 0;          // --seek <seconds>: where to start in it
// The picture is fed as NV12 wherever the hardware video processor has to be able
// to take it; elsewhere RGB32 keeps the film exact.
static bool NV12Source() { return g_bScalers || g_bToggle; }
static bool g_bChroma10 = false;       // --chroma10: 10 bits, P010 against Y410
static bool g_bInterlaced = false;     // --interlaced: the media type says so, to see who converts
static std::vector<BYTE> g_refPicture; // BGRA, top-down, the picture --chroma plays
static bool g_bChromaVerbose = false;  // --verbose: the whole overlay of each run

// The film: soft gradients, a textured band and discs moving at different speeds,
// so Optical Flow and the networks have real work. For --scalers it stands still, in
// NV12, with fine lines of colour and of luma for the scalers to show on.
class CFilmStream : public CSourceStream
{
	const int m_width;
	const int m_height;
	int m_frame = 0;
	std::vector<BYTE> m_rgb;

public:
	CFilmStream(HRESULT* phr, CSource* pParent, int width, int height)
		: CSourceStream(L"Film", phr, pParent, L"Out")
		, m_width(width), m_height(height)
	{}

	HRESULT OnThreadCreate() override
	{
		m_frame = 0; // a run after a stop starts again at stream time 0
		return S_OK;
	}

	HRESULT GetMediaType(CMediaType* pmt) override
	{
		if (g_bChroma) {
			// The colour is said, not guessed: both paths then read the picture the
			// same way and only the chroma upsampling differs.
			auto vih2 = (VIDEOINFOHEADER2*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER2));
			if (!vih2) {
				return E_OUTOFMEMORY;
			}
			ZeroMemory(vih2, sizeof(VIDEOINFOHEADER2));
			vih2->AvgTimePerFrame = g_frameDuration;
			vih2->dwPictAspectRatioX = m_width;
			vih2->dwPictAspectRatioY = m_height;
			DXVA2_ExtendedFormat ex = {};
			ex.SampleFormat           = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT;
			ex.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG2; // left sited
			ex.NominalRange           = DXVA2_NominalRange_16_235;
			ex.VideoTransferMatrix    = DXVA2_VideoTransferMatrix_BT709;
			ex.VideoLighting          = DXVA2_VideoLighting_dim;
			ex.VideoPrimaries         = DXVA2_VideoPrimaries_BT709;
			ex.VideoTransferFunction  = DXVA2_VideoTransFunc_709;
			vih2->dwControlFlags = ex.value;
			if (g_bInterlaced) {
				// Only the flag matters here: it decides which processor takes the picture.
				vih2->dwInterlaceFlags = AMINTERLACE_IsInterlaced | AMINTERLACE_DisplayModeBobOrWeave;
			}
			vih2->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			vih2->bmiHeader.biWidth = m_width;
			vih2->bmiHeader.biHeight = m_height;
			vih2->bmiHeader.biPlanes = 1;
			vih2->bmiHeader.biBitCount = g_bChroma444 ? 32 : g_bChroma10 ? 24 : 12;
			vih2->bmiHeader.biCompression = g_bChroma444
				? (g_bChroma10 ? MAKEFOURCC('Y', '4', '1', '0') : MAKEFOURCC('A', 'Y', 'U', 'V'))
				: (g_bChroma10 ? MAKEFOURCC('P', '0', '1', '0') : MAKEFOURCC('N', 'V', '1', '2'));
			vih2->bmiHeader.biSizeImage = SampleSize();
			SetRect(&vih2->rcSource, 0, 0, m_width, m_height);
			SetRect(&vih2->rcTarget, 0, 0, m_width, m_height);
			const FOURCCMap subtype(vih2->bmiHeader.biCompression);
			pmt->SetType(&MEDIATYPE_Video);
			pmt->SetSubtype(&subtype);
			pmt->SetFormatType(&FORMAT_VideoInfo2);
			pmt->SetTemporalCompression(FALSE);
			pmt->SetSampleSize(vih2->bmiHeader.biSizeImage);
			return S_OK;
		}

		auto vih = (VIDEOINFOHEADER*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER));
		if (!vih) {
			return E_OUTOFMEMORY;
		}
		ZeroMemory(vih, sizeof(VIDEOINFOHEADER));
		vih->AvgTimePerFrame = g_frameDuration;
		vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		vih->bmiHeader.biWidth = m_width;
		vih->bmiHeader.biHeight = m_height;
		vih->bmiHeader.biPlanes = 1;
		const bool bNV12 = NV12Source();
		vih->bmiHeader.biBitCount = bNV12 ? 12 : 32;
		vih->bmiHeader.biCompression = bNV12 ? MAKEFOURCC('N', 'V', '1', '2') : BI_RGB;
		vih->bmiHeader.biSizeImage = SampleSize();
		SetRect(&vih->rcSource, 0, 0, m_width, m_height);
		SetRect(&vih->rcTarget, 0, 0, m_width, m_height);
		pmt->SetType(&MEDIATYPE_Video);
		pmt->SetSubtype(bNV12 ? &MEDIASUBTYPE_NV12 : &MEDIASUBTYPE_RGB32);
		pmt->SetFormatType(&FORMAT_VideoInfo);
		pmt->SetTemporalCompression(FALSE);
		pmt->SetSampleSize(vih->bmiHeader.biSizeImage);
		return S_OK;
	}

	long SampleSize() const
	{
		if (g_bChroma) {
			return g_bChroma444 ? m_width * m_height * 4
				: g_bChroma10 ? m_width * m_height * 3 : m_width * m_height * 3 / 2;
		}
		return NV12Source() ? m_width * m_height * 3 / 2 : m_width * m_height * 4;
	}

	HRESULT DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pProps) override
	{
		CAutoLock lock(m_pFilter->pStateLock());
		pProps->cBuffers = std::max(pProps->cBuffers, 1L);
		pProps->cbBuffer = std::max(pProps->cbBuffer, SampleSize());
		ALLOCATOR_PROPERTIES actual = {};
		const HRESULT hr = pAlloc->SetProperties(pProps, &actual);
		if (FAILED(hr)) {
			return hr;
		}
		return actual.cbBuffer < pProps->cbBuffer ? E_FAIL : S_OK;
	}

	// BT.709, limited range, from a BGRA pixel.
	static void ToYCbCr(const BYTE* bgra, double& y, double& cb, double& cr)
	{
		const double r = bgra[2] / 255.0, g = bgra[1] / 255.0, b = bgra[0] / 255.0;
		const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		y  = 16.0 + 219.0 * luma;
		cb = 128.0 + 224.0 * (b - luma) / 1.8556;
		cr = 128.0 + 224.0 * (r - luma) / 1.5748;
	}

	// The picture as 4:4:4 (AYUV: V, U, Y, A) or as 4:2:0 (NV12) with the chroma
	// sited to the left, which is what the media type says and what video uses:
	// [1 2 1] / 4 across, centred on the even column, and the two rows averaged.
	// The same picture in ten bits: Y410 packs U, Y, V and two bits of alpha into a
	// word, P010 keeps the ten bits in the high bits of each 16-bit sample.
	void WriteChromaSample10(BYTE* p) const
	{
		const int w = m_width, h = m_height;
		const auto At = [&](int x, int y) { return &g_refPicture[((size_t)std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)) * 4]; };
		const auto Q = [](double v) { return (UINT)std::lround(std::clamp(v * 4.0, 0.0, 1023.0)); };
		if (g_bChroma444) {
			UINT* d = (UINT*)p;
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					double luma, cb, cr;
					ToYCbCr(At(x, y), luma, cb, cr);
					d[(size_t)y * w + x] = Q(cb) | (Q(luma) << 10) | (Q(cr) << 20) | (3u << 30);
				}
			}
			return;
		}
		UINT16* d = (UINT16*)p;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				double luma, cb, cr;
				ToYCbCr(At(x, y), luma, cb, cr);
				d[(size_t)y * w + x] = (UINT16)(Q(luma) << 6);
			}
		}
		UINT16* uv = d + (size_t)w * h;
		for (int y = 0; y < h / 2; y++) {
			for (int x = 0; x < w / 2; x++) {
				double cb = 0, cr = 0;
				for (int j = 0; j < 2; j++) {
					static const double weights[3] = { 0.25, 0.5, 0.25 };
					for (int i = -1; i <= 1; i++) {
						double luma, b, r;
						ToYCbCr(At(2 * x + i, 2 * y + j), luma, b, r);
						cb += weights[i + 1] * b;
						cr += weights[i + 1] * r;
					}
				}
				uv[(size_t)y * w + 2 * x]     = (UINT16)(Q(cb / 2) << 6);
				uv[(size_t)y * w + 2 * x + 1] = (UINT16)(Q(cr / 2) << 6);
			}
		}
	}

	void WriteChromaSample(BYTE* p) const
	{
		if (g_bChroma10) {
			WriteChromaSample10(p);
			return;
		}
		const int w = m_width, h = m_height;
		const auto At = [&](int x, int y) { return &g_refPicture[((size_t)std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)) * 4]; };
		if (g_bChroma444) {
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					double luma, cb, cr;
					ToYCbCr(At(x, y), luma, cb, cr);
					BYTE* d = p + ((size_t)y * w + x) * 4;
					d[0] = (BYTE)std::lround(std::clamp(cr, 0.0, 255.0));
					d[1] = (BYTE)std::lround(std::clamp(cb, 0.0, 255.0));
					d[2] = (BYTE)std::lround(std::clamp(luma, 0.0, 255.0));
					d[3] = 255;
				}
			}
			return;
		}
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				double luma, cb, cr;
				ToYCbCr(At(x, y), luma, cb, cr);
				p[(size_t)y * w + x] = (BYTE)std::lround(std::clamp(luma, 0.0, 255.0));
			}
		}
		BYTE* uv = p + (size_t)w * h;
		for (int y = 0; y < h / 2; y++) {
			for (int x = 0; x < w / 2; x++) {
				double cb = 0, cr = 0;
				for (int j = 0; j < 2; j++) {
					static const double weights[3] = { 0.25, 0.5, 0.25 };
					for (int i = -1; i <= 1; i++) {
						double luma, b, r;
						ToYCbCr(At(2 * x + i, 2 * y + j), luma, b, r);
						cb += weights[i + 1] * b;
						cr += weights[i + 1] * r;
					}
				}
				uv[(size_t)y * w + 2 * x]     = (BYTE)std::lround(std::clamp(cb / 2, 0.0, 255.0));
				uv[(size_t)y * w + 2 * x + 1] = (BYTE)std::lround(std::clamp(cr / 2, 0.0, 255.0));
			}
		}
	}

	HRESULT FillBuffer(IMediaSample* pSample) override
	{
		BYTE* p = nullptr;
		if (FAILED(pSample->GetPointer(&p)) || pSample->GetSize() < SampleSize()) {
			return E_FAIL;
		}
		if (g_bChroma) {
			WriteChromaSample(p);
			REFERENCE_TIME start = m_frame * g_frameDuration;
			REFERENCE_TIME end = start + g_frameDuration;
			pSample->SetTime(&start, &end);
			pSample->SetSyncPoint(TRUE);
			pSample->SetActualDataLength(SampleSize());
			m_frame++;
			return S_OK;
		}
		const int still = g_bScalers ? 0 : m_frame;
		const double t = still;
		const double cx1 = m_width * (0.5 + 0.35 * std::sin(t * 0.05)), cy1 = m_height * (0.5 + 0.3 * std::cos(t * 0.04));
		const double cx2 = std::fmod(t * 6.0, m_width + 200.0) - 100.0, cy2 = m_height * 0.7;
		const double r1 = m_height * 0.12, r2 = m_height * 0.08;
		m_rgb.resize((size_t)m_width * m_height * 4);
		for (int y = 0; y < m_height; y++) {
			BYTE* row = m_rgb.data() + (size_t)y * m_width * 4; // top-down
			for (int x = 0; x < m_width; x++) {
				double r = 60 + 80.0 * x / m_width;
				double g = 50 + 90.0 * y / m_height;
				double b = 90 + 40.0 * std::sin((x + 2.0 * t) * 0.02);
				if (y > m_height / 3 && y < m_height / 2) { // a textured band panning left
					const int u = x + 3 * still;
					const double tex = ((u * 37 ^ y * 91) & 63) - 32;
					r += tex; g += tex; b += tex;
				}
				if ((x - cx1) * (x - cx1) + (y - cy1) * (y - cy1) < r1 * r1) {
					r = 220; g = 180; b = 140;
				}
				if ((x - cx2) * (x - cx2) + (y - cy2) * (y - cy2) < r2 * r2) {
					r = 40; g = 70; b = 200;
				}
				if (g_bScalers && y > m_height * 3 / 4) {
					// Lines 1 to 4 pixels wide: red on blue on the left, then grey
					// diagonals, for chroma and luma edges.
					if (x < m_width / 2) {
						const int width = 1 + x * 8 / m_width;
						const bool on = (x % (2 * width)) < width;
						r = on ? 210 : 30; g = on ? 40 : 50; b = on ? 40 : 200;
					} else {
						const bool on = ((x + y) % 7) < 2 || ((x - y + 7000) % 11) < 1;
						r = g = b = on ? 230 : 25;
					}
				}
				row[x * 4 + 0] = (BYTE)std::clamp(b, 0.0, 255.0);
				row[x * 4 + 1] = (BYTE)std::clamp(g, 0.0, 255.0);
				row[x * 4 + 2] = (BYTE)std::clamp(r, 0.0, 255.0);
				row[x * 4 + 3] = 255;
			}
		}
		if (NV12Source()) {
			// BT.601 limited range, what the renderer assumes for a picture of this
			// size, chroma averaged over 2x2 blocks.
			for (int y = 0; y < m_height; y++) {
				for (int x = 0; x < m_width; x++) {
					const BYTE* s = &m_rgb[((size_t)y * m_width + x) * 4];
					p[(size_t)y * m_width + x] = (BYTE)std::lround(16.0 + (65.481 * s[2] + 128.553 * s[1] + 24.966 * s[0]) / 255.0);
				}
			}
			BYTE* uv = p + (size_t)m_width * m_height;
			for (int y = 0; y < m_height / 2; y++) {
				for (int x = 0; x < m_width / 2; x++) {
					double cb = 0, cr = 0;
					for (int j = 0; j < 2; j++) {
						for (int i = 0; i < 2; i++) {
							const BYTE* s = &m_rgb[((size_t)(2 * y + j) * m_width + 2 * x + i) * 4];
							cb += 128.0 + (-37.797 * s[2] - 74.203 * s[1] + 112.0 * s[0]) / 255.0;
							cr += 128.0 + (112.0 * s[2] - 93.786 * s[1] - 18.214 * s[0]) / 255.0;
						}
					}
					uv[(size_t)y * m_width + 2 * x] = (BYTE)std::lround(cb / 4);
					uv[(size_t)y * m_width + 2 * x + 1] = (BYTE)std::lround(cr / 4);
				}
			}
		} else {
			for (int y = 0; y < m_height; y++) {
				memcpy(p + (size_t)(m_height - 1 - y) * m_width * 4, &m_rgb[(size_t)y * m_width * 4], (size_t)m_width * 4); // bottom-up
			}
		}
		REFERENCE_TIME start = m_frame * g_frameDuration;
		REFERENCE_TIME end = start + g_frameDuration;
		pSample->SetTime(&start, &end);
		pSample->SetSyncPoint(TRUE);
		pSample->SetActualDataLength(SampleSize());
		m_frame++;
		return S_OK;
	}
};

class CFilmSource : public CSource
{
public:
	CFilmSource(HRESULT* phr, int width, int height)
		: CSource(L"Film source", nullptr, GUID_NULL, phr)
	{
		new CFilmStream(phr, this, width, height); // the pin registers itself with the filter
	}
};

static CComPtr<IPin> GetPin(IBaseFilter* pFilter, PIN_DIRECTION dir)
{
	CComPtr<IEnumPins> pEnum;
	if (SUCCEEDED(pFilter->EnumPins(&pEnum))) {
		for (CComPtr<IPin> pPin; pEnum->Next(1, &pPin, nullptr) == S_OK; pPin.Release()) {
			PIN_DIRECTION d;
			if (SUCCEEDED(pPin->QueryDirection(&d)) && d == dir) {
				return pPin;
			}
		}
	}
	return nullptr;
}

static void Pump(DWORD ms)
{
	const ULONGLONG end = GetTickCount64() + ms;
	for (;;) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		const ULONGLONG now = GetTickCount64();
		if (now >= end) {
			return;
		}
		MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)std::min<ULONGLONG>(end - now, 10), QS_ALLINPUT);
	}
}

static std::wstring StatsText(IBaseFilter* pRenderer)
{
	std::wstring text;
	if (CComQIPtr<IExFilterConfig> pConfig{ pRenderer }) {
		LPWSTR pstr = nullptr;
		if (S_OK == pConfig->Flt_GetString("statsText", &pstr, nullptr) && pstr) {
			text = pstr;
			CoTaskMemFree(pstr);
		}
	}
	return text;
}

// The rest of the line after a label such as L"Sync offset   : ".
static std::wstring StatsLine(const std::wstring& text, const wchar_t* label)
{
	const size_t pos = text.find(label);
	if (pos == std::wstring::npos) {
		return {};
	}
	const size_t start = pos + wcslen(label);
	const size_t end = text.find(L'\n', start);
	return text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

#include "screen_watch.inl"

struct Config {
	const char* name;
	bool bNR;
	bool bSR;
	bool bAhead;
	int iUpscaling = -1;     // UPSCALE_*, -1: the saved setting
	int iChromaScaling = -1; // CHROMA_*, -1: the saved setting
	bool bHardwareVP = false; // --chroma: this run goes through the hardware processor
	bool b444 = false;        // --chroma: this run feeds 4:4:4, the reference
	bool bReplaceChroma = false; // --chroma: the processor keeps the picture, the shaders its chroma
	bool bHdrOut = false;        // --chroma: HDR passthrough, for the RTX Video HDR runs
	int iRtxHdr = -1;            // and the extension: -1 leaves the setting, 0 off, 1 on
};

struct Result {
	bool bRan = false;
	std::wstring screenLine;   // what the desktop itself held, read past the renderer
	std::vector<BYTE> screenCrop;  // and the picture that reading was taken from
	std::string screenWhere;       // where on the desktop it was taken
	std::vector<BYTE> windowShot;  // the window with everything the renderer drew on it
	int windowShotW = 0, windowShotH = 0;
	int screenCropW = 0, screenCropH = 0;
	std::vector<int> syncs;
	int skippedAtStart = -1;
	int skippedAtEnd = -1;
	int framesAtStart = 0;
	int framesAtEnd = 0;
	int lateAtStart = 0;
	int lateAtEnd = 0;
	std::wstring nrLine, srLine, dlssTimes, aheadLine;
	std::wstring vprocLine, scalingLine, prescaleTimes;
	std::wstring statsAll; // --chroma: the whole overlay, to see what else differs
	std::vector<BYTE> picture;      // BGRA, top-down, as displayed
	std::vector<BYTE> statsPicture; // the same with the statistics drawn over it
	int pictureW = 0, pictureH = 0;
	double stopSeconds[3] = {};
	std::string error;
};

// The back buffer as the renderer hands it out (IExFilterConfig "displayedImage").
static bool GrabDisplayed(IBaseFilter* pRenderer, std::vector<BYTE>& bgra, int& w, int& h)
{
	CComQIPtr<IExFilterConfig> pConfig{ pRenderer };
	LPVOID data = nullptr;
	unsigned size = 0;
	if (!pConfig || S_OK != pConfig->Flt_GetBin("displayedImage", &data, &size) || !data) {
		return false;
	}
	const auto bih = (const BITMAPINFOHEADER*)data;
	const bool ok = bih->biBitCount == 32 && size >= sizeof(BITMAPINFOHEADER) + (size_t)bih->biWidth * std::abs(bih->biHeight) * 4;
	if (ok) {
		w = bih->biWidth;
		h = std::abs(bih->biHeight);
		const BYTE* bits = (const BYTE*)(bih + 1);
		bgra.assign(bits, bits + (size_t)w * h * 4);
	}
	LocalFree(data);
	return ok;
}

// The picture --chroma plays: the middle w x h of the file, so the renderer shows
// it at its own size and no resize shader takes part in the measurement.
static bool LoadPicture(const wchar_t* path, int w, int h, std::string& error)
{
	CComPtr<IWICImagingFactory> factory;
	HRESULT hr = factory.CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER);
	CComPtr<IWICBitmapDecoder> decoder;
	if (SUCCEEDED(hr)) {
		hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
	}
	CComPtr<IWICBitmapFrameDecode> frame;
	if (SUCCEEDED(hr)) {
		hr = decoder->GetFrame(0, &frame);
	}
	UINT sw = 0, sh = 0;
	if (SUCCEEDED(hr)) {
		hr = frame->GetSize(&sw, &sh);
	}
	if (FAILED(hr)) {
		error = std::format("cannot decode the picture (0x{:08X})", (unsigned)hr);
		return false;
	}
	// A 4K frame is reduced by two, which is what a careful encoder does and what
	// the harness measures on: the middle of the picture cropped at that size would
	// keep chroma far smoother than real video of this size ever is.
	const int factor = ((int)sw >= 2 * w && (int)sh >= 2 * h) ? 2 : 1;
	if ((int)sw < factor * w || (int)sh < factor * h) {
		error = std::format("the picture is {}x{}, smaller than the {}x{} asked for", sw, sh, w, h);
		return false;
	}
	CComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	if (SUCCEEDED(hr)) {
		hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	}
	std::vector<BYTE> taken(4 * (size_t)factor * w * factor * h);
	const WICRect rect = { (INT)(sw - factor * w) / 2, (INT)(sh - factor * h) / 2, factor * w, factor * h };
	if (SUCCEEDED(hr)) {
		hr = converter->CopyPixels(&rect, 4 * (UINT)factor * w, (UINT)taken.size(), taken.data());
	}
	if (FAILED(hr)) {
		error = std::format("cannot read the pixels (0x{:08X})", (unsigned)hr);
		return false;
	}
	g_refPicture.assign(4 * (size_t)w * h, 0);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			for (int k = 0; k < 4; k++) {
				int acc = 0;
				for (int j = 0; j < factor; j++) {
					for (int i = 0; i < factor; i++) {
						acc += taken[4 * ((size_t)(factor * y + j) * factor * w + factor * x + i) + k];
					}
				}
				g_refPicture[4 * ((size_t)y * w + x) + k] = (BYTE)((acc + factor * factor / 2) / (factor * factor));
			}
		}
	}
	printf("picture: %dx%d, reduced by %d from the file\n", w, h, factor);
	return true;
}

static bool PrintWindowToBgra(HWND hwnd, std::vector<BYTE>& bgra, int& w, int& h)
{
	RECT rc = {};
	GetClientRect(hwnd, &rc);
	w = rc.right;
	h = rc.bottom;
	if (w < 8 || h < 8) {
		return false;
	}
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	bool ok = false;
	if (bmp && bits) {
		HGDIOBJ old = SelectObject(mem, bmp);
		ok = PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */) != FALSE;
		if (ok) {
			bgra.assign((const BYTE*)bits, (const BYTE*)bits + (size_t)w * h * 4);
		}
		SelectObject(mem, old);
	}
	if (bmp) {
		DeleteObject(bmp);
	}
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return ok;
}

static bool SaveBmp(const std::wstring& path, const std::vector<BYTE>& bgra, int w, int h)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f) {
		return false;
	}
	BITMAPFILEHEADER bfh = {};
	BITMAPINFOHEADER bih = {};
	bih.biSize = sizeof(bih);
	bih.biWidth = w;
	bih.biHeight = -h; // top-down
	bih.biPlanes = 1;
	bih.biBitCount = 32;
	bih.biSizeImage = (DWORD)bgra.size();
	bfh.bfType = 0x4D42;
	bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
	bfh.bfSize = bfh.bfOffBits + bih.biSizeImage;
	fwrite(&bfh, sizeof(bfh), 1, f);
	fwrite(&bih, sizeof(bih), 1, f);
	fwrite(bgra.data(), 1, bgra.size(), f);
	fclose(f);
	return true;
}

static bool ParseSkipped(const std::wstring& text, int& frames, int& skipped)
{
	const std::wstring line = StatsLine(text, L"Frames        : ");
	int dropped2 = 0, failed = 0;
	return !line.empty() && swscanf_s(line.c_str(), L"%d, skipped: %d/%d, failed: %d", &frames, &skipped, &dropped2, &failed) >= 2;
}

// The late count at the end of the Render ahead line; 0 when there is none.
static int ParseLate(const std::wstring& text)
{
	const std::wstring line = StatsLine(text, L"Render ahead  : ");
	const size_t pos = line.rfind(L"late ");
	return pos == std::wstring::npos ? 0 : _wtoi(line.c_str() + pos + 5);
}

#include "file_graph.inl"

static Result RunConfig(HMODULE hFilter, HWND hwnd, const Config& config, SIZE source, SIZE window, int seconds)
{
	Result result;
	HRESULT hr = S_OK;

	CComPtr<IFilterGraph2> pGraph;
	if (FAILED(pGraph.CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER))) {
		result.error = "no filter graph";
		return result;
	}

	auto pSourceFilter = new CFilmSource(&hr, source.cx, source.cy);
	CComPtr<IBaseFilter> pSource = pSourceFilter;

	using PFN_DllGetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
	const auto pfnGetClassObject = (PFN_DllGetClassObject)GetProcAddress(hFilter, "DllGetClassObject");
	CComPtr<IClassFactory> pFactory;
	CComPtr<IBaseFilter> pRenderer;
	if (!pfnGetClassObject || FAILED(pfnGetClassObject(CLSID_MpcVideoRenderer, IID_IClassFactory, (LPVOID*)&pFactory))
			|| FAILED(pFactory->CreateInstance(nullptr, IID_IBaseFilter, (void**)&pRenderer))) {
		result.error = "the renderer could not be created";
		return result;
	}

	// This run's settings, handed over without saving them.
	CComQIPtr<IVideoRenderer> pVR(pRenderer.p);
	Settings_t sets;
	if (g_bAsIs) {
		// Nothing is written: what plays is what the registry says, for either filter.
		pVR->GetSettings(sets);
	} else {
	pVR->GetSettings(sets);
	sets.bUseD3D11 = true;
	sets.bShowStats = true;
	sets.bExclusiveFS = false;
	// The test never touches the screen: with a Windows 10 manifest the filter is
	// allowed to switch the display's HDR state itself, which is not what is measured
	// here and leaves the desktop changed behind it.
	sets.bHdrPassthrough = false;
	sets.iHdrToggleDisplay = HDRTD_Disabled;
	sets.bDlssNR = config.bNR;
	sets.bDlssSR = config.bSR;
	sets.bDlssRenderAhead = config.bAhead;
	if (config.iUpscaling >= 0) {
		sets.iUpscaling = config.iUpscaling;
	}
	if (config.iChromaScaling >= 0) {
		sets.iChromaScaling = config.iChromaScaling;
	}
	if (g_bChroma) {
		// Nothing but the chroma upsampling changes between the runs: the picture is
		// shown at its own size, the dither would add noise of its own, and the
		// formats decide whether the hardware processor converts or the shaders do.
		sets.VPFmts = { config.bHardwareVP, config.bHardwareVP, config.bHardwareVP, config.bHardwareVP };
		sets.bVPReplaceChroma = config.bReplaceChroma;
		sets.bVPScaling = false;
		sets.iTexFormat = TEXFMT_AUTOINT;
		sets.bUseDither = false;
		// The RTX Video HDR runs ask for HDR output. The desktop has to be in HDR
		// already for that to mean anything -- the filter is still not allowed to
		// switch it, iHdrToggleDisplay stays Disabled just above.
		if (config.bHdrOut) {
			sets.bHdrPassthrough = true;
		}
		if (config.iRtxHdr >= 0) {
			sets.bVPRTXVideoHDR = config.iRtxHdr != 0;
		}
	}
	if (g_filmFile) {
		// The film runs with the hardware processor taking every format, since that is
		// the path both extensions live on, and with whatever the command line asked
		// for. Only what was asked for is touched.
		sets.VPFmts = { true, true, true, true };
		if (g_filmVPScale >= 0) {
			sets.bVPScaling = g_filmVPScale != 0;
		}
		if (g_filmHdrOut >= 0) {
			sets.bHdrPassthrough = g_filmHdrOut != 0;
		}
		if (g_filmRtxHdr >= 0) {
			sets.bVPRTXVideoHDR = g_filmRtxHdr != 0;
		}
		if (g_filmSuperRes >= 0) {
			sets.iVPSuperRes = g_filmSuperRes;
		}
		if (g_filmPrepass >= 0) {
			sets.bVPReplaceChroma = g_filmPrepass != 0;
		}
		if (g_filmStats >= 0) {
			sets.bShowStats = g_filmStats != 0;
		}
	}
	if (g_bScalers && !g_bHardwareVP) {
		// The shader video processor converts and scales, not the hardware one.
		sets.VPFmts = { false, false, false, false };
		sets.bVPScaling = false;
		sets.iTexFormat = TEXFMT_AUTOINT;
	}
	pVR->SetSettings(sets);
	if (g_filmFile) {
		// Read back what the filter kept. A build the bench did not compile against
		// shares the interface but not necessarily the structure behind it, and this
		// is the one place that would show it.
		Settings_t back;
		pVR->GetSettings(back);
		printf("   settings kept: stats %d, rtxhdr %d, superres %d, prepass %d, hdrout %d, vpscale %d\n",
			(int)back.bShowStats, (int)back.bVPRTXVideoHDR, back.iVPSuperRes,
			(int)back.bVPReplaceChroma, (int)back.bHdrPassthrough, (int)back.bVPScaling);
	}
	}

	if (g_filmFile) {
		// The real file, split and decoded as the player does it: that is the only way
		// a decoded video surface ever reaches the processor.
		const std::string bad = BuildFileGraph(pGraph, g_filmFile, pRenderer);
		if (!bad.empty()) {
			result.error = bad;
			return result;
		}
	} else {
		pGraph->AddFilter(pSource, L"Film source");
		pGraph->AddFilter(pRenderer, L"MPC Video Renderer");
		hr = pGraph->ConnectDirect(GetPin(pSource, PINDIR_OUTPUT), GetPin(pRenderer, PINDIR_INPUT), nullptr);
		if (FAILED(hr)) {
			result.error = "connection failed";
			return result;
		}
	}

	CComQIPtr<IVideoWindow> pVW(pRenderer.p);
	CComQIPtr<IBasicVideo> pBV(pRenderer.p);
	pVW->put_Owner((OAHWND)hwnd);
	pVW->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
	pVW->SetWindowPosition(0, 0, window.cx, window.cy);
	pBV->SetDestinationPosition(0, 0, window.cx, window.cy);

	CComQIPtr<IMediaControl> pMC(pGraph.p);
	if (g_filmFile && g_filmSeek > 0) {
		SeekTo(pGraph, g_filmSeek);
	}
	pMC->Run();
	result.bRan = true;

	// Sessions, features and Optical Flow settle first.
	Pump(5000);
	// The desktop as Windows composes it, which is the only place the video
	// processor's own extensions can be seen at all.
	CScreenWatch watch;
	if (g_filmFile) {
		const std::string bad = watch.Start(hwnd);
		if (!bad.empty()) {
			printf("   screen watch: %s\n", bad.c_str());
		} else {
			printf("   screen watch: half floats asked for, 0x%08X\n", (unsigned)watch.HdrAttempt());
		}
	}

	std::wstring text = StatsText(pRenderer);
	ParseSkipped(text, result.framesAtStart, result.skippedAtStart);
	result.lateAtStart = ParseLate(text);

	for (int i = 0; i < seconds * 10; i++) {
		Pump(100);
		text = StatsText(pRenderer);
		const std::wstring sync = StatsLine(text, L"Sync offset   : ");
		int value = 0;
		if (!sync.empty() && swscanf_s(sync.c_str(), L"%d", &value) == 1) {
			result.syncs.push_back(value);
		}
	}
	if (g_filmFile && watch.Ready()) {
		// The desktop shows whatever is in front: a window left behind another one
		// is not what the duplication composes, and the reading would be of the
		// wrong picture entirely.
		SetForegroundWindow(hwnd);
		BringWindowToTop(hwnd);
		Pump(600);
		const CScreenWatch::Reading s = watch.Measure(hwnd);
		const RECT w = watch.LastRect();
		result.screenWhere = std::format("{},{} to {},{}", w.left, w.top, w.right, w.bottom);
		// And the window as the window manager draws it, which carries whatever the
		// renderer wrote over its own picture -- the only thing a build without
		// statsText will tell the bench.
		PrintWindowToBgra(hwnd, result.windowShot, result.windowShotW, result.windowShotH);
		result.screenCrop = s.crop;
		result.screenCropW = s.cropW;
		result.screenCropH = s.cropH;
		result.screenLine = s.ok
			? std::format(L"{}, mean {:.4f}, p99 {:.4f}, peak {:.3f}, colour {:.4f}, past SDR white {:.2f}%, detail {:.5f}",
				watch.Format() == DXGI_FORMAT_R16G16B16A16_FLOAT ? L"HDR desktop (scRGB half floats)" : L"SDR desktop (8-bit)",
				s.mean, s.p99, s.peak, s.colour, s.aboveWhite, s.detail)
			: L"could not be read";
	}

	ParseSkipped(text, result.framesAtEnd, result.skippedAtEnd);
	result.lateAtEnd = ParseLate(text);
	result.nrLine = StatsLine(text, L"DLSS 5 NR     : ");
	result.srLine = StatsLine(text, L"DLSS SR       : ");
	result.dlssTimes = StatsLine(text, L"DLSS (ms)     : ");
	result.aheadLine = StatsLine(text, L"Render ahead  : ");
	result.vprocLine = StatsLine(text, L"VideoProcessor: ");
	result.statsAll = text;
	result.scalingLine = StatsLine(text, L"Scaling       : ");
	result.prescaleTimes = StatsLine(text, L"Prescale (ms) : ");
	if (g_filmFile) {
		// Two renderers can only be compared on the same picture, and playback never
		// stops on the same frame twice. Paused and seeked again, the frame shown is
		// the one at that position and nothing else.
		pMC->Pause();
		Pump(500);
		SeekTo(pGraph, g_filmSeek);
		Pump(2500);
	}
	if (g_bScalers || g_bChroma || g_filmFile) {
		// One picture with the statistics over it, for the look of the box.
		GrabDisplayed(pRenderer, result.statsPicture, result.pictureW, result.pictureH);
		// Then one without, which is what the configurations are compared on. The
		// back buffer of a flip-discard swap chain is not always the picture just
		// presented, so the same picture has to come back twice before it is taken.
		// With --asis nothing may be written to the filter, so whether the statistics
		// are drawn is the registry's business and both pictures are the same one.
		if (!g_bAsIs) {
			sets.bShowStats = false;
			pVR->SetSettings(sets);
		}
		Pump(800);
		std::vector<BYTE> previous;
		for (int attempt = 0; attempt < 8; attempt++) {
			GrabDisplayed(pRenderer, result.picture, result.pictureW, result.pictureH);
			if (!result.picture.empty() && result.picture == previous) {
				break;
			}
			previous = result.picture;
			Pump(250);
		}
	}

	// State changes while pictures are being held.
	const auto TimeCall = [&](auto&& call) {
		const ULONGLONG t0 = GetTickCount64();
		call();
		return (GetTickCount64() - t0) / 1000.0;
	};
	result.stopSeconds[0] = TimeCall([&] { pMC->Pause(); });
	Pump(700);
	result.stopSeconds[1] = TimeCall([&] { pMC->Run(); });
	Pump(1500);
	result.stopSeconds[2] = TimeCall([&] { pMC->Stop(); });
	Pump(200);

	pVW->put_Visible(OAFALSE);
	pVW->put_Owner(0);
	pGraph->RemoveFilter(pRenderer);
	pGraph->RemoveFilter(pSource);
	return result;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The smallest site a property page accepts, to show one without registering the filter.
class CPageSite : public IPropertyPageSite
{
public:
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (riid == IID_IUnknown || riid == IID_IPropertyPageSite) {
			*ppv = static_cast<IPropertyPageSite*>(this);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // lives on the stack
	STDMETHODIMP_(ULONG) Release() override { return 1; }
	STDMETHODIMP OnStatusChange(DWORD) override { return S_OK; }
	STDMETHODIMP GetLocaleID(LCID* pLocaleID) override { *pLocaleID = LOCALE_USER_DEFAULT; return S_OK; }
	STDMETHODIMP GetPageContainer(IUnknown** ppUnk) override { *ppUnk = nullptr; return E_NOTIMPL; }
	STDMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
};

// What a window looks like, through PrintWindow: a property page is then checked from
// its picture rather than by eye.
static bool CaptureWindow(HWND hwnd, std::vector<BYTE>& bgra, int& w, int& h)
{
	RECT rc = {};
	if (!GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) {
		return false;
	}
	w = rc.right;
	h = rc.bottom;

	const HDC hdcWindow = GetDC(hwnd);
	const HDC hdc = CreateCompatibleDC(hdcWindow);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h; // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	const HBITMAP hbm = CreateDIBSection(hdcWindow, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	bool ok = false;
	if (hbm && bits) {
		const HGDIOBJ prev = SelectObject(hdc, hbm);
		ok = PrintWindow(hwnd, hdc, PW_CLIENTONLY) != FALSE;
		GdiFlush();
		if (ok) {
			bgra.assign((const BYTE*)bits, (const BYTE*)bits + (size_t)w * h * 4);
		}
		SelectObject(hdc, prev);
	}
	if (hbm) {
		DeleteObject(hbm);
	}
	DeleteDC(hdc);
	ReleaseDC(hwnd, hdcWindow);
	return ok;
}

// --dlsspage N, --mainpage N: one of the filter's property pages in the window for N
// seconds, to look at -- the greying and the layout without going through a player.
// Which of the controls that grey each other are live, and how the two scaling lists
// were built. A picture of the page says nothing about either, and a control that is
// accepted but does nothing is exactly the fault worth catching.
static void DumpPageState(HWND hwnd, const char* when)
{
    const HWND hDlg = GetWindow(hwnd, GW_CHILD);
    if (!hDlg) {
        return;
    }
    printf("\n-- the page %s\n", when);
    for (const auto& c : { std::pair{ 1025, "Use for resizing" },
                           std::pair{ 1048, "Request Super Resolution" },
                           std::pair{ 1249, "Replace VP chroma upsampling" },
                           std::pair{ 1045, "Chroma upsampling list" },
                           std::pair{ 1042, "Upscaling list" },
                           std::pair{ 1043, "Downscaling list" },
                           std::pair{ 1251, "Sharpening list" },
                           std::pair{ 1253, "Sharpening intensity" } }) {
        const HWND h = GetDlgItem(hDlg, c.first);
        if (!h) {
            printf("  %-30s (not on this page)\n", c.second);
            continue;
        }
        const bool bCheck = (SendMessageW(h, WM_GETDLGCODE, 0, 0) & DLGC_BUTTON) != 0;
        printf("  %-30s %-7s%s\n", c.second, IsWindowEnabled(h) ? "live" : "GREYED",
            bCheck ? (IsDlgButtonChecked(hDlg, c.first) == BST_CHECKED ? ", ticked" : ", unticked") : "");
    }
    for (const auto& list : { std::pair{ 1045, "Chroma upsampling" }, std::pair{ 1042, "Upscaling" } }) {
        const HWND hCombo = GetDlgItem(hDlg, list.first);
        if (!hCombo) {
            continue;
        }
        const LRESULT count = SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
        const LRESULT current = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        printf("%s, best first:\n", list.second);
        for (LRESULT i = 0; i < count; i++) {
            wchar_t text[64] = {};
            SendMessageW(hCombo, CB_GETLBTEXT, i, (LPARAM)text);
            wprintf(L"  %2d  %-20s value %2d%s\n", (int)i, text,
                (int)SendMessageW(hCombo, CB_GETITEMDATA, i, 0), i == current ? L"   <- selected" : L"");
        }
    }
    fflush(stdout);
}

// Its picture is saved next to the program as proppage.bmp.
static int ShowPropertyPage(HMODULE hFilter, HWND hwnd, int seconds, REFCLSID clsidPage,
	const std::vector<int>& clicks = {})
{
	using PFN_DllGetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
	const auto pfnGetClassObject = (PFN_DllGetClassObject)GetProcAddress(hFilter, "DllGetClassObject");
	CComPtr<IClassFactory> pRendererFactory, pPageFactory;
	CComPtr<IBaseFilter> pRenderer;
	CComPtr<IPropertyPage> pPage;
	if (!pfnGetClassObject
			|| FAILED(pfnGetClassObject(CLSID_MpcVideoRenderer, IID_IClassFactory, (LPVOID*)&pRendererFactory))
			|| FAILED(pRendererFactory->CreateInstance(nullptr, IID_IBaseFilter, (void**)&pRenderer))
			|| FAILED(pfnGetClassObject(clsidPage, IID_IClassFactory, (LPVOID*)&pPageFactory))
			|| FAILED(pPageFactory->CreateInstance(nullptr, IID_IPropertyPage, (void**)&pPage))) {
		printf("the property page could not be created\n");
		return 1;
	}
	CPageSite site;
	pPage->SetPageSite(&site);
	IUnknown* objects[] = { pRenderer.p };
	pPage->SetObjects(1, objects);
	PROPPAGEINFO info = { sizeof(info) };
	pPage->GetPageInfo(&info);
	CoTaskMemFree(info.pszTitle);
	CoTaskMemFree(info.pszDocString);
	CoTaskMemFree(info.pszHelpFile);

	RECT rc = { 0, 0, info.size.cx, info.size.cy };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	SetWindowPos(hwnd, nullptr, 20, 20, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
	RECT page = { 0, 0, info.size.cx, info.size.cy };
	pPage->Activate(hwnd, &page, FALSE);
	pPage->Show(SW_SHOW);
	printf("property page %ldx%ld shown for %d s\n", info.size.cx, info.size.cy, seconds);
	fflush(stdout);
	Pump(700);
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring dir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\'));
	const auto Shoot = [&](const wchar_t* name) {
		std::vector<BYTE> shot;
		int shotW = 0, shotH = 0;
		if (CaptureWindow(hwnd, shot, shotW, shotH)) {
			const std::wstring path = dir + L"\\" + name;
			SaveBmp(path, shot, shotW, shotH);
			wprintf(L"picture: %s\n", path.c_str());
		}
	};
	Shoot(L"proppage.bmp");

	DumpPageState(hwnd, "as it opens");

	// Tick a box and take the page again: how the greying answers, without a player.
	// Several boxes in turn, since a greying rule usually takes more than one of them
	// to show itself: --click 1025 --click 1249 ticks the resizing box, then the chroma
	// one, and the page is read out after each.
	for (const int clickId : clicks) {
		const HWND hDlg = GetWindow(hwnd, GW_CHILD);
		const HWND hControl = hDlg ? GetDlgItem(hDlg, clickId) : nullptr;
		if (hControl) {
			SendMessageW(hControl, BM_SETCHECK,
				IsDlgButtonChecked(hDlg, clickId) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
			SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(clickId, BN_CLICKED), (LPARAM)hControl);
			Pump(300);
			Shoot(L"proppage_clicked.bmp");
			char what[64] = {};
			sprintf_s(what, "after clicking %d", clickId);
			DumpPageState(hwnd, what);
		} else {
			printf("control %d not found\n", clickId);
		}
	}
	Pump(std::max(0, seconds * 1000 - 1000));

	pPage->Deactivate();
	pPage->SetObjects(0, nullptr);
	pPage->SetPageSite(nullptr);
	return 0;
}

// What one displayed picture keeps of the 4:4:4 one, chroma first: the luma is the
// same in both, so what differs is what the upsampler made of Cb and Cr.
struct ChromaMetrics {
	double psnrRgb = 0;
	double psnrChroma = 0; // Cb and Cr, from the displayed pixels
	double psnrEdges = 0;  // the same where the luma has edges, where bleeding shows
	double offsetCb = 0;   // a constant shift of the colour, in 8-bit levels
	double offsetCr = 0;
};

static ChromaMetrics ScoreChroma(const std::vector<BYTE>& test, const std::vector<BYTE>& ref, int w, int h)
{
	const auto Chroma = [](const BYTE* p, double& y, double& cb, double& cr) {
		const double r = p[2] / 255.0, g = p[1] / 255.0, b = p[0] / 255.0;
		y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		cb = (b - y) / 1.8556;
		cr = (r - y) / 1.5748;
	};
	double seRgb = 0, seChroma = 0, seEdges = 0, sumCb = 0, sumCr = 0;
	size_t edges = 0;
	const size_t n = (size_t)w * h;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t i = ((size_t)y * w + x) * 4;
			double ty, tcb, tcr, ry, rcb, rcr;
			Chroma(&test[i], ty, tcb, tcr);
			Chroma(&ref[i], ry, rcb, rcr);
			for (int k = 0; k < 3; k++) {
				const double d = (test[i + k] - ref[i + k]) / 255.0;
				seRgb += d * d;
			}
			const double dcb = tcb - rcb, dcr = tcr - rcr;
			seChroma += dcb * dcb + dcr * dcr;
			sumCb += dcb;
			sumCr += dcr;
			// An edge of the luma next door: chroma errors show as bleeding there.
			const size_t right = ((size_t)y * w + std::min(x + 1, w - 1)) * 4;
			const size_t below = ((size_t)std::min(y + 1, h - 1) * w + x) * 4;
			double ny, ncb, ncr, by, bcb, bcr;
			Chroma(&ref[right], ny, ncb, ncr);
			Chroma(&ref[below], by, bcb, bcr);
			if (std::abs(ny - ry) + std::abs(by - ry) > 0.05) {
				seEdges += dcb * dcb + dcr * dcr;
				edges++;
			}
		}
	}
	const auto Psnr = [](double mse) { return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0; };
	ChromaMetrics m;
	m.psnrRgb = Psnr(seRgb / (3.0 * n));
	m.psnrChroma = Psnr(seChroma / (2.0 * n));
	m.psnrEdges = edges ? Psnr(seEdges / (2.0 * edges)) : 99.0;
	m.offsetCb = 255.0 * sumCb / n;
	m.offsetCr = 255.0 * sumCr / n;
	return m;
}

#include "d3d11_source.inl"
#include "toggle_suite.inl"

int wmain(int argc, wchar_t* argv[])
{
	// Declared before any window exists, because that is the only time it counts --
	// and without it the desktop duplication will not hand over anything but 8 bits,
	// which on an HDR desktop is a tone-mapped copy and not what is on the screen.
	if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
		using PFN = HANDLE (WINAPI*)(HANDLE);
		if (auto set = (PFN)GetProcAddress(user32, "SetProcessDpiAwarenessContext")) {
			set((HANDLE)-4);  // per-monitor aware, v2
		}
	}
	setvbuf(stdout, nullptr, _IONBF, 0); // so a crash keeps what was already reported

	int seconds = 20;
	int only = -1;
	int pageSeconds = 0;
	bool bMainPage = false;
	std::vector<int> clickControls; // --click N, repeatable: boxes to tick once the page is up
	const wchar_t* chromaFile = nullptr; // --chroma <file>: the picture to measure on
	const wchar_t* filterFile = nullptr; // --filter <path>: another build of the x64 filter, to compare with
	SIZE source = { 800, 450 };
	SIZE window = { 1280, 720 };
	for (int i = 1; i + 1 < argc; i++) {
		if (!wcscmp(argv[i], L"--seconds")) {
			seconds = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--size")) {
			swscanf_s(argv[i + 1], L"%dx%d", &source.cx, &source.cy);
		} else if (!wcscmp(argv[i], L"--window")) {
			swscanf_s(argv[i + 1], L"%dx%d", &window.cx, &window.cy);
		} else if (!wcscmp(argv[i], L"--fps")) {
			g_frameDuration = (REFERENCE_TIME)std::llround(10000000.0 / _wtof(argv[i + 1]));
		} else if (!wcscmp(argv[i], L"--only")) {
			only = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--dlsspage")) {
			pageSeconds = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--click")) {
			clickControls.push_back(_wtoi(argv[i + 1]));
		} else if (!wcscmp(argv[i], L"--filter")) {
			filterFile = argv[i + 1];
		} else if (!wcscmp(argv[i], L"--file")) {
			g_filmFile = argv[i + 1];
		} else if (!wcscmp(argv[i], L"--seek")) {
			g_filmSeek = _wtof(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--rtxhdr")) {
			g_filmRtxHdr = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--superres")) {
			g_filmSuperRes = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--prepass")) {
			g_filmPrepass = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--hdrout")) {
			g_filmHdrOut = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--vpscale")) {
			g_filmVPScale = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--stats")) {
			g_filmStats = _wtoi(argv[i + 1]);
		} else if (!wcscmp(argv[i], L"--chroma") || !wcscmp(argv[i], L"--chroma10")) {
			g_bChroma = true;
			g_bChroma10 = !wcscmp(argv[i], L"--chroma10");
			chromaFile = argv[i + 1];
		} else if (!wcscmp(argv[i], L"--mainpage")) {
			pageSeconds = _wtoi(argv[i + 1]);
			bMainPage = true;
		}
	}
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--scalers")) {
			g_bScalers = true;
			source.cx &= ~1; // NV12
			source.cy &= ~1;
		} else if (!wcscmp(argv[i], L"--vp")) {
			g_bHardwareVP = true;
		} else if (!wcscmp(argv[i], L"--verbose")) {
			g_bChromaVerbose = true;
		} else if (!wcscmp(argv[i], L"--interlaced")) {
			g_bInterlaced = true;
		} else if (!wcscmp(argv[i], L"--hdr")) {
			g_bToggleHdr = true;
		} else if (!wcscmp(argv[i], L"--switch")) {
			g_bToggleSwitch = true;
		} else if (!wcscmp(argv[i], L"--gpu")) {
			g_bToggleGpu = true;
		} else if (!wcscmp(argv[i], L"--thread")) {
			g_bToggleThread = true;
		} else if (!wcscmp(argv[i], L"--asis")) {
			g_bAsIs = true;
		} else if (!wcscmp(argv[i], L"--toggle")) {
			g_bToggle = true;
			source.cx &= ~1; // NV12
			source.cy &= ~1;
		}
	}

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	if (g_bChroma) {
		// The picture is shown at its own size: no resize shader takes part, so the
		// only difference between the runs is what rebuilt Cb and Cr.
		source.cx &= ~1;
		source.cy &= ~1;
		std::string error = chromaFile ? std::string() : "a picture file is needed: --chroma <file>";
		if (!chromaFile || !LoadPicture(chromaFile, source.cx, source.cy, error)) {
			printf("--chroma: %s\n", error.c_str());
			return 1;
		}
		window = source;
	}

	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring filterPath = exe;
	filterPath = filterPath.substr(0, filterPath.find_last_of(L'\\')) + L"\\..\\..\\_bin\\Filter_x64\\MpcVideoRenderer64.ax";
	if (filterFile) {
		filterPath = filterFile;
	}
	HMODULE hFilter = LoadLibraryExW(filterPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!hFilter) {
		wprintf(L"cannot load %s\n", filterPath.c_str());
		return 1;
	}

	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = L"RenderAheadTest";
	RegisterClassW(&wc);
	RECT rc = { 0, 0, window.cx, window.cy };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"MPC Video Renderer -- render ahead test",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, 20, 20, rc.right - rc.left, rc.bottom - rc.top,
		nullptr, nullptr, wc.hInstance, nullptr);
	// The first ShowWindow of a process takes the show state it was started with,
	// hidden when a script starts it hidden, and a hidden window is never composed.
	ShowWindow(hwnd, SW_SHOWNORMAL);
	ShowWindow(hwnd, SW_SHOWNORMAL);
	SetForegroundWindow(hwnd);
	Pump(300);
	if (!IsWindowVisible(hwnd)) {
		printf("the test window could not be shown\n");
		return 1;
	}

	if (pageSeconds > 0) {
		static const CLSID CLSID_DlssPage = { 0xE3A1C5D7, 0x6B2F, 0x4F19, { 0xA8, 0xD4, 0x5C, 0x0B, 0x9E, 0x7F, 0x21, 0x36 } };
		static const CLSID CLSID_MainPage = { 0xDA46D181, 0x07D6, 0x441D, { 0xB3, 0x14, 0x01, 0x9A, 0xEB, 0x10, 0x14, 0x8A } };
		const int rc = ShowPropertyPage(hFilter, hwnd, pageSeconds, bMainPage ? CLSID_MainPage : CLSID_DlssPage, clickControls);
		DestroyWindow(hwnd);
		CoUninitialize();
		return rc;
	}

	if (g_bToggle) {
		const int rc = RunToggleSuite(hFilter, hwnd, source, window, seconds > 20 ? 20 : seconds);
		DestroyWindow(hwnd);
		CoUninitialize();
		return rc;
	}

	static const Config dlssConfigs[] = {
		{ "DLSS off",                       false, false, true  },
		{ "DLSS SR, render ahead off",      false, true,  false },
		{ "DLSS SR, render ahead on",       false, true,  true  },
		{ "NR + SR, render ahead off",      true,  true,  false },
		{ "NR + SR, render ahead on",       true,  true,  true  },
	};
	// The first is the one the others are compared with.
	static const Config scalerConfigs[] = {
		{ "Catmull-Rom, chroma Catmull-Rom", false, false, true, UPSCALE_CatmullRom, CHROMA_CatmullRom },
		{ "Jinc2m, chroma Catmull-Rom",      false, false, true, UPSCALE_Jinc2,      CHROMA_CatmullRom },
		{ "FSRCNNX 8, chroma Catmull-Rom",   false, false, true, UPSCALE_FSRCNNX8,   CHROMA_CatmullRom },
		{ "FSRCNNX 16, chroma Catmull-Rom",  false, false, true, UPSCALE_FSRCNNX16,  CHROMA_CatmullRom },
		{ "RAVU-zoom, chroma Catmull-Rom",   false, false, true, UPSCALE_RAVUZoom,   CHROMA_CatmullRom },
		{ "FSRCNNX 8 AR, chroma Catmull-Rom", false, false, true, UPSCALE_FSRCNNX8AR, CHROMA_CatmullRom },
		{ "FSRCNNX 16 AR, chroma Catmull-Rom", false, false, true, UPSCALE_FSRCNNX16AR, CHROMA_CatmullRom },
		{ "ArtCNN C4F16 DS, chroma Catmull-Rom", false, false, true, UPSCALE_ArtCNN, CHROMA_CatmullRom },
		{ "Catmull-Rom, chroma RAVU-zoom",   false, false, true, UPSCALE_CatmullRom, CHROMA_RAVU       },
		{ "RAVU-zoom, chroma RAVU-zoom",     false, false, true, UPSCALE_RAVUZoom,   CHROMA_RAVU       },
		{ "ArtCNN C4F16 DS, chroma Jinc",    false, false, true, UPSCALE_ArtCNN,     CHROMA_Jinc       },
	};
	// The reference comes first: the same picture in 4:4:4, which needs no chroma
	// upsampling at all, through the same conversion to RGB.
	static const Config chromaConfigs[] = {
		{ "4:4:4 (AYUV), nothing to upsample", false, false, true, -1, -1,                false, true  },
		{ "4:2:0, hardware video processor",   false, false, true, -1, -1,                true,  false },
		{ "4:2:0, hardware VP, chroma replaced", false, false, true, -1, CHROMA_CatmullRom, true, false, true },
		{ "4:2:0, hardware VP, chroma replaced, RAVU", false, false, true, -1, CHROMA_RAVU, true, false, true },
		{ "4:2:0, shaders, Nearest",           false, false, true, -1, CHROMA_Nearest                  },
		{ "4:2:0, shaders, Bilinear",          false, false, true, -1, CHROMA_Bilinear                 },
		{ "4:2:0, shaders, Catmull-Rom",       false, false, true, -1, CHROMA_CatmullRom               },
		{ "4:2:0, shaders, RAVU-zoom",         false, false, true, -1, CHROMA_RAVU                     },
		{ "4:2:0, shaders, Jinc (EWA)",        false, false, true, -1, CHROMA_Jinc                     },
		{ "4:2:0, shaders, FSRCNNX 8 AR",      false, false, true, -1, CHROMA_FSRCNNX8AR               },
		{ "4:2:0, hardware VP, chroma replaced, Jinc", false, false, true, -1, CHROMA_Jinc, true, false, true },
		{ "4:2:0, hardware VP, chroma replaced, FSRCNNX 8 AR", false, false, true, -1, CHROMA_FSRCNNX8AR, true, false, true },
		// Does the driver's RTX Video HDR do anything to this picture? The pair differs
		// by the extension alone, so the answer is whether their two pictures differ.
		{ "4:2:0, hardware VP, HDR out, RTX Video HDR OFF", false, false, true, -1, -1, true, false, false, true, 0 },
		{ "4:2:0, hardware VP, HDR out, RTX Video HDR ON",  false, false, true, -1, -1, true, false, false, true, 1 },
		{ "4:2:0, VP + 4:4:4 pre-pass, HDR out, RTX Video HDR ON", false, false, true, -1, CHROMA_CatmullRom, true, false, true, true, 1 },
	};
	const Config* configs = g_bChroma ? chromaConfigs : g_bScalers ? scalerConfigs : dlssConfigs;
	const int configCount = (int)(g_bChroma ? std::size(chromaConfigs)
		: g_bScalers ? std::size(scalerConfigs) : std::size(dlssConfigs));

	printf("Film %ldx%ld at %.3f fps in a %ldx%ld window, %d s per configuration after 5 s to settle.\n",
		source.cx, source.cy, 10000000.0 / g_frameDuration, window.cx, window.cy, seconds);
	if (g_bChroma) {
		printf("BT.709, limited range, chroma sited left, shown at 1:1 with the dither off.\n");
		printf("psnr-c: Cb and Cr of the displayed picture against the 4:4:4 one; psnr-e: the\n");
		printf("same where the luma has edges, where bleeding shows; offset: the constant\n");
		printf("shift of Cb and Cr, in 8-bit levels.\n\n");
	} else {
		printf("Sync offset in ms: when Present returned, against the picture's time.\n\n");
	}

	const std::wstring exeDir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\'));
	std::vector<BYTE> firstPicture;
	int firstW = 0, firstH = 0;
	int failures = 0;
	for (int c = 0; c < configCount; c++) {
		if (only >= 0 && c != only) {
			continue;
		}
		const Config& config = configs[c];
		g_bChroma444 = config.b444; // read by the source pin when the graph is built
		const Result r = RunConfig(hFilter, hwnd, config, source, window, seconds);
		printf("== %s\n", config.name);
		if (!r.error.empty()) {
			printf("   FAILED: %s\n", r.error.c_str());
			failures++;
			continue;
		}
		if (g_bChroma) {
			wprintf(L"   processor    : %s\n", r.vprocLine.c_str());
			if (g_bChromaVerbose) {
				wprintf(L"%s\n", r.statsAll.c_str());
			}
			if (!r.prescaleTimes.empty()) {
				wprintf(L"   prescale (ms): %s\n", r.prescaleTimes.c_str());
			}
			if (r.picture.empty()) {
				printf("   FAIL: no displayed picture\n");
				failures++;
				continue;
			}
			SaveBmp(exeDir + L"\\chroma_" + std::to_wstring(c) + L".bmp", r.picture, r.pictureW, r.pictureH);
			if (firstPicture.empty()) {
				firstPicture = r.picture;
				firstW = r.pictureW;
				firstH = r.pictureH;
				printf("   this is the reference the others are measured against\n\n");
				printf("   %-36s %8s %8s %9s %13s\n", "", "psnr-c", "psnr-e", "psnr-rgb", "offset Cb/Cr");
			} else if (r.pictureW == firstW && r.pictureH == firstH) {
				const ChromaMetrics m = ScoreChroma(r.picture, firstPicture, firstW, firstH);
				printf("   %-36s %8.2f %8.2f %9.2f  %+6.2f/%+6.2f\n", config.name,
					m.psnrChroma, m.psnrEdges, m.psnrRgb, m.offsetCb, m.offsetCr);
			}
			continue;
		}
		if (g_filmFile && !r.screenCrop.empty()) {
			// The desktop's own picture, saved so it can be looked at and compared
			// between two renderers -- which the filter's snapshot cannot be.
			const std::wstring path = exeDir + L"\\screen_" + std::to_wstring(c) + L".bmp";
			SaveBmp(path, r.screenCrop, r.screenCropW, r.screenCropH);
			wprintf(L"   screen shot  : %s (%dx%d)\n", path.c_str(), r.screenCropW, r.screenCropH);
			printf("   screen area  : %s\n", r.screenWhere.c_str());
		}
		if (g_filmFile && !r.windowShot.empty()) {
			const std::wstring path = exeDir + L"\\window_" + std::to_wstring(c) + L".bmp";
			SaveBmp(path, r.windowShot, r.windowShotW, r.windowShotH);
			wprintf(L"   window shot  : %s (%dx%d)\n", path.c_str(), r.windowShotW, r.windowShotH);
		}
		if (g_filmFile) {
			// What Windows really composed. The filter's own snapshot cannot answer
			// this: it renders into an 8-bit target and, when the output is HDR, puts
			// a PQ-to-SDR correction in front on purpose so the picture is viewable.
			// Tone mapping and Super Resolution are both invisible to it.
			wprintf(L"   screen       : %s\n", r.screenLine.c_str());
		}
		if ((g_bAsIs || g_filmFile) && !r.statsAll.empty()) {
			// Whatever this renderer says about itself: the only way to read a filter
			// the bench did not build, since it cannot be asked anything else.
			wprintf(L"--- statistics as the filter reports them ---\n%s\n", r.statsAll.c_str());
		}
		if (g_filmFile && !r.picture.empty()) {
			const std::wstring path = exeDir + L"\\film_" + std::to_wstring(c) + L".bmp";
			SaveBmp(path, r.picture, r.pictureW, r.pictureH);
			wprintf(L"   picture      : %s (%dx%d)\n", path.c_str(), r.pictureW, r.pictureH);
			if (!r.statsPicture.empty()) {
				SaveBmp(exeDir + L"\\film_" + std::to_wstring(c) + L"_stats.bmp", r.statsPicture, r.pictureW, r.pictureH);
			}
		}
		if (g_bScalers) {
			wprintf(L"   processor    : %s\n", r.vprocLine.c_str());
			wprintf(L"   scaling      : %s\n", r.scalingLine.c_str());
			if (!r.prescaleTimes.empty()) {
				wprintf(L"   prescale (ms): %s\n", r.prescaleTimes.c_str());
			}
			if (r.picture.empty()) {
				printf("   FAIL: no displayed picture\n");
				failures++;
			} else {
				const std::wstring path = exeDir + L"\\scalers_" + std::to_wstring(c) + L".bmp";
				SaveBmp(path, r.picture, r.pictureW, r.pictureH);
				if (!r.statsPicture.empty()) {
					SaveBmp(exeDir + L"\\scalers_" + std::to_wstring(c) + L"_stats.bmp", r.statsPicture, r.pictureW, r.pictureH);
				}
				if (firstPicture.empty()) {
					firstPicture = r.picture;
					firstW = r.pictureW;
					firstH = r.pictureH;
				} else if (r.pictureW == firstW && r.pictureH == firstH) {
					// Mean absolute difference over RGB, and the share of values more
					// than 24 levels apart: scalers differ a little, a broken pass a lot.
					double sum = 0;
					size_t beyond = 0;
					for (size_t i = 0; i < r.picture.size(); i += 4) {
						for (int k = 0; k < 3; k++) {
							const int d = std::abs((int)r.picture[i + k] - (int)firstPicture[i + k]);
							sum += d;
							beyond += d > 24;
						}
					}
					const double values = 3.0 * r.picture.size() / 4;
					printf("   against the first: mean difference %.2f levels, %.3f%% of values beyond 24\n",
						sum / values, 100.0 * beyond / values);
				}
				wprintf(L"   picture      : %s\n", path.c_str());
			}
		}
		if (config.bNR) {
			wprintf(L"   DLSS 5 NR    : %s\n", r.nrLine.c_str());
		}
		if (config.bSR) {
			wprintf(L"   DLSS SR      : %s\n", r.srLine.c_str());
		}
		if (!r.dlssTimes.empty()) {
			wprintf(L"   DLSS (ms)    : %s\n", r.dlssTimes.c_str());
		}
		if (!r.aheadLine.empty()) {
			wprintf(L"   Render ahead : %s\n", r.aheadLine.c_str());
		}
		if (!r.syncs.empty()) {
			double sum = 0, squares = 0;
			for (int v : r.syncs) {
				sum += v;
				squares += (double)v * v;
			}
			const double mean = sum / r.syncs.size();
			const double sd = std::sqrt(std::max(0.0, squares / r.syncs.size() - mean * mean));
			std::vector<int> sorted = r.syncs;
			std::sort(sorted.begin(), sorted.end());
			printf("   sync offset  : mean %+.1f, sd %.1f, min %+d, p5 %+d, p95 %+d, max %+d  (%zu samples)\n",
				mean, sd, sorted.front(), sorted[sorted.size() * 5 / 100], sorted[sorted.size() * 95 / 100], sorted.back(), sorted.size());
		} else {
			printf("   no Sync offset read\n");
			failures++;
		}
		printf("   frames       : %d shown while measuring, %d skipped, %d late (%d while settling)\n",
			r.framesAtEnd - r.framesAtStart, r.skippedAtEnd - r.skippedAtStart, r.lateAtEnd - r.lateAtStart, r.lateAtStart);
		printf("   state changes: pause %.2f s, run %.2f s, stop %.2f s\n", r.stopSeconds[0], r.stopSeconds[1], r.stopSeconds[2]);
		for (double s : r.stopSeconds) {
			if (s > 1.0) {
				printf("   FAIL: a state change took more than a second\n");
				failures++;
				break;
			}
		}
		printf("\n");
	}

	DestroyWindow(hwnd);
	CoUninitialize();
	printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
