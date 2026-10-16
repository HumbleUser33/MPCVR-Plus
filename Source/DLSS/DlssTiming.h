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

#pragma once

#include <d3d11.h>
#include <atlbase.h>
#include <atomic>
#include <string>
#include <vector>

// Mean and maximum of the last few values, in ms.
class CRollingMs
{
public:
	void Reset() { m_count = 0; m_next = 0; }
	void Add(double ms);
	int Count() const { return m_count; }
	double Mean() const; // negative when empty
	double Max() const;  // negative when empty

private:
	static constexpr int kSize = 32; // about 1.3 s of film

	float m_values[kSize] = {};
	int m_count = 0;
	int m_next = 0;
};

// CRenderAhead
//
// The DirectShow renderer wakes up 8 ms before a picture's time and only then
// processes it, so whatever the DLSS passes take beyond those 8 ms reaches the
// screen late: 11 to 19 ms of Sync offset with DLSS 5 NR and SR on an RTX 4060,
// plus DLSS SR's own GPU time, which runs after Present has returned. The delay
// also varies from picture to picture, enough to move some of them to the next
// refresh of a 60 Hz screen.
//
// Render ahead measures each picture from the start of its processing to the GPU
// being done with it, starts the next ones that much earlier, and holds each
// picture until its time while the GPU finishes it: presentation stays on the
// reference clock, where it was before DLSS. A picture the GPU has not finished by
// then counts as late and moves the start earlier. The measure rises at once and
// falls slowly, so one slow picture moves the start forward straight away and a
// quiet stretch does not pull it back too eagerly.

class CRenderAhead
{
public:
	// CBaseVideoRenderer2 already schedules every picture 8 ms early.
	static constexpr double kRendererAllowanceMs = 8.0;
	// Spare time on top of the slowest recent picture.
	static constexpr double kMarginMs = 3.0;
	// How fast the measure falls back when pictures get quicker.
	static constexpr double kFallMsPerPicture = 0.25;
	// Never earlier than this. Three refreshes at 24 Hz and a frame of drawing fit in it.
	static constexpr double kMaxAheadMs = 250.0;

	void Reset();

	// One picture: latencyMs from the start of its processing to the GPU being done
	// with it, or to its present time when the GPU was not done by then.
	// presentLeadMs is how long before its time the renderer presents a picture
	// (half a refresh), frameMs the duration of a frame.
	void AddLatency(double latencyMs, double presentLeadMs, double frameMs);
	// The GPU was not done with a picture at its present time.
	void CountLate() { m_late++; }

	// How much earlier than usual processing starts, in 100 ns units.
	int GetAhead() const { return m_ahead.load(std::memory_order_relaxed); }

	// For the statistics.
	double MeanLatencyMs() const { return m_latency.Mean(); }
	double MaxLatencyMs() const { return m_latency.Max(); }
	int LateCount() const { return m_late; }

private:
	CRollingMs m_latency;
	double m_aheadMs = 0;
	std::atomic<int> m_ahead = 0;
	int m_late = 0;
};

// Sleeps to about a millisecond whatever the system timer resolution, through a
// high-resolution waitable timer (Windows 10 1803 and later), Sleep() otherwise.
class CPreciseSleep
{
public:
	CPreciseSleep() = default;
	~CPreciseSleep();
	CPreciseSleep(const CPreciseSleep&) = delete;
	CPreciseSleep& operator=(const CPreciseSleep&) = delete;

	void Sleep(double ms);

private:
	HANDLE m_hTimer = nullptr;
	bool m_bTried = false;
};

// CPresentSchedule
//
// Which refresh each picture is for, and when to call Present so that it gets it.
//
// A renderer that presents a picture when its time comes leaves the rest to chance:
// the picture is shown on whichever refresh follows, a refresh or two later, and when
// the film has one frame per refresh -- 25 fps on a television at 25 Hz -- that chance
// is drawn once, when the film starts, and kept. Measured in MPC-HC on a television at
// 25 Hz, through the video processor with HDR output: Present called on a vertical
// blank put the picture on the screen 80 ms later, every picture, with the Sync offset
// reading anything from -8 to +46 ms from one start to the next, since it only says
// when Present returned.
//
// What decides is where in a refresh Present is called. Called at any moment from a
// blank until a few milliseconds before the next, the picture is shown on the blank
// after that one: "depth" 1, the compositor's refresh. Called in those last few
// milliseconds it slips one more, and not every time -- 5 ms before the blank at 25 Hz
// lost a picture in twelve, 2 ms before did every time; at 60 Hz the last 2 to 3 ms.
// So Present is called a third of a refresh after a blank, clear of that edge and of
// the one at the start of a refresh (kCallPhase), and the refresh it then lands on is
// known in advance.
//
// From there the rest follows. DXGI gives the time of the refreshes; the reference
// clock gives the moment the picture is due; the refresh nearest to that moment is
// the picture's. A film with a whole number of refreshes per frame then keeps that
// step, picture after picture, and is only moved -- one picture given a refresh more,
// or left out -- when the two clocks have drifted three quarters of a refresh apart:
// the nearest refresh alone would flicker between two whenever the picture's time
// fell half way. And the sample has to be asked for early enough to be drawn before
// its Present, which is what CRenderAhead is for.
//
// How deep the queue is, is measured and not assumed: DXGI says on which refresh each
// Present ended up, and three in a row that disagree with the depth in use change it.
//
// One picture waits at a time (SetMaximumFrameLatency(1)), and that is what makes a
// queue that has gone wrong visible at once. Two Presents inside one refresh -- the
// burst a film starts with -- and the second does not return until the first has been
// taken; it then goes in just after that refresh instead of well before it, is itself
// still waiting when the next Present comes, and so on for as long as the film plays:
// every picture a refresh late, and 35 ms spent inside each Present at 25 Hz. It does
// not mend by itself. A Present that had to wait says so by the time it took, and the
// pictures after it are called a refresh later than they would have been, where
// nothing is in their way; if that leaves them too far from their own time one of
// them is left out, and they are back where they were.

class CPresentSchedule
{
public:
	// Called this far after a refresh, as a part of the refresh, and never more than
	// this many milliseconds. Not later, because of the edge described above. Not
	// earlier either: the start of a refresh is an edge too. In the bench's window a
	// Present called 3 ms after a refresh was in time or not from one picture to the
	// next -- every second or third then waited a whole refresh, 22 pictures were lost
	// in the first 1.6 s of a film at 25 fps, and the depth was measured as 2, then
	// as 0. A third of the way in is clear of both, there and fullscreen in a player.
	static constexpr double kCallPhase = 1.0 / 3.0;
	static constexpr double kCallPhaseMaxMs = 15.0;
	// A Present later than this before a refresh is not counted on to make it.
	static constexpr double kGuardPhase = 0.3;
	static constexpr double kGuardMaxMs = 12.0;
	// How far a picture may sit from its own time, in refreshes, before the step is broken.
	static constexpr double kRelock = 0.75;
	// The first pictures after a start or a seek come as they can, the very first late
	// by force. Until this many have been shown the nearest refresh is simply taken,
	// so that the step is the right one when it starts being kept.
	static constexpr int kSettling = 6;
	// A picture is left out so that the one after it is on time. It has been drawn by
	// then, so that only works when the next one can be drawn and still be in time
	// for its refresh, which a film whose pictures take most of a frame to draw
	// cannot do. Measured with DLSS 5 NR on a 25 fps film: at 52 ms a picture every
	// one came behind and every one was left out, 744 of 767, a screen that moved once
	// in two seconds; at 33 ms, on a 25 Hz screen, one picture a refresh late was
	// followed by one left out, whose own drawing made the next a refresh late again,
	// and the film played at 12.5 pictures a second for as long as it lasted (463 left
	// out of 944). So when leaving this one out would not put the next on time, this one
	// is shown where it still can be and it is the next that goes, before anything is
	// spent on it (Plan::bSkipNext).
	//
	// And whatever the reason, never more than this many drawn and left out in a row:
	// the screen moves.
	static constexpr int kMaxDropRun = 8;

	struct Plan {
		bool bValid = false;       // a refresh was chosen; otherwise present as before
		bool bDrop = false;        // no refresh left worth having: do not present it
		bool bSkipNext = false;    // the sample after this one is to be left out undrawn
		bool bRepeat = false;      // the picture before it is given a refresh more
		long long refresh = 0;     // the refresh it is for
		long long tickCall = 0;    // when to call Present
		long long tickWanted = 0;  // when it would have been called, had the picture come in time
		long long tickRefresh = 0; // when that refresh is
		long long tickDue = 0;     // when the picture's own time is
	};

	void Reset();   // another swap chain or another display mode
	void Flush();   // the film goes on from somewhere else

	// DXGI's account of the swap chain, as often as it is read. nominalPeriodMs is the
	// display mode's, until the refreshes themselves have said better.
	void Seen(unsigned lastPresent, const DXGI_FRAME_STATISTICS& stats, double nominalPeriodMs);
	bool Ready(long long tickNow) const;

	// tickDue: when the reference clock reaches the picture's time, on the performance
	// counter. frameMs: how long a picture of this film lasts. drawMs: how long one
	// takes from its sample to being ready to present.
	Plan Choose(long long tickNow, long long tickDue, double frameMs, double drawMs);
	// A sample was left out before it was drawn, as a Plan asked.
	void LeftOut();
	// msInside: how long Present took to return.
	void Presented(unsigned presentId, const Plan& plan, long long tickCalled, double msInside);

	// For the statistics. What happened while the film played: the pictures it loses
	// in the first moments after a start or a seek are not counted.
	int Depth() const { return m_depth; }
	unsigned RepeatedCount() const { return m_repeated; }
	unsigned DroppedCount() const { return m_dropped; }
	unsigned LateCount() const { return m_late; }      // shown later than the refresh chosen
	bool ShownKnown() const { return m_bShownKnown; }
	double ShownOffsetMs() const { return m_shownOffsetMs; } // the last one DXGI reported: its refresh, less its time
	double PeriodMs() const;

private:
	long long RefreshTime(long long index) const;
	double RefreshIndex(long long tick) const;   // fractional

	// The refreshes: the last one DXGI timed, and how long one lasts.
	bool m_bClock = false;
	long long m_anchorIndex = 0;
	long long m_anchorTick = 0;
	long long m_baseIndex = 0;     // an older one, to measure the period over,
	long long m_baseTick = 0;
	long long m_midIndex = 0;      // and the one that takes its place when it has grown too old
	long long m_midTick = 0;
	double m_period = 0;           // ticks
	double m_nominalMs = 0;
	unsigned m_lastSync = 0;

	// What was presented and is not yet known to be on the screen.
	struct Pending {
		unsigned id = 0;
		long long refresh = 0;     // the one chosen
		long long firstAfter = 0;  // the first refresh after the call
		long long tickDue = 0;
		bool bSafe = false;        // called far enough from both refreshes to tell the depth by
		bool bCounted = false;     // presented once the film had settled: late counts if it is
	};
	static constexpr int kPending = 16;
	Pending m_pending[kPending] = {};
	int m_nPending = 0;

	int m_depth = 1;
	int m_observed[3] = { 1, 1, 1 };
	int m_nObserved = 0;

	bool m_bLast = false;          // a picture has been given a refresh since the last flush
	long long m_lastRefresh = 0;
	int m_sinceFlush = 0;          // how many, up to kSettling
	int m_dropRun = 0;             // pictures left out since the last one presented

	unsigned m_repeated = 0;
	unsigned m_dropped = 0;
	unsigned m_late = 0;
	bool m_bShownKnown = false;
	double m_shownOffsetMs = 0;
};

// CPacingTrace
//
// What became of each picture on its way to the screen, one line apiece, for a program
// to read afterwards: when the picture was taken, drawn and presented, what the
// reference clock said at that moment, and what DXGI says of the pictures already
// shown. The statistics drawn on the picture are an average of the last second seen by
// whoever is looking; whether every picture got a refresh of its own, and which one,
// is only in a list of them all.
//
// Off unless the environment variable MPCVR_PACING_TRACE names a file when the
// renderer is made. The lines are kept in memory and written when the stream is
// flushed or the renderer goes: a file written on the thread that presents would be
// measuring itself.

class CPacingTrace
{
public:
	enum : unsigned {
		Held        = 1,    // render ahead held it for its time
		VBlankWait  = 2,    // "wait for VBlank before Present" was done
		SecondField = 4,
		NotPresented = 8,   // left out to catch up a refresh
		HardwareVP  = 16,   // the video processor converted it, not the shaders
		RtxVideoHDR = 32,
		HdrOutput   = 64,
		Exclusive   = 128,
		Scheduled   = 256,  // presented for a refresh chosen in advance
		Repeated    = 512,  // the scheduler gave it a second refresh's worth of wait
		Dropped     = 1024, // the scheduler left it out
		DlssNR      = 2048, // DLSS 5 NR ran on it
		NRFlow      = 4096, // and was steadied with Optical Flow
		NRDetector  = 8192, // or with the shader detector, which is gone: no longer set
		NRVectors   = 16384, // the network was given the vectors as well
		DlssSR      = 32768, // DLSS Super Resolution enlarged it
		Skipped     = 65536, // left out before it was drawn, as the scheduler asked
		VPAfterNR   = 131072, // the video processor enlarged what DLSS 5 NR left
		SceneCut    = 262144, // cs_dlss_scene_cut.hlsl called it a cut: DLSS SR's history was dropped
	};

	// DXGI's account of the swap chain at one moment.
	struct Shown {
		unsigned lastPresent = 0;     // the number of the last Present call
		unsigned presentCount = 0;    // the last one that reached the screen...
		unsigned presentRefresh = 0;  // ...and the refresh it reached it on
		unsigned syncRefresh = 0;     // a refresh DXGI has timed...
		long long syncQpc = 0;        // ...and when it was
	};

	struct Picture {
		long long rtStart = 0;            // the picture's time, 100 ns
		long long rtClockAtCall = 0;      // the reference clock as Present was called
		long long rtClockAtReturn = 0;    // and as it returned
		long long tickSample = 0;         // performance counter: the sample arrived
		long long tickDrawn = 0;          // its last command was issued
		long long tickAfterVBlank = 0;    // the wait for a vertical blank ended (0: none)
		long long tickPresentCall = 0;
		long long tickPresentReturn = 0;
		long long tickTarget = 0;         // the refresh it was meant for (0: none chosen)
		Shown before, after;              // around the Present call
		int renderAhead = 0;              // how early the sample was asked for, 100 ns
		unsigned flags = 0;
		// DLSS 5 NR's own account of the picture, microseconds: waiting for Direct3D 11
		// to finish its input, recording the network's work, waiting for the GPU to run it.
		int nrWaitUs = 0, nrRecordUs = 0, nrGpuUs = 0;
		// What cs_dlss_scene_cut.hlsl measured on it for DLSS SR, read back later
		// (CCutReadback): the histogram distance in thousandths, the residual's ratio to
		// the pictures before in tenths, the residual in millionths, the median correlation
		// in thousandths. -1: not measured.
		int cutHist = -1, cutRatio = -1, cutResid = -1, cutCorr = -1;
	};

	CPacingTrace();
	~CPacingTrace() { Write(); }

	bool Enabled() const { return m_bEnabled; }
	// A line of text ahead of the pictures that follow: the display, the swap chain.
	void Note(const std::wstring& text);
	void Add(const Picture& picture);
	// The cut measures of a picture already added, found by its time.
	void SetCut(long long rtStart, const float* state);
	void Write();

private:
	bool m_bEnabled = false;
	std::wstring m_path;
	std::vector<std::wstring> m_notes;
	std::vector<Picture> m_pictures;
	unsigned long long m_written = 0;
};

// CCutReadback
//
// What cs_dlss_scene_cut.hlsl said of each picture, for the pacing trace: its state
// (CDlssStabilizer::GetCutState) copied once the picture's motion is measured and read
// back a few pictures later, never waited for. A picture whose copy finds every slot
// still with the GPU goes without.

class CCutReadback
{
public:
	void Release();
	void Copy(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Buffer* pState, long long rtStart);

	// What the GPU has finished, oldest first: found(rtStart, state), state as
	// GetCutState describes it.
	template<class F> void Collect(ID3D11DeviceContext* pContext, F&& found)
	{
		for (;;) {
			Slot* pOldest = nullptr;
			for (Slot& slot : m_slots) {
				if (slot.bPending && (!pOldest || slot.order < pOldest->order)) {
					pOldest = &slot;
				}
			}
			D3D11_MAPPED_SUBRESOURCE mr = {};
			if (!pOldest || pContext->Map(pOldest->pStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr) != S_OK) {
				return;
			}
			float state[16];
			memcpy(state, mr.pData, sizeof(state));
			pContext->Unmap(pOldest->pStaging, 0);
			pOldest->bPending = false;
			found(pOldest->rtStart, state);
		}
	}

private:
	static constexpr int kSlots = 8;
	struct Slot {
		CComPtr<ID3D11Buffer> pStaging;
		long long rtStart = 0;
		uint64_t order = 0;
		bool bPending = false;
	};
	Slot m_slots[kSlots];
	uint64_t m_copies = 0;
};

// CGpuStageTimes
//
// GPU time of the DLSS stages, for the statistics: timestamp queries read back a
// few pictures later, never waited for. A picture for which every query set is
// still with the GPU goes untimed. DLSS 5 NR runs on a Direct3D 12 device of its
// own and is timed on the CPU instead, where the renderer waits for it.

class CGpuStageTimes
{
public:
	// NRReduce and NRCompose: DLSS 5 NR at a working size below the picture's.
	enum Stage { NRMotion, NRStabilize, SRMotion, SR, MpvLuma, MpvChroma, VPScale, NRReduce, NRCompose, StageCount };

	void Release();

	void BeginFrame(ID3D11Device* pDevice, ID3D11DeviceContext* pContext);
	void Begin(ID3D11DeviceContext* pContext, Stage stage);
	void End(ID3D11DeviceContext* pContext, Stage stage);
	void EndFrame(ID3D11DeviceContext* pContext);

	// Reads back what the GPU has finished.
	void Collect(ID3D11DeviceContext* pContext);

	// Mean time of a stage over its recent pictures; negative when it has not run lately.
	double MeanMs(Stage stage) const;

private:
	static constexpr int kSets = 8;
	static constexpr uint64_t kStalePictures = 48;

	struct QuerySet {
		CComPtr<ID3D11Query> pDisjoint;
		CComPtr<ID3D11Query> pBegin[StageCount];
		CComPtr<ID3D11Query> pEnd[StageCount];
		bool bOpen[StageCount] = {};
		bool bUsed[StageCount] = {};
		bool bPending = false;
		uint64_t picture = 0;
	};

	QuerySet m_sets[kSets];
	int m_iRecording = -1;
	uint64_t m_pictures = 0;
	bool m_bFailed = false;

	CRollingMs m_times[StageCount];
	uint64_t m_lastSeen[StageCount] = {};
};
