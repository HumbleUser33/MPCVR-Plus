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
#include <commdlg.h>
#include "resource.h"
#include "Helper.h"
#include "../../Include/FilterInterfaces.h"
#include <uxtheme.h>
#include "SettingsPage.h"

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

// Combo boxes are addressed by window handle here rather than by dialog id,
// because the control is not a child of the page but of one of its sections.

static void Combo_AddData(HWND hCombo, LPCWSTR str, LONG_PTR data)
{
	if (!hCombo) {
		return;
	}
	const LRESULT index = SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)str);
	if (index != CB_ERR) {
		SendMessageW(hCombo, CB_SETITEMDATA, index, data);
	}
}

static LONG_PTR Combo_CurData(HWND hCombo)
{
	LRESULT value = hCombo ? SendMessageW(hCombo, CB_GETCURSEL, 0, 0) : CB_ERR;
	if (value != CB_ERR) {
		value = SendMessageW(hCombo, CB_GETITEMDATA, value, 0);
	}
	return value;
}

static void Combo_SelectData(HWND hCombo, LONG_PTR data)
{
	const LRESULT count = hCombo ? SendMessageW(hCombo, CB_GETCOUNT, 0, 0) : CB_ERR;
	for (LRESULT i = 0; count != CB_ERR && i < count; i++) {
		if (SendMessageW(hCombo, CB_GETITEMDATA, i, 0) == data) {
			SendMessageW(hCombo, CB_SETCURSEL, i, 0);
			break;
		}
	}
}

// The network strengths are stored x100 and shown as 1.00.
static std::wstring StrengthText(int value)
{
	return std::format(L"{:.2f}", (float)value / DLSSNR_STR_SCALE);
}

// The stabilizer is a plain 0..100, where 0 turns it off.
static std::wstring StabilizerText(int value)
{
	return value ? std::to_wstring(value) : std::wstring(L"off");
}

static HWND CreateHintWindow(HWND parent, int timePop)
{
	HWND hhint = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
		WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, nullptr, nullptr);

	::SetWindowPos(hhint, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(timePop, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELONG(70, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_RESHOW, MAKELONG(7, 0));
	SendMessageW(hhint, TTM_SETMAXTIPWIDTH, 0, 470);
	return hhint;
}

// What the list on the left offers, in the order the picture goes through them.
static const struct {
	int section;
	int dialogId;
	const wchar_t* name;
} g_sections[] = {
	{ CVRSettingsPPage::SECTION_Source,  IDD_SECTION_SOURCE,  L"Source"       },
	{ CVRSettingsPPage::SECTION_Chroma,  IDD_SECTION_CHROMA,  L"Chroma"       },
	{ CVRSettingsPPage::SECTION_Scaling, IDD_SECTION_SCALING, L"Scaling"      },
	{ CVRSettingsPPage::SECTION_Detail,  IDD_SECTION_DETAIL,  L"Detail"       },
	{ CVRSettingsPPage::SECTION_DlssNR,  IDD_SECTION_DLSSNR,  L"DLSS 5 NR"    },
	{ CVRSettingsPPage::SECTION_Hdr,     IDD_SECTION_HDR,     L"HDR"          },
	{ CVRSettingsPPage::SECTION_Present, IDD_SECTION_PRESENT, L"Presentation" },
};


// ---------------------------------------------------------------- the look
//
// A property page is a child of the player's own frame, so the frame draws the
// tabs and the buttons and we draw everything inside. Flat surfaces, one accent,
// a hairline where a group box used to be, and the switches drawn rather than
// the system's check boxes -- but drawn *around* the real control, which keeps
// its state, its keyboard and its accessibility. An owner-draw button would have
// cost all three.
//
// Windows' own dark mode is followed. The theme names below are not documented;
// where they stop working the page is light in places and still works, which is
// why nothing depends on them.

struct Theme {
	COLORREF bg;        // the content surface
	COLORREF panel;     // the list down the side
	COLORREF text;
	COLORREF textDim;   // descriptions, headings
	COLORREF textOff;   // greyed
	COLORREF line;      // hairlines, and a switch that is off
	COLORREF accent;
	COLORREF onAccent;
};

static Theme  g_th = {};
static bool   g_bDark = false;
static int    g_pagesDressed = 0;   // how many pages the brushes below belong to
static HBRUSH g_hbrBg = nullptr;
static HBRUSH g_hbrPanel = nullptr;
static HBRUSH g_hbrAccent = nullptr;
static HBRUSH g_hbrLine = nullptr;
static HFONT  g_hTitleFont = nullptr;
static HFONT  g_hHeadFont = nullptr;

#include "cards.inc"
// The one-line descriptions at the top of each section.
static const int g_descriptions[] = {
	IDC_STATIC_DESC_SOURCE, IDC_STATIC_DESC_CHROMA, IDC_STATIC_DESC_SCALING,
	IDC_STATIC_DESC_DETAIL, IDC_STATIC_DESC_DLSSNR, IDC_STATIC_DESC_HDR,
	IDC_STATIC_DESC_PRESENT,
};

// Which section was being looked at when the page was last closed. It lives
// beside the settings but is not one of them: Settings_t is append-only and
// shared with upstream, and where the user had scrolled to is nobody's setting.
static const wchar_t* const kRegKey = L"Software\\MPC-BE Filters\\MPC Video Renderer";
static const wchar_t* const kRegSection = L"SettingsPageSection";

static int LastSectionLookedAt()
{
	DWORD value = 0;
	DWORD size = sizeof(value);
	if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegSection, RRF_RT_REG_DWORD,
			nullptr, &value, &size) == ERROR_SUCCESS) {
		return (int)value;
	}
	return 0;
}

static void RememberSection(int section)
{
	const DWORD value = (DWORD)section;
	RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, kRegSection, REG_DWORD, &value, sizeof(value));
}

static bool SystemUsesDarkApps()
{
	DWORD value = 1;
	DWORD size = sizeof(value);
	if (RegGetValueW(HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
		return value == 0;
	}
	return false;   // no key, no dark mode: light is the safe answer
}

// Windows only hands out the dark variants of the common controls -- the boxes,
// the lists, the buttons -- to a process that has asked for them, through an entry
// point uxtheme exports by ordinal and does not name. Where it is missing, on an
// older Windows or a future one that moved it, nothing happens and those controls
// stay light: the page is then less dark than it could be, and never broken.
static void AllowDarkModeForThisProcess()
{
	static bool bAsked = false;
	if (bAsked) {
		return;
	}
	bAsked = true;
	const HMODULE hUx = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!hUx) {
		return;
	}
	// 135 is AllowDarkModeForApp(BOOL) on Windows 10 1809 and SetPreferredAppMode
	// on 1903 and later; 1 means "allow dark" to both of them.
	using PFN_PreferredAppMode = int(WINAPI*)(int);
	using PFN_RefreshColours = void(WINAPI*)();
	if (const auto pfnMode = (PFN_PreferredAppMode)GetProcAddress(hUx, MAKEINTRESOURCEA(135))) {
		pfnMode(1);
	}
	if (const auto pfnRefresh = (PFN_RefreshColours)GetProcAddress(hUx, MAKEINTRESOURCEA(104))) {
		pfnRefresh();
	}
	// Kept loaded on purpose: the policy belongs to the process, not to this call.
}

static void FreeTheme()
{
	for (HBRUSH* p : { &g_hbrBg, &g_hbrPanel, &g_hbrAccent, &g_hbrLine }) {
		if (*p) {
			DeleteObject(*p);
			*p = nullptr;
		}
	}
	for (HFONT* p : { &g_hTitleFont, &g_hHeadFont }) {
		if (*p) {
			DeleteObject(*p);
			*p = nullptr;
		}
	}
}

static void LoadTheme(HWND hRef)
{
	// Freeing them while another page is still drawing with them would leave it
	// painting through dangling handles, which is a page with no surfaces at all.
	if (g_pagesDressed <= 1) {
		FreeTheme();
	}
	g_bDark = SystemUsesDarkApps();
	if (g_bDark) {
		AllowDarkModeForThisProcess();
	}
	if (g_bDark) {
		g_th = { RGB(32, 32, 32), RGB(43, 43, 43), RGB(255, 255, 255), RGB(165, 165, 165),
				 RGB(110, 110, 110), RGB(64, 64, 64), RGB(76, 194, 255), RGB(0, 0, 0) };
	} else {
		g_th = { RGB(255, 255, 255), RGB(243, 243, 243), RGB(26, 26, 26), RGB(99, 99, 99),
				 RGB(160, 160, 160), RGB(224, 224, 224), RGB(0, 95, 184), RGB(255, 255, 255) };
	}
	g_hbrBg     = CreateSolidBrush(g_th.bg);
	g_hbrPanel  = CreateSolidBrush(g_th.panel);
	g_hbrAccent = CreateSolidBrush(g_th.accent);
	g_hbrLine   = CreateSolidBrush(g_th.line);

	// Two sizes derived from the dialog's own font, so they follow the host's DPI
	// rather than a number of pixels picked here.
	LOGFONTW lf = {};
	if (HFONT hDlgFont = (HFONT)SendMessageW(hRef, WM_GETFONT, 0, 0)) {
		GetObjectW(hDlgFont, sizeof(lf), &lf);
	}
	if (!lf.lfHeight) {
		lf.lfHeight = -12;
		wcscpy_s(lf.lfFaceName, L"Segoe UI");
	}
	LOGFONTW lfTitle = lf;
	lfTitle.lfHeight = (LONG)(lf.lfHeight * 1.35);
	lfTitle.lfWeight = FW_SEMIBOLD;
	g_hTitleFont = CreateFontIndirectW(&lfTitle);
	LOGFONTW lfHead = lf;
	lfHead.lfWeight = FW_SEMIBOLD;
	g_hHeadFont = CreateFontIndirectW(&lfHead);
}

// A rounded rectangle without a pen of its own.
static void FillRound(HDC hdc, const RECT& rc, COLORREF colour, int radius)
{
	HBRUSH hbr = CreateSolidBrush(colour);
	HPEN hpen = CreatePen(PS_SOLID, 1, colour);
	HGDIOBJ oldBr = SelectObject(hdc, hbr);
	HGDIOBJ oldPen = SelectObject(hdc, hpen);
	RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
	SelectObject(hdc, oldBr);
	SelectObject(hdc, oldPen);
	DeleteObject(hbr);
	DeleteObject(hpen);
}

// The check boxes are drawn as switches. The control underneath is still a plain
// auto check box: it keeps its state, its space bar and its notifications, and
// only its painting is taken over.
static LRESULT CALLBACK SwitchProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR idSubclass, DWORD_PTR refData)
{
	switch (uMsg) {
	case WM_NCDESTROY:
		RemoveWindowSubclass(hWnd, SwitchProc, idSubclass);
		break;

	case WM_ERASEBKGND:
		return 1;   // WM_PAINT fills it

	case WM_PAINT: {
		PAINTSTRUCT ps = {};
		HDC hdc = BeginPaint(hWnd, &ps);
		RECT rc = {};
		GetClientRect(hWnd, &rc);
		FillRect(hdc, &rc, g_hbrPanel);

		const bool bOn = SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
		const bool bEnabled = IsWindowEnabled(hWnd) != FALSE;

		// The switch is as tall as the row allows, up to a sensible ceiling, and
		// twice as wide as it is tall.
		const int h = std::min<int>(rc.bottom - rc.top, MulDiv(14, GetDeviceCaps(hdc, LOGPIXELSY), 96));
		const int top = rc.top + ((rc.bottom - rc.top) - h) / 2;
		const RECT track = { rc.left, top, rc.left + h * 2, top + h };
		const COLORREF trackColour = !bEnabled ? g_th.line : (bOn ? g_th.accent : g_th.line);
		FillRound(hdc, track, trackColour, h);

		const int pad = std::max(2, h / 7);
		const int knob = h - pad * 2;
		const int knobLeft = bOn ? (track.right - pad - knob) : (track.left + pad);
		const RECT knobRc = { knobLeft, top + pad, knobLeft + knob, top + pad + knob };
		COLORREF knobColour = bOn ? g_th.onAccent : g_th.textDim;
		if (!bEnabled) {
			knobColour = g_th.textOff;
		}
		FillRound(hdc, knobRc, knobColour, knob);

		wchar_t label[256] = {};
		GetWindowTextW(hWnd, label, (int)std::size(label));
		RECT rcText = { track.right + MulDiv(8, GetDeviceCaps(hdc, LOGPIXELSX), 96), rc.top, rc.right, rc.bottom };
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, bEnabled ? g_th.text : g_th.textOff);
		HGDIOBJ oldFont = SelectObject(hdc, (HFONT)SendMessageW(hWnd, WM_GETFONT, 0, 0));
		DrawTextW(hdc, label, -1, &rcText,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		if (GetFocus() == hWnd) {
			RECT rcFocus = rcText;
			DrawTextW(hdc, label, -1, &rcFocus,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
			InflateRect(&rcFocus, 2, 1);
			rcFocus.top = rc.top;
			rcFocus.bottom = rc.bottom;
			DrawFocusRect(hdc, &rcFocus);
		}
		SelectObject(hdc, oldFont);
		EndPaint(hWnd, &ps);
		return 0;
	}

	// The state changes before we are asked to paint again, and a switch that
	// slides only on the next mouse move looks broken.
	case BM_SETCHECK:
	case WM_LBUTTONUP:
	case WM_KEYUP:
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	case WM_ENABLE: {
		const LRESULT r = DefSubclassProc(hWnd, uMsg, wParam, lParam);
		InvalidateRect(hWnd, nullptr, FALSE);
		return r;
	}
	}
	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Everything the look needs doing once the controls exist.
void CVRSettingsPPage::DressUp()
{
	g_pagesDressed++;
	LoadTheme(m_hwnd);

	if (HWND h = ::GetDlgItem(m_hwnd, IDC_STATIC_TITLE)) {
		SendMessageW(h, WM_SETFONT, (WPARAM)g_hTitleFont, TRUE);
	}
	for (const int id : g_cardTitles) {
		if (HWND h = Item(id)) {
			SendMessageW(h, WM_SETFONT, (WPARAM)g_hHeadFont, TRUE);
		}
	}

	// Every check box becomes a switch, and every list and box is told about dark
	// mode. The theme names are undocumented: when they do nothing the control
	// simply stays light, which is why the failure is invisible rather than ugly.
	for (HWND hSection : m_hSections) {
		if (!hSection) {
			continue;
		}
		for (HWND h = ::GetWindow(hSection, GW_CHILD); h; h = ::GetWindow(h, GW_HWNDNEXT)) {
			wchar_t cls[32] = {};
			::GetClassNameW(h, cls, (int)std::size(cls));
			if (!_wcsicmp(cls, L"Button")) {
				const LONG style = ::GetWindowLongW(h, GWL_STYLE);
				if ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX) {
					// Removed first so a second pass puts ours back on the outside:
					// a player that themes the whole sheet does so after the page is
					// activated, and the outermost subclass is the one that paints.
					RemoveWindowSubclass(h, SwitchProc, 1);
					SetWindowSubclass(h, SwitchProc, 1, 0);
				} else if (g_bDark) {
					SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
				}
			} else if (g_bDark) {
				SetWindowTheme(h, (!_wcsicmp(cls, L"ComboBox") || !_wcsicmp(cls, L"Edit"))
					? L"DarkMode_CFD" : L"DarkMode_Explorer", nullptr);
			}
		}
	}
	if (g_bDark) {
		if (HWND h = ::GetDlgItem(m_hwnd, IDC_BUTTON1)) {
			SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
		}
	}
}

// What a dialog of ours answers when Windows asks what colour something is.
static INT_PTR ColourMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, bool bOnCard)
{
	switch (uMsg) {
	case WM_CTLCOLORDLG:
		return (INT_PTR)g_hbrBg;

	case WM_CTLCOLORBTN:
		return (INT_PTR)(bOnCard ? g_hbrPanel : g_hbrBg);

	// A writable box sends this one and a read-only box sends WM_CTLCOLORSTATIC,
	// so both have to be answered or the two look nothing like each other.
	case WM_CTLCOLOREDIT: {
		const HDC hdc = (HDC)wParam;
		SetBkMode(hdc, OPAQUE);
		SetBkColor(hdc, bOnCard ? g_th.panel : g_th.bg);
		SetTextColor(hdc, ::IsWindowEnabled((HWND)lParam) ? g_th.text : g_th.textOff);
		return (INT_PTR)(bOnCard ? g_hbrPanel : g_hbrBg);
	}

	case WM_CTLCOLORSTATIC: {
		const HDC hdc = (HDC)wParam;
		const HWND hCtrl = (HWND)lParam;
		const int id = ::GetDlgCtrlID(hCtrl);
		bool bDim = false;
		for (const int d : g_descriptions) {
			bDim = bDim || (id == d);
		}
			SetBkMode(hdc, TRANSPARENT);
		// A read-only edit is a static as far as this message is concerned, and it
		// is the only one that wants the surface drawn under it.
		wchar_t cls[16] = {};
		::GetClassNameW(hCtrl, cls, (int)std::size(cls));
		const bool bEdit = !_wcsicmp(cls, L"Edit");
		SetTextColor(hdc, !::IsWindowEnabled(hCtrl) ? g_th.textOff
			: (bDim ? g_th.textDim : g_th.text));
		if (bEdit) {
			SetBkMode(hdc, OPAQUE);
			SetBkColor(hdc, bOnCard ? g_th.panel : g_th.bg);
		}
		// The one-line description sits on the page; the rest sits on a card.
		return (INT_PTR)((bOnCard && !bDim) ? g_hbrPanel : g_hbrBg);
	}
	}
	return 0;
}


// One line of what it does, one of what it needs. The measurements and the
// reasoning are in README-DLSS5.md; a tooltip that has to be read twice is one
// nobody reads.
static const struct { int id; const wchar_t* text; } g_hints[] = {
	// Source
	{ IDC_CHECK1,
		L"Direct3D 11 instead of Direct3D 9.\n"
		"Everything below that says Direct3D 11 needs it: the shader\n"
		"prescalers, the chroma replacement, sharpening and DLSS.\n"
		"Windows 8 or newer." },
	{ IDC_COMBO1,
		L"The precision the renderer works in.\n"
		"Auto follows the source. 16-bit float is forced while DLSS 5 NR\n"
		"runs, and costs twice the video memory of 8-bit." },
	{ IDC_STATIC1,
		L"Formats handed to the hardware video processor.\n"
		"Unticking one gives that format to the shaders instead, which is\n"
		"what makes the chroma and scaling lists below live again." },
	{ IDC_CHECK4,
		L"Everything the processor takes beyond NV12, P010/P016 and YUY2.\n"
		"Untick it and those formats go to the shaders." },
	{ IDC_COMBO9,
		L"Deinterlacing by the hardware video processor.\n"
		"HACK future frames works around drivers that need them; try it\n"
		"only if interlaced video judders." },
	{ IDC_CHECK3,
		L"One field becomes one frame: 50i plays as 50p instead of 25p.\n"
		"Smoother motion, twice the work. Interlaced sources only." },
	{ IDC_CHECK17,
		L"Blends the two fields with a shader instead of deinterlacing\n"
		"them. Softer but free of combing. Progressive video is not\n"
		"affected. Direct3D 11 and YUV 4:2:0." },

	// Chroma
	{ IDC_COMBO5,
		L"Rebuilds the colour of a 4:2:0/4:2:2 picture.\n"
		"Used when the shaders do the conversion; greyed while the video\n"
		"processor does it, except for what it refuses (Dolby Vision,\n"
		"YCgCo, RGB on Nvidia). Listed best measured first.\n"
		"RAVU-zoom and FSRCNNX cost a few milliseconds; Jinc is free." },
	{ IDC_CHECK27,
		L"The shaders rebuild the chroma with the method above and hand the\n"
		"processor a 4:4:4 picture, so it stays in the chain.\n"
		"Gains 5.6 dB of colour on 10-bit, 0.6 on 8-bit.\n"
		"It costs RTX Video Super Resolution, which does nothing to a\n"
		"4:4:4 picture. Progressive YUV, Direct3D 11." },

	// Scaling
	{ IDC_CHECK5,
		L"The hardware video processor resizes instead of the shaders.\n"
		"Fast, and lower quality than the lists below.\n"
		"Request Super Resolution needs it. DLSS takes the resizing back\n"
		"while it runs." },
	{ IDC_COMBO8,
		L"The driver sharpens as the processor enlarges.\n"
		"Needs \"Use the video processor for resizing\" ticked, and greys\n"
		"while DLSS is enlarging instead.\n"
		"Greys as well while the chroma replacement above is on: the\n"
		"driver will not touch the 4:4:4 picture it then receives.\n"
		"Nvidia RTX (x64) or Intel UHD 610 and later." },
	{ IDC_COMBO2,
		L"Enlarges when the shaders do the resizing.\n"
		"Greyed while the video processor resizes, or while DLSS Super\n"
		"Resolution does. Listed best measured first.\n"
		"AR holds what the network invented inside the source's range;\n"
		"ArtCNN is the best and the dearest, about 13 ms 1080p to 4K.\n"
		"Direct3D 11." },
	{ IDC_COMBO3,
		L"Reduces when the shaders do the resizing.\n"
		"Greyed while the video processor resizes." },
	{ IDC_CHECK6,
		L"At exactly half size, reduce with the Upscaling method above\n"
		"rather than the Downscaling one.\n"
		"Sharper on 4K played in a 1080p window." },
	{ IDC_CHECK25,
		L"Enlarges with NVIDIA DLSS Super Resolution instead of the\n"
		"Upscaling method, which then greys.\n"
		"Needs nvngx_dlss.dll and an RTX card. Works with or without\n"
		"DLSS 5 NR.\n"
		"Experimental: moving subjects can still shimmer on grainy film." },
	{ IDC_COMBO15,
		L"Which DLSS model to use. Automatic picks it from the scale.\n"
		"Live only while DLSS Super Resolution is ticked." },
	{ IDC_EDIT9,
		L"Path to nvngx_dlss.dll, or its folder; the file must keep that\n"
		"name. Empty means look next to the filter, then one and two\n"
		"directories up." },

	// Detail
	{ IDC_SHARPEN,
		L"Sharpens after the resize, so it also reaches a film already at\n"
		"the screen's size.\n"
		"Adaptive-Sharpen is the only one that does not amplify grain,\n"
		"and the dearest: about 3 ms for a 4K frame.\n"
		"Unsharp + Clamp rings least and costs five times less.\n"
		"Direct3D 11." },
	{ IDC_SHARPEN_LEVEL,
		L"The same amount of sharpening whichever method is chosen:\n"
		"+4, +8, +13, +20 and +28 per cent of mean gradient.\n"
		"3 is where the measurements settle; 4 and 5 are past fidelity.\n"
		"Live only while a method is chosen." },
	{ IDC_CHECK10,
		L"Adds an ordered noise below the last bit before output, so flat\n"
		"gradients band less -- skies, fades to black.\n"
		"Costs nothing measurable. Leave it on." },

	// DLSS 5 NR
	{ IDC_CHECK20,
		L"Neural reconstruction of each picture.\n"
		"Needs nvngx_dlssnr.dll, Direct3D 11, x64 and an NVIDIA card.\n"
		"Forces 16-bit float textures and about 500 MB of video memory\n"
		"at 1080p." },
	{ IDC_CHECK23,
		L"Run the network on the enlarged picture instead of the source.\n"
		"At 4K output that is roughly four times the pixels, and four\n"
		"times the cost." },
	{ IDC_COMBO13,
		L"Switches DLSS on and off during playback.\n"
		"The filter swallows this key, so pick one the player does not\n"
		"need. None disables the shortcut." },
	{ IDC_EDIT7,
		L"Path to nvngx_dlssnr.dll.\n"
		"Empty means look next to the filter, then one and two\n"
		"directories up." },
	{ IDC_COMBO11,
		L"How far the network is allowed to reinterpret the picture.\n"
		"Default is the measured one." },
	{ IDC_COMBO12,
		L"This DLL build ships a single network, so every preset falls\n"
		"back to the same one. Kept for other builds." },
	{ IDC_CHECK21,
		L"Lets the network decide where to apply itself rather than\n"
		"treating the whole picture alike." },
	{ IDC_SLIDER3,
		L"How much of the network's result is kept. 1.00 is all of it." },
	{ IDC_SLIDER4,
		L"Local contrast the network adds. Kept low on purpose: the local\n"
		"terms amplify what changes from one frame to the next." },
	{ IDC_SLIDER5,
		L"Fine structure the network adds. Same caution as local tone." },
	{ IDC_SLIDER6,
		L"Fine structure on skin, separately. Higher than the others\n"
		"because faces carry the detail the eye checks first." },
	{ IDC_SLIDER7,
		L"Steadies the network's effect over time, after it runs, so the\n"
		"video is never delayed. Removes most of the shimmer DLSS adds.\n"
		"100 is the measured setting; 0 runs nothing." },
	{ IDC_COMBO14,
		L"Where the stabilizer takes motion from.\n"
		"Optical Flow follows the picture, so moving areas are steadied\n"
		"too: 2.8 ms a picture at 1080p against 0.8 for the shader\n"
		"detector, which only steadies what stands still." },
	{ IDC_CHECK24,
		L"Also gives the Optical Flow vectors to the network itself.\n"
		"Steadier, but the network renders differently around moving\n"
		"objects.\n"
		"Needs Optical Flow and a stabilizer above 0." },
	{ IDC_CHECK22,
		L"Makes the network ignore its own previous output every frame.\n"
		"The stabilizer is not affected: it works after the network." },
	{ IDC_BUTTON3,
		L"Back to the measured tuning. Whether DLSS is on, its key and the\n"
		"DLL paths are left alone." },

	// HDR
	{ IDC_CHECK18,
		L"When a stream carries both, take the Dolby Vision layer rather\n"
		"than its PQ or HLG base." },
	{ IDC_COMBO10,
		L"What to do with an HDR source.\n"
		"Passthrough sends it untouched to an HDR display; the others\n"
		"tone map it here instead.\n"
		"RTX Video HDR needs Passthrough and greys with anything else." },
	{ IDC_EDIT_DISPLAYMAX,
		L"The peak your display really reaches, used by the tone mapping\n"
		"chosen above. 100 to 10000.\n"
		"Live only while a tone mapping is chosen." },
	{ IDC_COMBO7,
		L"Whether the filter may switch the desktop into HDR by itself\n"
		"when an HDR film starts, and back afterwards." },
	{ IDC_SLIDER1,
		L"How bright subtitles and the statistics are drawn over an HDR\n"
		"picture. Raise it if they look grey." },
	{ IDC_CHECK14,
		L"When HDR cannot be passed through, tone map it to SDR here\n"
		"rather than showing it washed out." },
	{ IDC_SLIDER2,
		L"The brightness that conversion aims at. Applies as you drag it,\n"
		"so the picture follows.\n"
		"Live only while Convert to SDR is ticked." },
	{ IDC_CHECK19,
		L"The driver makes an HDR picture out of an SDR source.\n"
		"Needs Passthrough above, an HDR display, x64 and an RTX card.\n"
		"The statistics carry a star while it is really running." },

	// Presentation
	{ IDC_COMBO4,
		L"How finished pictures reach the screen.\n"
		"Flip is the modern path and the lower latency one. Discard is\n"
		"the old one, for drivers that misbehave with flip." },
	{ IDC_CHECK11,
		L"Takes the display outright instead of going through the desktop.\n"
		"Lower latency, and it can break overlays, alt-tab and anything\n"
		"else drawn over the video." },
	{ IDC_CHECK15,
		L"Waits for the display's vertical blank before presenting.\n"
		"Can steady tearing on some setups. It costs a refresh of\n"
		"latency -- 42 ms at 24 Hz -- during which the renderer is busy." },
	{ IDC_CHECK13,
		L"Holds each picture until its own time on the audio clock rather\n"
		"than presenting it as soon as it is ready.\n"
		"Leave it on: it is what keeps the picture on the sound." },
	{ IDC_CHECK16,
		L"Rebuilds the device when the window moves to another screen.\n"
		"Needed only if moving the player between monitors leaves the\n"
		"picture wrong." },
	{ IDC_CHECK26,
		L"Starts each picture early by the time the heavy passes take --\n"
		"DLSS, FSRCNNX, RAVU-zoom -- and holds it until its own time, so\n"
		"they do not make the video late against the sound.\n"
		"Does nothing while none of them runs. Direct3D 11." },
	{ IDC_CHECK2,
		L"Draws the renderer's own statistics over the picture: formats,\n"
		"what each stage costs, sync offset.\n"
		"Ctrl+J in MPC-HC toggles the same thing." },
	{ IDC_COMBO6,
		L"Whether the statistics keep one size or grow with the window." },
};


// CVRSettingsPPage

CVRSettingsPPage::CVRSettingsPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"SettingsProp", lpunk, IDD_SETTINGSPAGE, IDS_SETTINGSPAGE_TITLE)
{
	DLog(L"CVRSettingsPPage()");
}

CVRSettingsPPage::~CVRSettingsPPage()
{
	DLog(L"~CVRSettingsPPage()");
}

INT_PTR CALLBACK CVRSettingsPPage::SectionProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_INITDIALOG) {
		::SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)lParam);
		return TRUE;
	}
	auto* page = (CVRSettingsPPage*)::GetWindowLongPtrW(hDlg, GWLP_USERDATA);
	return page ? page->OnSectionMessage(hDlg, uMsg, wParam, lParam) : (INT_PTR)FALSE;
}

HWND CVRSettingsPPage::Item(int id) const
{
	for (HWND hSection : m_hSections) {
		if (hSection) {
			if (HWND hItem = ::GetDlgItem(hSection, id)) {
				return hItem;
			}
		}
	}
	return m_hwnd ? ::GetDlgItem(m_hwnd, id) : nullptr;
}

void CVRSettingsPPage::Enable(int id, BOOL bEnable) const
{
	if (HWND h = Item(id)) {
		::EnableWindow(h, bEnable);
	}
}

bool CVRSettingsPPage::Checked(int id) const
{
	HWND h = Item(id);
	return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void CVRSettingsPPage::SetCheck(int id, bool bChecked) const
{
	if (HWND h = Item(id)) {
		SendMessageW(h, BM_SETCHECK, bChecked ? BST_CHECKED : BST_UNCHECKED, 0);
	}
}

LRESULT CVRSettingsPPage::Send(int id, UINT uMsg, WPARAM wParam, LPARAM lParam) const
{
	HWND h = Item(id);
	return h ? SendMessageW(h, uMsg, wParam, lParam) : 0;
}

void CVRSettingsPPage::SetText(int id, LPCWSTR text) const
{
	if (HWND h = Item(id)) {
		::SetWindowTextW(h, text);
	}
}

int CVRSettingsPPage::SectionOf(HWND hDlg) const
{
	for (int i = 0; i < SECTION_COUNT; i++) {
		if (m_hSections[i] == hDlg) {
			return i;
		}
	}
	return -1;
}

void CVRSettingsPPage::ShowSection(int section)
{
	m_iSection = section;
	for (int i = 0; i < SECTION_COUNT; i++) {
		if (m_hSections[i]) {
			::ShowWindow(m_hSections[i], (i == section) ? SW_SHOW : SW_HIDE);
		}
	}
}

HRESULT CVRSettingsPPage::OnConnect(IUnknown* pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRSettingsPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	if (m_SetsPP.iSDRDisplayNits != m_oldSDRDisplayNits) {
		// OK or Apply was not pressed, and the nits slider applies as it is dragged.
		m_pVideoRenderer->GetSettings(m_SetsPP);
		m_SetsPP.iSDRDisplayNits = m_oldSDRDisplayNits;
		m_pVideoRenderer->SetSettings(m_SetsPP);
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

void CVRSettingsPPage::FillCombos()
{
	Send(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Fixed font size");
	Send(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Increase font by window");

	Combo_AddData(Item(IDC_COMBO1), L"Auto 8/10-bit Integer",  0);
	Combo_AddData(Item(IDC_COMBO1), L"8-bit Integer",          8);
	Combo_AddData(Item(IDC_COMBO1), L"10-bit Integer",        10);
	Combo_AddData(Item(IDC_COMBO1), L"16-bit Floating Point", 16);

	Send(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	Send(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Enable");
	Send(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"HACK future frames");

	Send(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	Send(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for SD");
	Send(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 720p");
	Send(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1080p");
	Send(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1440p");

	Send(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Do not change");
	Send(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on (fullscreen)");
	Send(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on");
	Send(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off (fullscreen)");
	Send(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off");

	// Both lists are shown best first, as they measured on ten film references
	// brought from 1080p to 4K (tools/dlssnr_probe, --tchroma and --tupscale, the
	// tables in README-DLSS5.md). The number each entry carries is what is saved, so
	// the order can change without moving anybody's setting.
	Combo_AddData(Item(IDC_COMBO5), L"Jinc (EWA)",         CHROMA_Jinc);
	Combo_AddData(Item(IDC_COMBO5), L"RAVU-zoom",          CHROMA_RAVU);
	Combo_AddData(Item(IDC_COMBO5), L"Catmull-Rom",        CHROMA_CatmullRom);
	Combo_AddData(Item(IDC_COMBO5), L"FSRCNNX 8 AR",       CHROMA_FSRCNNX8AR);
	Combo_AddData(Item(IDC_COMBO5), L"Bilinear",           CHROMA_Bilinear);
	Combo_AddData(Item(IDC_COMBO5), L"Nearest-neighbor",   CHROMA_Nearest);

	Combo_AddData(Item(IDC_COMBO2), L"ArtCNN C4F16 DS",    UPSCALE_ArtCNN);
	Combo_AddData(Item(IDC_COMBO2), L"RAVU-zoom",          UPSCALE_RAVUZoom);
	Combo_AddData(Item(IDC_COMBO2), L"FSRCNNX 16 AR",      UPSCALE_FSRCNNX16AR);
	Combo_AddData(Item(IDC_COMBO2), L"FSRCNNX 8 AR",       UPSCALE_FSRCNNX8AR);
	Combo_AddData(Item(IDC_COMBO2), L"FSRCNNX 16",         UPSCALE_FSRCNNX16);
	Combo_AddData(Item(IDC_COMBO2), L"FSRCNNX 8",          UPSCALE_FSRCNNX8);
	Combo_AddData(Item(IDC_COMBO2), L"Catmull-Rom",        UPSCALE_CatmullRom);
	Combo_AddData(Item(IDC_COMBO2), L"Lanczos2",           UPSCALE_Lanczos2);
	Combo_AddData(Item(IDC_COMBO2), L"Mitchell-Netravali", UPSCALE_Mitchell);
	Combo_AddData(Item(IDC_COMBO2), L"Lanczos3",           UPSCALE_Lanczos3);
	Combo_AddData(Item(IDC_COMBO2), L"Jinc2m",             UPSCALE_Jinc2);
	Combo_AddData(Item(IDC_COMBO2), L"Nearest-neighbor",   UPSCALE_Nearest);

	Send(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Box");
	Send(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bilinear");
	Send(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Hamming");
	Send(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic");
	Send(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic sharp");
	Send(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Lanczos");

	Send(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Discard");
	Send(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Flip");

	// In the order of the enum, so the position is the value.
	Send(IDC_SHARPEN, CB_ADDSTRING, 0, (LPARAM)L"Disabled");
	Send(IDC_SHARPEN, CB_ADDSTRING, 0, (LPARAM)L"Adaptive-Sharpen");
	Send(IDC_SHARPEN, CB_ADDSTRING, 0, (LPARAM)L"Unsharp + Clamp");

	Combo_AddData(Item(IDC_COMBO10), L"Ignore", -1);
	Combo_AddData(Item(IDC_COMBO10), L"Passthrough", 0);
	Combo_AddData(Item(IDC_COMBO10), L"ACES", 1);
	Combo_AddData(Item(IDC_COMBO10), L"Reinhard", 2);
	Combo_AddData(Item(IDC_COMBO10), L"Hable", 3);
	Combo_AddData(Item(IDC_COMBO10), L"Mobius", 4);
	Combo_AddData(Item(IDC_COMBO10), L"BT2390/ST 2094-10", 5);

	Combo_AddData(Item(IDC_COMBO11), L"Default",   DLSSNR_STYLE_Default);
	Combo_AddData(Item(IDC_COMBO11), L"Natural",   DLSSNR_STYLE_Natural);
	Combo_AddData(Item(IDC_COMBO11), L"Cinematic", DLSSNR_STYLE_Cinematic);

	for (int i = 0; i < DLSSNR_PRESET_COUNT; i++) {
		Combo_AddData(Item(IDC_COMBO12), std::format(L"Preset {}", i).c_str(), i);
	}

	Combo_AddData(Item(IDC_COMBO14), L"NVIDIA Optical Flow", DLSSNR_MOTION_OPTICALFLOW);
	Combo_AddData(Item(IDC_COMBO14), L"Shader detector (still areas)", DLSSNR_MOTION_DETECTOR);

	Combo_AddData(Item(IDC_COMBO15), L"Automatic", DLSSSR_PRESET_DEF);
	for (int preset = DLSSSR_PRESET_J; preset <= DLSSSR_PRESET_M; preset++) {
		Combo_AddData(Item(IDC_COMBO15),
			std::format(L"Preset {}", (wchar_t)(L'J' + preset - DLSSSR_PRESET_J)).c_str(), preset);
	}

	// Keys that players rarely bind to anything destructive. The hook swallows
	// whichever one is chosen, so it must not be something the player needs.
	static const struct { const wchar_t* name; int vk; } dlssKeys[] = {
		{ L"None", 0 }, { L"Home", VK_HOME }, { L"End", VK_END },
		{ L"Insert", VK_INSERT }, { L"Delete", VK_DELETE },
		{ L"Page Up", VK_PRIOR }, { L"Page Down", VK_NEXT },
		{ L"Pause", VK_PAUSE }, { L"Scroll Lock", VK_SCROLL },
		{ L"F9", VK_F9 }, { L"F10", VK_F10 }, { L"F11", VK_F11 }, { L"F12", VK_F12 },
	};
	for (const auto& k : dlssKeys) {
		Combo_AddData(Item(IDC_COMBO13), k.name, k.vk);
	}

	// Sliders and their ranges.
	Send(IDC_SHARPEN_LEVEL, TBM_SETRANGE, 0, MAKELONG(SHARPEN_LEVEL_MIN, SHARPEN_LEVEL_MAX));
	Send(IDC_SHARPEN_LEVEL, TBM_SETTIC, 0, 1);
	Send(IDC_SHARPEN_LEVEL, TBM_SETLINESIZE, 0, 1);
	Send(IDC_SHARPEN_LEVEL, TBM_SETPAGESIZE, 0, 1);

	Send(IDC_SLIDER1, TBM_SETRANGE, 0, MAKELONG(0, 2));
	Send(IDC_SLIDER1, TBM_SETTIC, 0, 1);

	Send(IDC_SLIDER2, TBM_SETRANGE, 0, MAKELONG(SDR_NITS_MIN / SDR_NITS_STEP, SDR_NITS_MAX / SDR_NITS_STEP));
	Send(IDC_SLIDER2, TBM_SETTIC, 0, SDR_NITS_DEF / SDR_NITS_STEP);
	Send(IDC_SLIDER2, TBM_SETLINESIZE, 0, 1); // arrow keys
	Send(IDC_SLIDER2, TBM_SETPAGESIZE, 0, 5); // clicks on the trackbar's channel

	for (const int id : { IDC_SLIDER3, IDC_SLIDER4, IDC_SLIDER5, IDC_SLIDER6 }) {
		const int minimum = (id == IDC_SLIDER6) ? DLSSNR_SKIN_MIN : DLSSNR_STR_MIN;
		Send(id, TBM_SETRANGE, 0, MAKELONG(minimum, DLSSNR_STR_MAX));
		Send(id, TBM_SETTIC, 0, DLSSNR_STR_DEF);
		Send(id, TBM_SETLINESIZE, 0, 1);
		Send(id, TBM_SETPAGESIZE, 0, 10);
	}
	Send(IDC_SLIDER7, TBM_SETRANGE, 0, MAKELONG(DLSSNR_STAB_MIN, DLSSNR_STAB_MAX));
	Send(IDC_SLIDER7, TBM_SETLINESIZE, 0, 1);
	Send(IDC_SLIDER7, TBM_SETPAGESIZE, 0, 10);
}

void CVRSettingsPPage::SetControls()
{
	SetCheck(IDC_CHECK1,  m_SetsPP.bUseD3D11);
	SetCheck(IDC_CHECK2,  m_SetsPP.bShowStats);
	Combo_SelectData(Item(IDC_COMBO1), m_SetsPP.iTexFormat);

	SetCheck(IDC_CHECK7,  m_SetsPP.VPFmts.bNV12);
	SetCheck(IDC_CHECK8,  m_SetsPP.VPFmts.bP01x);
	SetCheck(IDC_CHECK9,  m_SetsPP.VPFmts.bYUY2);
	SetCheck(IDC_CHECK4,  m_SetsPP.VPFmts.bOther);
	Send(IDC_COMBO9, CB_SETCURSEL, m_SetsPP.iVPDeinterlacing, 0);
	SetCheck(IDC_CHECK3,  m_SetsPP.bDeintDouble);
	SetCheck(IDC_CHECK17, m_SetsPP.bDeintBlend);

	SetCheck(IDC_CHECK5,  m_SetsPP.bVPScaling);
	Send(IDC_COMBO8, CB_SETCURSEL, m_SetsPP.iVPSuperRes, 0);
	Combo_SelectData(Item(IDC_COMBO2), m_SetsPP.iUpscaling);
	Send(IDC_COMBO3, CB_SETCURSEL, m_SetsPP.iDownscaling, 0);
	SetCheck(IDC_CHECK6,  m_SetsPP.bInterpolateAt50pct);

	Combo_SelectData(Item(IDC_COMBO5), m_SetsPP.iChromaScaling);
	SetCheck(IDC_CHECK27, m_SetsPP.bVPReplaceChroma);

	Send(IDC_SHARPEN, CB_SETCURSEL, m_SetsPP.iSharpen, 0);
	Send(IDC_SHARPEN_LEVEL, TBM_SETPOS, 1, m_SetsPP.iSharpenLevel);
	SetText(IDC_SHARPEN_LEVEL_TEXT, std::to_wstring(m_SetsPP.iSharpenLevel).c_str());
	SetCheck(IDC_CHECK10, m_SetsPP.bUseDither);

	if (m_SetsPP.bHdrPassthrough) {
		Combo_SelectData(Item(IDC_COMBO10), 0);
	} else if (m_SetsPP.bHdrLocalToneMapping) {
		Combo_SelectData(Item(IDC_COMBO10), m_SetsPP.iHdrLocalToneMappingType);
	} else {
		Combo_SelectData(Item(IDC_COMBO10), -1);
	}
	SetCheck(IDC_CHECK18, m_SetsPP.bHdrPreferDoVi);
	SetCheck(IDC_CHECK14, m_SetsPP.bConvertToSdr);
	SetCheck(IDC_CHECK19, m_SetsPP.bVPRTXVideoHDR);
	Send(IDC_COMBO7, CB_SETCURSEL, m_SetsPP.iHdrToggleDisplay, 0);
	Send(IDC_SLIDER1, TBM_SETPOS, 1, m_SetsPP.iHdrOsdBrightness);
	Send(IDC_SLIDER2, TBM_SETPOS, 1, m_SetsPP.iSDRDisplayNits / SDR_NITS_STEP);
	SetText(IDC_EDIT1, std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());
	m_SetsPP.iHdrDisplayMaxNits = discard<int>(m_SetsPP.iHdrDisplayMaxNits, HDR_NITS_DEF, HDR_NITS_MIN, HDR_NITS_MAX);
	SetText(IDC_EDIT_DISPLAYMAX, std::to_wstring(m_SetsPP.iHdrDisplayMaxNits).c_str());

	Send(IDC_COMBO4, CB_SETCURSEL, m_SetsPP.iSwapEffect, 0);
	SetCheck(IDC_CHECK11, m_SetsPP.bExclusiveFS);
	SetCheck(IDC_CHECK15, m_SetsPP.bVBlankBeforePresent);
	SetCheck(IDC_CHECK13, m_SetsPP.bAdjustPresentTime);
	SetCheck(IDC_CHECK16, m_SetsPP.bReinitByDisplay);
	SetCheck(IDC_CHECK26, m_SetsPP.bDlssRenderAhead);
	Send(IDC_COMBO6, CB_SETCURSEL, m_SetsPP.iResizeStats, 0);

	SetCheck(IDC_CHECK20, m_SetsPP.bDlssNR);
	SetCheck(IDC_CHECK21, m_SetsPP.bDlssNRAutoMask);
	SetCheck(IDC_CHECK22, m_SetsPP.bDlssNRNoHistory);
	SetCheck(IDC_CHECK23, m_SetsPP.bDlssNRAfterUpscale);
	SetCheck(IDC_CHECK24, m_SetsPP.bDlssNRMotionVectors);
	Combo_SelectData(Item(IDC_COMBO11), m_SetsPP.iDlssNRStyle);
	Combo_SelectData(Item(IDC_COMBO12), m_SetsPP.iDlssNRPreset);
	Combo_SelectData(Item(IDC_COMBO13), m_SetsPP.iDlssNRToggleKey);
	Combo_SelectData(Item(IDC_COMBO14), m_SetsPP.iDlssNRMotion);

	const struct { int idSlider; int idEdit; int value; bool bStrength; } sliders[] = {
		{ IDC_SLIDER3, IDC_EDIT3, m_SetsPP.iDlssNRIntensity,      true  },
		{ IDC_SLIDER4, IDC_EDIT4, m_SetsPP.iDlssNRLocalTone,      true  },
		{ IDC_SLIDER5, IDC_EDIT5, m_SetsPP.iDlssNRLocalStructure, true  },
		{ IDC_SLIDER6, IDC_EDIT6, m_SetsPP.iDlssNRSkinStructure,  true  },
		{ IDC_SLIDER7, IDC_EDIT8, m_SetsPP.iDlssNRStabilizer,     false },
	};
	for (const auto& s : sliders) {
		Send(s.idSlider, TBM_SETPOS, TRUE, (LPARAM)s.value);
		SetText(s.idEdit, (s.bStrength ? StrengthText(s.value) : StabilizerText(s.value)).c_str());
	}
	SetText(IDC_EDIT7, m_SetsPP.szDlssNRDllPath);

	SetCheck(IDC_CHECK25, m_SetsPP.bDlssSR);
	Combo_SelectData(Item(IDC_COMBO15), m_SetsPP.iDlssSRPreset);
	SetText(IDC_EDIT9, m_SetsPP.szDlssSRDllPath);
}

void CVRSettingsPPage::EnableControls()
{
	if (!IsWindows8OrGreater()) { // Windows 7: no Direct3D 11 video processor
		const BOOL bEnable = !m_SetsPP.bUseD3D11;
		for (const int id : { IDC_STATIC1, IDC_STATIC2, IDC_CHECK7, IDC_CHECK8, IDC_CHECK9,
				IDC_CHECK4, IDC_CHECK3, IDC_CHECK5, IDC_STATIC3, IDC_COMBO4 }) {
			Enable(id, bEnable);
		}
	}
	else if (IsWindows10OrGreater()) {
		const BOOL bEnable = m_SetsPP.bUseD3D11;
		for (const int id : { IDC_COMBO10, IDC_STATIC5, IDC_COMBO7, IDC_STATIC6, IDC_SLIDER1 }) {
			Enable(id, bEnable);
		}
		// Super Resolution is settled below, with the rest of what the processor is
		// really doing: asking for it is not enough to make it happen.
#ifdef _WIN64
		Enable(IDC_CHECK19, bEnable && m_SetsPP.bHdrPassthrough);
#endif
	}

	Enable(IDC_STATIC8, m_SetsPP.bConvertToSdr);
	Enable(IDC_EDIT1, m_SetsPP.bConvertToSdr);
	Enable(IDC_SLIDER2, m_SetsPP.bConvertToSdr);
	Enable(IDC_EDIT_DISPLAYMAX, m_SetsPP.bHdrLocalToneMapping);

	// What the video processor takes, the shaders never see. It converts the formats
	// ticked in Source, chroma upsampling included, and resizes as well when "Use the
	// video processor for resizing" is on -- except while DLSS 5 NR or DLSS Super
	// Resolution runs, which keeps it at the source size and hands the resizing back
	// to the shaders. DLSS Super Resolution enlarges in its place, so the Upscaling
	// method then only stands in where it cannot run.
	// "Replace the video processor's chroma upsampling" gives the chroma of a
	// progressive 4:2:0/4:2:2 picture back to the shaders, and only the chroma: the
	// processor is handed a 4:4:4 picture and goes on converting and resizing it.
	// While something plays, the renderer also says what it is really doing with it,
	// and a list is only greyed when both agree that it has nothing to do.
	const bool bAllVPFormats = m_SetsPP.VPFmts.bNV12 && m_SetsPP.VPFmts.bP01x
		&& m_SetsPP.VPFmts.bYUY2 && m_SetsPP.VPFmts.bOther;
	const bool bVPAvailable = !(m_SetsPP.bUseD3D11 && !IsWindows8OrGreater());
	const bool bDlssPass = m_SetsPP.bUseD3D11 && (m_SetsPP.bDlssNR || m_SetsPP.bDlssSR);
	const bool bChromaToShaders = m_SetsPP.bUseD3D11 && m_SetsPP.bVPReplaceChroma;
	const bool bVPTakesPicture = bAllVPFormats && bVPAvailable;
	const bool bVPConverts = bVPTakesPicture && !bChromaToShaders
		&& (!m_bRendererActive || (m_uVPUse & VPUSE_Converting));
	const bool bVPResizes = bVPTakesPicture && m_SetsPP.bVPScaling && !bDlssPass
		&& (!m_bRendererActive || (m_uVPUse & VPUSE_Resizing));

	const BOOL bChromaList = !bVPConverts;
	const BOOL bDownscalingList = !bVPResizes;
	const BOOL bUpscalingList = !bVPResizes && !(m_SetsPP.bUseD3D11 && m_SetsPP.bDlssSR);
	Enable(IDC_STATIC40, bChromaList);
	Enable(IDC_COMBO5, bChromaList);
	Enable(IDC_STATIC39, bUpscalingList);
	Enable(IDC_COMBO2, bUpscalingList);
	Enable(IDC_STATIC41, bDownscalingList);
	Enable(IDC_COMBO3, bDownscalingList);
	Enable(IDC_CHECK6, bUpscalingList || bDownscalingList);

	// RTX Video Super Resolution lives inside the processor and only does something
	// while the processor is the one enlarging the picture -- which is what "Use the
	// video processor for resizing" gives it, and what a DLSS pass takes away. It
	// also needs a subsampled picture: the driver leaves a 4:4:4 one untouched, so it
	// can do nothing while the chroma replacement is really handing one over.
#ifdef _WIN64
	const bool bChromaReplacedNow = bChromaToShaders
		&& (!m_bRendererActive || !(m_uVPUse & VPUSE_Converting));
	const BOOL bSuperRes = m_SetsPP.bUseD3D11 && IsWindows10OrGreater()
		&& bVPResizes && !bChromaReplacedNow;
#else
	const BOOL bSuperRes = FALSE; // the extension is x64 only
#endif
	Enable(IDC_STATIC7, bSuperRes);
	Enable(IDC_COMBO8, bSuperRes);

	// The sharpening pass is a Direct3D 11 one, and its intensity says nothing
	// while no method is chosen.
	Enable(IDC_STATIC_SHARPEN, m_SetsPP.bUseD3D11);
	Enable(IDC_SHARPEN, m_SetsPP.bUseD3D11);
	const BOOL bSharpLevel = m_SetsPP.bUseD3D11 && m_SetsPP.iSharpen != SHARPEN_Disabled;
	Enable(IDC_STATIC_SHARPEN_LEVEL, bSharpLevel);
	Enable(IDC_SHARPEN_LEVEL, bSharpLevel);
	Enable(IDC_SHARPEN_LEVEL_TEXT, bSharpLevel);

	// Render ahead and the chroma replacement belong to the Direct3D 11 processor.
	Enable(IDC_CHECK26, m_SetsPP.bUseD3D11);
	Enable(IDC_CHECK27, m_SetsPP.bUseD3D11);

	// DLSS. The path boxes stay live even when the feature is off, so a wrong path
	// can be fixed without enabling it first.
	const BOOL bD3D11 = m_SetsPP.bUseD3D11;
	const BOOL bNROn  = bD3D11 && m_SetsPP.bDlssNR;
	for (const int id : { IDC_CHECK20, IDC_STATIC27, IDC_EDIT7, IDC_BUTTON2, IDC_STATIC29, IDC_COMBO13 }) {
		Enable(id, bD3D11);
	}
	for (const int id : { IDC_CHECK21, IDC_CHECK22, IDC_CHECK23, IDC_COMBO11, IDC_COMBO12,
			IDC_SLIDER3, IDC_SLIDER4, IDC_SLIDER5, IDC_SLIDER6,
			IDC_EDIT3, IDC_EDIT4, IDC_EDIT5, IDC_EDIT6,
			IDC_STATIC21, IDC_STATIC22, IDC_STATIC23, IDC_STATIC24, IDC_STATIC25, IDC_STATIC26,
			IDC_STATIC30, IDC_BUTTON3 }) {
		Enable(id, bNROn);
	}
	// The stabilizer works after the network, whatever its own history does.
	for (const int id : { IDC_STATIC31, IDC_STATIC32, IDC_SLIDER7, IDC_EDIT8, IDC_STATIC34, IDC_COMBO14 }) {
		Enable(id, bNROn);
	}
	// Vectors exist only with Optical Flow, and only while the stabilizer runs.
	Enable(IDC_CHECK24, bNROn && m_SetsPP.iDlssNRStabilizer > 0
		&& m_SetsPP.iDlssNRMotion == DLSSNR_MOTION_OPTICALFLOW);

	// DLSS Super Resolution does not depend on DLSS 5 NR: another DLL, another session.
	for (const int id : { IDC_CHECK25, IDC_STATIC35, IDC_STATIC37, IDC_EDIT9, IDC_BUTTON4 }) {
		Enable(id, bD3D11);
	}
	for (const int id : { IDC_STATIC36, IDC_COMBO15 }) {
		Enable(id, bD3D11 && m_SetsPP.bDlssSR);
	}
}

HRESULT CVRSettingsPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	m_pVideoRenderer->GetSettings(m_SetsPP);
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
	m_bDlssNRSeen = m_SetsPP.bDlssNR;
	m_bRendererActive = m_pVideoRenderer->GetActive();
	m_uVPUse = m_bRendererActive ? m_pVideoRenderer->GetVideoProcessorUse() : 0;

	// Where the sections go, in the page's own units, so the host's font decides.
	RECT rcContent = { 112, 26, 412, 318 };
	MapDialogRect(m_hwnd, &rcContent);

	HWND hList = ::GetDlgItem(m_hwnd, IDC_NAV);
	for (const auto& s : g_sections) {
#ifndef _WIN64
		if (s.section == SECTION_DlssNR) {
			continue; // the snippet is x64 only, and so is its section
		}
#endif
		HWND hSection = CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(s.dialogId),
			m_hwnd, SectionProc, (LPARAM)this);
		if (!hSection) {
			continue;
		}
		m_hSections[s.section] = hSection;
		::SetWindowPos(hSection, nullptr, rcContent.left, rcContent.top,
			rcContent.right - rcContent.left, rcContent.bottom - rcContent.top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		if (hList) {
			const LRESULT index = SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)s.name);
			if (index != LB_ERR) {
				SendMessageW(hList, LB_SETITEMDATA, index, s.section);
			}
		}
	}

	if (!IsWindows7SP1OrGreater()) {
		Enable(IDC_CHECK1, FALSE);
		m_SetsPP.bUseD3D11 = false;
	}
	if (!IsWindows10OrGreater()) {
		for (const int id : { IDC_COMBO10, IDC_STATIC5, IDC_COMBO7, IDC_STATIC6,
				IDC_SLIDER1, IDC_STATIC7, IDC_COMBO8, IDC_CHECK19 }) {
			Enable(id, FALSE);
		}
	}
#ifndef _WIN64
	for (const int id : { IDC_STATIC7, IDC_COMBO8, IDC_CHECK19 }) {
		Enable(id, FALSE);
	}
#endif

	FillCombos();

	// Show whether each DLSS session actually came up, and why not if it did not.
	{
		const struct { const char* field; int id; } statuses[] = {
			{ "dlssStatus",   IDC_STATIC28 },
			{ "dlssSRStatus", IDC_STATIC38 },
		};
		for (const auto& s : statuses) {
			std::wstring status;
			if (CComQIPtr<IExFilterConfig> pIExFilterConfig = m_pVideoRenderer.p) {
				LPWSTR pstr = nullptr;
				if (S_OK == pIExFilterConfig->Flt_GetString(s.field, &pstr, nullptr) && pstr) {
					status = pstr;
					CoTaskMemFree(pstr);
				}
			}
			SetText(s.id, status.empty() ? L"" : (L"Status: " + status).c_str());
		}
	}

	SetControls();
	EnableControls();
	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());
	DressUp();

	{
		int wanted = LastSectionLookedAt();
		if (wanted < 0 || wanted >= SECTION_COUNT || !m_hSections[wanted]) {
			wanted = SECTION_Source;
		}
		if (hList) {
			const LRESULT count = SendMessageW(hList, LB_GETCOUNT, 0, 0);
			for (LRESULT i = 0; i < count; i++) {
				if ((int)SendMessageW(hList, LB_GETITEMDATA, i, 0) == wanted) {
					SendMessageW(hList, LB_SETCURSEL, i, 0);
					break;
				}
			}
		}
		ShowSection(wanted);
	}

	for (const auto& hint : g_hints) {
		AddHint(hint.id, hint.text);
	}

	// Nothing tells this page that the renderer moved under it: the toggle key can
	// switch DLSS while it is open, and what the processor is doing moves with the
	// film.
	SetTimer(kRefreshTimer, 500);

	// A player that paints the property sheet in its own colours does it once the
	// page is up, which would leave its check boxes over our switches. Asking again
	// a moment later puts ours back outermost; it costs one tick and nothing after.
	SetTimer(kDressTimer, 700);

	// From here on an edit is the user's: SetDirty does nothing before this, so
	// filling the controls in above does not light the Apply button.
	m_bActivated = true;

	return S_OK;
}

HRESULT CVRSettingsPPage::OnDeactivate()
{
	KillTimer(kRefreshTimer);
	KillTimer(kDressTimer);
	RememberSection(m_iSection);

	// The page's dialog is destroyed right after this, and its children with it.
	// Forgetting them here is what stops a later activation from adding tooltips to
	// a dead window and quietly losing every hint.
	for (HWND& hSection : m_hSections) {
		if (hSection) {
			::DestroyWindow(hSection);
			hSection = nullptr;
		}
	}
	if (m_hHint) {
		::DestroyWindow(m_hHint);
		m_hHint = nullptr;
	}
	if (--g_pagesDressed <= 0) {
		g_pagesDressed = 0;
		FreeTheme();
	}
	m_bActivated = false;

	return S_OK;
}

INT_PTR CVRSettingsPPage::OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (const INT_PTR colour = ColourMessage(uMsg, wParam, lParam, false)) {
		return colour;
	}

	// The list down the side is ours to draw: a pill in the accent colour behind
	// the section being looked at, and nothing else.
	if (uMsg == WM_MEASUREITEM) {
		auto* mis = (MEASUREITEMSTRUCT*)lParam;
		if (mis->CtlID == IDC_NAV) {
			RECT rc = { 0, 0, 0, 13 };   // one row, in the page's own units
			MapDialogRect(m_hwnd, &rc);
			mis->itemHeight = rc.bottom;
			return (INT_PTR)TRUE;
		}
	}
	if (uMsg == WM_DRAWITEM) {
		const auto* dis = (const DRAWITEMSTRUCT*)lParam;
		if (dis->CtlID == IDC_NAV && (int)dis->itemID >= 0) {
			const bool bSel = (dis->itemState & ODS_SELECTED) != 0;
			RECT rc = dis->rcItem;
			FillRect(dis->hDC, &rc, g_hbrPanel);
			RECT pill = rc;
			InflateRect(&pill, -2, -1);
			if (bSel) {
				FillRound(dis->hDC, pill, g_th.accent, (pill.bottom - pill.top) / 2);
			}
			wchar_t name[64] = {};
			SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)name);
			RECT rcText = pill;
			rcText.left += MulDiv(12, GetDeviceCaps(dis->hDC, LOGPIXELSX), 96);
			SetBkMode(dis->hDC, TRANSPARENT);
			SetTextColor(dis->hDC, bSel ? g_th.onAccent : g_th.text);
			DrawTextW(dis->hDC, name, -1, &rcText,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			return (INT_PTR)TRUE;
		}
	}
	if (uMsg == WM_CTLCOLORLISTBOX) {
		return (INT_PTR)g_hbrPanel;
	}
	// Windows says so when the user switches light and dark while this is open.
	if (uMsg == WM_SETTINGCHANGE && lParam
			&& !wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet")) {
		DressUp();
		::InvalidateRect(m_hwnd, nullptr, TRUE);
		for (HWND hSection : m_hSections) {
			if (hSection) {
				::InvalidateRect(hSection, nullptr, TRUE);
			}
		}
	}

	if (uMsg == WM_TIMER && wParam == kDressTimer) {
		KillTimer(kDressTimer);
		g_pagesDressed--;        // DressUp counts itself, and this is the same page
		DressUp();
		::InvalidateRect(m_hwnd, nullptr, TRUE);
		for (HWND hSection : m_hSections) {
			if (hSection) {
				::InvalidateRect(hSection, nullptr, TRUE);
			}
		}
		return (INT_PTR)TRUE;
	}

	if ((uMsg == WM_TIMER && wParam == kRefreshTimer || uMsg == WM_SHOWWINDOW && wParam) && m_pVideoRenderer) {
		// What the renderer is doing with the picture decides part of the greying,
		// and the toggle key can switch DLSS 5 NR while this page is open. Nothing
		// else of the user's edits is touched.
		Settings_t current;
		m_pVideoRenderer->GetSettings(current);
		const bool bActive = m_pVideoRenderer->GetActive();
		const unsigned uVPUse = bActive ? m_pVideoRenderer->GetVideoProcessorUse() : 0;
		// Only a change the page did not make is the toggle key's: comparing against
		// what the page holds would undo a tick that has not been applied yet.
		const bool bToggled = (current.bDlssNR != m_bDlssNRSeen);
		if (bToggled || bActive != m_bRendererActive || uVPUse != m_uVPUse || uMsg == WM_SHOWWINDOW) {
			if (bToggled) {
				m_bDlssNRSeen = current.bDlssNR;
				m_SetsPP.bDlssNR = current.bDlssNR;
				SetCheck(IDC_CHECK20, m_SetsPP.bDlssNR);
			}
			m_bRendererActive = bActive;
			m_uVPUse = uVPUse;
			EnableControls();
		}
	}

	if (uMsg == WM_COMMAND) {
		const int nID = LOWORD(wParam);
		const int action = HIWORD(wParam);

		if (action == LBN_SELCHANGE && nID == IDC_NAV) {
			HWND hList = ::GetDlgItem(m_hwnd, IDC_NAV);
			const LRESULT index = hList ? SendMessageW(hList, LB_GETCURSEL, 0, 0) : LB_ERR;
			if (index != LB_ERR) {
				ShowSection((int)SendMessageW(hList, LB_GETITEMDATA, index, 0));
			}
			return (INT_PTR)1;
		}
		if (action == BN_CLICKED && nID == IDC_BUTTON1) {
			// Back to the defaults, except the DLSS tuning, which has its own button
			// in its own section -- the same split the two old pages had.
			Settings_t defaults;
			CopyDlssSettings(defaults, m_SetsPP);
			m_SetsPP = defaults;
			SetControls();
			EnableControls();
			SetDirty();
			return (INT_PTR)1;
		}
	}

	return CBasePropertyPage::OnReceiveMessage(hwnd, uMsg, wParam, lParam);
}

INT_PTR CVRSettingsPPage::OnSectionMessage(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (const INT_PTR colour = ColourMessage(uMsg, wParam, lParam, true)) {
		return colour;
	}

	// The page, then the cards on it. Painted here rather than in WM_PAINT so the
	// surfaces land under the controls instead of over them.
	if (uMsg == WM_ERASEBKGND) {
		const HDC hdc = (HDC)wParam;
		RECT rc = {};
		::GetClientRect(hDlg, &rc);
		FillRect(hdc, &rc, g_hbrBg);
		const int section = SectionOf(hDlg);
		const int radius = MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSX), 96);
		for (const auto& card : g_cards) {
			if (card.section != section) {
				continue;
			}
			RECT rcCard = { card.rc.left, card.rc.top, card.rc.right, card.rc.bottom };
			MapDialogRect(hDlg, &rcCard);
			FillRound(hdc, rcCard, g_th.panel, radius);
			// In the light theme the card is barely lighter than the page, so it
			// needs an edge to be a card at all; in the dark one it does not.
			if (!g_bDark) {
				HBRUSH hbr = CreateSolidBrush(g_th.line);
				FrameRect(hdc, &rcCard, hbr);
				DeleteObject(hbr);
			}
		}
		return (INT_PTR)TRUE;
	}

	if (uMsg == WM_COMMAND) {
		const int nID = LOWORD(wParam);
		const int action = HIWORD(wParam);

		if (action == BN_CLICKED) {
			// bEnables: the box decides whether other controls still serve.
			const struct { int id; bool* pValue; bool bEnables; } checks[] = {
				{ IDC_CHECK1,  &m_SetsPP.bUseD3D11,            true  },
				{ IDC_CHECK2,  &m_SetsPP.bShowStats,           false },
				{ IDC_CHECK3,  &m_SetsPP.bDeintDouble,         false },
				{ IDC_CHECK4,  &m_SetsPP.VPFmts.bOther,        true  },
				{ IDC_CHECK5,  &m_SetsPP.bVPScaling,           true  },
				{ IDC_CHECK6,  &m_SetsPP.bInterpolateAt50pct,  false },
				{ IDC_CHECK7,  &m_SetsPP.VPFmts.bNV12,         true  },
				{ IDC_CHECK8,  &m_SetsPP.VPFmts.bP01x,         true  },
				{ IDC_CHECK9,  &m_SetsPP.VPFmts.bYUY2,         true  },
				{ IDC_CHECK10, &m_SetsPP.bUseDither,           false },
				{ IDC_CHECK11, &m_SetsPP.bExclusiveFS,         false },
				{ IDC_CHECK13, &m_SetsPP.bAdjustPresentTime,   false },
				{ IDC_CHECK14, &m_SetsPP.bConvertToSdr,        true  },
				{ IDC_CHECK15, &m_SetsPP.bVBlankBeforePresent, false },
				{ IDC_CHECK16, &m_SetsPP.bReinitByDisplay,     false },
				{ IDC_CHECK17, &m_SetsPP.bDeintBlend,          false },
				{ IDC_CHECK18, &m_SetsPP.bHdrPreferDoVi,       false },
				{ IDC_CHECK19, &m_SetsPP.bVPRTXVideoHDR,       false },
				{ IDC_CHECK20, &m_SetsPP.bDlssNR,              true  },
				{ IDC_CHECK21, &m_SetsPP.bDlssNRAutoMask,      false },
				{ IDC_CHECK22, &m_SetsPP.bDlssNRNoHistory,     false },
				{ IDC_CHECK23, &m_SetsPP.bDlssNRAfterUpscale,  false },
				{ IDC_CHECK24, &m_SetsPP.bDlssNRMotionVectors, false },
				{ IDC_CHECK25, &m_SetsPP.bDlssSR,              true  },
				{ IDC_CHECK26, &m_SetsPP.bDlssRenderAhead,     false },
				{ IDC_CHECK27, &m_SetsPP.bVPReplaceChroma,     true  },
			};
			for (const auto& c : checks) {
				if (nID == c.id) {
					*c.pValue = Checked(c.id);
					if (c.bEnables) {
						EnableControls();
					}
					SetDirty();
					return (INT_PTR)1;
				}
			}

			if (nID == IDC_BUTTON2 || nID == IDC_BUTTON4) {
				const bool bSR = (nID == IDC_BUTTON4);
				wchar_t* target = bSR ? m_SetsPP.szDlssSRDllPath : m_SetsPP.szDlssNRDllPath;
				wchar_t path[MAX_PATH] = {};
				wcscpy_s(path, target);

				OPENFILENAMEW ofn = {};
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner   = hDlg;
				ofn.lpstrFilter = bSR
					? L"DLSS Super Resolution\0nvngx_dlss.dll\0DLL files (*.dll)\0*.dll\0All files (*.*)\0*.*\0\0"
					: L"NGX snippet\0nvngx_dlssnr.dll;nvngx*.dll\0DLL files (*.dll)\0*.dll\0All files (*.*)\0*.*\0\0";
				ofn.lpstrFile   = path;
				ofn.nMaxFile    = (DWORD)std::size(path);
				ofn.lpstrTitle  = bSR ? L"Select nvngx_dlss.dll" : L"Select nvngx_dlssnr.dll";
				ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

				if (GetOpenFileNameW(&ofn)) {
					wcscpy_s(target, MAX_PATH, path);
					SetText(bSR ? IDC_EDIT9 : IDC_EDIT7, target);
					SetDirty();
				}
				return (INT_PTR)1;
			}

			if (nID == IDC_BUTTON3) {
				// Back to the default DLSS tuning. Whether DLSS is on, its key and
				// where the DLL lives are not tuning, so they stay as they are.
				Settings_t defaults;
				defaults.bDlssNR = m_SetsPP.bDlssNR;
				defaults.iDlssNRToggleKey = m_SetsPP.iDlssNRToggleKey;
				wcscpy_s(defaults.szDlssNRDllPath, m_SetsPP.szDlssNRDllPath);
				defaults.bDlssSR = m_SetsPP.bDlssSR;
				wcscpy_s(defaults.szDlssSRDllPath, m_SetsPP.szDlssSRDllPath);
				CopyDlssSettings(m_SetsPP, defaults);
				SetControls();
				EnableControls();
				SetDirty();
				return (INT_PTR)1;
			}
		}

		if (action == EN_CHANGE) {
			if (nID == IDC_EDIT_DISPLAYMAX) {
				SetDirty();
				return (INT_PTR)1;
			}
			if (nID == IDC_EDIT7 || nID == IDC_EDIT9) {
				// SetControls fills the box too; only a real edit makes it dirty.
				wchar_t* target = (nID == IDC_EDIT9) ? m_SetsPP.szDlssSRDllPath : m_SetsPP.szDlssNRDllPath;
				wchar_t path[MAX_PATH] = {};
				if (HWND h = Item(nID)) {
					::GetWindowTextW(h, path, (int)std::size(path));
				}
				if (wcscmp(path, target)) {
					wcscpy_s(target, MAX_PATH, path);
					SetDirty();
				}
				return (INT_PTR)1;
			}
		}

		if (action == CBN_SELCHANGE) {
			// Lists whose selected position is the value that gets saved. Their order
			// must never change: it is what the registry holds.
			const struct { int id; int* pValue; bool bEnables; } byPosition[] = {
				{ IDC_COMBO3,  &m_SetsPP.iDownscaling,       false },
				{ IDC_COMBO4,  &m_SetsPP.iSwapEffect,        false },
				{ IDC_COMBO6,  &m_SetsPP.iResizeStats,       false },
				{ IDC_COMBO7,  &m_SetsPP.iHdrToggleDisplay,  false },
				{ IDC_COMBO8,  &m_SetsPP.iVPSuperRes,        false },
				{ IDC_COMBO9,  &m_SetsPP.iVPDeinterlacing,   false },
				{ IDC_SHARPEN, &m_SetsPP.iSharpen,           true  },
			};
			for (const auto& c : byPosition) {
				if (nID == c.id) {
					const int value = (int)Send(c.id, CB_GETCURSEL);
					if (value != *c.pValue) {
						*c.pValue = value;
						SetDirty();
						if (c.bEnables) {
							EnableControls();
						}
					}
					return (INT_PTR)1;
				}
			}

			// Lists that carry their value as item data, so they can be ordered freely.
			const struct { int id; int* pValue; bool bEnables; } byData[] = {
				{ IDC_COMBO2,  &m_SetsPP.iUpscaling,       false },
				{ IDC_COMBO5,  &m_SetsPP.iChromaScaling,   false },
				{ IDC_COMBO11, &m_SetsPP.iDlssNRStyle,     false },
				{ IDC_COMBO12, &m_SetsPP.iDlssNRPreset,    false },
				{ IDC_COMBO13, &m_SetsPP.iDlssNRToggleKey, false },
				{ IDC_COMBO14, &m_SetsPP.iDlssNRMotion,    true  },
				{ IDC_COMBO15, &m_SetsPP.iDlssSRPreset,    false },
			};
			for (const auto& c : byData) {
				if (nID == c.id) {
					const int value = (int)Combo_CurData(Item(c.id));
					if (value != *c.pValue) {
						*c.pValue = value;
						SetDirty();
						if (c.bEnables) {
							EnableControls();
						}
					}
					return (INT_PTR)1;
				}
			}

			if (nID == IDC_COMBO1) {
				const int value = (int)Combo_CurData(Item(IDC_COMBO1));
				if (value != m_SetsPP.iTexFormat) {
					m_SetsPP.iTexFormat = value;
					SetDirty();
#ifdef _WIN64
					Enable(IDC_CHECK19, m_SetsPP.bUseD3D11 && m_SetsPP.bHdrPassthrough
						&& m_SetsPP.iTexFormat != TEXFMT_8INT);
#endif
				}
				return (INT_PTR)1;
			}

			if (nID == IDC_COMBO10) {
				// One list for two fields: passthrough, or a local tone mapping of a
				// given kind.
				const LRESULT value = Send(IDC_COMBO10, CB_GETCURSEL);
				switch (value) {
				case 0:
					m_SetsPP.bHdrPassthrough = false;
					m_SetsPP.bHdrLocalToneMapping = false;
					break;
				case 1:
					m_SetsPP.bHdrPassthrough = true;
					m_SetsPP.bHdrLocalToneMapping = false;
					break;
				case 2: case 3: case 4: case 5: case 6:
					m_SetsPP.bHdrPassthrough = false;
					m_SetsPP.bHdrLocalToneMapping = true;
					m_SetsPP.iHdrLocalToneMappingType = (int)value - 1;
					break;
				default:
					break;
				}
				SetDirty();
				EnableControls();
				return (INT_PTR)1;
			}
		}
	}
	else if (uMsg == WM_HSCROLL) {
		const HWND hCtrl = (HWND)lParam;

		if (hCtrl == Item(IDC_SHARPEN_LEVEL)) {
			const int value = (int)Send(IDC_SHARPEN_LEVEL, TBM_GETPOS);
			if (value != m_SetsPP.iSharpenLevel) {
				m_SetsPP.iSharpenLevel = value;
				SetText(IDC_SHARPEN_LEVEL_TEXT, std::to_wstring(value).c_str());
				SetDirty();
			}
			return (INT_PTR)1;
		}
		if (hCtrl == Item(IDC_SLIDER1)) {
			const int value = (int)Send(IDC_SLIDER1, TBM_GETPOS);
			if (value != m_SetsPP.iHdrOsdBrightness) {
				m_SetsPP.iHdrOsdBrightness = value;
				SetDirty();
			}
			return (INT_PTR)1;
		}
		if (hCtrl == Item(IDC_SLIDER2)) {
			const int value = (int)Send(IDC_SLIDER2, TBM_GETPOS) * SDR_NITS_STEP;
			if (value != m_SetsPP.iSDRDisplayNits) {
				m_SetsPP.iSDRDisplayNits = value;
				SetText(IDC_EDIT1, std::to_wstring(value).c_str());
				SetDirty();
				// This one applies as it is dragged, so the picture follows the slider.
				Settings_t sets;
				m_pVideoRenderer->GetSettings(sets);
				sets.iSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
				m_pVideoRenderer->SetSettings(sets);
			}
			return (INT_PTR)1;
		}

		// The DLSS sliders are not applied on drag: a settings round-trip per pixel
		// of travel would recreate state on every mouse move.
		const struct { int idSlider; int idEdit; int* pValue; bool bStrength; } sliders[] = {
			{ IDC_SLIDER3, IDC_EDIT3, &m_SetsPP.iDlssNRIntensity,      true  },
			{ IDC_SLIDER4, IDC_EDIT4, &m_SetsPP.iDlssNRLocalTone,      true  },
			{ IDC_SLIDER5, IDC_EDIT5, &m_SetsPP.iDlssNRLocalStructure, true  },
			{ IDC_SLIDER6, IDC_EDIT6, &m_SetsPP.iDlssNRSkinStructure,  true  },
			{ IDC_SLIDER7, IDC_EDIT8, &m_SetsPP.iDlssNRStabilizer,     false },
		};
		for (const auto& s : sliders) {
			if (hCtrl == Item(s.idSlider)) {
				const int value = (int)Send(s.idSlider, TBM_GETPOS);
				if (value != *s.pValue) {
					*s.pValue = value;
					SetText(s.idEdit, (s.bStrength ? StrengthText(value) : StabilizerText(value)).c_str());
					if (!s.bStrength) {
						EnableControls(); // the vectors box needs a running stabilizer
					}
					SetDirty();
				}
				return (INT_PTR)1;
			}
		}
	}

	return (INT_PTR)FALSE;
}

HRESULT CVRSettingsPPage::OnApplyChanges()
{
	BOOL translated = FALSE;
	int displayMaxNits = 0;
	if (HWND h = Item(IDC_EDIT_DISPLAYMAX)) {
		wchar_t text[16] = {};
		::GetWindowTextW(h, text, (int)std::size(text));
		translated = (text[0] != L'\0');
		displayMaxNits = _wtoi(text);
	}
	if (!translated || displayMaxNits <= HDR_NITS_MIN || displayMaxNits > HDR_NITS_MAX) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
	}
	else {
		m_SetsPP.iHdrDisplayMaxNits = displayMaxNits;
	}

	// The DLL paths are free text; an empty box means "locate it automatically".
	if (HWND h = Item(IDC_EDIT7)) {
		::GetWindowTextW(h, m_SetsPP.szDlssNRDllPath, (int)std::size(m_SetsPP.szDlssNRDllPath));
	}
	if (HWND h = Item(IDC_EDIT9)) {
		::GetWindowTextW(h, m_SetsPP.szDlssSRDllPath, (int)std::size(m_SetsPP.szDlssSRDllPath));
	}

	m_pVideoRenderer->SetSettings(m_SetsPP);
	m_pVideoRenderer->SaveSettings();

	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
	m_bDlssNRSeen = m_SetsPP.bDlssNR;

	EnableControls(); // what was just applied decides part of the greying

	// And whether the DLSS sessions came up with it.
	if (CComQIPtr<IExFilterConfig> pIExFilterConfig = m_pVideoRenderer.p) {
		const struct { const char* field; int id; } statuses[] = {
			{ "dlssStatus",   IDC_STATIC28 },
			{ "dlssSRStatus", IDC_STATIC38 },
		};
		for (const auto& s : statuses) {
			LPWSTR pstr = nullptr;
			std::wstring status;
			if (S_OK == pIExFilterConfig->Flt_GetString(s.field, &pstr, nullptr) && pstr) {
				status = pstr;
				CoTaskMemFree(pstr);
			}
			SetText(s.id, status.empty() ? L"" : (L"Status: " + status).c_str());
		}
	}

	return S_OK;
}

void CVRSettingsPPage::AddHint(int id, LPCWSTR text)
{
	HWND hItem = Item(id);
	if (!hItem) {
		return;
	}
	if (!m_hHint) {
		m_hHint = CreateHintWindow(m_Dlg, 15000);
	}
	TOOLINFOW ti = { sizeof(TOOLINFOW) };
	ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
	ti.hwnd = ::GetParent(hItem);
	ti.uId = (UINT_PTR)hItem;
	ti.lpszText = const_cast<LPWSTR>(text);
	SendMessageW(m_hHint, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
