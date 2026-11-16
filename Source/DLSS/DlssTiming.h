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
	// A swap chain whose flips the system has begun to hold back. Measured in MPC-HC at
	// 23.976 Hz, the picture on a plane of its own: after 30 to 55 minutes of playing in
	// one session -- 30, 35, 37, 38, 39 and 45 on a 4K HDR10 film, 55 on a 1080p SDR one --
	// one picture every 1.0245 s reached the screen a refresh after the one it was
	// presented for, the GPU done with it 33 ms and Present called 27 ms before that
	// refresh, and the next was left out to catch up: two hitches a second for as long as
	// the film went on, 1110 of each in 19 minutes. That period is the beat of the screen's
	// rate with 23 Hz, 1 / (f - 23), to a few hundredths of a percent in each of four
	// sessions: the flips were let through 23 times a second. The graphics kernel's own
	// events say why: once its VSync smoothing has been reset it goes by the mode's period,
	// which it holds as 1/23 s, and the hardware flip queue spaces the flips by it
	// (README-DLSS5.md, "A swap chain that starts skipping flips"). Neither a deeper queue,
	// a pause nor a seek changed anything; resizing the swap chain to its own size ended it
	// at once, every time. With
	// the GPU's hardware scheduling off there was none in a session of 90 minutes.
	//
	// So the late pictures that were ready in time are watched for that beat. Late ones on
	// refreshes at most kStuckGroup apart are one group -- a stall of the system puts
	// several in a row late, the held-back flips one and the picture after it -- and a group
	// that begins the beat's spacing after the one before, within kStuckTolerance, carries
	// a run on; any other begins it again. kStuckRun groups in a run, three seconds at
	// 23.976 Hz, and the renderer renews the swap chain
	// (CDX11VideoProcessor::RenewSwapChainIfStuck). The rule before this one, five ready
	// pictures late in eight seconds, was set off once in 90 minutes by two stalls 0.3 s
	// apart; this one, replayed on eight hours of traces, acts at the start of each of the
	// seven times it happened and nowhere else, at three groups in a run as at five.
	// Another fractional rate, 29.97 or 59.94 Hz, would beat with its own whole number: not
	// measured. A mode of a whole number of hertz has no beat, and the rule is off.
	static constexpr int kStuckGroup = 3;        // refreshes
	static constexpr int kStuckRun = 4;
	static constexpr double kStuckTolerance = 0.10;
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
	// A picture's time is its stamp, and a stamp is not exact: Matroska keeps them to the
	// millisecond, so a 23.976 fps film steps 41, 42, 42, 41... ms, half a millisecond
	// either way of the truth. Where a frame lasts a whole number of refreshes that is
	// nothing. Where it does not -- 2.5025 refreshes at 60 Hz -- the pictures' times cross
	// half way between two refreshes once every 8.3 s, the nearest refresh flips between
	// the two for as long as the rounding straddles it, and the 3:2 cadence breaks five or
	// six times in half a second where once was all it needed: 78 breaks in 116 s of the
	// bench's film, measured on the screen itself. So the time the schedule goes by is the stamp
	// carried forward a frame at a time and drawn a thirty-second of the way towards each
	// new stamp (kDueGain), which leaves a few hundredths of a millisecond of the rounding:
	// 18 breaks in the same 116 s, one each time the cadence has to change step. A stamp
	// further than kDueResetMs from where the frames put it is a clock or a stream that
	// moved, and is taken as it is.
	static constexpr double kDueGain = 1.0 / 32.0;
	static constexpr double kDueResetMs = 4.0;
	// Where the cadence slips, and what is shown on the refresh it adds (SetCadenceSlips).
	//
	// A screen a little faster than the film's whole step -- 23.976 fps at 60.000 Hz, 2.5025
	// refreshes a frame where the 3:2 cadence gives 2.5; 29.97 or 59.94 fps at 60.000 Hz,
	// 2.002 and 1.001 where the step is 2 and 1; a film at its own rate whose clock and the
	// screen's drift some parts in a million apart -- has to give one picture a refresh more
	// now and then: a 3,3 every 200 pictures (8.34 s) at 23.976 fps on 60 Hz, a picture held
	// 3 refreshes or repeated every 16.7 s at 29.97 and 59.94, one in an hour or so at the
	// film's own rate. Where it falls is free within a little of the picture's time, and on
	// a scene cut it does not show. So the step is kept past the point where the nearest
	// refresh would break it, and broken on the first cut found from kSlipEarly of a refresh
	// before that point (a 3:2 cadence) or half a refresh (a whole step) -- the picture that
	// opens the new shot is shown a refresh later, the last of the old one holding the screen
	// a refresh longer -- and at the latest kSlipEarly after it (3:2), or when a picture would
	// be shown kSlipLimitMs before its time, or kRelock of a refresh, whichever is more (a
	// whole step). That last one may show a picture made between the two (Plan::bSlipMade) on
	// the refresh it adds. A picture shown too late rather than too early, at a film's own
	// rate, is left out on a cut instead.
	//
	// In a 3:2 cadence only a 2 is made a 3, never a 3 a 4 -- save for a made picture that
	// would not be ready in time on a 2 (SetMadeMs): the forced slip is then taken on the 3
	// before it, a picture sooner, and the made picture goes on the 3's fourth refresh. It has
	// from the 3's Present to that refresh -- four refreshes, where a 2 leaves three -- to
	// draw the picture after it and make itself. A 4 takes a whole refresh where a 3,3 takes
	// half: such slips come half as often, every 16.7 s at 23.976 fps on 60 Hz, and the
	// pictures between them are shown up to 12.5 ms after their time as well as before it.
	//
	// A cut that opens on a picture of 3 gives the slip to the picture after it: the first
	// picture of the new shot holds the
	// screen a refresh longer. And a slip there takes half a refresh, the 3 and the 2 trading
	// places, so its window is half a refresh wide, kSlipEarly each side of the point. A
	// forced slip leaves the cadence at the window's start, and a cut soon after may take the
	// next one there and then (seen in MPC-HC, 0.7 s apart): the forced one was shown anyway,
	// the second is on a cut, and the slips come no more often for it over the film.
	enum CadenceSlips { CadenceSlipsWhereTheyFall = 0, CadenceSlipsOnCut = 1, CadenceSlipsOnCutOrMade = 2 };
	static constexpr double kSlipEarly = 0.25;        // refresh
	static constexpr double kSlipLimitMs = 17.0;
	// The screen this much faster than 2.5 refreshes a frame, give or take kNearCadenceTol:
	// a 3:2 cadence whose slip can be placed. 0.001 is 60.000 Hz against 23.976 fps.
	static constexpr double kNearCadence = 0.001;
	static constexpr double kNearCadenceTol = 0.0006;

	struct Plan {
		bool bValid = false;       // a refresh was chosen; otherwise present as before
		bool bDrop = false;        // no refresh left worth having: do not present it
		bool bSkipNext = false;    // the sample after this one is to be left out undrawn
		bool bRepeat = false;      // the picture before it is given a refresh more
		// The slip of a cadence placed on a scene cut (kSlipEarly). A cut found on this
		// picture shows it a refresh later (bSlipIfCut) or leaves it out (bDropIfCut):
		// ApplyCut does it. bSlipForced: no cut came in time, and the refresh before this
		// one is the one added; bSlipMade: a picture made between the two may go on it
		// (MadePlan). bSlipOnCut: ApplyCut took the slip on a cut. bBetween: the plan of
		// such a made picture, not of a drawn one.
		bool bSlipIfCut = false;
		bool bSlipNextIfCut = false;   // a 3 in a 3:2 cadence: a cut on it slips the next picture
		bool bDropIfCut = false;
		bool bSlipForced = false;
		bool bSlipMade = false;
		bool bSlipOnCut = false;
		bool bBetween = false;
		long long refresh = 0;     // the refresh it is for
		long long tickCall = 0;    // when to call Present
		long long tickWanted = 0;  // when it would have been called, had the picture come in time
		long long tickRefresh = 0; // when that refresh is
		long long tickDue = 0;     // when the picture's own time is, as the schedule goes by it
		long long tickStamp = 0;   // and when its stamp is
	};

	void Reset();   // another swap chain or another display mode
	void Flush();   // the film goes on from somewhere else
	// One of CadenceSlips. Kept across Reset and Flush.
	void SetCadenceSlips(int mode);
	int CadenceSlipsMode() const { return m_cadenceSlips; }
	// What a made picture takes the GPU now, once the picture after it is drawn: where a 2
	// of a 3:2 cadence leaves it too little time, a forced slip goes on a 3.
	void SetMadeMs(double ms) { m_madeMs = ms; }

	// DXGI's account of the swap chain, as often as it is read. nominalPeriodMs is the
	// display mode's, until the refreshes themselves have said better.
	void Seen(unsigned lastPresent, const DXGI_FRAME_STATISTICS& stats, double nominalPeriodMs);
	bool Ready(long long tickNow) const;

	// tickStamp: when the reference clock reaches the picture's stamp, on the performance
	// counter. frameMs: how long a picture of this film lasts. drawMs: how long one
	// takes from its sample to being ready to present. The plan's tickDue is the
	// picture's time as the schedule went by it (kDueGain).
	Plan Choose(long long tickNow, long long tickStamp, double frameMs, double drawMs);
	// Whether a scene cut was found on the picture, once known and before its Present:
	// takes the slip the plan left to a cut (bSlipIfCut, bDropIfCut).
	void ApplyCut(Plan& plan, bool bCut);
	// The plan of a picture made between the last one shown and `next`, for the refresh
	// a forced slip adds (bSlipMade); not valid when there is none, or no time left for it.
	// t: where that refresh falls between the two pictures' own times, 0 to 1.
	Plan MadePlan(long long tickNow, const Plan& next, double& t) const;
	// The last moment the GPU can finish a picture planned for plan.refresh and still have
	// it shown there, less the guard: that refresh, where the picture is flipped to the
	// screen itself (depth 0), and the refresh before where the desktop composes it (depth 1,
	// a window): the desktop takes what is finished when it starts.
	long long ReadyBy(const Plan& plan) const;
	// A sample was left out before it was drawn, as a Plan asked.
	void LeftOut();
	// msInside: how long Present took to return. bReady: the GPU had finished the picture
	// by then.
	void Presented(unsigned presentId, const Plan& plan, long long tickCalled, double msInside, bool bReady);
	// The flips have begun to be held back (kStuckRun): once, and the count starts again.
	bool FlipsStuck();
	// The refreshes between two of the held-back flips: the display mode's rate over its
	// part above a whole number of hertz. 0 for a mode of a whole number, or none known.
	double StuckSpacing() const;

	// For the statistics. What happened while the film played: the pictures it loses
	// in the first moments after a start or a seek are not counted.
	int Depth() const { return m_depth; }
	unsigned RepeatedCount() const { return m_repeated; }
	unsigned DroppedCount() const { return m_dropped; }
	unsigned LateCount() const { return m_late; }      // shown later than the refresh chosen
	// Cadence slips (SetCadenceSlips): taken on a cut, forced for want of one, and of those
	// forced, how many showed a made picture on the refresh they added.
	unsigned SlipsOnCut() const { return m_slipsOnCut; }
	unsigned SlipsForced() const { return m_slipsForced; }
	unsigned MadeShown() const { return m_madeShown; }
	// The step is being kept past the nearest refresh, waiting for a cut.
	bool SlipWaiting() const { return m_bSlipWaiting; }
	// The last picture was in a cadence whose slips are placed: what the renderer copies
	// pictures out for.
	bool SlipsPlaced() const { return m_bSlipCadence; }
	bool ShownKnown() const { return m_bShownKnown; }
	double ShownOffsetMs() const { return m_shownOffsetMs; } // the last one DXGI reported: its refresh, less its time
	double PeriodMs() const;

private:
	long long RefreshTime(long long index) const;
	double RefreshIndex(long long tick) const;   // fractional
	long long SmoothDue(long long tickStamp, double frameMs);

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
		bool bReady = false;       // the GPU had finished it when Present was called
	};
	static constexpr int kPending = 16;
	Pending m_pending[kPending] = {};
	int m_nPending = 0;

	int m_depth = 1;
	int m_observed[3] = { 1, 1, 1 };
	int m_nObserved = 0;

	// The pictures that were ready and still shown late, in groups (kStuckGroup): the
	// refresh the last group began on and the last one late in it, and how many groups in a
	// row have begun the beat's spacing apart (kStuckRun).
	void LateReady(long long refresh);
	bool m_bLateGroup = false;
	long long m_lateStart = 0;
	long long m_lateLast = 0;
	int m_stuckRun = 0;

	bool m_bLast = false;          // a picture has been given a refresh since the last flush
	long long m_lastRefresh = 0;
	long long m_lastGap = 0;       // refreshes between the last two pictures shown (0: not known)
	long long m_lastDue = 0;       // the last picture's own time, as the schedule went by it

	int m_cadenceSlips = CadenceSlipsWhereTheyFall;
	double m_madeMs = 0.0;         // SetMadeMs
	bool m_bSlipWaiting = false;
	bool m_bSlipCadence = false;
	bool m_bSlipNext = false;      // the last picture opened a shot on a 3: this one takes the slip
	unsigned m_slipsOnCut = 0;
	unsigned m_slipsForced = 0;
	unsigned m_madeShown = 0;

	// The pictures' times as the schedule goes by them (kDueGain).
	bool m_bDue = false;
	double m_dueSmooth = 0;        // ticks
	long long m_dueStamp = 0;      // the last stamp's own time, ticks
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
		// How the last picture shown went on the screen (IDXGISwapChainMedia): put
		// together with the desktop by the compositor, scanned out from a plane of its
		// own, or flipped straight to the screen. None of the three: DXGI did not say.
		Composed    = 524288,
		Overlay     = 1048576,
		DirectFlip  = 2097152,
		// A picture made between two drawn ones (the frame generation branch gives the same
		// value to its own), here for the refresh a forced cadence slip adds.
		Generated   = 4194304,
		// Cadence slips (CPresentSchedule::SetCadenceSlips): this picture opened a scene cut
		// the slip was taken on, or no cut came in time and the slip was taken here.
		SlipOnCut   = 16777216,
		SlipForced  = 33554432,
		CadenceCut  = 67108864, // the cadence slips' own detector (CSceneCut) called it a cut
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
		// When the GPU was seen done with it, waiting for its Present; 0 when it was not
		// done by then, or nothing waited.
		long long tickGpuDone = 0;
		long long tickStamp = 0;          // its stamp on the performance counter, as scheduled
		long long tickDue = 0;            // and the time the schedule went by (kDueGain)
		// GPU time, microseconds, while the statistics show (CGpuStageTimes): the video
		// processor's turn, and every stage timed. -1: not measured.
		int gpuVpUs = -1, gpuAllUs = -1;
	};

	CPacingTrace();
	~CPacingTrace() { Write(); }

	bool Enabled() const { return m_bEnabled; }
	// A line of text ahead of the pictures that follow: the display, the swap chain.
	void Note(const std::wstring& text);
	void Add(const Picture& picture);
	// The cut measures of a picture already added, found by its time.
	void SetCut(long long rtStart, const float* state);
	// Those measures (cs_dlss_scene_cut.hlsl's state) into a picture, without the flag.
	static void TakeCut(Picture& p, const float* state);
	// Its GPU times (CGpuStageTimes::Take), likewise.
	void SetGpu(long long rtStart, float vpMs, float allMs);
	// Out to the file, all but the last `keep` pictures, which SetCut and SetGpu may still fill.
	void Write(size_t keep = 0);

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
// GPU time of each step a picture goes through, for the statistics: timestamp
// queries read back a few pictures later, never waited for. A picture for which
// every query set is still with the GPU goes untimed. DLSS 5 NR runs on a Direct3D
// 12 device of its own and is timed there, on its queue (CDlssNR::LastTiming).

class CGpuStageTimes
{
public:
	// NRReduce and NRCompose: DLSS 5 NR at a working size below the picture's.
	// VP: the video processor's first turn. ShaderConvert: the conversion shader.
	// Prepass: the chroma rebuilt for the video processor, a prescaler's included.
	// Resize: the resize shader. Finish: everything after it, dither included.
	// Deblock, Deband: the artifact removal on the decoded planes, as the sample is
	// copied; the write back into the processor's input is the last one's. Cadence: the
	// picture copied out for the cadence slips and measured for a scene cut.
	enum Stage { NRMotion, NRStabilize, SRMotion, SR, MpvLuma, MpvChroma, VPScale, NRReduce, NRCompose,
		VP, ShaderConvert, Prepass, Resize, Finish, Deblock, Deband, Logo, Cadence, StageCount };

	void Release();

	// tag: the picture's time, which the pacing trace finds the picture by (Take).
	void BeginFrame(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, long long tag = 0);
	// A picture is being timed: a query set is open.
	bool IsRecording() const { return m_iRecording >= 0; }
	void Begin(ID3D11DeviceContext* pContext, Stage stage);
	void End(ID3D11DeviceContext* pContext, Stage stage);
	void EndFrame(ID3D11DeviceContext* pContext);

	// Reads back what the GPU has finished.
	void Collect(ID3D11DeviceContext* pContext);

	// Mean time of a stage over its recent pictures; negative when it has not run lately.
	double MeanMs(Stage stage) const;

	// For the pacing trace: each picture read back since the last call, oldest first --
	// its tag, the video processor's time and that of every stage together, in ms
	// (negative: did not run).
	struct Measured { long long tag; float vpMs; float allMs; };
	template<class F> void Take(F&& found)
	{
		for (int i = 0; i < m_nMeasured; i++) {
			found(m_measured[i]);
		}
		m_nMeasured = 0;
	}

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
		long long tag = 0;
	};

	QuerySet m_sets[kSets];
	int m_iRecording = -1;
	uint64_t m_pictures = 0;
	bool m_bFailed = false;

	CRollingMs m_times[StageCount];
	static constexpr int kMeasured = 16;
	Measured m_measured[kMeasured] = {};
	int m_nMeasured = 0;
	uint64_t m_lastSeen[StageCount] = {};
};
