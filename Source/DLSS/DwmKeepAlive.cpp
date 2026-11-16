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
#include "DwmKeepAlive.h"
#include "../Times.h"

namespace {

// Visible, not minimised and not over the whole of its monitor; hMonitor is the monitor
// it is on.
bool PlaysInAWindow(HWND hWnd, HMONITOR& hMonitor)
{
	hMonitor = nullptr;
	if (!IsWindowVisible(hWnd) || IsIconic(GetAncestor(hWnd, GA_ROOT))) {
		return false;
	}
	RECT rc = {};
	if (!GetWindowRect(hWnd, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) {
		return false;
	}
	hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(mi) };
	if (!GetMonitorInfoW(hMonitor, &mi)) {
		return false;
	}
	return !(rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top
		&& rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom);
}

CComPtr<IDXGIOutput> OutputOf(IDXGIAdapter* pAdapter, HMONITOR hMonitor)
{
	CComPtr<IDXGIOutput> pOutput;
	for (UINT i = 0; pAdapter->EnumOutputs(i, &pOutput) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_OUTPUT_DESC desc = {};
		if (SUCCEEDED(pOutput->GetDesc(&desc)) && desc.Monitor == hMonitor) {
			return pOutput;
		}
		pOutput.Release();
	}
	return nullptr;
}

} // namespace

void CDwmKeepAlive::Start(HWND hWnd, IDXGIAdapter* pAdapter)
{
	Stop();

	static const bool s_bAllowed = [] {
		wchar_t value[16] = {};
		const DWORD length = GetEnvironmentVariableW(L"MPCVR_DWM_KEEPALIVE", value, (DWORD)std::size(value));
		return !(length && length < std::size(value) && _wtoi(value) == 0);
	}();
	if (!s_bAllowed || !hWnd || !pAdapter) {
		return;
	}

	m_hWnd = hWnd;
	m_pAdapter = pAdapter;
	m_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (m_hStop) {
		// Said at once, for the trace's first note; the thread looks again straight away.
		HMONITOR hMonitor = nullptr;
		m_state = PlaysInAWindow(hWnd, hMonitor) ? Drawing : Waiting;
		m_hThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
	}
	if (!m_hThread) {
		Stop();
	}
}

void CDwmKeepAlive::Stop()
{
	if (m_hThread) {
		// The thread waits for nothing longer than a vertical blank, and a display
		// asleep answers that at once.
		SetEvent(m_hStop);
		WaitForSingleObject(m_hThread, INFINITE);
		CloseHandle(m_hThread);
		m_hThread = nullptr;
	}
	if (m_hStop) {
		CloseHandle(m_hStop);
		m_hStop = nullptr;
	}
	m_pAdapter.Release();
	m_hWnd = nullptr;
	m_state = Off;
}

DWORD WINAPI CDwmKeepAlive::ThreadProc(LPVOID pParam)
{
	static_cast<CDwmKeepAlive*>(pParam)->Run();
	return 0;
}

// Nothing here sends the window a message: the window's thread may be the one waiting
// in Stop().
void CDwmKeepAlive::Run()
{
	CComPtr<IDXGIOutput> pOutput;
	HMONITOR hOutputMonitor = nullptr;
	bool bDrawing = false;
	bool bOdd = false;
	ULONGLONG tickLook = 0;
	const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;

	for (;;) {
		// Where the window is, four times a second: a move to another monitor, to the
		// player's fullscreen and back.
		const ULONGLONG now = GetTickCount64();
		if (now >= tickLook) {
			tickLook = now + 250;
			HMONITOR hMonitor = nullptr;
			bDrawing = PlaysInAWindow(m_hWnd, hMonitor);
			if (bDrawing && hMonitor != hOutputMonitor) {
				pOutput = OutputOf(m_pAdapter, hMonitor);
				hOutputMonitor = hMonitor;
			}
			bDrawing = bDrawing && pOutput;
			m_state = bDrawing ? Drawing : Waiting;
		}
		if (!bDrawing) {
			if (WaitForSingleObject(m_hStop, 250) != WAIT_TIMEOUT) {
				break;
			}
			continue;
		}

		// A display asleep, or an output gone, answers at once: never spin on it.
		const uint64_t tick0 = GetPreciseTick();
		const HRESULT hr = pOutput->WaitForVBlank();
		const bool bAtOnce = FAILED(hr) || (double)(GetPreciseTick() - tick0) < 2.0 * ticksPerMs;
		if (WaitForSingleObject(m_hStop, bAtOnce ? 8 : 0) != WAIT_TIMEOUT) {
			break;
		}

		// Under the swap chain's picture, and two colours a step apart: not to be seen
		// even where nothing covers it.
		if (HDC hdc = GetDC(m_hWnd)) {
			SetPixelV(hdc, 0, 0, bOdd ? RGB(1, 1, 1) : RGB(0, 0, 0));
			ReleaseDC(m_hWnd, hdc);
			bOdd = !bOdd;
		}
	}
}
