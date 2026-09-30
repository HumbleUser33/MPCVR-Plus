// The freeze, reproduced against the real player.
//
// The report is precise: pause and play tapped faster than one displayed picture --
// a double click on the space bar -- freezes the picture while the sound goes on,
// on a display at 24 or 25 Hz and never at 30 or 60. The bench's own graph does not
// show it, so this drives MPC-HC itself: it starts the player on a film, waits for
// the picture to move, then posts the space bar twice with a chosen gap between the
// two and reads the screen through desktop duplication, which is the only thing that
// sees what a flip model swap chain really put there.
//
// A still picture on its own proves nothing -- the player may simply be paused,
// because one of the two keys was swallowed. So the player is started in slave mode
// and says what it thinks it is doing: only "the player says it is playing and the
// screen does not move" is the freeze. When one is caught it then taps once to pause
// and once to play, which the report says clears it, and says whether it did.
//
//   player_tap.exe --film "D:\film.mkv"
//   player_tap.exe --film ... --player "C:\Program Files\MPC-HC\mpc-hc64.exe"
//   player_tap.exe --film ... --rounds 5 --settle 12 --gaps 0,5,20
//
// Nothing is installed and no setting is written: the player runs as it is.

#include <windows.h>
#include <atlbase.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include "screen_watch.inl"
#include "freeze_report.inl"

static const wchar_t* g_players[] = {
	L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe",
	L"C:\\Program Files\\MPC-BE x64\\mpc-be64.exe",
	L"C:\\Program Files (x86)\\MPC-HC\\mpc-hc.exe",
};

// What the player tells a slave host, from MPC-HC's own MpcApi.h. Only the two that
// matter here are named; everything else is printed once, raw, so a player that
// numbers them differently is visible rather than silently believed.
static const DWORD CMD_CONNECT  = 0x50000000;
static const DWORD CMD_PLAYMODE = 0x50000002;

// MPC_PLAYSTATE
static const int PS_PLAY = 0, PS_PAUSE = 1, PS_STOP = 2;

static std::atomic<int> g_playMode = -1;  // what the player last said it was doing
static std::atomic<HWND> g_playerApi = nullptr;  // the window it listens on
static bool g_bTrace = false;
static bool g_bDump = false;   // --dump: every thread of the player, the moment it freezes
static bool g_bStats = false;
static int g_waitAlone = 0;    // --wait <s>: on a catch, touch nothing and see if it comes back  // --stats: the renderer's own counters drawn in the corner

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_COPYDATA) {
		const auto* cds = (const COPYDATASTRUCT*)lp;
		std::wstring text;
		if (cds->cbData >= sizeof(wchar_t)) {
			text.assign((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
			while (!text.empty() && text.back() == L'\0') {
				text.pop_back();
			}
		}
		if (cds->dwData == CMD_CONNECT) {
			g_playerApi = (HWND)(ULONG_PTR)_wtoi64(text.c_str());
		} else if (cds->dwData == CMD_PLAYMODE) {
			g_playMode = _wtoi(text.c_str());
		}
		if (g_bTrace) {
			wprintf(L"    [player] 0x%08X %s\n", (unsigned)cds->dwData, text.c_str());
		}
		return TRUE;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// The window the player reports to lives on a thread of its own, which does nothing
// but read messages.
//
// It has to. The player sends WM_COPYDATA rather than posting it, so its own window
// thread waits for this program to answer -- and reading the screen takes the best
// part of a second inside DXGI, without touching a message queue. Put the two on one
// thread and the measurement freezes the player it is measuring: that is a freeze
// this program caused, indistinguishable in the report from the one being looked for.
// It cost a run to find out.
struct HostThread {
	std::thread thread;
	HWND hwnd = nullptr;
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	DWORD threadId = 0;

	void Start()
	{
		thread = std::thread([this] {
			threadId = GetCurrentThreadId();
			WNDCLASSEXW wc = { sizeof(wc) };
			wc.lpfnWndProc = HostWndProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"MpcvrPlayerTapHost";
			RegisterClassExW(&wc);
			hwnd = CreateWindowExW(0, wc.lpszClassName, L"player_tap", 0,
				0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
			SetEvent(ready);

			MSG msg;
			while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		});
		WaitForSingleObject(ready, 5000);
	}

	void Stop()
	{
		if (thread.joinable()) {
			PostThreadMessageW(threadId, WM_QUIT, 0, 0);
			thread.join();
		}
		CloseHandle(ready);
	}
};

struct FindWindowData {
	DWORD pid = 0;
	HWND hwnd = nullptr;
	long area = 0;
};

static BOOL CALLBACK FindMainWindow(HWND hwnd, LPARAM param)
{
	auto* data = (FindWindowData*)param;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != data->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
		return TRUE;
	}
	RECT rc = {};
	GetWindowRect(hwnd, &rc);
	const long area = (rc.right - rc.left) * (rc.bottom - rc.top);
	if (area > data->area) {
		data->area = area;
		data->hwnd = hwnd;
	}
	return TRUE;
}

// The window the picture is drawn in, a child of the player's main window. Desktop
// duplication reads a client rectangle, and a seek bar or a clock in the title never
// stops moving, so the closer to the picture alone the better.
static HWND VideoChild(HWND parent)
{
	HWND best = nullptr;
	long bestArea = 0;
	for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		if (!IsWindowVisible(child)) {
			continue;
		}
		RECT rc = {};
		GetWindowRect(child, &rc);
		const long area = (rc.right - rc.left) * (rc.bottom - rc.top);
		if (area > bestArea) {
			bestArea = area;
			best = child;
		}
	}
	// Go on down: the player puts a view inside the frame and the renderer puts its
	// own window inside that, and only the last one holds nothing but the picture.
	return best ? VideoChild(best) : parent;
}

static void Pump(int ms)
{
	const ULONGLONG end = GetTickCount64() + ms;
	for (;;) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		const ULONGLONG now = GetTickCount64();
		if (now >= end) {
			return;
		}
		MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)(end - now), QS_ALLINPUT);
	}
}

// One press of the space bar, posted rather than sent: the player reads it from its
// own message loop, exactly as it reads a real key.
static void TapSpace(HWND hwnd)
{
	PostMessageW(hwnd, WM_KEYDOWN, VK_SPACE, 0x00390001);
	PostMessageW(hwnd, WM_KEYUP, VK_SPACE, 0xC0390001);
}

// Whether the picture moves at all, looked at for up to about a second and a half.
// A film changes a good part of the frame from one picture to the next; a still
// picture re-presented moves nothing but its dither, so a threshold of two per cent
// of the points sampled separates the two cleanly.
static double g_lastMoved = 0;
// Which part of the window counts as the film. With the statistics drawn in the
// corner, the corner has to be left out of it or the counter alone would read as a
// film that is running.
static double g_film[4] = { 0.0, 0.0, 1.0, 1.0 };

static bool ScreenMoves(CScreenWatch& watch, HWND hwnd)
{
	if (!watch.Ready()) {
		return true;   // nothing can be said, so nothing is claimed
	}
	// Every interval has to move, not just one of them: the first reading after a
	// state change can still hold the picture from before it, and that one jump
	// would otherwise read as a film that is running.
	std::vector<BYTE> before = watch.Fingerprint(hwnd, g_film[0], g_film[1], g_film[2], g_film[3]);
	double least = 100.0;
	for (int look = 0; look < 4; look++) {
		Pump(400);
		std::vector<BYTE> now = watch.Fingerprint(hwnd, g_film[0], g_film[1], g_film[2], g_film[3]);
		least = std::min(least, CScreenWatch::Changed(before, now));
		before.swap(now);
	}
	g_lastMoved = least;
	return least > 2.0;
}

// How much the corner where the renderer draws its statistics moved. With --stats it
// answers the one question a frozen picture cannot: is the renderer still drawing and
// still reaching the screen, with only the film standing still, or is nothing at all
// getting there? The number is printed, not judged: a few digits changing is a small
// share of the corner.
static double CornerMoved(CScreenWatch& watch, HWND hwnd)
{
	if (!watch.Ready()) {
		return -1.0;
	}
	// The box starts ten pixels in from the corner and is a few hundred across, and
	// every point of it is read: the counters that move are a handful of digits.
	std::vector<BYTE> before = watch.Fingerprint(hwnd, 0.0, 0.0, 0.16, 0.20, 2, 2);
	double most = 0;
	for (int look = 0; look < 3; look++) {
		Pump(500);
		std::vector<BYTE> now = watch.Fingerprint(hwnd, 0.0, 0.0, 0.16, 0.20, 2, 2);
		most = std::max(most, CScreenWatch::Changed(before, now));
		before.swap(now);
	}
	return most;
}

static const char* ModeName(int mode)
{
	switch (mode) {
	case PS_PLAY:  return "playing";
	case PS_PAUSE: return "paused";
	case PS_STOP:  return "stopped";
	default:       return "silent";
	}
}

int wmain(int argc, wchar_t* argv[])
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	const wchar_t* film = nullptr;
	const wchar_t* player = nullptr;
	int rounds = 3;
	int settle = 12;
	std::vector<int> gaps = { 0, 2, 5, 10, 20, 40, 80 };

	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--film") && i + 1 < argc) {
			film = argv[++i];
		} else if (!wcscmp(argv[i], L"--player") && i + 1 < argc) {
			player = argv[++i];
		} else if (!wcscmp(argv[i], L"--rounds") && i + 1 < argc) {
			rounds = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--settle") && i + 1 < argc) {
			settle = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--trace")) {
			g_bTrace = true;
		} else if (!wcscmp(argv[i], L"--dump")) {
			g_bDump = true;
		} else if (!wcscmp(argv[i], L"--stats")) {
			g_bStats = true;
		} else if (!wcscmp(argv[i], L"--wait") && i + 1 < argc) {
			g_waitAlone = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--gaps") && i + 1 < argc) {
			gaps.clear();
			std::wstring list = argv[++i];
			size_t at = 0;
			while (at < list.size()) {
				const size_t comma = list.find(L',', at);
				gaps.push_back(_wtoi(list.substr(at, comma - at).c_str()));
				at = (comma == std::wstring::npos) ? list.size() : comma + 1;
			}
		}
	}
	if (!film) {
		printf("give a film: player_tap.exe --film \"D:\\film.mkv\"\n");
		return 1;
	}
	if (!player) {
		for (const wchar_t* candidate : g_players) {
			if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
				player = candidate;
				break;
			}
		}
	}
	if (!player) {
		printf("no player found. Give one: --player \"C:\\...\\mpc-hc64.exe\"\n");
		return 1;
	}

	// The window the player reports to, so a still picture can be told apart from a
	// paused one.
	HostThread host;
	host.Start();

	wchar_t command[2048] = {};
	swprintf_s(command, L"\"%s\" \"%s\" /play /slave %llu",
		player, film, (unsigned long long)(ULONG_PTR)host.hwnd);
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		wprintf(L"the player did not start: %s\n", command);
		return 1;
	}
	wprintf(L"Player  %s\nFilm    %s\n", player, film);

	HWND hwnd = nullptr;
	for (int i = 0; i < 200 && !hwnd; i++) {
		Pump(100);
		FindWindowData data;
		data.pid = pi.dwProcessId;
		EnumWindows(FindMainWindow, (LPARAM)&data);
		hwnd = data.hwnd;
	}
	if (!hwnd) {
		printf("the player never showed a window\n");
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	printf("waiting %d s for the film to settle\n", settle);
	Pump(settle * 1000);
	printf("the player says it is %s\n", ModeName(g_playMode));
	if (g_playMode < 0) {
		printf("it is not answering as a slave, so a still picture cannot be told from\n"
			   "a paused one; the rows below say only whether the screen moved.\n");
	}

	// The picture has to be the only thing in the read rectangle, and it has to be on
	// top: desktop duplication reads the desktop as it is composed, so a console
	// window lying over the player would be read as a picture that keeps moving.
	ShowWindow(hwnd, SW_MAXIMIZE);
	SetForegroundWindow(hwnd);
	Pump(1500);

	if (g_bStats) {
		// Ctrl+J. This one has to go through the keyboard for real: the player reads
		// it with an accelerator table, and an accelerator looks at the thread's key
		// state, which a posted message never touches.
		INPUT keys[4] = {};
		for (INPUT& key : keys) {
			key.type = INPUT_KEYBOARD;
		}
		keys[0].ki.wVk = VK_CONTROL;
		keys[1].ki.wVk = 'J';
		keys[2].ki.wVk = 'J';
		keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
		keys[3].ki.wVk = VK_CONTROL;
		keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput((UINT)std::size(keys), keys, sizeof(INPUT));
		Pump(1500);
		// The corner now carries the counters, so the film is read away from it.
		g_film[0] = 0.30;
		g_film[1] = 0.30;
		printf("the renderer's statistics are on; the film is read away from the corner\n");
	}

	const HWND video = VideoChild(hwnd);
	{
		wchar_t className[64] = {};
		GetClassNameW(video, className, (int)std::size(className));
		RECT rc = {};
		GetClientRect(video, &rc);
		wprintf(L"reading %ldx%ld of the %s window\n", rc.right, rc.bottom, className);
	}
	CScreenWatch watch;
	const std::string error = watch.Start(video);
	if (!error.empty()) {
		printf("screen watch: %s -- nothing can be said about the picture\n", error.c_str());
	}

	// Before anything is claimed about a picture that stopped, the reading has to be
	// shown to tell a stopped picture from a running one at all. When it cannot --
	// another window over the player, a picture that never started -- the run stops
	// here rather than printing sixty rows that mean nothing. Twice it has.
	for (int attempt = 0; ; attempt++) {
		TapSpace(hwnd);
		Pump(1500);
		const bool bStillWhenPaused = !ScreenMoves(watch, video);
		const double pausedMoved = g_lastMoved;
		TapSpace(hwnd);
		Pump(1500);
		const bool bMovingWhenPlaying = ScreenMoves(watch, video);
		const double movedWhilePlaying = g_lastMoved;
		printf("reading check: paused moves %.2f%% of the picture, playing moves %.2f%% -- %s\n",
			pausedMoved, movedWhilePlaying,
			(bStillWhenPaused && bMovingWhenPlaying) ? "good"
				: "the screen column below cannot be believed");
		if (g_bStats) {
			const double corner = CornerMoved(watch, video);
			printf("reading check: the statistics corner moves %.2f%% while playing -- %s\n",
				corner, corner > 0.05 ? "good" : "the counters cannot be read, so --stats says nothing");
		}
		if (bStillWhenPaused && bMovingWhenPlaying) {
			break;
		}
		if (attempt >= 2) {
			printf("\nthe picture cannot be read, so nothing below would mean anything. Stopping.\n"
				   "Something is over the player's window, or the film is not playing.\n");
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			Pump(2000);
			TerminateProcess(pi.hProcess, 0);
			host.Stop();
			return 1;
		}
		printf("trying again: putting the player back in front\n");
		ShowWindow(hwnd, SW_MAXIMIZE);
		SetForegroundWindow(hwnd);
		Pump(2000);
	}

	printf("\n  the space bar twice, %d times for each gap. FROZEN means the player says it\n", rounds);
	printf("  is playing and the screen does not move.\n\n");
	printf("  %-26s %9s %9s  %s\n", "gap between the two taps", "player", "screen", "then a slow pause and play");

	// The player's own name for the process, so the report below says whose threads
	// these are, and the filter's pdb beside its .ax so its frames carry names.
	char symbolPath[1024] = {};
	{
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring dir = exe;
		dir = dir.substr(0, dir.find_last_of(L'\\'));
		const std::wstring search = dir + L"\\..\\..\\_bin\\Filter_x64;" + dir
			+ L";C:\\Program Files\\MPC-HC\\MPCVR";
		WideCharToMultiByte(CP_ACP, 0, search.c_str(), -1, symbolPath, sizeof(symbolPath), nullptr, nullptr);
	}

	int failures = 0;
	int lost = 0;
	int dumped = 0;
	// Each round starts from a picture that is really moving. When it will not, the
	// freeze from the round before never went away, and that is worth saying once.
	const auto Playing = [&] {
		for (int fix = 0; fix < 4 && g_playMode >= 0 && g_playMode != PS_PLAY; fix++) {
			TapSpace(hwnd);
			Pump(700);
		}
	};
	const auto Dump = [&](const char* what) {
		if (!g_bDump || dumped) {
			return;
		}
		dumped++;
		printf("\n  --- %s: every thread of the player, process %lu ---\n", what, pi.dwProcessId);
		fopen_s(&g_freezeFile, "player_tap_freeze.txt", "w");
		FreezeSay("%s, process %lu\n\n", what, pi.dwProcessId);
		// Three of them, seconds apart. One stack says where a thread was; three say
		// whether it is stuck there. The streaming thread waits inside Present for up
		// to a whole refresh in the ordinary way, which at 24 Hz is 42 ms -- a single
		// snapshot cannot tell that apart from a wait that never ends.
		for (int shot = 0; shot < 3; shot++) {
			if (shot) {
				Pump(1500);
			}
			FreezeSay("\n===== snapshot %d, %.1f s in =====\n\n", shot + 1, shot * 1.5);
			FreezeReport(pi.dwProcessId, 0, symbolPath);
		}
		if (g_freezeFile) {
			fclose(g_freezeFile);
			g_freezeFile = nullptr;
		}
		printf("  --- also written to player_tap_freeze.txt ---\n\n");
	};

	for (const int gap : gaps) {
		for (int round = 0; round < rounds; round++) {
			char name[64] = {};
			sprintf_s(name, "%d ms, round %d", gap, round + 1);

			Playing();
			if (!ScreenMoves(watch, video)) {
				// Still frozen from before. A slow pause and play is what the report
				// says clears it, so that is what is tried.
				if (g_bStats) {
					const double corner = CornerMoved(watch, video);
					printf("      the statistics corner moved %.2f%% while the picture stood still: %s\n",
						corner, corner > 0.05 ? "the renderer is still drawing and still reaching the screen"
											  : "nothing at all is reaching the screen");
				}
				Dump("frozen, before any tap of this round");
				TapSpace(hwnd);
				Pump(600);
				TapSpace(hwnd);
				Pump(1000);
				const bool bBack = ScreenMoves(watch, video);
				printf("  %-26s %9s %9s  %s\n", name, ModeName(g_playMode), "still",
					bBack ? "still frozen from before; a slow pause and play cleared it"
						  : "still frozen from before; a slow pause and play did NOT clear it");
				failures++;
				if (!bBack) {
					Playing();
					Pump(1500);
				}
				continue;
			}

			TapSpace(hwnd);
			if (gap) {
				Pump(gap);
			}
			TapSpace(hwnd);

			Pump(1200);
			const int mode = g_playMode;
			const bool bMoving = ScreenMoves(watch, video);
			const char* verdict = "";
			if (mode >= 0 && mode != PS_PLAY) {
				// Only one of the two presses took; the player is simply paused.
				lost++;
				verdict = "one press took, not a freeze";
			} else if (!bMoving) {
				failures++;
				// Left alone, does it come back? A pause and a play look like they
				// clear it, but so would simply waiting: everything the program does
				// after catching one takes seconds. Nothing is touched here until the
				// picture moves again or the patience runs out, and the answer
				// decides what kind of fault this is.
				if (g_waitAlone) {
					const ULONGLONG t0 = GetTickCount64();
					bool bBack = false;
					while (!bBack && (int)((GetTickCount64() - t0) / 1000) < g_waitAlone) {
						bBack = ScreenMoves(watch, video);
					}
					printf("      left alone: %s after %.1f s\n",
						bBack ? "it came back by itself" : "still frozen",
						(GetTickCount64() - t0) / 1000.0);
				}
				if (g_bStats) {
					const double corner = CornerMoved(watch, video);
					printf("      the statistics corner moved %.2f%% while the picture stood still: %s\n",
						corner, corner > 0.05 ? "the renderer is still drawing and still reaching the screen"
											  : "nothing at all is reaching the screen");
				}
				Dump("frozen by the tap");
				TapSpace(hwnd);
				Pump(600);
				TapSpace(hwnd);
				Pump(1200);
				verdict = ScreenMoves(watch, video) ? "a slow pause and play cleared it"
													: "a slow pause and play did NOT clear it";
			}
			printf("  %-26s %9s %9s  %s\n", name, ModeName(mode),
				bMoving ? "moving" : "FROZEN", verdict);
		}
	}

	PostMessageW(hwnd, WM_CLOSE, 0, 0);
	Pump(2000);
	if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) {
		TerminateProcess(pi.hProcess, 0);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	host.Stop();

	if (lost) {
		printf("\n%d tap(s) reached the player as one press, which says nothing either way.\n", lost);
	}
	printf("\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
