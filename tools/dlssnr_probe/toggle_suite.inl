// --toggle: changing a setting while the film plays.
//
// The player calls SetSettings on the thread that owns the video window, and that
// thread stops reading its messages until the call returns. Anything the streaming
// thread sends to a window of that thread then waits for it, and a wait on both
// sides is a freeze the player never comes out of. This suite makes the same call
// from the same thread, times it, checks the picture goes on afterwards, and when a
// call does not come back prints where every thread stands.

#include "freeze_report.inl"

struct Watchdog {
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::thread thread;

	// Prints every stack and ends the process: after a freeze nothing else can run.
	Watchdog(const char* what, int seconds)
	{
		thread = std::thread([this, what, seconds] {
			if (WaitForSingleObject(done, seconds * 1000) == WAIT_OBJECT_0) {
				return;
			}
			printf("   FREEZE: %s did not come back in %d s\n\n", what, seconds);
			FreezeReport(GetCurrentProcessId(), GetCurrentThreadId(), "..\\..\\_bin\\Filter_x64");
			printf("\n1 failure(s)\n");
			fflush(stdout);
			TerminateProcess(GetCurrentProcess(), 2);
		});
	}
	~Watchdog()
	{
		SetEvent(done);
		thread.join();
		CloseHandle(done);
	}
};

// What one toggle does to the settings.
struct Toggle {
	const char* name;
	void (*apply)(Settings_t& sets);
};

// --toggle --hdr: what the user sees freeze, which all leave HDR output behind. The
// display is left exactly as it is: HDR passthrough is turned on, but "Windows HDR"
// stays Do not change, so the filter never switches the screen itself. It only means
// anything while the desktop is already in HDR, which the suite says.
static const Toggle g_togglesHdr[] = {
	{ "RTX Video HDR on",                       [](Settings_t& s) { s.bVPRTXVideoHDR = true; } },
	{ "NV12 off, with RTX Video HDR on",        [](Settings_t& s) { s.VPFmts.bNV12 = false; } },
	{ "NV12 back on",                           [](Settings_t& s) { s.VPFmts.bNV12 = true; } },
	{ "Replace VP chroma on",                   [](Settings_t& s) { s.bVPReplaceChroma = true; } },
	{ "Replace VP chroma off, RTX Video HDR on",[](Settings_t& s) { s.bVPReplaceChroma = false; } },
	{ "RTX Video HDR off",                      [](Settings_t& s) { s.bVPRTXVideoHDR = false; } },
	{ "RTX Video HDR on again",                 [](Settings_t& s) { s.bVPRTXVideoHDR = true; } },
};

// --toggle --switch: the processor's own extras as the chroma moves to the shaders
// and back. On a 10-bit source RTX Video HDR only does something on a 4:4:4 picture,
// and Super Resolution only on a subsampled one, so each should follow the box.
static const Toggle g_togglesSwitch[] = {
	{ "Replace VP chroma on",                   [](Settings_t& s) { s.bVPReplaceChroma = true; } },
	{ "RTX Video HDR on",                       [](Settings_t& s) { s.bVPRTXVideoHDR = true; } },
	{ "Replace VP chroma off",                  [](Settings_t& s) { s.bVPReplaceChroma = false; } },
	{ "Replace VP chroma on again",             [](Settings_t& s) { s.bVPReplaceChroma = true; } },
	{ "Super Resolution on (chroma replaced)",  [](Settings_t& s) { s.bVPScaling = true; s.iVPSuperRes = SUPERRES_1440p; } },
	{ "Replace VP chroma off, Super Resolution on", [](Settings_t& s) { s.bVPReplaceChroma = false; } },
	{ "Replace VP chroma on, Super Resolution on",  [](Settings_t& s) { s.bVPReplaceChroma = true; } },
	// The other way round: the box off first, then the setting ticked, which is the
	// order the option is met in the player.
	{ "Super Resolution off",                   [](Settings_t& s) { s.bVPScaling = false; s.iVPSuperRes = SUPERRES_Disable; } },
	{ "Replace VP chroma off, no Super Resolution", [](Settings_t& s) { s.bVPReplaceChroma = false; } },
	{ "Super Resolution on (chroma not replaced)", [](Settings_t& s) { s.bVPScaling = true; s.iVPSuperRes = SUPERRES_1440p; } },
	{ "RTX Video HDR off then on",              [](Settings_t& s) { s.bVPRTXVideoHDR = !s.bVPRTXVideoHDR; } },
};

static const Toggle g_toggles[] = {
	{ "NV12 off (hardware processor dropped)",  [](Settings_t& s) { s.VPFmts.bNV12 = false; } },
	{ "NV12 back on",                           [](Settings_t& s) { s.VPFmts.bNV12 = true; } },
	{ "RTX Video HDR on",                       [](Settings_t& s) { s.bVPRTXVideoHDR = true; } },
	{ "RTX Video HDR off",                      [](Settings_t& s) { s.bVPRTXVideoHDR = false; } },
	{ "Replace VP chroma upsampling on",        [](Settings_t& s) { s.bVPReplaceChroma = true; } },
	{ "Chroma upsampling to RAVU-zoom",         [](Settings_t& s) { s.iChromaScaling = CHROMA_RAVU; } },
	{ "Chroma upsampling to Jinc (EWA)",        [](Settings_t& s) { s.iChromaScaling = CHROMA_Jinc; } },
	{ "Chroma upsampling to FSRCNNX 8 AR",      [](Settings_t& s) { s.iChromaScaling = CHROMA_FSRCNNX8AR; } },
	{ "Replace VP chroma upsampling off",       [](Settings_t& s) { s.bVPReplaceChroma = false; } },
	{ "Upscaling to ArtCNN C4F16 DS",           [](Settings_t& s) { s.iUpscaling = UPSCALE_ArtCNN; } },
	{ "Upscaling to FSRCNNX 16 AR",             [](Settings_t& s) { s.iUpscaling = UPSCALE_FSRCNNX16AR; } },
	{ "Super Resolution on",                    [](Settings_t& s) { s.bVPScaling = true; s.iVPSuperRes = SUPERRES_SD; } },
	{ "Super Resolution off",                   [](Settings_t& s) { s.bVPScaling = false; } },
	{ "DLSS SR on",                             [](Settings_t& s) { s.bDlssSR = true; } },
	{ "DLSS SR off",                            [](Settings_t& s) { s.bDlssSR = false; } },
};

static int RunToggleSuite(HMODULE hFilter, HWND hwnd, SIZE source, SIZE window, int seconds)
{
	HRESULT hr = S_OK;
	CComPtr<IFilterGraph2> pGraph;
	if (FAILED(pGraph.CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER))) {
		printf("no filter graph\n");
		return 1;
	}
	using PFN_DllGetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
	const auto pfnGetClassObject = (PFN_DllGetClassObject)GetProcAddress(hFilter, "DllGetClassObject");
	CComPtr<IClassFactory> pFactory;
	CComPtr<IBaseFilter> pRenderer;
	if (!pfnGetClassObject || FAILED(pfnGetClassObject(CLSID_MpcVideoRenderer, IID_IClassFactory, (LPVOID*)&pFactory))
			|| FAILED(pFactory->CreateInstance(nullptr, IID_IBaseFilter, (void**)&pRenderer))) {
		printf("the renderer could not be created\n");
		return 1;
	}

	CComQIPtr<IVideoRenderer> pVR(pRenderer.p);
	Settings_t sets;
	pVR->GetSettings(sets);
	sets.bUseD3D11 = true;
	sets.bShowStats = true;
	sets.bExclusiveFS = false;
	// The test never touches the screen: "Windows HDR" stays Do not change, so the
	// filter switches nothing by itself. With --hdr it does ask for HDR passthrough,
	// which only means something while the desktop is already in HDR.
	sets.bHdrPassthrough = g_bToggleHdr;
	sets.iHdrToggleDisplay = HDRTD_Disabled;
	sets.bDlssNR = false;
	sets.bDlssSR = false;
	sets.bDlssRenderAhead = true;
	sets.VPFmts = { true, true, true, true };
	// The settings the toggles move start from a known place, not from whatever the
	// registry of this machine holds.
	sets.bVPReplaceChroma = false;
	// RAVU-zoom is what the user runs, and a prescaler is what puts render ahead to
	// work: a rebuild then happens with pictures already in its queue.
	sets.iChromaScaling = g_bToggleHdr ? CHROMA_RAVU : CHROMA_CatmullRom;
	sets.bVPRTXVideoHDR = false;
	sets.bVPScaling = false;
	pVR->SetSettings(sets);

	CComQIPtr<IVideoWindow> pVW(pRenderer.p);
	CComQIPtr<IBasicVideo> pBV(pRenderer.p);
	pVW->put_Owner((OAHWND)hwnd);
	pVW->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
	pVW->SetWindowPosition(0, 0, window.cx, window.cy);
	pBV->SetDestinationPosition(0, 0, window.cx, window.cy);

	CComPtr<IBaseFilter> pSource;
	if (g_filmFile) {
		const std::string error = BuildFileGraph(pGraph, g_filmFile, pRenderer);
		if (!error.empty()) {
			printf("%s\n", error.c_str());
			return 1;
		}
		wprintf(L"Playing %s\n", g_filmFile);
	} else if (g_bToggleGpu) {
		// The pictures come as D3D11 textures, the way a hardware decoder sends
		// them: the renderer then runs on that device and copies from its array.
		pSource = new CD3D11FilmSource(&hr, source.cx, source.cy);
		if (FAILED(hr)) {
			printf("the D3D11 source could not be created: 0x%08X\n", (unsigned)hr);
			return 1;
		}
		pGraph->AddFilter(pSource, L"D3D11 film source");
		pGraph->AddFilter(pRenderer, L"MPC Video Renderer");
		if (FAILED(pGraph->ConnectDirect(GetPin(pSource, PINDIR_OUTPUT), GetPin(pRenderer, PINDIR_INPUT), nullptr))) {
			printf("connection failed\n");
			return 1;
		}
	} else {
		pSource = new CFilmSource(&hr, source.cx, source.cy);
		pGraph->AddFilter(pSource, L"Film source");
		pGraph->AddFilter(pRenderer, L"MPC Video Renderer");
		if (FAILED(pGraph->ConnectDirect(GetPin(pSource, PINDIR_OUTPUT), GetPin(pRenderer, PINDIR_INPUT), nullptr))) {
			printf("connection failed\n");
			return 1;
		}
	}

	CComQIPtr<IMediaControl> pMC(pGraph.p);
	if (g_filmSeek > 0) {
		SeekTo(pGraph, g_filmSeek);
	}
	pMC->Run();
	Pump(5000);   // sessions and features settle
	// Which filter gives the graph its clock: the video renderer waits on it for
	// each picture's turn, and a graph clocked by the sound is the player's case.
	if (CComQIPtr<IMediaFilter> pMediaFilter = pGraph.p) {
		CComPtr<IReferenceClock> pClock;
		if (SUCCEEDED(pMediaFilter->GetSyncSource(&pClock)) && pClock) {
			CComQIPtr<IBaseFilter> pClockFilter(pClock.p);
			FILTER_INFO info = {};
			if (pClockFilter && SUCCEEDED(pClockFilter->QueryFilterInfo(&info))) {
				wprintf(L"Clock: %s\n", info.achName);
				if (info.pGraph) {
					info.pGraph->Release();
				}
			} else {
				printf("Clock: the system clock\n");
			}
		} else {
			printf("Clock: none\n");
		}
	}

	// The screen itself, watched apart from what the renderer believes it drew.
	CScreenWatch watch;
	const std::string watchError = watch.Start(hwnd);
	if (!watchError.empty()) {
		printf("screen watch: %s -- the screen column says nothing\n", watchError.c_str());
	}

	printf("Each setting is changed while the film plays, from the thread that owns the\n");
	printf("window, as the player does. A call that does not come back in %d s is a freeze.\n", seconds);
	if (g_bToggleHdr) {
		printf("HDR passthrough is on, so the display has to be in HDR already for this to say\n");
		printf("anything; the filter is not allowed to switch it.\n");
	}
	{
		const std::wstring text = StatsText(pRenderer);
		if (g_bToggleGpu) {
			wprintf(L"--- what the renderer says before any change ---\n%s\n---\n", text.c_str());
		}
		wprintf(L"\n  input: %s\n  before any change: %s | HDR: %s\n\n",
			StatsLine(text, L"Input format  : ").c_str(),
			StatsLine(text, L"VideoProcessor: ").c_str(),
			StatsLine(text, L"HDR processing: ").c_str());
	}
	printf("  %-38s %8s %7s %8s  %s\n", "setting changed", "call (s)", "fps", "screen", "what the picture goes through");

	int failures = 0;
	const Toggle* toggles = g_bToggleSwitch ? g_togglesSwitch : g_bToggleHdr ? g_togglesHdr : g_toggles;
	const size_t toggleCount = g_bToggleSwitch ? std::size(g_togglesSwitch)
		: g_bToggleHdr ? std::size(g_togglesHdr) : std::size(g_toggles);
	for (size_t t = 0; t < toggleCount; t++) {
		const Toggle& toggle = toggles[t];
		toggle.apply(sets);
		const ULONGLONG t0 = GetTickCount64();
		{
			Watchdog watchdog(toggle.name, seconds);
			if (g_bToggleThread) {
				// The player applies a setting from its property page, which is not
				// the thread that owns the video window; meanwhile that thread is in
				// the renderer of its own accord, repainting. Whatever the rebuild
				// sends to the window then waits for a thread that is itself waiting
				// for the renderer's lock.
				std::atomic<bool> done{ false };
				std::thread page([&] {
					pVR->SetSettings(sets);
					done = true;
				});
				while (!done) {
					long size = 0;
					pBV->GetCurrentImage(&size, nullptr);   // takes the renderer's lock, as a repaint does
					Pump(5);
				}
				page.join();
			} else {
				pVR->SetSettings(sets);
			}
		}
		const double took = (GetTickCount64() - t0) / 1000.0;

		// The rebuild restarts the frame count, so what says the picture goes on is
		// two readings after the change, not one against what came before.
		Pump(1500);
		int framesA = 0, framesB = 0, skipped = 0;
		ParseSkipped(StatsText(pRenderer), framesA, skipped);
		Pump(1500);
		std::wstring text = StatsText(pRenderer);
		ParseSkipped(text, framesB, skipped);

		const double fps = (framesB - framesA) / 1.5;
		std::wstring processor = StatsLine(text, L"VideoProcessor: ");
		const std::wstring scaling = StatsLine(text, L"Scaling       : ");
		if (!scaling.empty()) {
			processor += L" | " + scaling;
		}
		const std::wstring hdr = StatsLine(text, L"HDR processing: ");
		if (!hdr.empty()) {
			processor += L" | HDR: " + hdr;
		}
		const std::wstring chroma = StatsLine(text, L"Chroma scaling: ");
		if (!chroma.empty()) {
			processor += L", chroma " + chroma;
		}
		// The renderer may draw every picture into a swap chain the screen no
		// longer shows, so what counts is whether the window itself changes.
		// Three readings, not two: the desktop hands out a frame only when something
		// changed, so a single quiet moment must not read as a frozen screen.
		bool bScreenMoves = !watch.Ready();
		uint64_t before = watch.Signature(hwnd);
		for (int look = 0; look < 3 && !bScreenMoves; look++) {
			Pump(400);
			const uint64_t now = watch.Signature(hwnd);
			bScreenMoves = (now != before);
			before = now;
		}
		printf("  %-38s %8.2f %7.1f %8s  %S%s\n", toggle.name, took, fps,
			bScreenMoves ? "moving" : "FROZEN", processor.c_str(),
			fps > 1.0 && bScreenMoves ? "" : "   FAIL");
		if (fps <= 1.0 || !bScreenMoves) {
			failures++;
		}
	}

	// The double click: paused, the window changes, playback resumes. What the
	// player really does is pause, go full screen and run again within a few
	// milliseconds of each other, and the report says it freezes there -- on a
	// display at 25 Hz, and not at 60. So the same three things, in that order,
	// as fast as the renderer will take them.
	if (g_bToggleWindow) {
		RECT rcMon = { 0, 0, 1920, 1080 };
		if (HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
			MONITORINFO mi = { sizeof(mi) };
			if (GetMonitorInfoW(hMon, &mi)) {
				rcMon = mi.rcMonitor;
			}
		}
		RECT rcWindowed = {};
		GetWindowRect(hwnd, &rcWindowed);
		const LONG_PTR styleWindowed = GetWindowLongPtrW(hwnd, GWL_STYLE);
		bool bFull = false;

		// A real switch, not a resize: the player takes the frame off the window and
		// lays it over the whole monitor, which is what Alt-Enter does and what the
		// renderer reads as full screen.
		const auto GoFullScreen = [&](bool set) {
			bFull = set;
			if (set) {
				SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
				SetWindowPos(hwnd, HWND_TOP, rcMon.left, rcMon.top,
					rcMon.right - rcMon.left, rcMon.bottom - rcMon.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			} else {
				SetWindowLongPtrW(hwnd, GWL_STYLE, styleWindowed);
				SetWindowPos(hwnd, HWND_TOP, rcWindowed.left, rcWindowed.top,
					rcWindowed.right - rcWindowed.left, rcWindowed.bottom - rcWindowed.top,
					SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			}
			RECT rc = {};
			GetClientRect(hwnd, &rc);
			pVW->SetWindowPosition(0, 0, rc.right, rc.bottom);
			pBV->SetDestinationPosition(0, 0, rc.right, rc.bottom);
		};

		// Pause and run back to back, with nothing between them: the report is that
		// the two have to land inside one displayed frame, which at 25 Hz is 40 ms
		// and at 60 is 17 -- and that it freezes at the low end only.
		enum Order { PauseRunThenSwitch, PauseSwitchRun, SwitchPauseRun, PauseRunAroundSwitch };
		static const char* const orderName[] = {
			"pause+run, then full screen",
			"pause, full screen, run",
			"full screen, pause+run",
			"pause, run, full screen at once",
		};

		printf("\n  the double click: pause and run inside one frame, with a real full-screen\n");
		printf("  switch between %ldx%ld and %ldx%ld\n\n", window.cx, window.cy,
			rcMon.right - rcMon.left, rcMon.bottom - rcMon.top);
		printf("  %-38s %8s %7s %8s\n", "step", "took s", "fps", "screen");

		for (int pass = 0; pass < 8; pass++) {
			const Order order = (Order)(pass % 4);
			const bool bTo = !bFull;
			char name[64] = {};
			sprintf_s(name, "%s, to %s", orderName[order], bTo ? "full screen" : "window");

			const ULONGLONG t0 = GetTickCount64();
			{
				Watchdog watchdog(name, seconds);
				switch (order) {
				case PauseRunThenSwitch:
					pMC->Pause();
					pMC->Run();
					GoFullScreen(bTo);
					break;
				case PauseSwitchRun:
					pMC->Pause();
					GoFullScreen(bTo);
					pMC->Run();
					break;
				case SwitchPauseRun:
					GoFullScreen(bTo);
					pMC->Pause();
					pMC->Run();
					break;
				default:
					pMC->Pause();
					pMC->Run();
					GoFullScreen(bTo);
					pMC->Pause();
					pMC->Run();
					break;
				}
			}
			const double took = (GetTickCount64() - t0) / 1000.0;

			Pump(1500);
			int framesA = 0, framesB = 0, skipped = 0;
			ParseSkipped(StatsText(pRenderer), framesA, skipped);
			Pump(1500);
			ParseSkipped(StatsText(pRenderer), framesB, skipped);
			const double fps = (framesB - framesA) / 1.5;

			bool bScreenMoves = !watch.Ready();
			uint64_t before = watch.Signature(hwnd);
			for (int look = 0; look < 3 && !bScreenMoves; look++) {
				Pump(400);
				const uint64_t now = watch.Signature(hwnd);
				bScreenMoves = (now != before);
				before = now;
			}
			printf("  %-38s %8.2f %7.1f %8s%s\n", name, took, fps,
				bScreenMoves ? "moving" : "FROZEN", (fps > 1.0 && bScreenMoves) ? "" : "   FAIL");
			if (fps <= 1.0 || !bScreenMoves) {
				failures++;
			}
		}
		if (bFull) {
			GoFullScreen(false);
		}
	}

	pMC->Stop();
	pGraph->RemoveFilter(pRenderer);
	if (pSource) {
		pGraph->RemoveFilter(pSource);
	}
	printf("\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
