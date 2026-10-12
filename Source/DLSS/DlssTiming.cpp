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

#include "stdafx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include "DlssTiming.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 // Windows 10 1803; the SDK hides it below that target
#endif

// CRollingMs

void CRollingMs::Add(double ms)
{
	m_values[m_next] = (float)ms;
	m_next = (m_next + 1) % kSize;
	if (m_count < kSize) {
		m_count++;
	}
}

double CRollingMs::Mean() const
{
	if (!m_count) {
		return -1.0;
	}
	double sum = 0;
	for (int i = 0; i < m_count; i++) {
		sum += m_values[i];
	}
	return sum / m_count;
}

double CRollingMs::Max() const
{
	if (!m_count) {
		return -1.0;
	}
	return *std::max_element(m_values, m_values + m_count);
}

// CRenderAhead

void CRenderAhead::Reset()
{
	m_latency.Reset();
	m_aheadMs = 0;
	m_ahead.store(0, std::memory_order_relaxed);
	m_late = 0;
}

void CRenderAhead::AddLatency(double latencyMs, double presentLeadMs, double frameMs)
{
	m_latency.Add(latencyMs);

	// Processing starts kRendererAllowanceMs + ahead before the picture's time and
	// the picture is presented presentLeadMs before it, so it is ready in time when
	// ahead >= latency + margin + presentLead - allowance, for the slowest recent picture.
	//
	// The two parts are not bounded alike. A picture cannot start before the one
	// before it has left, about a frame earlier, so more than a frame of drawing is
	// never to be had. How long before its time it is presented is another matter:
	// that only moves the whole film earlier against the clock, the picture before
	// having left that much earlier too. A queue a refresh deep on a screen at 24 Hz
	// wants Present 70 ms before the picture's time; it is asked for, and given.
	const double drawMs = std::min(m_latency.Max() + kMarginMs, frameMs > 0 ? frameMs : kMaxAheadMs);
	const double needMs = std::clamp(drawMs + presentLeadMs - kRendererAllowanceMs, 0.0, kMaxAheadMs);

	if (needMs >= m_aheadMs) {
		m_aheadMs = needMs;
	} else {
		m_aheadMs = std::max(needMs, m_aheadMs - kFallMsPerPicture);
	}
	m_ahead.store((int)(m_aheadMs * 10000.0), std::memory_order_relaxed);
}

// CPreciseSleep

CPreciseSleep::~CPreciseSleep()
{
	if (m_hTimer) {
		CloseHandle(m_hTimer);
	}
}

void CPreciseSleep::Sleep(double ms)
{
	if (ms <= 0) {
		return;
	}
	if (!m_bTried) {
		m_bTried = true;
		m_hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}
	if (m_hTimer) {
		LARGE_INTEGER due;
		due.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)(ms * 10000.0)); // relative, in 100 ns
		if (SetWaitableTimer(m_hTimer, &due, 0, nullptr, nullptr, FALSE)) {
			WaitForSingleObject(m_hTimer, INFINITE);
			return;
		}
	}
	::Sleep((DWORD)std::max(1.0, ms + 0.5));
}

// CPresentSchedule

static double TicksPerMs()
{
	static const double ticks = []() {
		LARGE_INTEGER frequency = {};
		QueryPerformanceFrequency(&frequency);
		return frequency.QuadPart / 1000.0;
	}();
	return ticks;
}

void CPresentSchedule::Reset()
{
	*this = CPresentSchedule();
}

void CPresentSchedule::Flush()
{
	m_bLast = false;
	m_sinceFlush = 0;
}

long long CPresentSchedule::RefreshTime(long long index) const
{
	return m_anchorTick + (long long)std::llround((double)(index - m_anchorIndex) * m_period);
}

double CPresentSchedule::RefreshIndex(long long tick) const
{
	return (double)m_anchorIndex + (double)(tick - m_anchorTick) / m_period;
}

double CPresentSchedule::PeriodMs() const
{
	return m_bClock ? m_period / TicksPerMs() : 0.0;
}

bool CPresentSchedule::Ready(long long tickNow) const
{
	// A refresh time two seconds old is a screen that has not been presented to for as
	// long, and says nothing of where its refreshes are now.
	return m_bClock && m_period > 0 && tickNow - m_anchorTick < (long long)(2000.0 * TicksPerMs());
}

void CPresentSchedule::Seen(unsigned lastPresent, const DXGI_FRAME_STATISTICS& stats, double nominalPeriodMs)
{
	if (!stats.SyncQPCTime.QuadPart || nominalPeriodMs <= 0) {
		return;
	}
	const double nominal = nominalPeriodMs * TicksPerMs();
	const long long tick = stats.SyncQPCTime.QuadPart;

	if (m_bClock && nominalPeriodMs != m_nominalMs) {
		Reset(); // another display mode
	}
	if (m_bClock && stats.SyncRefreshCount != m_lastSync) {
		const int step = (int)(stats.SyncRefreshCount - m_lastSync);
		// A count that goes backwards, or a time that is not where that many refreshes
		// put it, is another count altogether.
		if (step < 0 || std::abs((double)(tick - m_anchorTick) - step * m_period) > 0.5 * m_period) {
			Reset();
		} else {
			m_anchorIndex += step;
			m_anchorTick = tick;
			m_lastSync = stats.SyncRefreshCount;
			// The period, over the last four to eight seconds of refreshes: successive
			// times are good to a few microseconds, and the display mode's own figure
			// is off by tens of parts in a million.
			const long long window = (long long)(4000.0 / nominalPeriodMs);
			if (m_anchorIndex - m_midIndex >= window) {
				if (m_midIndex > m_baseIndex) {
					m_baseIndex = m_midIndex;
					m_baseTick = m_midTick;
				}
				m_midIndex = m_anchorIndex;
				m_midTick = m_anchorTick;
			}
			if (m_anchorIndex - m_baseIndex >= window) {
				const double measured = (double)(m_anchorTick - m_baseTick) / (double)(m_anchorIndex - m_baseIndex);
				if (std::abs(measured - nominal) < 0.01 * nominal) {
					m_period = measured;
				}
			}
		}
	}
	if (!m_bClock) {
		m_bClock = true;
		m_nominalMs = nominalPeriodMs;
		m_period = nominal;
		m_anchorIndex = m_baseIndex = m_midIndex = 0;
		m_anchorTick = m_baseTick = m_midTick = tick;
		m_lastSync = stats.SyncRefreshCount;
	}

	// Which Present reached the screen, and on which refresh.
	const long long shownOn = m_anchorIndex + (int)(stats.PresentRefreshCount - stats.SyncRefreshCount);
	for (int i = 0; i < m_nPending; i++) {
		if (m_pending[i].id != stats.PresentCount) {
			continue;
		}
		const Pending shown = m_pending[i];
		// It and everything presented before it are accounted for.
		m_nPending -= i + 1;
		std::copy(m_pending + i + 1, m_pending + i + 1 + m_nPending, m_pending);

		m_bShownKnown = true;
		m_shownOffsetMs = (double)(RefreshTime(shownOn) - shown.tickDue) / TicksPerMs();
		if (shownOn > shown.refresh && shown.bCounted) {
			m_late++;
		}
		if (shown.bSafe) {
			m_observed[m_nObserved++ % 3] = (int)(shownOn - shown.firstAfter);
			const int depth = m_observed[0];
			if (m_nObserved >= 3 && depth != m_depth && depth == m_observed[1] && depth == m_observed[2]
					&& depth >= 0 && depth <= 4) {
				// Everything chosen on the old depth lands that much further on.
				const int moved = depth - m_depth;
				m_depth = depth;
				m_lastRefresh += moved;
				for (int k = 0; k < m_nPending; k++) {
					m_pending[k].refresh += moved;
				}
			}
		}
		break;
	}
	(void)lastPresent;
}

CPresentSchedule::Plan CPresentSchedule::Choose(long long tickNow, long long tickDue, double frameMs)
{
	Plan plan;
	if (!Ready(tickNow) || frameMs <= 0) {
		return plan;
	}
	const double callPhase = std::min(kCallPhase * m_period, kCallPhaseMaxMs * TicksPerMs());
	const double guard = std::min(kGuardPhase * m_period, kGuardMaxMs * TicksPerMs());

	// Where the picture's time falls among the refreshes, and how many a frame lasts.
	const double x = RefreshIndex(tickDue);
	const double perFrame = frameMs * TicksPerMs() / m_period;
	const long long whole = std::llround(perFrame);
	const bool bWholeStep = whole >= 1 && std::abs(perFrame - (double)whole) < 0.02;
	const long long nearest = (long long)std::floor(x + 0.5);

	long long refresh = nearest;
	bool bDrop = false;
	if (m_bLast && bWholeStep) {
		// The step is kept while the picture stays within reach of its own time.
		refresh = m_lastRefresh + whole;
		const double off = (double)refresh - x;
		const double reach = m_sinceFlush < kSettling ? 0.5 : kRelock;
		if (off > reach) {
			// Shown too long after its time. An earlier refresh if one is free, which
			// there is only when a frame lasts several; otherwise this picture goes,
			// and the next finds itself a refresh nearer its own.
			const long long earlier = std::max(m_lastRefresh + 1, nearest);
			if (earlier < refresh) {
				refresh = earlier;
			} else {
				bDrop = true;
			}
		} else if (off < -reach) {
			refresh = nearest; // too soon: the picture before it keeps the screen longer
		}
	} else if (m_bLast && refresh <= m_lastRefresh) {
		// Never two pictures for one refresh: the next one, then. But a film with more
		// pictures than the screen has refreshes -- 50 fps at 25 Hz, or played at twice
		// its speed -- cannot have one for each, and moving every picture on would
		// send the whole film later and later. One that would be shown more than a
		// refresh after its time is the one that goes.
		refresh = m_lastRefresh + 1;
		if ((double)refresh - x > 1.0) {
			bDrop = true;
		}
	}

	plan.tickWanted = RefreshTime(refresh - m_depth - 1) + (long long)callPhase;
	if (!bDrop && tickNow > RefreshTime(refresh - m_depth) - (long long)guard) {
		// It did not come in time for that refresh. The first it can still have:
		const long long first = (long long)std::ceil(RefreshIndex(tickNow + (long long)guard)) + m_depth;
		// A film has to start with something, however late. After that, a picture more
		// than half a frame behind its time has the next one closer to that refresh
		// than itself: its turn simply passes, and the step is kept.
		if (m_bLast && (double)first - x > std::max(0.5, perFrame / 2)) {
			bDrop = true;
			if (bWholeStep) {
				m_lastRefresh += whole;
			}
		} else {
			refresh = first;
		}
	}

	plan.bValid = true;
	plan.bDrop = bDrop;
	plan.tickDue = tickDue;
	if (bDrop) {
		// The pictures a film loses as it starts are the price of starting on time and
		// are not what the count is for: it says what happened while it played.
		if (m_sinceFlush >= kSettling) {
			m_dropped++;
		}
		return plan;
	}
	plan.bRepeat = m_bLast && bWholeStep && refresh > m_lastRefresh + whole;
	plan.refresh = refresh;
	plan.tickRefresh = RefreshTime(refresh);
	plan.tickCall = std::max(tickNow, RefreshTime(refresh - m_depth - 1) + (long long)callPhase);
	return plan;
}

void CPresentSchedule::Presented(unsigned presentId, const Plan& plan, long long tickCalled, double msInside)
{
	const bool bSettled = m_sinceFlush >= kSettling;
	if (plan.bRepeat && bSettled) {
		m_repeated++;
	}
	m_bLast = true;
	m_lastRefresh = plan.refresh;
	if (m_sinceFlush < kSettling) {
		m_sinceFlush++;
	}
	// Present returns in a fraction of a millisecond when nothing is in its way. One
	// that was waiting for the picture before it to be taken returns when that
	// happens, which is a millisecond or so before the refresh and not on it: called
	// 3 ms after a refresh at 25 Hz, such a Present took 35.5 ms, every time. It went
	// in too late for the refresh it was meant to go in before, and is shown a refresh
	// later than chosen. One that merely took its time -- the driver was busy; 18 ms
	// of it, once in two minutes, on the machine this was measured on -- and still
	// returned well before the refresh went in when it returned, and is on time.
	const double at = RefreshIndex(tickCalled);
	const long long tickReturned = tickCalled + (long long)(msInside * TicksPerMs());
	const double lateGuard = std::min(kGuardPhase * m_period, kGuardMaxMs * TicksPerMs());
	const bool bWaited = msInside > 2.0
		&& (double)tickReturned > (double)RefreshTime((long long)std::ceil(at)) - lateGuard;
	long long shownOn = plan.refresh;
	if (bWaited) {
		// It goes in as Present returns and takes the first refresh it can from there.
		// The pictures that follow keep their step from that one: a refresh later than
		// they were, all of them, and none waits again -- or, when that puts them too
		// far from their own time, one of them is left out and the rest are back where
		// they were. Which of the two is the ordinary choice in Choose.
		shownOn = std::max(plan.refresh + 1,
			(long long)std::ceil(RefreshIndex(tickReturned + (long long)lateGuard)) + m_depth);
		m_lastRefresh = shownOn;
		if (bSettled) {
			m_late++;
		}
	}

	if (m_nPending == kPending) {
		// DXGI has stopped saying what reached the screen; the oldest go unaccounted.
		std::copy(m_pending + 1, m_pending + kPending, m_pending);
		m_nPending--;
	}
	Pending& pending = m_pending[m_nPending++];
	pending.id = presentId;
	pending.refresh = plan.refresh;
	pending.tickDue = plan.tickDue;
	pending.firstAfter = (long long)std::ceil(at);
	// Told apart from the depth of the queue only when it went in well clear of both
	// refreshes, which is when it returned and not when it was called.
	const double inAt = RefreshIndex(tickReturned);
	const double sinceRefresh = (inAt - std::floor(inAt)) * m_period;
	const double guard = std::min(kGuardPhase * m_period, kGuardMaxMs * TicksPerMs());
	pending.bSafe = !bWaited && sinceRefresh > 0.02 * m_period && m_period - sinceRefresh > guard;
	pending.refresh = shownOn; // where it will really be: not late a second time when DXGI says so
	pending.bCounted = bSettled;
}

// CPacingTrace

CPacingTrace::CPacingTrace()
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetEnvironmentVariableW(L"MPCVR_PACING_TRACE", path, MAX_PATH);
	if (length > 0 && length < MAX_PATH) {
		m_path = path;
		m_bEnabled = true;
		m_pictures.reserve(16384); // eleven minutes of film before the list has to grow
	}
}

void CPacingTrace::Note(const std::wstring& text)
{
	if (m_bEnabled) {
		// A note is about the pictures that follow it, so the ones before go out first.
		Write();
		m_notes.push_back(text);
	}
}

void CPacingTrace::Add(const Picture& picture)
{
	if (m_bEnabled) {
		m_pictures.push_back(picture);
	}
}

void CPacingTrace::Write()
{
	if (!m_bEnabled || (m_pictures.empty() && m_notes.empty())) {
		return;
	}
	FILE* file = nullptr;
	if (_wfopen_s(&file, m_path.c_str(), L"a") != 0 || !file) {
		m_pictures.clear();
		m_notes.clear();
		return;
	}
	if (!m_written) {
		LARGE_INTEGER frequency = {};
		QueryPerformanceFrequency(&frequency);
		fprintf(file, "# MPC Video Renderer pacing trace, process %lu, %lld ticks per second\n",
			GetCurrentProcessId(), frequency.QuadPart);
		fprintf(file, "# n,rtStart,rtClockAtCall,rtClockAtReturn,tickSample,tickDrawn,tickAfterVBlank,tickPresentCall,"
			"tickPresentReturn,tickTarget,renderAhead,flags,"
			"lastPresent0,presentCount0,presentRefresh0,syncRefresh0,syncQpc0,"
			"lastPresent1,presentCount1,presentRefresh1,syncRefresh1,syncQpc1\n");
	}
	for (const std::wstring& note : m_notes) {
		fprintf(file, "# %ls\n", note.c_str());
	}
	for (const Picture& p : m_pictures) {
		fprintf(file, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%u,%u,%u,%u,%u,%lld,%u,%u,%u,%u,%lld\n",
			m_written++, p.rtStart, p.rtClockAtCall, p.rtClockAtReturn, p.tickSample, p.tickDrawn, p.tickAfterVBlank,
			p.tickPresentCall, p.tickPresentReturn, p.tickTarget, p.renderAhead, p.flags,
			p.before.lastPresent, p.before.presentCount, p.before.presentRefresh, p.before.syncRefresh, p.before.syncQpc,
			p.after.lastPresent, p.after.presentCount, p.after.presentRefresh, p.after.syncRefresh, p.after.syncQpc);
	}
	fclose(file);
	m_pictures.clear();
	m_notes.clear();
}

// CGpuStageTimes

void CGpuStageTimes::Release()
{
	for (auto& set : m_sets) {
		set = QuerySet{};
	}
	m_iRecording = -1;
	m_bFailed = false;
	for (auto& times : m_times) {
		times.Reset();
	}
}

void CGpuStageTimes::BeginFrame(ID3D11Device* pDevice, ID3D11DeviceContext* pContext)
{
	if (m_iRecording >= 0) {
		EndFrame(pContext);
	}
	if (m_bFailed) {
		return;
	}
	m_pictures++;

	for (int i = 0; i < kSets; i++) {
		QuerySet& set = m_sets[i];
		if (set.bPending) {
			continue;
		}
		if (!set.pDisjoint) {
			D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			bool bOk = SUCCEEDED(pDevice->CreateQuery(&desc, &set.pDisjoint));
			desc.Query = D3D11_QUERY_TIMESTAMP;
			for (int s = 0; s < StageCount && bOk; s++) {
				bOk = SUCCEEDED(pDevice->CreateQuery(&desc, &set.pBegin[s]))
					&& SUCCEEDED(pDevice->CreateQuery(&desc, &set.pEnd[s]));
			}
			if (!bOk) {
				set = QuerySet{};
				m_bFailed = true;
				return;
			}
		}
		std::fill(std::begin(set.bOpen), std::end(set.bOpen), false);
		std::fill(std::begin(set.bUsed), std::end(set.bUsed), false);
		set.picture = m_pictures;
		pContext->Begin(set.pDisjoint);
		m_iRecording = i;
		return;
	}
	// Every set is still with the GPU: this picture goes untimed.
}

void CGpuStageTimes::Begin(ID3D11DeviceContext* pContext, Stage stage)
{
	if (m_iRecording >= 0) {
		QuerySet& set = m_sets[m_iRecording];
		pContext->End(set.pBegin[stage]);
		set.bOpen[stage] = true;
	}
}

void CGpuStageTimes::End(ID3D11DeviceContext* pContext, Stage stage)
{
	if (m_iRecording >= 0 && m_sets[m_iRecording].bOpen[stage]) {
		QuerySet& set = m_sets[m_iRecording];
		pContext->End(set.pEnd[stage]);
		set.bOpen[stage] = false;
		set.bUsed[stage] = true;
	}
}

void CGpuStageTimes::EndFrame(ID3D11DeviceContext* pContext)
{
	if (m_iRecording < 0) {
		return;
	}
	QuerySet& set = m_sets[m_iRecording];
	pContext->End(set.pDisjoint);
	set.bPending = true;
	m_iRecording = -1;
}

void CGpuStageTimes::Collect(ID3D11DeviceContext* pContext)
{
	for (;;) {
		// Oldest first: a later set is never ready before an earlier one.
		QuerySet* pSet = nullptr;
		for (auto& set : m_sets) {
			if (set.bPending && (!pSet || set.picture < pSet->picture)) {
				pSet = &set;
			}
		}
		if (!pSet) {
			return;
		}

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
		const HRESULT hr = pContext->GetData(pSet->pDisjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (hr == S_FALSE) {
			return;
		}
		if (hr == S_OK && !disjoint.Disjoint && disjoint.Frequency) {
			for (int s = 0; s < StageCount; s++) {
				UINT64 t0 = 0, t1 = 0;
				if (pSet->bUsed[s]
						&& S_OK == pContext->GetData(pSet->pBegin[s], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH)
						&& S_OK == pContext->GetData(pSet->pEnd[s], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH)
						&& t1 >= t0) {
					m_times[s].Add((double)(t1 - t0) * 1000.0 / (double)disjoint.Frequency);
					m_lastSeen[s] = pSet->picture;
				}
			}
		}
		pSet->bPending = false;
	}
}

double CGpuStageTimes::MeanMs(Stage stage) const
{
	if (!m_times[stage].Count() || m_pictures - m_lastSeen[stage] > kStalePictures) {
		return -1.0;
	}
	return m_times[stage].Mean();
}
