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

#include <dxgi.h>
#include <atlbase.h>
#include <atomic>

// Keeps the desktop's compositor composing on every refresh while the film plays in a
// window. With nothing else moving on the desktop, DWM composes only when a picture
// arrives, and then it does not keep time: in a window on an idle desktop a third of
// the times DXGI gave for the refreshes were 1 to 16 ms off the refreshes' grid, its
// count lost three refreshes a second, and the screen held film pictures 1 and 4
// refreshes where it should hold 2 and 3 (461 times in two minutes of a 23.976 film at
// 60 Hz). Anything else that moves on the screen ends it -- the player's own pacing was
// perfect for as long as another program animated -- and so does this: a pixel of the
// video window, under the swap chain's picture, drawn again on every vertical blank,
// gives DWM something to compose on every refresh. The same two minutes with a
// window of 4 pixels doing that: no time off the grid, no refresh lost, and every
// picture held 2 or 3 refreshes. A window that covers its monitor -- the player's
// fullscreen -- is left alone: its pictures go to the screen without DWM, and a pixel
// drawn there would bring DWM back.
// MPCVR_DWM_KEEPALIVE=0 in the environment leaves it off, to measure without it.
class CDwmKeepAlive
{
public:
	enum State { Off = 0, Waiting = 1, Drawing = 2 };

	~CDwmKeepAlive() { Stop(); }

	// hWnd is the window the swap chain presents into; the adapter gives the outputs
	// whose vertical blanks are waited for.
	void Start(HWND hWnd, IDXGIAdapter* pAdapter);
	void Stop();

	// Off: not running. Waiting: running, the window covering its monitor or hidden.
	// Drawing: keeping DWM composing.
	State Now() const { return (State)m_state.load(std::memory_order_relaxed); }

private:
	static DWORD WINAPI ThreadProc(LPVOID pParam);
	void Run();

	HWND m_hWnd = nullptr;
	CComPtr<IDXGIAdapter> m_pAdapter;
	HANDLE m_hStop = nullptr;
	HANDLE m_hThread = nullptr;
	std::atomic<int> m_state = Off;
};
