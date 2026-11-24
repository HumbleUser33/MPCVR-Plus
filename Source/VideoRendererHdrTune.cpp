/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// CMpcVideoRenderer: the HDR curve's tune. On its key, the film is paused and moved to
// sixteen places spread over it; at each, the picture the curve receives is measured
// (DX11VideoProcessorHdrCurve.cpp, MeasureHdrTune); the film goes back to the picture it was
// on and plays again if it was playing; the curve's values come from what was measured
// (HdrCurveTune.h) and are kept as its Custom values, as a preset's key keeps a preset.
//
// The renderer moves the graph it is in, which nothing else of it does. So the work is on a
// thread of its own, which holds no lock of the renderer while it asks anything of the graph:
// a seek flushes the renderer's input on the thread that seeks, under both of its locks, and
// the graph calls Stop holding its own lock, which a seek of this thread may be waiting on.
// Stop, Run and leaving the graph only raise a flag (CHdrTuneProbe::Abort); nothing waits
// for the thread. It holds the filter and the module until it ends.
//
// All of it is this fork's, and kept out of VideoRenderer.cpp so that a merge of upstream's
// file meets as little of ours as possible (MERGING-UPSTREAM.md).

#include "stdafx.h"
#include <format>
#include "VideoRenderer.h"
#include "HdrCurveTune.h"

namespace {

// Sixteen places, from 4 % of the film to 92 %: the opening logos and the credits left out.
constexpr int kPlaces = 16;
constexpr double kFirst = 0.04;
constexpr double kSpan = 0.88;
// The moves stop there, so that the tune is over within 30 s. Sixteen paused seeks took 3 to
// 15 s in playback_test --seektime 16 on an RTX 4060 (a 1080p film under RTX Video HDR, HEVC
// and AV1 masters up to 4K), the AV1 4K one the slowest.
constexpr ULONGLONG kSamplingMs = 22000;
// One place's longest wait for its picture; the slowest measured took 1.8 s.
constexpr DWORD kPictureMs = 2500;
constexpr int kMissesInARow = 3;
// A picture further than this from where the seek went is not that place's.
constexpr REFERENCE_TIME kNear = 3 * 10000000LL;

struct TuneThreadArgs {
	CMpcVideoRenderer* filter;
	IFilterGraph* graph;
	int mode;
	HMODULE module;
};

void NameThisThread(const wchar_t* name)
{
	using PFN_SetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static PFN_SetThreadDescription pfn = []() -> PFN_SetThreadDescription {
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		return kernel ? (PFN_SetThreadDescription)GetProcAddress(kernel, "SetThreadDescription") : nullptr;
	}();
	if (pfn) {
		pfn(GetCurrentThread(), name);
	}
}

std::wstring Seconds(REFERENCE_TIME t)
{
	return std::format(L"{:.1f} s", t / 1e7);
}

const wchar_t* ReasonName(CHdrTuneProbe::Reason why)
{
	switch (why) {
	case CHdrTuneProbe::Cancelled:   return L"cancelled by its key";
	case CHdrTuneProbe::ExternalRun: return L"the player played the film";
	case CHdrTuneProbe::Stopped:     return L"the film was stopped";
	case CHdrTuneProbe::Removed:     return L"the renderer left the graph";
	}
	return L"";
}

} // namespace

// 1 tunes the curve and keeps it, 2 scans and reports only, 0 stops a scan under way. A scan
// under way is stopped by any call, as the key pressed again stops it.
HRESULT CMpcVideoRenderer::StartHdrTune(int mode)
{
	{
		CAutoLock lock(&m_HdrTuneLock);
		if (m_hHdrTuneThread) {
			if (WaitForSingleObject(m_hHdrTuneThread, 0) == WAIT_TIMEOUT) {
				m_HdrTune.Abort(CHdrTuneProbe::Cancelled);
				return S_FALSE;
			}
			CloseHandle(m_hHdrTuneThread);
			m_hHdrTuneThread = nullptr;
		}
	}
	if (mode != 1 && mode != 2) {
		return mode == 0 ? S_OK : E_INVALIDARG;
	}

	const wchar_t* refusal = nullptr;
	{
		CAutoLock cRendererLock(&m_RendererLock);
		refusal = m_VideoProcessor ? m_VideoProcessor->HdrTuneRefusal() : L"no video";
	}
	if (!refusal && m_filterState == State_Stopped) {
		refusal = L"the film is stopped";
	}
	if (!refusal && m_Stepping) {
		refusal = L"a frame step is under way";
	}
	if (!refusal && !m_bValidBuffer) {
		refusal = L"no picture yet";
	}
	FILTER_INFO info = {};
	if (!refusal && (FAILED(QueryFilterInfo(&info)) || !info.pGraph)) {
		refusal = L"the renderer is in no graph";
	}
	if (refusal) {
		SetHdrTuneReport(std::format(L"not started: {}\n", refusal));
		PostNotice(std::format(L"HDR curve tune: {}", refusal), 3000);
		return E_FAIL;
	}

	// The thread holds the graph (QueryFilterInfo's reference), the filter and this module.
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&HdrTuneThreadProc, &module);
	auto* args = new TuneThreadArgs{ this, info.pGraph, mode, module };
	AddRef();
	const auto GiveBack = [&]() {
		info.pGraph->Release();
		delete args;
		if (module) {
			FreeLibrary(module);
		}
		Release();
	};

	CAutoLock lock(&m_HdrTuneLock);
	if (m_hHdrTuneThread) {
		GiveBack(); // another call started one meanwhile
		return S_FALSE;
	}
	m_HdrTune.Begin();
	m_hHdrTuneThread = CreateThread(nullptr, 0, HdrTuneThreadProc, args, 0, nullptr);
	if (!m_hHdrTuneThread) {
		m_HdrTune.EndSampling();
		GiveBack();
		return E_FAIL;
	}
	return S_OK;
}

DWORD WINAPI CMpcVideoRenderer::HdrTuneThreadProc(LPVOID param)
{
	auto* args = static_cast<TuneThreadArgs*>(param);
	CMpcVideoRenderer* filter = args->filter;
	const HMODULE module = args->module;
	NameThisThread(L"MPCVR HDR curve tune");
	const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	filter->HdrTuneRun(args->graph, args->mode);

	args->graph->Release();
	delete args;
	// The filter may go with this reference, here, on this thread: nothing of it after.
	filter->Release();
	if (SUCCEEDED(hrCom)) {
		CoUninitialize();
	}
	if (module) {
		FreeLibraryAndExitThread(module, 0);
	}
	return 0;
}

void CMpcVideoRenderer::HdrTuneRun(IFilterGraph* pGraph, int mode)
{
	const ULONGLONG start = GetTickCount64();
	std::wstring report;

	CComQIPtr<IMediaSeeking> pMS(pGraph);
	CComQIPtr<IMediaControl> pMC(pGraph);
	GUID format = {};
	LONGLONG duration = 0;
	DWORD caps = 0;
	// Not AM_SEEKING_CanGetCurrentPos: the graph over LAV Splitter does not say it (0x37 in
	// playback_test --seektime), and gives the position all the same, from its clock.
	const DWORD wanted = AM_SEEKING_CanSeekAbsolute;
	const HRESULT hrFormat = pMS ? pMS->GetTimeFormat(&format) : E_NOINTERFACE;
	const HRESULT hrCaps = pMS ? pMS->GetCapabilities(&caps) : E_NOINTERFACE;
	const HRESULT hrDuration = pMS ? pMS->GetDuration(&duration) : E_NOINTERFACE;
	if (!pMS || !pMC || FAILED(hrFormat) || format != TIME_FORMAT_MEDIA_TIME || FAILED(hrCaps) || (caps & wanted) != wanted
			|| FAILED(hrDuration) || duration < 10 * 10000000LL) {
		m_HdrTune.EndSampling();
		SetHdrTuneReport(std::format(L"not started: the film cannot be moved through (seeking {}, control {}, time format 0x{:08X} {},"
			L" capabilities 0x{:08X} 0x{:04X}, duration 0x{:08X} {})\n", pMS ? L"yes" : L"no", pMC ? L"yes" : L"no",
			(unsigned)hrFormat, format == TIME_FORMAT_MEDIA_TIME ? L"media time" : L"other", (unsigned)hrCaps, caps,
			(unsigned)hrDuration, Seconds(duration)));
		PostNotice(L"HDR curve tune: the film cannot be moved through", 3000);
		return;
	}

	// The film paused where it is, and that picture remembered: GetCurrentPosition is only
	// the picture shown once the graph has stopped moving.
	OAFilterState before = State_Stopped, state = State_Stopped;
	pMC->GetState(0, &before);
	const bool bWasRunning = before == State_Running;
	if (bWasRunning) {
		pMC->Pause();
	}
	pMC->GetState(2000, &state);
	LONGLONG back = 0;
	pMS->GetCurrentPosition(&back);
	report += std::format(L"film of {}, {}, at {}\n", Seconds(duration), bWasRunning ? L"playing" : L"paused", Seconds(back));

	// The measurement step in the chain while the film is moved.
	m_HdrTune.SetActive(true);
	{
		CAutoLock cRendererLock(&m_RendererLock);
		if (m_VideoProcessor) {
			m_VideoProcessor->HdrTuneStepChanged();
		}
	}

	std::vector<HdrCurveTune::Stats> pictures;
	int misses = 0, missesInARow = 0;
	for (int i = 0; i < kPlaces && !m_HdrTune.AbortedNow(); i++) {
		if (GetTickCount64() - start >= kSamplingMs || missesInARow >= kMissesInARow) {
			break;
		}
		// Up before the seek: the place's picture is drawn with it.
		PostNotice(std::format(L"HDR curve tune {}/{}", i + 1, kPlaces), 3000, false);
		LONGLONG target = (LONGLONG)(duration * (kFirst + kSpan * (i + 0.5) / kPlaces));
		const ULONGLONG t0 = GetTickCount64();
		m_HdrTune.Arm(m_nFlushGen);
		const HRESULT hr = pMS->SetPositions(&target, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
		const ULONGLONG t1 = GetTickCount64();
		HdrTunePicture picture;
		const CHdrTuneProbe::Wait got = FAILED(hr) ? CHdrTuneProbe::TimedOut : m_HdrTune.WaitPicture(kPictureMs, picture);
		const ULONGLONG t2 = GetTickCount64();
		if (got == CHdrTuneProbe::Aborted) {
			break;
		}
		if (got == CHdrTuneProbe::TimedOut) {
			misses++;
			missesInARow++;
			report += std::format(L"{:2}  {:>9}  no picture (seek 0x{:08X}, {} ms)\n", i + 1, Seconds(target), (unsigned)hr, t2 - t0);
			continue;
		}
		missesInARow = 0;
		if (std::abs(picture.rtFilm - target) > kNear) {
			report += std::format(L"{:2}  {:>9}  another picture, at {}\n", i + 1, Seconds(target), Seconds(picture.rtFilm));
			continue;
		}
		const HdrCurveTune::Stats s = HdrCurveTune::Measure(picture.hist.data(), picture.hist.data() + HdrCurveTune::kBins);
		pictures.push_back(s);
		report += std::format(L"{:2}  {:>9}  picture at {:>9}, seek {} ms, picture {} ms, measured {:.1f} ms, {}x{}: {}, black {:.0f} %,"
			L" p10 {:.3f}, median {:.2f}, p90 {:.1f}, key {:.2f}, peak {:.0f}\n",
			i + 1, Seconds(target), Seconds(picture.rtFilm), t1 - t0, t2 - t1, picture.ms, picture.width, picture.height,
			HdrCurveTune::KindName(s.kind), 100.0 * s.black, s.p10, s.p50, s.p90, s.key, s.peak);
	}

	m_HdrTune.EndSampling();
	m_HdrTune.SetActive(false);
	{
		CAutoLock cRendererLock(&m_RendererLock);
		if (m_VideoProcessor) {
			m_VideoProcessor->HdrTuneStepChanged();
		}
	}
	const CHdrTuneProbe::Reason why = m_HdrTune.Why();
	report += std::format(L"{} of {} places measured, {} without a picture, in {:.1f} s{}{}\n", pictures.size(), kPlaces, misses,
		(GetTickCount64() - start) / 1000.0, why ? L"; stopped: " : L"", ReasonName(why));
	if (why == CHdrTuneProbe::Stopped || why == CHdrTuneProbe::Removed) {
		SetHdrTuneReport(report); // nothing more asked of the graph
		return;
	}

	// Back to the picture the film was on.
	pMS->SetPositions(&back, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
	pMC->GetState(3000, &state);
	LONGLONG now = 0;
	pMS->GetCurrentPosition(&now);
	report += std::format(L"back at {}, {:+.3f} s from where it was\n", Seconds(now), (now - back) / 1e7);

	int displayNits = 0;
	{
		CAutoLock cRendererLock(&m_RendererLock);
		displayNits = m_Sets.iHdrDisplayMaxNits;
	}
	const HdrCurveTune::Tuned tuned = HdrCurveTune::Tune(pictures, displayNits);
	const HdrCurveTune::Film& f = tuned.film;
	const HdrCurveTune::Targets& t = HdrCurveTune::kMasters;
	report += std::format(L"film: {} lit pictures ({} dark, {} normal, {} bright), {} black; depth {:.2f} -> {:.2f} (target {:.2f}),"
		L" key {:.2f} -> {:.2f} nits (target {:.2f}), contrast {:.2f} -> {:.2f}, peak {:.0f} -> {:.0f} (display {})\n",
		f.lit, f.dark, f.normal, f.bright, f.black, f.depth, tuned.depthAfter, t.depth, f.key, tuned.keyAfter, t.key,
		f.contrast, tuned.contrastAfter, f.peak, tuned.peakAfter, displayNits);
	report += std::format(L"values: {}{}\n", HdrCurveTune::Describe(tuned.values), tuned.guarded ? L", guarded" : L"");

	std::wstring notice;
	if (why == CHdrTuneProbe::Cancelled) {
		notice = L"cancelled";
	} else if (!tuned.enough) {
		notice = std::format(L"{} pictures, not enough to tune on", pictures.size());
	} else if (mode == 1) {
		ApplyTunedCurve(tuned.values);
		notice = HdrCurveTune::Describe(tuned.values);
		report += L"applied and kept as the curve's Custom values\n";
	} else {
		notice = L"measured, " + HdrCurveTune::Describe(tuned.values);
	}
	// Playing again, as it was, unless the player has already seen to it.
	if (bWasRunning && (why == CHdrTuneProbe::None || why == CHdrTuneProbe::Cancelled)) {
		pMC->Run();
	}
	report += std::format(L"took {:.1f} s\n", (GetTickCount64() - start) / 1000.0);
	SetHdrTuneReport(report);
	PostNotice(L"HDR curve tune: " + notice, 6000);
}

// The tuned values as the curve's Custom ones, at full strength, and kept, as a preset's key
// keeps the preset it recalls.
void CMpcVideoRenderer::ApplyTunedCurve(const HdrCurve::Values& values)
{
	Settings_t sets;
	{
		CAutoLock cRendererLock(&m_RendererLock);
		sets = m_Sets;
	}
	sets.bHdrCurve = true;
	sets.iHdrCurvePreset = HdrCurve::Custom;
	sets.iHdrCurveStrength = HdrCurve::kStrengthDef;
	HdrCurve::StoreValues(sets, values);
	SetSettings(sets);
	SaveSettings();
}

void CMpcVideoRenderer::SetHdrTuneReport(const std::wstring& report)
{
	CAutoLock lock(&m_HdrTuneLock);
	m_strHdrTuneReport = report;
}

void CMpcVideoRenderer::PostNotice(const std::wstring& text, DWORD ms, bool bRedraw)
{
	{
		CAutoLock cRendererLock(&m_RendererLock);
		if (m_VideoProcessor) {
			m_VideoProcessor->ShowNotice(text, ms);
		}
	}
	{
		// Deleting the timer waits for its callback, which redraws under the renderer's
		// lock: not held here.
		CAutoLock lock(&m_NoticeLock);
		if (m_hNoticeTimer) {
			DeleteTimerQueueTimer(nullptr, m_hNoticeTimer, INVALID_HANDLE_VALUE);
			m_hNoticeTimer = nullptr;
		}
		CreateTimerQueueTimer(&m_hNoticeTimer, nullptr, NoticeTimerProc, this, ms + 100, 0, WT_EXECUTEONLYONCE);
	}
	if (bRedraw && m_filterState != State_Running) {
		Redraw();
	}
}
