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
	const int cadenceSlips = m_cadenceSlips;
	const unsigned timesOffGrid = m_timesOffGrid;
	*this = CPresentSchedule();
	m_cadenceSlips = cadenceSlips;
	m_timesOffGrid = timesOffGrid;
}

void CPresentSchedule::Flush()
{
	m_bLast = false;
	m_sinceFlush = 0;
	m_dropRun = 0;
	m_bDue = false;
	m_lastGap = 0;
	m_bSlipWaiting = false;
	m_bSlipNext = false;
}

void CPresentSchedule::SetCadenceSlips(const int mode)
{
	const int clamped = std::clamp(mode, (int)CadenceSlipsWhereTheyFall, (int)CadenceSlipsOnCutOrMade);
	if (clamped != m_cadenceSlips) {
		m_cadenceSlips = clamped;
		m_bSlipWaiting = false;
		m_bSlipNext = false;
	}
}

long long CPresentSchedule::SmoothDue(long long tickStamp, double frameMs)
{
	const double frameTicks = frameMs * TicksPerMs();
	double due = (double)tickStamp;
	if (m_bDue && frameTicks > 0) {
		// How many frames on from the last picture: one, or more when samples were left out.
		const double frames = std::round((double)(tickStamp - m_dueStamp) / frameTicks);
		if (frames >= 1 && frames <= 8) {
			const double carried = m_dueSmooth + frames * frameTicks;
			const double error = (double)tickStamp - carried;
			if (std::abs(error) < kDueResetMs * TicksPerMs()) {
				due = carried + kDueGain * error;
			}
		}
	}
	m_bDue = true;
	m_dueSmooth = due;
	m_dueStamp = tickStamp;
	return std::llround(due);
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

int CPresentSchedule::Seen(unsigned lastPresent, const DXGI_FRAME_STATISTICS& stats, double nominalPeriodMs)
{
	if (!stats.SyncQPCTime.QuadPart || nominalPeriodMs <= 0) {
		return 0;
	}
	const double nominal = nominalPeriodMs * TicksPerMs();
	const long long tick = stats.SyncQPCTime.QuadPart;
	int astray = 0;

	if (m_bClock && nominalPeriodMs != m_nominalMs) {
		Reset(); // another display mode
	}
	bool bShownSure = true;   // the refresh this reading puts the last Present on can be relied on
	if (m_bClock && stats.SyncRefreshCount != m_lastSync) {
		// The refresh count and its time, each against the grid of refreshes found so far.
		// Either can go astray on its own, and the one that agrees with the grid is gone by:
		//
		// - The count, its time on the grid. In MPC-HC at 60 Hz on an RTX 4060, full screen,
		//   it lost a refresh five times in one second and kept that offset for the minute
		//   and a half that followed, its times exact all along. Taken for another count
		//   altogether, each reset the schedule, its cadence and the queue's depth with it,
		//   and the screen showed a second of 3,3s and 2,2s. A count off the grid by the same
		//   refreshes twice in a row is taken as astray and the times are gone by; once, it
		//   may be a time that happens to fall a whole refresh late.
		// - The time, its count right. In a window composed by the desktop, the same machine
		//   gave a third of its times 1 to 16 ms after the refresh its count named, in runs
		//   of up to forty readings: the moment the desktop composed, not the refresh. Each
		//   taken as the grid moved the refreshes the schedule aims at by as much, pictures
		//   went on the wrong refresh, and the statistics' graph showed it. Such a time is
		//   left out, the count followed and the grid kept; kRegrid in a row off it by the
		//   same amount are the refreshes truly moved, and the grid moves with them.
		//
		// A count that goes back, or a time neither on the grid nor where its count puts it
		// give or take a refresh, is another count altogether: the schedule starts again.
		const long long step = (long long)(int)(stats.SyncRefreshCount - m_lastSync);
		const double periods = (double)(tick - m_anchorTick) / m_period;
		const long long whole = std::llround(periods);
		const double phase = periods - (double)whole;
		const double tol = std::max(kOnGridPhase, kOnGridMinMs * TicksPerMs() / m_period);
		const bool bOnGrid = std::abs(phase) < tol;
		long long advance = -1;   // refreshes the anchor moves on; -1: start again
		bool bTimed = false;      // the anchor takes this reading's time
		if (step < 0) {
			advance = -1;
		} else if (bOnGrid && whole == step) {
			advance = step;
			bTimed = true;
			m_bCountDoubt = false;
			m_offGridRun = 0;
		} else if (bOnGrid) {
			m_offGridRun = 0;
			if (m_bCountDoubt && whole - step == m_countDoubtBy) {
				advance = whole;
				bTimed = true;
				astray = (int)(step - whole);
				m_bCountDoubt = false;
			} else {
				// Once: the anchor stays where it was, the count is not taken yet.
				m_bCountDoubt = true;
				m_countDoubtBy = whole - step;
				advance = 0;
				bShownSure = false;
			}
		} else if (periods - (double)step > -0.5 && periods - (double)step < 1.5) {
			m_bCountDoubt = false;
			m_timesOffGrid++;
			if (m_offGridRun > 0 && std::abs(phase - m_offGridPhase) < tol) {
				m_offGridRun++;
			} else {
				m_offGridRun = 1;
				m_offGridPhase = phase;
			}
			advance = step;
			bTimed = m_offGridRun >= kRegrid;   // the refreshes moved: the grid follows
			if (bTimed) {
				advance = whole;
				m_offGridRun = 0;
			}
		}
		if (advance < 0) {
			Reset();
		} else if (advance == 0 && !bTimed) {
			// The count not taken: read again from the same anchor.
		} else {
			m_anchorIndex += advance;
			m_anchorTick = bTimed ? tick : m_anchorTick + std::llround((double)advance * m_period);
			m_lastSync = stats.SyncRefreshCount;
			if (bTimed && m_offGridRun == 0 && std::abs(phase) >= tol) {
				// A grid moved: the period is measured afresh from here.
				m_baseIndex = m_midIndex = m_anchorIndex;
				m_baseTick = m_midTick = m_anchorTick;
			}
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

	// Which Present reached the screen, and on which refresh: not from a count in doubt.
	const long long shownOn = m_anchorIndex + (int)(stats.PresentRefreshCount - stats.SyncRefreshCount);
	for (int i = 0; i < m_nPending && bShownSure; i++) {
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
			if (shown.bReady) {
				LateReady(shownOn);
			}
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
	return astray;
}

CPresentSchedule::Plan CPresentSchedule::Choose(long long tickNow, long long tickStamp, double frameMs, double drawMs)
{
	Plan plan;
	if (!Ready(tickNow) || frameMs <= 0) {
		m_bDue = false;
		return plan;
	}
	const long long tickDue = SmoothDue(tickStamp, frameMs);
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
	long long slotAfter = 0;   // were this one left out: the refresh the next would be given
	// Cadence slips placed (SetCadenceSlips), once the film has settled. The limit is the
	// picture shown kSlipLimitMs before its time, or kRelock of a refresh if that is more.
	const bool bPlace = m_cadenceSlips != CadenceSlipsWhereTheyFall && m_sinceFlush >= kSettling;
	const double slipLimit = std::max(kRelock, kSlipLimitMs * TicksPerMs() / m_period);
	const bool bMayMake = m_cadenceSlips == CadenceSlipsOnCutOrMade;
	bool bSlipIfCut = false, bSlipNextIfCut = false, bDropIfCut = false, bSlipForced = false, bSlipMade = false, bSlipOnCut = false;
	const bool bSlipNow = m_bSlipNext;   // the picture before opened a shot on a 3
	m_bSlipNext = false;
	const bool bNear32 = std::abs(perFrame / 2.5 - (1.0 + kNearCadence)) <= kNearCadenceTol;
	m_bSlipWaiting = false;
	m_bSlipCadence = m_cadenceSlips != CadenceSlipsWhereTheyFall && (bWholeStep || bNear32);
	if (m_bLast && bWholeStep) {
		// The step is kept while the picture stays within reach of its own time.
		refresh = m_lastRefresh + whole;
		const double off = (double)refresh - x;
		const double reach = m_sinceFlush < kSettling ? 0.5 : kRelock;
		const double reachEarly = bPlace ? slipLimit : reach;
		if (off > reach) {
			// Shown too long after its time. An earlier refresh if one is free, which
			// there is only when a frame lasts several; otherwise this picture goes,
			// and the next finds itself a refresh nearer its own.
			const long long earlier = std::max(m_lastRefresh + 1, nearest);
			if (earlier < refresh) {
				refresh = earlier;
			} else {
				bDrop = true;
				slotAfter = refresh;
			}
		} else if (off < -reachEarly) {
			refresh = nearest; // too soon: the picture before it keeps the screen longer
			if (bPlace) {
				bSlipForced = true;
				bSlipMade = bMayMake && refresh == m_lastRefresh + whole + 1;
			}
		} else if (bPlace && off <= -0.5) {
			// Owed a refresh more: on a cut, or at the limit.
			bSlipIfCut = true;
			m_bSlipWaiting = true;
		} else if (bPlace && whole == 1 && off >= 0.5) {
			// Owed a picture less, at a film's own rate: on a cut, or at kRelock as ever.
			// Only where the next one can be drawn in time for this one's refresh, from the
			// moment the cut is known: this picture's own turn to be presented.
			const long long tickDecided = std::max(tickNow, RefreshTime(refresh - m_depth - 1) + (long long)callPhase);
			const long long tickNextReady = tickDecided + (long long)(drawMs * TicksPerMs());
			bDropIfCut = m_dropRun < kMaxDropRun
				&& tickNextReady <= RefreshTime(refresh - m_depth) - (long long)guard;
			m_bSlipWaiting = bDropIfCut;
		}
	} else if (m_bLast && bPlace && (m_lastGap == 2 || m_lastGap == 3) && bNear32) {
		// A 3:2 cadence on a screen a little faster than it: the 3 and 2 kept by turns past
		// the point where the nearest refresh would give a 3,3, the slip -- always a 2 made a
		// 3, never a 3 made a 4, but for a made picture that needs a 3 -- taken on a cut from
		// kSlipEarly of a refresh before that point, or at the limit.
		const long long gap = m_lastGap == 3 ? 2 : 3;
		const long long held = m_lastRefresh + gap;
		const double e = (double)held - x;
		// From the Present of the picture before the slip to the moment the made picture has
		// to be ready, after a 2 (one refresh fewer after a 3): the picture after it drawn, and
		// the made one made, in that time.
		const double periodMs = m_period / TicksPerMs();
		const double madeSpanMs = 3.0 * periodMs - (callPhase + guard) / TicksPerMs();
		const bool bMadeOnThree = bMayMake && m_madeMs > 0.0 && drawMs + m_madeMs > madeSpanMs
			&& drawMs + m_madeMs <= madeSpanMs + periodMs;
		if (e < -2.0 || e > 1.5) {
			// Nowhere near the cadence (a clock that jumped): the nearest refresh again.
			refresh = std::max(nearest, m_lastRefresh + 1);
		} else {
			refresh = held;
			if (gap == 2) {
				if (bSlipNow && e < -0.5 + kSlipEarly) {
					// The picture before opened a shot: this one, its second, is the slip.
					refresh = held + 1;
					bSlipOnCut = true;
				} else if (e < -0.5 - kSlipEarly) {
					refresh = held + 1;
					bSlipForced = true;
					bSlipMade = bMayMake;
				} else if (e < -0.5 + kSlipEarly) {
					bSlipIfCut = true;
					m_bSlipWaiting = true;
				}
			} else if (bMadeOnThree && (double)held + 2.0 - (x + perFrame) < -0.5 - kSlipEarly) {
				// A 3, and the 2 after it would be forced with too little time for its made
				// picture: the slip here, the 3 made a 4 and the made picture on its fourth
				// refresh.
				refresh = held + 1;
				bSlipForced = true;
				bSlipMade = true;
			} else if ((double)held + 2.0 - (x + perFrame) < -0.5 + kSlipEarly) {
				// A 3, with the 2 after it in the window: a cut here slips that one.
				bSlipNextIfCut = true;
				m_bSlipWaiting = true;
			}
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
			slotAfter = refresh;
		}
	}

	const long long planned = refresh;
	plan.tickWanted = RefreshTime(refresh - m_depth - 1) + (long long)callPhase;
	// The first refresh it can still have, drawn as it is now. A film has to start
	// with something, however late. After that, a picture more than half a frame
	// behind its time has the next one closer to that refresh than itself.
	const long long first = (long long)std::ceil(RefreshIndex(tickNow + (long long)guard)) + m_depth;
	bool bBehindDrop = false;
	if (!bDrop && tickNow > RefreshTime(refresh - m_depth) - (long long)guard) {
		// It did not come in time for the refresh chosen: its turn simply passes and the
		// step is kept, or it takes the first it can.
		if (m_bLast && (double)first - x > std::max(0.5, perFrame / 2)) {
			bDrop = bBehindDrop = true;
			slotAfter = bWholeStep ? refresh + whole
				: std::max(m_lastRefresh + 1, (long long)std::floor(x + perFrame + 0.5));
		} else {
			refresh = first;
		}
	}
	if (bDrop) {
		// Is the next one in time for its refresh, with this one left out? (kMaxDropRun)
		const long long tickNextReady = tickNow + (long long)(drawMs * TicksPerMs());
		const bool bWorks = tickNextReady <= RefreshTime(slotAfter - m_depth) - (long long)guard;
		if (!bWorks || m_dropRun >= kMaxDropRun) {
			bDrop = bBehindDrop = false;
			refresh = std::max({ refresh, first, m_lastRefresh + 1 });
			plan.bSkipNext = !bWorks;
		}
	}
	if (bBehindDrop && bWholeStep) {
		m_lastRefresh += whole;
	}

	plan.bValid = true;
	plan.bDrop = bDrop;
	plan.tickDue = tickDue;
	plan.tickStamp = tickStamp;
	if (bDrop) {
		m_dropRun++;
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
	// A slip planned for a refresh this picture no longer has (it came late) is not one.
	if (refresh == planned) {
		plan.bSlipIfCut = bSlipIfCut;
		plan.bSlipNextIfCut = bSlipNextIfCut;
		plan.bDropIfCut = bDropIfCut;
		plan.bSlipForced = bSlipForced;
		plan.bSlipMade = bSlipMade;
		plan.bSlipOnCut = bSlipOnCut;
	}
	return plan;
}

void CPresentSchedule::ApplyCut(Plan& plan, const bool bCut)
{
	if (!bCut || !plan.bValid || plan.bDrop || plan.bBetween) {
		return;
	}
	if (plan.bSlipIfCut) {
		const long long period = (long long)m_period;
		plan.refresh += 1;
		plan.tickRefresh = RefreshTime(plan.refresh);
		plan.tickCall += period;
		plan.tickWanted += period;
		plan.bSlipIfCut = false;
		plan.bSlipOnCut = true;
		m_bSlipWaiting = false;
	} else if (plan.bSlipNextIfCut) {
		// The cut opened on a 3: the next picture takes the slip (Choose).
		plan.bSlipNextIfCut = false;
		m_bSlipNext = true;
		m_bSlipWaiting = false;
	} else if (plan.bDropIfCut) {
		plan.bDrop = true;
		plan.bDropIfCut = false;
		plan.bSlipOnCut = true;
		m_bSlipWaiting = false;
		m_dropRun++;
		if (m_sinceFlush >= kSettling) {
			m_dropped++;
			m_slipsOnCut++;
		}
	}
}

CPresentSchedule::Plan CPresentSchedule::MadePlan(const long long tickNow, const Plan& next, double& t) const
{
	Plan made;
	t = 0.5;
	if (!next.bValid || next.bDrop || !next.bSlipForced || !next.bSlipMade || !m_bLast) {
		return made;
	}
	// The refresh the slip adds is the one before the drawn picture's own.
	const long long refresh = next.refresh - 1;
	if (refresh <= m_lastRefresh) {
		return made;
	}
	const double callPhase = std::min(kCallPhase * m_period, kCallPhaseMaxMs * TicksPerMs());
	const double guard = std::min(kGuardPhase * m_period, kGuardMaxMs * TicksPerMs());
	if (tickNow > RefreshTime(refresh - m_depth) - (long long)guard) {
		return made; // too late for it: the slip stays a plain one
	}
	// Each picture goes on the refresh nearest its own time, so a refresh shows the film as
	// it is at that refresh's moment: the made picture is the film at the moment of the
	// refresh it is for, between the two pictures' own times.
	const double span = (double)(next.tickDue - m_lastDue);
	if (span <= 0) {
		return made;
	}
	t = std::clamp((double)(RefreshTime(refresh) - m_lastDue) / span, 0.05, 0.95);
	made.bValid = true;
	made.bBetween = true;
	made.refresh = refresh;
	made.tickRefresh = RefreshTime(refresh);
	made.tickWanted = RefreshTime(refresh - m_depth - 1) + (long long)callPhase;
	made.tickCall = std::max(tickNow, made.tickWanted);
	made.tickDue = m_lastDue + (long long)(t * span);
	made.tickStamp = made.tickDue;
	return made;
}

long long CPresentSchedule::ReadyBy(const Plan& plan) const
{
	const double guard = std::min(kGuardPhase * m_period, kGuardMaxMs * TicksPerMs());
	return RefreshTime(plan.refresh - m_depth) - (long long)guard;
}

double CPresentSchedule::StuckSpacing() const
{
	if (m_nominalMs <= 0) {
		return 0.0;
	}
	// The display mode's own rate, as Windows' display configuration gives it (23.976 Hz)
	// and not what the refreshes measure, so that a mode of 24 Hz measured a little under it
	// is still a whole number.
	const double hz = 1000.0 / m_nominalMs;
	const double above = hz - std::floor(hz);
	return above < 0.02 ? 0.0 : hz / above;
}

void CPresentSchedule::LateReady(const long long refresh)
{
	if (m_bLateGroup && refresh - m_lateLast <= kStuckGroup) {
		m_lateLast = refresh;
		return;
	}
	const double spacing = StuckSpacing();
	const double since = (double)(refresh - m_lateStart);
	if (m_bLateGroup && spacing > 0 && std::abs(since - spacing) <= kStuckTolerance * spacing) {
		m_stuckRun++;
	} else {
		m_stuckRun = 1;
	}
	m_bLateGroup = true;
	m_lateStart = m_lateLast = refresh;
}

bool CPresentSchedule::FlipsStuck()
{
	if (m_stuckRun < kStuckRun) {
		return false;
	}
	m_stuckRun = 0;
	return true;
}

void CPresentSchedule::LeftOut()
{
	if (m_sinceFlush >= kSettling) {
		m_dropped++;
	}
}

void CPresentSchedule::Presented(unsigned presentId, const Plan& plan, long long tickCalled, double msInside, bool bReady)
{
	const bool bSettled = m_sinceFlush >= kSettling;
	if (plan.bBetween) {
		// A made picture between two drawn ones: on the screen like any, but the step, the
		// cadence and the counts go by the drawn pictures alone.
		if (bSettled) {
			m_madeShown++;
		}
	} else {
		if (plan.bRepeat && bSettled && !plan.bSlipOnCut && !plan.bSlipForced) {
			m_repeated++;
		}
		if (bSettled) {
			m_slipsOnCut += plan.bSlipOnCut ? 1 : 0;
			m_slipsForced += plan.bSlipForced ? 1 : 0;
		}
		m_lastGap = m_bLast ? plan.refresh - m_lastRefresh : 0;
		if (m_lastGap == 4 && plan.bSlipForced) {
			m_lastGap = 3;   // a 3 made a 4 for a made picture: the picture after it is a 2
		}
		m_lastDue = plan.tickDue;
		m_bLast = true;
		m_lastRefresh = plan.refresh;
		m_dropRun = 0;
		if (m_sinceFlush < kSettling) {
			m_sinceFlush++;
		}
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
		if (!plan.bBetween) {
			m_lastRefresh = shownOn;
			m_lastGap = 0; // the cadence is found again from the nearest refresh
			if (bSettled) {
				m_late++;
			}
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
	pending.bCounted = bSettled && !plan.bBetween;
	pending.bReady = bReady;
}

// CPacingTrace

CPacingTrace::CPacingTrace()
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetEnvironmentVariableW(L"MPCVR_PACING_TRACE", path, MAX_PATH);
	if (length > 0 && length < MAX_PATH) {
		m_path = path;
		m_bEnabled = true;
		m_pictures.reserve(1024 + 64); // written out every 1024 (Add)
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
		// Out to the file every forty seconds or so at 24 fps, so that a trace can be read
		// while the film plays: what goes wrong after half an hour is watched for, not waited
		// for until the player closes.
		if (m_pictures.size() >= 1024 + 32) {
			Write(32);
		}
	}
}

void CPacingTrace::SetCut(long long rtStart, const float* state)
{
	// A few pictures back at most: those the GPU had not yet run when they were added.
	const size_t n = m_pictures.size();
	for (size_t i = 0; i < n && i < 32; i++) {
		Picture& p = m_pictures[n - 1 - i];
		if (p.rtStart == rtStart && p.cutHist < 0) {
			TakeCut(p, state);
			if (state[0] > 0.5f) {
				p.flags |= SceneCut;
			}
			return;
		}
	}
}

void CPacingTrace::TakeCut(Picture& p, const float* state)
{
	p.cutHist  = (int)std::lround(std::clamp(state[1], 0.0f, 2.0f) * 1000.0f);
	p.cutResid = (int)std::lround(std::clamp(state[2], 0.0f, 1.0f) * 1e6f);
	p.cutRatio = (int)std::lround(std::clamp(state[3], 0.0f, 1e7f) * 10.0f);
	p.cutCorr  = (int)std::lround(std::clamp(state[14], -1.0f, 1.0f) * 1000.0f);
}

void CPacingTrace::SetGpu(long long rtStart, float vpMs, float allMs)
{
	const size_t n = m_pictures.size();
	for (size_t i = 0; i < n && i < 32; i++) {
		Picture& p = m_pictures[n - 1 - i];
		if (p.rtStart == rtStart && p.gpuAllUs < 0) {
			p.gpuVpUs = vpMs >= 0 ? (int)std::lround(vpMs * 1000.0f) : -1;
			p.gpuAllUs = allMs >= 0 ? (int)std::lround(allMs * 1000.0f) : -1;
			return;
		}
	}
}

void CPacingTrace::Write(const size_t keep)
{
	if (!m_bEnabled || (m_pictures.empty() && m_notes.empty())) {
		return;
	}
	const size_t count = m_pictures.size() > keep ? m_pictures.size() - keep : 0;
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
			"lastPresent1,presentCount1,presentRefresh1,syncRefresh1,syncQpc1,"
			"nrWaitUs,nrRecordUs,nrGpuUs,cutHist,cutRatio,cutResid,cutCorr,tickGpuDone,tickStamp,tickDue,gpuVpUs,gpuAllUs\n");
	}
	for (const std::wstring& note : m_notes) {
		fprintf(file, "# %ls\n", note.c_str());
	}
	for (size_t i = 0; i < count; i++) {
		const Picture& p = m_pictures[i];
		fprintf(file, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%u,%u,%u,%u,%u,%lld,%u,%u,%u,%u,%lld,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%lld,%d,%d\n",
			m_written++, p.rtStart, p.rtClockAtCall, p.rtClockAtReturn, p.tickSample, p.tickDrawn, p.tickAfterVBlank,
			p.tickPresentCall, p.tickPresentReturn, p.tickTarget, p.renderAhead, p.flags,
			p.before.lastPresent, p.before.presentCount, p.before.presentRefresh, p.before.syncRefresh, p.before.syncQpc,
			p.after.lastPresent, p.after.presentCount, p.after.presentRefresh, p.after.syncRefresh, p.after.syncQpc,
			p.nrWaitUs, p.nrRecordUs, p.nrGpuUs, p.cutHist, p.cutRatio, p.cutResid, p.cutCorr,
			p.tickGpuDone, p.tickStamp, p.tickDue, p.gpuVpUs, p.gpuAllUs);
	}
	fclose(file);
	m_pictures.erase(m_pictures.begin(), m_pictures.begin() + count);
	m_notes.clear();
}

// CCutReadback

void CCutReadback::Release()
{
	for (Slot& slot : m_slots) {
		slot = Slot{};
	}
}

void CCutReadback::Copy(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Buffer* pState, long long rtStart)
{
	if (!pDevice || !pContext || !pState) {
		return;
	}
	for (Slot& slot : m_slots) {
		if (slot.bPending) {
			continue;
		}
		if (!slot.pStaging) {
			D3D11_BUFFER_DESC desc = {};
			pState->GetDesc(&desc);
			desc.Usage          = D3D11_USAGE_STAGING;
			desc.BindFlags      = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags      = 0;
			if (FAILED(pDevice->CreateBuffer(&desc, nullptr, &slot.pStaging))) {
				return;
			}
		}
		pContext->CopyResource(slot.pStaging, pState);
		slot.rtStart = rtStart;
		slot.order = m_copies++;
		slot.bPending = true;
		return;
	}
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

void CGpuStageTimes::BeginFrame(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, long long tag)
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
		set.tag = tag;
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
			Measured measured = { pSet->tag, -1.0f, -1.0f };
			for (int s = 0; s < StageCount; s++) {
				UINT64 t0 = 0, t1 = 0;
				if (pSet->bUsed[s]
						&& S_OK == pContext->GetData(pSet->pBegin[s], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH)
						&& S_OK == pContext->GetData(pSet->pEnd[s], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH)
						&& t1 >= t0) {
					const double ms = (double)(t1 - t0) * 1000.0 / (double)disjoint.Frequency;
					m_times[s].Add(ms);
					m_lastSeen[s] = pSet->picture;
					if (s == VP) {
						measured.vpMs = (float)ms;
					}
					measured.allMs = std::max(measured.allMs, 0.0f) + (float)ms;
				}
			}
			if (m_nMeasured == kMeasured) {
				std::copy(m_measured + 1, m_measured + kMeasured, m_measured);
				m_nMeasured--;
			}
			m_measured[m_nMeasured++] = measured;
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
