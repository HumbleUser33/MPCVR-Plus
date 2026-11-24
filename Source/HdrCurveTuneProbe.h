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

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>

// What the HDR curve's tune shares between its worker, which moves the film from picture to
// picture (VideoRendererHdrTune.cpp), and the render path, which measures the picture each
// move brings (DX11VideoProcessorHdrCurve.cpp). The worker never touches the device and the
// render path never waits for the worker: one picture handed over at a time, an event to say
// it is there, another to say the scan is to stop.

struct HdrTunePicture {
	std::vector<unsigned> hist;      // HdrCurveTune::kBins of the light, then as many of the brightest channel
	REFERENCE_TIME rtFilm = 0;       // where in the film the picture is: the segment's start and the picture's own
	double ms = 0;                   // the measurement, read back included
	int width = 0, height = 0;       // the rectangle measured
};

class CHdrTuneProbe
{
public:
	// Why a scan stopped before its end.
	enum Reason : int {
		None = 0,
		Cancelled,   // the key pressed again
		ExternalRun, // the player started the film playing
		Stopped,     // the player stopped the film, or is closing it
		Removed,     // the renderer left the graph
	};

	CHdrTuneProbe()
		: m_hPicture(CreateEventW(nullptr, FALSE, FALSE, nullptr))
		, m_hAbort(CreateEventW(nullptr, TRUE, FALSE, nullptr))
	{}
	~CHdrTuneProbe()
	{
		CloseHandle(m_hPicture);
		CloseHandle(m_hAbort);
	}
	CHdrTuneProbe(const CHdrTuneProbe&) = delete;
	CHdrTuneProbe& operator=(const CHdrTuneProbe&) = delete;

	// A scan begins: nothing asked yet, nothing aborted.
	void Begin()
	{
		m_bArmed = false;
		m_reason = None;
		ResetEvent(m_hAbort);
		ResetEvent(m_hPicture);
		m_bSampling = true;
	}
	// The measurement step forced into the chain, or let go.
	void SetActive(bool bActive) { m_bActive = bActive; }
	bool Active() const { return m_bActive; }
	// Moving the film from picture to picture; over once the last is in or the scan stopped.
	bool Sampling() const { return m_bSampling; }
	void EndSampling()
	{
		m_bSampling = false;
		m_bArmed = false;
	}

	// The next picture wanted: the first new one once the flush count has passed `afterGen`.
	void Arm(unsigned afterGen)
	{
		ResetEvent(m_hPicture);
		m_afterGen = afterGen;
		m_bArmed = true;
	}
	bool Wants(unsigned genNow) const { return m_bArmed && genNow > m_afterGen; }
	void Deliver(HdrTunePicture&& picture)
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_picture = std::move(picture);
		}
		m_bArmed = false;
		SetEvent(m_hPicture);
	}

	enum Wait { Got, Aborted, TimedOut };
	Wait WaitPicture(DWORD ms, HdrTunePicture& out)
	{
		const HANDLE handles[2] = { m_hAbort, m_hPicture };
		const DWORD r = WaitForMultipleObjects(2, handles, FALSE, ms);
		if (r == WAIT_OBJECT_0 + 1) {
			std::lock_guard<std::mutex> lock(m_lock);
			out = std::move(m_picture);
			return Got;
		}
		return r == WAIT_OBJECT_0 ? Aborted : TimedOut;
	}

	// The first reason given is kept.
	void Abort(Reason why)
	{
		int none = None;
		m_reason.compare_exchange_strong(none, why);
		SetEvent(m_hAbort);
	}
	Reason Why() const { return (Reason)m_reason.load(); }
	bool AbortedNow() const { return WaitForSingleObject(m_hAbort, 0) == WAIT_OBJECT_0; }

private:
	HANDLE m_hPicture;
	HANDLE m_hAbort;
	std::atomic<bool> m_bActive = false;
	std::atomic<bool> m_bSampling = false;
	std::atomic<bool> m_bArmed = false;
	std::atomic<unsigned> m_afterGen = 0;
	std::atomic<int> m_reason = None;
	std::mutex m_lock;
	HdrTunePicture m_picture;
};
