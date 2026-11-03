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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include "LogoAnalysis.h"
#include "LogoLibrary.h"

// The analysis of "Find the logo", off the render thread. The renderer hands it the
// pictures it asks for, as it reads them back from the GPU: the luma at the working size
// while it looks for the logo, then the logo's box of every plane while it fits it. It
// publishes a fit as soon as one can be trusted, and better ones as more pictures come,
// up to the last, which goes into the library.
//
// On a new film it first tries the logos the library keeps for its size, each a couple of
// pixels either way, on the box round them all: the one on screen is taken.

namespace Logo {

class CWorker
{
public:
	enum class Phase { Idle, Recognising, Locating, Fitting, Done, NotFound, Unknown };

	~CWorker() { Stop(); }

	// A new analysis of a picture of W x H, chroma divided by divW x divH.
	void Start(int W, int H, int divW, int divH, const std::wstring& film);
	// The library's logos for this size tried on the next pictures.
	void Recognise(int W, int H, int divW, int divH, std::vector<Stored>&& kept);
	// A logo of the library taken as it is, chosen on the settings page.
	void Use(int W, int H, int divW, int divH, Stored&& chosen);
	void Stop();

	Phase GetPhase() const { return m_phase.load(); }
	// What the render thread is to read back next: the luma (scale 1 or 2), or the box.
	bool WantsLuma(int* scale) const;
	bool WantsBox(Rect* box) const;
	// The pictures it asked for. Called from the render thread; never waits for the work.
	void PushLuma(Plane&& luma);
	void PushBox(Crop&& crop);

	// The latest fit, once each time a new one is ready.
	bool TakeMaps(Maps& out, bool* final);
	// The file the final fit was kept in, if any.
	std::wstring StoredPath() const;
	// For the statistics and the settings page.
	std::wstring Status() const;

private:
	void Run();
	void RunRecognise();
	void Launch(int W, int H, int divW, int divH, Phase phase);

	std::thread m_thread;
	mutable std::mutex m_lock;
	std::condition_variable m_wake;
	std::deque<Plane> m_luma;
	std::deque<Crop> m_crops;
	bool m_quit = false;

	std::atomic<Phase> m_phase{ Phase::Idle };
	int m_W = 0, m_H = 0, m_divW = 2, m_divH = 2, m_scale = 1;
	std::wstring m_film;
	Rect m_box;
	int m_looked = 0, m_fitted = 0;

	Maps m_ready;
	bool m_haveReady = false, m_readyFinal = false;
	std::wstring m_stored;
	std::vector<Stored> m_kept;   // what is being recognised
	std::wstring m_knownFrom;     // the film a recognised logo was found on
	bool m_chosen = false;        // taken from the list on the settings page
};

} // namespace Logo
