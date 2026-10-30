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

// CDX11VideoProcessor: the scheduled presentation, render ahead and the pacing trace.
// All of it is this fork's, and kept out of DX11VideoProcessor.cpp so that a merge of
// upstream's file meets as little of ours as possible (MERGING-UPSTREAM.md).

#include "stdafx.h"
#include "Helper.h"
#include "Times.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"
#include "DX11VideoProcessorShared.h"
#include "Shaders.h"

// Present returns when the picture has been queued, not when it is shown. With a
// refresh for every frame -- a 24 fps film on a 24 Hz screen -- a picture that once
// arrived late sits behind the one before it, and so does every picture after: the
// queue never empties by itself, and the film runs a refresh behind until something
// flushes it. Nothing the renderer times can see that, so DXGI is asked.
void CDX11VideoProcessor::MeasurePresentQueue()
{
	m_tickOnScreen = 0;
	UINT last = 0;
	DXGI_FRAME_STATISTICS stats = {};
	if (!m_pDXGISwapChain1 || FAILED(m_pDXGISwapChain1->GetLastPresentCount(&last))
			|| FAILED(m_pDXGISwapChain1->GetFrameStatistics(&stats)) || !stats.SyncQPCTime.QuadPart) {
		return;   // disjoint, or a presentation model that keeps no count
	}
	// PresentCount is the last picture that reached the screen and PresentRefreshCount
	// the refresh it reached it on; everything presented since is still waiting, this
	// one included. SyncQPCTime is another thing again -- when refresh SyncRefreshCount
	// happened, which is simply the latest one DXGI sampled.
	m_uPresentQueue = last - stats.PresentCount;
	if (m_dRefreshPeriodMs > 0) {
		const double periodTicks = m_dRefreshPeriodMs * GetPreciseTicksPerSecond() / 1000.0;
		const double sinceSync = (double)GetPreciseTick() - (double)stats.SyncQPCTime.QuadPart;
		// A picture takes its place in the queue, one refresh each, and cannot be
		// shown on a refresh that has already gone by.
		const int64_t byQueue = (int64_t)stats.PresentRefreshCount + m_uPresentQueue;
		const int64_t byClock = (int64_t)stats.SyncRefreshCount + (int64_t)std::floor(std::max(sinceSync, 0.0) / periodTicks) + 1;
		const int64_t refresh = std::max(byQueue, byClock);
		m_tickOnScreen = (uint64_t)((double)stats.SyncQPCTime.QuadPart
			+ (double)(refresh - (int64_t)stats.SyncRefreshCount) * periodTicks);
	}
}

// DXGI's account of the swap chain, handed to the schedule: where the refreshes are,
// and which Present has reached the screen.
void CDX11VideoProcessor::ReadPresentSchedule()
{
	UINT last = 0;
	DXGI_FRAME_STATISTICS stats = {};
	if (!m_pDXGISwapChain1 || FAILED(m_pDXGISwapChain1->GetLastPresentCount(&last))) {
		return;
	}
	const HRESULT hr = m_pDXGISwapChain1->GetFrameStatistics(&stats);
	if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
		m_PresentSchedule.Reset(); // the counts start again: a mode change, a new output
	} else if (SUCCEEDED(hr)) {
		m_PresentSchedule.Seen(last, stats, m_dRefreshPeriodMs);
	}
}

// The wait between a picture being drawn and the moment chosen to present it. The GPU
// finishes the picture meanwhile, and how long it took from the sample arriving tells
// how early the next ones have to be asked for.
//
// Nothing here waits for the GPU: a picture it has not finished by then is presented
// anyway and reaches the screen when it is done (see HoldUntilPresentTime).
void CDX11VideoProcessor::WaitForScheduledPresent(const CPresentSchedule::Plan& plan, const bool bMeasure)
{
	const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;
	const uint64_t tickStart = GetPreciseTick();
	// A picture is never more than a frame early, since the one before it has only
	// just left; a wait much longer than that is a clock that has jumped, and the
	// renderer keeps its locks while it waits.
	const double maxWaitMs = 250.0;

	uint64_t tickDone = 0;
	bool bQuery = m_pPictureDoneQuery != nullptr;
	const auto PollDone = [&]() {
		if (bQuery && !tickDone) {
			BOOL bDone = FALSE;
			const HRESULT hr = m_pDeviceContext->GetData(m_pPictureDoneQuery, &bDone, sizeof(bDone), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (hr == S_OK) {
				tickDone = GetPreciseTick();
			} else if (FAILED(hr)) {
				bQuery = false;
			}
		}
	};

	for (;;) {
		PollDone();
		const uint64_t tickNow = GetPreciseTick();
		const double remainingMs = (double)(plan.tickCall - (long long)tickNow) / ticksPerMs;
		if (remainingMs <= 0 || m_pFilter->m_filterState != State_Running || (double)(tickNow - tickStart) / ticksPerMs >= maxWaitMs) {
			break;
		}
		// Every millisecond while the GPU works, then in steps of at most 10 ms.
		m_PreciseSleep.Sleep(std::min(remainingMs, tickDone ? 10.0 : 1.0));
	}

	if (!tickDone) {
		m_RenderAhead.CountLate(); // not done at its present time
	}
	m_tickPictureDone = tickDone;
	if (bMeasure) {
		const uint64_t tickEnd = tickDone ? tickDone : GetPreciseTick();
		// Present is called this long before the picture's time, to be on its refresh.
		const double leadMs = (double)(plan.tickDue - plan.tickWanted) / ticksPerMs;
		m_RenderAhead.AddLatency((double)(tickEnd - m_tickSampleStart) / ticksPerMs, leadMs,
			m_pFilter->m_FrameStats.GetAverageFrameDuration() / 10000.0);
	}

	m_rtHeld += (REFERENCE_TIME)((GetPreciseTick() - tickStart) * 10000000.0 / GetPreciseTicksPerSecond());
}

// For the trace: DXGI's account of the swap chain as it stands.
void CDX11VideoProcessor::ReadShownForTrace(CPacingTrace::Shown& shown)
{
	UINT last = 0;
	DXGI_FRAME_STATISTICS stats = {};
	if (m_pDXGISwapChain1 && SUCCEEDED(m_pDXGISwapChain1->GetLastPresentCount(&last))) {
		shown.lastPresent = last;
		if (SUCCEEDED(m_pDXGISwapChain1->GetFrameStatistics(&stats))) {
			shown.presentCount = stats.PresentCount;
			shown.presentRefresh = stats.PresentRefreshCount;
			shown.syncRefresh = stats.SyncRefreshCount;
			shown.syncQpc = stats.SyncQPCTime.QuadPart;
		}
	}
}

// For the trace: what the pictures that follow are presented into, said once and
// again whenever it changes.
void CDX11VideoProcessor::NoteSwapChainForTrace()
{
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	if (!m_pDXGISwapChain1 || FAILED(m_pDXGISwapChain1->GetDesc1(&desc))) {
		return;
	}
	const std::wstring note = std::format(
		L"swap chain {}x{} format {} buffers {} effect {} flags {:#x}, refresh {:.4f} ms, wait for vblank {}, adjust present time {}, render ahead {}, exclusive {}, video delay {} ms",
		desc.Width, desc.Height, (int)desc.Format, desc.BufferCount, (int)desc.SwapEffect, desc.Flags,
		m_dRefreshPeriodMs, m_bVBlankBeforePresent ? 1 : 0, m_bAdjustPresentTime ? 1 : 0, RenderAheadActive() ? 1 : 0,
		m_bExclusiveScreen ? 1 : 0, m_rtVideoDelay / 10000);
	if (note != m_strTraceNote) {
		m_strTraceNote = note;
		m_PacingTrace.Note(note);
	}
}

// For the trace: what the two DLSS sessions say of themselves, said once and again
// whenever it changes. A session that has latched itself off is a line here, where
// otherwise it is a picture that quietly stopped being processed.
void CDX11VideoProcessor::NoteDlssForTrace()
{
	std::wstring note = L"dlss:";
	if (m_bDlssNR) {
		note += std::format(L" NR {} ({})", m_bDlssNRActive ? L"on" : L"off", GetDlssStatus());
		if (m_bDlssNRActive && m_DlssCompose.IsCreated()) {
			note += std::format(L" at {}% {}x{}", m_iDlssNRWorkingScale, m_DlssCompose.WorkWidth(), m_DlssCompose.WorkHeight());
		}
	}
	if (m_bDlssSR) {
		note += std::format(L" SR {} ({})", m_bDlssSRActive ? L"on" : L"off", GetDlssSRStatus());
	}
	if (!m_bDlssNR && !m_bDlssSR) {
		note += L" none asked for";
	}
	if (note != m_strTraceDlss) {
		m_strTraceDlss = note;
		m_PacingTrace.Note(note);
	}
}

int CDX11VideoProcessor::GetRenderAhead()
{
	return RenderAheadActive() ? m_RenderAhead.GetAhead() : 0;
}

// Render ahead, first half: a marker after the picture's last command, which tells
// when the GPU is done with it -- DLSS SR included, which runs after the render
// thread has moved on.
void CDX11VideoProcessor::MarkPictureSubmitted()
{
	if (!m_pPictureDoneQuery) {
		const D3D11_QUERY_DESC desc = { D3D11_QUERY_EVENT, 0 };
		m_pDevice->CreateQuery(&desc, &m_pPictureDoneQuery);
	}
	if (m_pPictureDoneQuery) {
		m_pDeviceContext->End(m_pPictureDoneQuery);
		m_pDeviceContext->Flush(); // GetData below does not flush
	}
}

// Render ahead, second half: hold the picture until the moment SyncFrameToStreamTime
// presents at, half a refresh before its time on the reference clock, and learn how
// long the picture took.
//
// Nothing here waits for the GPU. It finishes DLSS SR while the renderer holds the
// picture, and a picture it has not finished at its present time is presented anyway:
// the screen shows it once it is done, as it would have without render ahead. Waiting
// for the GPU instead took that time from the next sample, which measured 25 skipped
// frames in 20 s of 29.97 fps film whose DLSS chain took 30 ms.
void CDX11VideoProcessor::HoldUntilPresentTime(const REFERENCE_TIME frameStartTime, const bool bMeasure)
{
	const REFERENCE_TIME rtTarget = frameStartTime - (REFERENCE_TIME)m_uHalfRefreshPeriodMs * 10000;
	const uint64_t tickStart = GetPreciseTick();
	const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;
	// Never longer than a hundred milliseconds, two frames of 24 fps and a little: past
	// that the clock has jumped, and the renderer keeps its locks while it waits.
	const double maxHoldMs = 102.0;

	uint64_t tickDone = 0;
	bool bQuery = m_pPictureDoneQuery != nullptr;
	const auto PollDone = [&]() {
		if (bQuery && !tickDone) {
			BOOL bDone = FALSE;
			const HRESULT hr = m_pDeviceContext->GetData(m_pPictureDoneQuery, &bDone, sizeof(bDone), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (hr == S_OK) {
				tickDone = GetPreciseTick();
			} else if (FAILED(hr)) {
				bQuery = false;
			}
		}
	};

	for (;;) {
		PollDone();
		if (m_pFilter->m_filterState != State_Running || FAILED(m_pFilter->StreamTime(m_streamTime))) {
			break;
		}
		const REFERENCE_TIME rtRemaining = rtTarget - m_streamTime;
		if (rtRemaining <= 0 || (GetPreciseTick() - tickStart) / ticksPerMs >= maxHoldMs) {
			break;
		}
		// Every millisecond while the GPU works, then in steps of at most 10 ms,
		// reading the clock again after each: the reference clock need not run at
		// the pace of the performance counter.
		m_PreciseSleep.Sleep(std::min(rtRemaining / 10000.0, tickDone ? 10.0 : 1.0));
	}

	if (!tickDone) {
		m_RenderAhead.CountLate(); // not done at its present time
	}
	m_tickPictureDone = tickDone;
	if (bMeasure) {
		// Exact when the GPU was done in time. Otherwise the picture took at least
		// until now, and the start moves earlier until pictures are done in time.
		const uint64_t tickEnd = tickDone ? tickDone : GetPreciseTick();
		const REFERENCE_TIME rtFrameDur = m_pFilter->m_FrameStats.GetAverageFrameDuration();
		m_RenderAhead.AddLatency((tickEnd - m_tickSampleStart) / ticksPerMs, (double)m_uHalfRefreshPeriodMs, rtFrameDur / 10000.0);
	}

	m_rtHeld += (REFERENCE_TIME)((GetPreciseTick() - tickStart) * 10000000.0 / GetPreciseTicksPerSecond());
}
