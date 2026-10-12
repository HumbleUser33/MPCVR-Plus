// Drives a real MPC-HC through what a user does with the renderer's options, and says
// whether the mouse pointer is hidden afterwards.
//
// The benches in this folder host the filter themselves, on one thread, in a window
// of their own. Some faults only exist in a player: MPC-HC opens the film on a worker
// thread, hides the pointer with a timer of its own and owns the window ours sits in.
// This starts the player on a film, in fullscreen, and for each step moves the mouse
// over the picture, waits past the player's two seconds and asks Windows whether a
// pointer is showing.
//
//   mpchc_drive.exe <mpc-hc64.exe> <film> [--windowed] [--select A,B,...] [--combo ID]
//
// --select lists the positions to put the list on, one visit to the options each
// (default: the HDR10/DV list, IDC_COMBO10, moved to 3 and back to 1). Position -1
// opens and closes the options without touching anything, which is the control.
//
// Point it at a copy of the player, not the installed one, and save
// HKCU\Software\MPC-HC first: the player writes its settings when it closes.
//
// Build: cl /nologo /EHsc /std:c++20 /O2 /MT mpchc_drive.cpp user32.lib gdi32.lib

#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static const UINT ID_MENU_FILTERS = 951;      // mpc-hc resource.h
static const UINT ID_APPLY_NOW_BTN = 0x3021;  // a property sheet's Apply
static const UINT MN_GETHMENU_MSG = 0x01E1;

static DWORD g_pid = 0;
static bool g_noApply = false;   // --noapply: OK alone, which applies as it closes
static bool g_byMouse = false;    // --mouse: the options reached by right click, and fullscreen by its command
static int g_hz = 0;              // --hz N: the screen at N Hz for the run, and back after
static int g_hold = 0;            // --hold N: only play for N seconds, for something else to watch
static std::wstring g_size;       // --size W,H: the player's window, when not fullscreen
static int g_attach = 0;          // --attach N: watch a player that is already running for N seconds, touch nothing
static std::wstring g_shots;      // --shots PREFIX: the options, one picture a tab, as the player's theme leaves them
static bool g_verbose = false;   // --verbose: what the Filters menu held

// --windows: every window the player makes, shows, hides or destroys while the options
// are being used, with its class, its thread and where it is. A window that flashes for
// one picture is seen here and nowhere else.
static bool g_bWindows = false;
static bool g_bWindowsNow = false;
static ULONGLONG g_tickWindows = 0;

static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD thread, DWORD)
{
	if (!g_bWindowsNow || idObject != OBJID_WINDOW || idChild != 0 || !hwnd) {
		return;
	}
	wchar_t cls[64] = {};
	GetClassNameW(hwnd, cls, 64);
	// The options' own controls are not what is looked for.
	wchar_t top[64] = {};
	GetClassNameW(GetAncestor(hwnd, GA_ROOT), top, 64);
	if (!wcscmp(top, L"#32770") || !wcscmp(top, L"#32768") || !wcscmp(cls, L"tooltips_class32") || !wcscmp(cls, L"IME")
			|| !wcscmp(cls, L"MSCTFIME UI") || !wcscmp(cls, L"SysShadow") || !wcscmp(cls, L"ComboLBox")) {
		return;
	}
	// A move is only worth a line for the picture's own window and the player's.
	if (event == EVENT_OBJECT_LOCATIONCHANGE && wcscmp(cls, L"VRWindow") && wcscmp(cls, L"MediaPlayerClassicW")) {
		return;
	}
	if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY && event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE
			&& event != EVENT_OBJECT_REORDER && event != EVENT_OBJECT_PARENTCHANGE && event != EVENT_OBJECT_LOCATIONCHANGE) {
		return;
	}
	RECT rc = {};
	GetWindowRect(hwnd, &rc);
	const wchar_t* what = event == EVENT_OBJECT_CREATE ? L"made" : event == EVENT_OBJECT_DESTROY ? L"destroyed"
		: event == EVENT_OBJECT_SHOW ? L"shown" : event == EVENT_OBJECT_HIDE ? L"hidden"
		: event == EVENT_OBJECT_REORDER ? L"reordered" : event == EVENT_OBJECT_PARENTCHANGE ? L"reparented" : L"moved";
	printf("      +%5llu ms  %-10ls %p %-28.28ls thread %lu  %ld,%ld %ldx%ld  parent %p\n", GetTickCount64() - g_tickWindows, what,
		(void*)hwnd, cls, thread, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, (void*)GetParent(hwnd));
}

// Sleep, with the messages the event hook needs pumped meanwhile.
static void Wait(DWORD ms)
{
	const ULONGLONG end = GetTickCount64() + ms;
	for (;;) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			DispatchMessageW(&msg);
		}
		const ULONGLONG now = GetTickCount64();
		if (now >= end) {
			return;
		}
		MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)(end - now), QS_ALLINPUT);
	}
}
#define Sleep Wait

struct Find { const wchar_t* cls; DWORD pid; HWND found; HWND except; };

static HWND TopLevel(const wchar_t* cls, HWND except = nullptr)
{
	Find f = { cls, g_pid, nullptr, except };
	EnumWindows([](HWND h, LPARAM l) -> BOOL {
		Find& f = *(Find*)l;
		DWORD pid = 0;
		GetWindowThreadProcessId(h, &pid);
		wchar_t name[64] = {};
		GetClassNameW(h, name, 64);
		if (pid == f.pid && IsWindowVisible(h) && h != f.except && !wcscmp(name, f.cls)) {
			f.found = h;
			return FALSE;
		}
		return TRUE;
	}, (LPARAM)&f);
	return f.found;
}

static HWND Child(HWND parent, const wchar_t* cls, int id)
{
	struct C { const wchar_t* cls; int id; HWND found; } c = { cls, id, nullptr };
	EnumChildWindows(parent, [](HWND h, LPARAM l) -> BOOL {
		C& c = *(C*)l;
		wchar_t name[64] = {};
		GetClassNameW(h, name, 64);
		if ((!c.cls || !wcscmp(name, c.cls)) && (c.id < 0 || GetDlgCtrlID(h) == c.id)) {
			c.found = h;
			return FALSE;
		}
		return TRUE;
	}, (LPARAM)&c);
	return c.found;
}

static HWND WaitFor(HWND (*get)(), int ms)
{
	for (int t = 0; t < ms; t += 100) {
		if (HWND h = get()) {
			return h;
		}
		Sleep(100);
	}
	return nullptr;
}

static HWND g_main = nullptr;

static void Key(WORD vk)
{
	INPUT in[2] = {};
	in[0].type = in[1].type = INPUT_KEYBOARD;
	in[0].ki.wVk = in[1].ki.wVk = vk;
	in[1].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(2, in, sizeof(INPUT));
}

// The mouse is put on the picture and moved a little, as a hand does; the player
// starts its two seconds from the last move. Then: is a pointer showing?
static void Wiggle()
{
	for (int i = 0; i < 6; i++) {
		INPUT in = {};
		in.type = INPUT_MOUSE;
		in.mi.dx = (i & 1) ? -4 : 4;
		in.mi.dy = (i & 1) ? -4 : 4;
		in.mi.dwFlags = MOUSEEVENTF_MOVE;
		SendInput(1, &in, sizeof(INPUT));
		Sleep(60);
	}
}

// bMove false is the hand that clicked OK and let go of the mouse: the pointer is
// where the button was, over the picture now, and nothing moves it again.
static void Pointer(const char* when, bool bMove = true)
{
	if (bMove) {
		RECT rc = {};
		GetWindowRect(g_main, &rc);
		SetForegroundWindow(g_main);
		SetCursorPos((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
		Wiggle();
	}
	// Sampled on the way: a pointer that was never there proves nothing by being gone.
	int showing = 0;
	for (int i = 0; i < 45; i++) {
		CURSORINFO now = { sizeof(now) };
		GetCursorInfo(&now);
		showing += ((now.flags & CURSOR_SHOWING) && now.hCursor) ? 1 : 0;
		Sleep(100);
	}
	printf("%-44s shown for %.1f s of 4.5, then ", when, showing / 10.0);
	when = "";
	CURSORINFO ci = { sizeof(ci) };
	GetCursorInfo(&ci);
	POINT pt = {};
	GetCursorPos(&pt);
	const HWND under = WindowFromPoint(pt);
	wchar_t cls[64] = {};
	GetClassNameW(under, cls, 64);
	const HWND video = Child(g_main, L"VRWindow", -1);
	const bool hidden = !(ci.flags & CURSOR_SHOWING) || !ci.hCursor;
	printf("%spointer %s | under it: %ls on thread %lu | video window %p on thread %lu, player on %lu\n",
		when, hidden ? "hidden" : "SHOWING", cls, GetWindowThreadProcessId(under, nullptr),
		(void*)video, video ? GetWindowThreadProcessId(video, nullptr) : 0, GetWindowThreadProcessId(g_main, nullptr));
	fflush(stdout);
}

static void Click(int x, int y, bool right = false)
{
	SetCursorPos(x, y);
	Wiggle();
	INPUT in[2] = {};
	in[0].type = in[1].type = INPUT_MOUSE;
	in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
	in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
	SendInput(2, in, sizeof(INPUT));
}

// Every popup menu window of the player that is up, newest last.
static std::vector<HWND> Popups()
{
	struct P { std::vector<HWND> v; } p;
	EnumWindows([](HWND h, LPARAM l) -> BOOL {
		DWORD pid = 0;
		GetWindowThreadProcessId(h, &pid);
		wchar_t name[16] = {};
		GetClassNameW(h, name, 16);
		if (pid == g_pid && IsWindowVisible(h) && !wcscmp(name, L"#32768")) {
			((P*)l)->v.insert(((P*)l)->v.begin(), h);   // EnumWindows goes top down
		}
		return TRUE;
	}, (LPARAM)&p);
	return p.v;
}

// The entry of a popup whose text holds one of two words; clicked where it is drawn.
static bool ClickEntry(HWND popup, const wchar_t* a, const wchar_t* b)
{
	const HMENU menu = (HMENU)SendMessageW(popup, MN_GETHMENU_MSG, 0, 0);
	for (int i = 0; menu && i < GetMenuItemCount(menu); i++) {
		wchar_t text[256] = {};
		GetMenuStringW(menu, i, text, 256, MF_BYPOSITION);
		if (g_verbose) {
			printf("   menu %d: \"%ls\"\n", i, text);
		}
		if (wcsstr(text, a) && (!b || wcsstr(text, b))) {
			RECT rc = {};
			if (GetMenuItemRect(nullptr, menu, i, &rc)) {
				Click((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
				return true;
			}
		}
	}
	return false;
}

// The way a hand does it: right click on the picture, Filters, the renderer.
static bool OpenOptionsByMouse()
{
	const HWND before = TopLevel(L"#32770");
	RECT rc = {};
	GetWindowRect(g_main, &rc);
	SetForegroundWindow(g_main);
	Click((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2, true);
	Sleep(700);
	std::vector<HWND> popups = Popups();
	if (popups.empty() || !ClickEntry(popups.back(), L"Filt", nullptr)) {
		printf("   no Filters entry in the context menu\n");
		Key(VK_ESCAPE);
		return false;
	}
	Sleep(700);
	popups = Popups();
	if (popups.size() < 2 || !(ClickEntry(popups.back(), L"MPC", L"rendu") || ClickEntry(popups.back(), L"MPC", L"Renderer"))) {
		printf("   no renderer under Filters\n");
		Key(VK_ESCAPE);
		Key(VK_ESCAPE);
		return false;
	}
	for (int t = 0; t < 5000; t += 100) {
		Sleep(100);
		const HWND dlg = TopLevel(L"#32770", before);
		if (dlg && Child(dlg, L"Button", (int)ID_APPLY_NOW_BTN)) {
			return true;
		}
	}
	printf("   the options did not open\n");
	return false;
}

// The Filters menu, asked for by its command, read for the entry that is the
// renderer, and closed again; the entry's command then opens the options.
static bool OpenOptions()
{
	const HWND before = TopLevel(L"#32770");
	SetForegroundWindow(g_main);
	PostMessageW(g_main, WM_COMMAND, ID_MENU_FILTERS, 0);
	const HWND popup = WaitFor([] { return TopLevel(L"#32768"); }, 3000);
	if (!popup) {
		printf("   the Filters menu did not open\n");
		return false;
	}
	const HMENU menu = (HMENU)SendMessageW(popup, MN_GETHMENU_MSG, 0, 0);
	UINT id = 0;
	for (int i = 0; menu && i < GetMenuItemCount(menu); i++) {
		wchar_t text[256] = {};
		GetMenuStringW(menu, i, text, 256, MF_BYPOSITION);
		if (g_verbose) {
			printf("   menu %d: \"%ls\"%s\n", i, text, GetSubMenu(menu, i) ? " >" : "");
		}
		// The player names the renderer in its own language.
		if (wcsstr(text, L"MPC") && (wcsstr(text, L"Renderer") || wcsstr(text, L"rendu"))) {
			// With pins to list the entry is a submenu, and its first line the options.
			const HMENU sub = GetSubMenu(menu, i);
			id = sub ? GetMenuItemID(sub, 0) : GetMenuItemID(menu, i);
		}
	}
	Key(VK_ESCAPE);
	Sleep(300);
	if (!id || id == (UINT)-1) {
		printf("   no MPC Video Renderer in the Filters menu\n");
		return false;
	}
	PostMessageW(g_main, WM_COMMAND, id, 0);
	for (int t = 0; t < 5000; t += 100) {
		Sleep(100);
		const HWND dlg = TopLevel(L"#32770", before);
		if (dlg && Child(dlg, L"Button", (int)ID_APPLY_NOW_BTN)) {
			return true;
		}
	}
	printf("   the options did not open\n");
	return false;
}

static void Visit(int comboId, int position)
{
	if (!(g_byMouse ? OpenOptionsByMouse() : OpenOptions())) {
		return;
	}
	const HWND dlg = TopLevel(L"#32770");
	Sleep(500);
	g_tickWindows = GetTickCount64();
	g_bWindowsNow = g_bWindows;
	if (position >= 0) {
		const HWND combo = Child(dlg, L"ComboBox", comboId);
		if (!combo) {
			printf("   no list %d in the options\n", comboId);
		} else {
			const int was = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
			SendMessageW(combo, CB_SETCURSEL, position, 0);
			SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(comboId, CBN_SELCHANGE), (LPARAM)combo);
			wchar_t text[128] = {};
			SendMessageW(combo, CB_GETLBTEXT, position, (LPARAM)text);
			printf("   list %d: position %d -> %d (%ls)\n", comboId, was, position, text);
			Sleep(300);
			if (!g_noApply) {
				if (HWND apply = Child(dlg, L"Button", (int)ID_APPLY_NOW_BTN)) {
					SendMessageW(apply, BM_CLICK, 0, 0);
				}
				Sleep(1500);
			}
		}
	}
	if (HWND ok = Child(dlg, L"Button", IDOK)) {
		// A real click, with the mouse brought to the button first.
		RECT rc = {};
		GetWindowRect(ok, &rc);
		SetCursorPos((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
		Wiggle();
		INPUT in[2] = {};
		in[0].type = in[1].type = INPUT_MOUSE;
		in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
		SendInput(2, in, sizeof(INPUT));
	}
	Sleep(800);
	g_bWindowsNow = false;
}

// A window's rectangle off the screen, as a BMP: what the user sees, theme and all.
static void SaveWindow(HWND hwnd, const std::wstring& path)
{
	RECT rc = {};
	GetWindowRect(hwnd, &rc);
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, bmp);
	BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
	const DWORD stride = ((w * 3 + 3) & ~3), size = stride * h;
	BITMAPFILEHEADER fh = {};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
	fh.bfSize = fh.bfOffBits + size;
	FILE* f = nullptr;
	if (!_wfopen_s(&f, path.c_str(), L"wb") && f) {
		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
		fwrite(bits, size, 1, f);
		fclose(f);
	}
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

// The options opened, each of the page's tabs brought up in turn and photographed,
// and the options cancelled.
static void ShootOptions()
{
	if (!OpenOptions()) {
		return;
	}
	const HWND dlg = TopLevel(L"#32770");
	Sleep(800);
	// The renderer's own tab control is the one with the most tabs; the sheet's has two.
	struct T { HWND best; int count; } t = {};
	EnumChildWindows(dlg, [](HWND h, LPARAM l) -> BOOL {
		wchar_t name[64] = {};
		GetClassNameW(h, name, 64);
		if (!wcscmp(name, L"SysTabControl32")) {
			const int n = (int)SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
			if (n > ((T*)l)->count) {
				((T*)l)->best = h;
				((T*)l)->count = n;
			}
		}
		return TRUE;
	}, (LPARAM)&t);
	printf("   options %p, tab control %p with %d tabs\n", (void*)dlg, (void*)t.best, t.count);
	// The mouse out of the way: a hovered control is drawn differently.
	RECT rc = {};
	GetWindowRect(dlg, &rc);
	SetCursorPos(rc.right + 40, rc.bottom + 40);
	for (int i = 0; i < (std::max)(t.count, 1); i++) {
		if (t.best) {
			// By message and without a pointer: the tab control is in another process,
			// and a rectangle asked of it would be written through our address in its
			// memory. Setting the focus tab selects it and tells the page.
			SendMessageW(t.best, TCM_SETCURFOCUS, i, 0);
		}
		Sleep(700);
		SaveWindow(dlg, g_shots + std::to_wstring(i) + L".bmp");
	}
	if (HWND cancel = Child(dlg, L"Button", IDCANCEL)) {
		SendMessageW(cancel, BM_CLICK, 0, 0);
	}
	Sleep(500);
}

// The mode is put back whatever happens: this is somebody's screen.
static DEVMODEW g_modeBefore = {};
static bool g_bModeChanged = false;

static void RestoreDisplayMode()
{
	if (g_bModeChanged) {
		ChangeDisplaySettingsExW(nullptr, &g_modeBefore, nullptr, 0, nullptr);
		g_bModeChanged = false;
		Sleep(1500);
	}
}

static bool SetDisplayHz(int hz)
{
	g_modeBefore.dmSize = sizeof(g_modeBefore);
	if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &g_modeBefore)) {
		return false;
	}
	DEVMODEW mode = g_modeBefore;
	mode.dmDisplayFrequency = hz;
	mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
	if (ChangeDisplaySettingsExW(nullptr, &mode, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) {
		return false;
	}
	g_bModeChanged = true;
	atexit(RestoreDisplayMode);
	Sleep(2500);
	return true;
}

// --attach: somebody else does the handling. For N seconds, every window the player
// makes or loses and every time the pointer appears or goes, each with its moment, so
// that the two can be laid side by side.
static int Attach(int seconds)
{
	const HWND main = FindWindowW(L"MediaPlayerClassicW", nullptr);
	if (!main) {
		printf("no MPC-HC is running\n");
		return 1;
	}
	GetWindowThreadProcessId(main, &g_pid);
	g_main = main;
	SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_PARENTCHANGE, nullptr, OnWinEvent, g_pid, 0, WINEVENT_OUTOFCONTEXT);
	g_tickWindows = GetTickCount64();
	g_bWindowsNow = true;
	const HWND video = Child(main, L"VRWindow", -1);
	printf("watching MPC-HC (process %lu, interface thread %lu) for %d s; video window %p on thread %lu\n", g_pid,
		GetWindowThreadProcessId(main, nullptr), seconds, (void*)video, video ? GetWindowThreadProcessId(video, nullptr) : 0);
	int was = -1;
	HWND underWas = nullptr;
	for (int i = 0; i < seconds * 20; i++) {
		Wait(50);
		CURSORINFO ci = { sizeof(ci) };
		GetCursorInfo(&ci);
		const int showing = ((ci.flags & CURSOR_SHOWING) && ci.hCursor) ? 1 : 0;
		POINT pt = {};
		GetCursorPos(&pt);
		const HWND under = WindowFromPoint(pt);
		if (showing != was || under != underWas) {
			wchar_t cls[64] = {};
			GetClassNameW(under, cls, 64);
			printf("      +%5llu ms  pointer %s at %ld,%ld over %p %.28ls thread %lu\n", GetTickCount64() - g_tickWindows,
				showing ? "SHOWING" : "hidden", pt.x, pt.y, (void*)under, cls, GetWindowThreadProcessId(under, nullptr));
			fflush(stdout);
			was = showing;
			underWas = under;
		}
	}
	return 0;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc >= 3 && !wcscmp(argv[1], L"--attach")) {
		return Attach(_wtoi(argv[2]));
	}
	if (argc < 3) {
		printf("mpchc_drive.exe <mpc-hc64.exe> <film> [--windowed] [--select A,B,...] [--combo ID]\n");
		return 2;
	}
	bool windowed = false;
	int comboId = 1063; // IDC_COMBO10, the HDR10/DV list
	std::vector<int> positions = { -1, 3, 1 };
	for (int i = 3; i < argc; i++) {
		if (!wcscmp(argv[i], L"--windowed")) {
			windowed = true;
		} else if (!wcscmp(argv[i], L"--hz") && i + 1 < argc) {
			g_hz = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--hold") && i + 1 < argc) {
			g_hold = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--size") && i + 1 < argc) {
			g_size = argv[++i];
		} else if (!wcscmp(argv[i], L"--windows")) {
			g_bWindows = true;
		} else if (!wcscmp(argv[i], L"--shots") && i + 1 < argc) {
			g_shots = argv[++i];
		} else if (!wcscmp(argv[i], L"--mouse")) {
			g_byMouse = true;
		} else if (!wcscmp(argv[i], L"--noapply")) {
			g_noApply = true;
		} else if (!wcscmp(argv[i], L"--verbose")) {
			g_verbose = true;
		} else if (!wcscmp(argv[i], L"--combo") && i + 1 < argc) {
			comboId = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--select") && i + 1 < argc) {
			positions.clear();
			for (wchar_t* p = argv[++i]; *p;) {
				positions.push_back(wcstol(p, &p, 10));
				if (*p == L',') {
					p++;
				}
			}
		}
	}

	std::wstring cmd = std::wstring(L"\"") + argv[1] + L"\" \"" + argv[2] + L"\" /new" + ((windowed || g_byMouse) ? L"" : L" /fullscreen");
	if (!g_size.empty()) {
		cmd += L" /fixedsize " + g_size;
	}
	if (g_hz > 0 && !SetDisplayHz(g_hz)) {
		printf("the screen would not go to %d Hz\n", g_hz);
		return 1;
	}
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		printf("could not start the player (%lu)\n", GetLastError());
		return 1;
	}
	g_pid = pi.dwProcessId;
	g_main = WaitFor([] { return TopLevel(L"MediaPlayerClassicW"); }, 15000);
	if (!g_main) {
		printf("the player's window never came\n");
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	if (g_bWindows) {
		SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_PARENTCHANGE, nullptr, OnWinEvent, g_pid, 0, WINEVENT_OUTOFCONTEXT);
	}
	for (int t = 0; t < 15000 && !Child(g_main, L"VRWindow", -1); t += 200) {
		Sleep(200);
	}
	Sleep(3000);

	POINT home = {};
	GetCursorPos(&home);

	if (g_byMouse && !windowed) {
		// Fullscreen entered while the film plays, as it is from the keyboard.
		PostMessageW(g_main, WM_COMMAND, 830 /* ID_VIEW_FULLSCREEN */, 0);
		Sleep(3000);
	}

	if (!g_shots.empty()) {
		ShootOptions();
		positions.clear();
	} else if (g_hold > 0) {
		Sleep(g_hold * 1000);
		positions.clear();
	} else {
		Pointer("playing, nothing touched:");
	}
	for (const int position : positions) {
		Visit(comboId, position);
		char when[64];
		if (position < 0) {
			sprintf_s(when, "options opened and closed, nothing changed:");
		} else {
			sprintf_s(when, "list %d put on %d, Apply, OK:", comboId, position);
		}
		Pointer(when, false);
		Pointer("   ... and once the mouse has moved again:");
	}

	SetCursorPos(home.x, home.y);
	PostMessageW(g_main, WM_CLOSE, 0, 0);
	if (WaitForSingleObject(pi.hProcess, 8000) != WAIT_OBJECT_0) {
		TerminateProcess(pi.hProcess, 0);
	}
	return 0;
}
