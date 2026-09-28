/*
 * (C) 2018-2026 see Authors.txt
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
#include "resource.h"
#include "Helper.h"
#include "DisplayConfig.h"
#include "PropPage.h"

void SetCursor(HWND hWnd, LPCWSTR lpCursorName)
{
	SetClassLongPtrW(hWnd, GCLP_HCURSOR, (LONG_PTR)::LoadCursorW(nullptr, lpCursorName));
}

void SetCursor(HWND hWnd, UINT nID, LPCWSTR lpCursorName)
{
	SetCursor(::GetDlgItem(hWnd, nID), lpCursorName);
}

inline void ComboBox_AddStringData(HWND hWnd, int nIDComboBox, LPCWSTR str, LONG_PTR data)
{
	LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_ADDSTRING, 0, (LPARAM)str);
	if (lValue != CB_ERR) {
		SendDlgItemMessageW(hWnd, nIDComboBox, CB_SETITEMDATA, lValue, data);
	}
}

inline LONG_PTR ComboBox_GetCurItemData(HWND hWnd, int nIDComboBox)
{
	LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETCURSEL, 0, 0);
	if (lValue != CB_ERR) {
		lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETITEMDATA, lValue, 0);
	}
	return lValue;
}

void ComboBox_SelectByItemData(HWND hWnd, int nIDComboBox, LONG_PTR data)
{
	LRESULT lCount = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETCOUNT, 0, 0);
	if (lCount != CB_ERR) {
		for (int idx = 0; idx < lCount; idx++) {
			const LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETITEMDATA, idx, 0);
			if (data == lValue) {
				SendDlgItemMessageW(hWnd, nIDComboBox, CB_SETCURSEL, idx, 0);
				break;
			}
		}
	}
}


// CVRMainPPage

// https://msdn.microsoft.com/ru-ru/library/windows/desktop/dd375010(v=vs.85).aspx

CVRMainPPage::CVRMainPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"MainProp", lpunk, IDD_MAINPROPPAGE, IDS_MAINPROPPAGE_TITLE)
{
	DLog(L"CVRMainPPage()");
}

CVRMainPPage::~CVRMainPPage()
{
	DLog(L"~CVRMainPPage()");
}

void CVRMainPPage::SetControls()
{
	CheckDlgButton(IDC_CHECK1, m_SetsPP.bUseD3D11             ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK2, m_SetsPP.bShowStats            ? BST_CHECKED : BST_UNCHECKED);

	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO1, m_SetsPP.iTexFormat);

	CheckDlgButton(IDC_CHECK7, m_SetsPP.VPFmts.bNV12          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK8, m_SetsPP.VPFmts.bP01x          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK9, m_SetsPP.VPFmts.bYUY2          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK4, m_SetsPP.VPFmts.bOther         ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO9, CB_SETCURSEL, m_SetsPP.iVPDeinterlacing, 0);
	CheckDlgButton(IDC_CHECK3, m_SetsPP.bDeintDouble          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK5, m_SetsPP.bVPScaling            ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO8, CB_SETCURSEL, m_SetsPP.iVPSuperRes, 0);
	CheckDlgButton(IDC_CHECK19, m_SetsPP.bVPRTXVideoHDR       ? BST_CHECKED : BST_UNCHECKED);

	if (m_SetsPP.bHdrPassthrough) {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, 0);
	} else if (m_SetsPP.bHdrLocalToneMapping) {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, m_SetsPP.iHdrLocalToneMappingType);
	} else {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, -1);
	}

	CheckDlgButton(IDC_CHECK18, m_SetsPP.bHdrPreferDoVi       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK14, m_SetsPP.bConvertToSdr        ? BST_CHECKED : BST_UNCHECKED);

	SendDlgItemMessageW(IDC_COMBO7, CB_SETCURSEL, m_SetsPP.iHdrToggleDisplay, 0);
	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETPOS, 1, m_SetsPP.iHdrOsdBrightness);

	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETPOS, 1, m_SetsPP.iSDRDisplayNits / SDR_NITS_STEP);
	GetDlgItem(IDC_EDIT1).SetWindowTextW(std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());

	CheckDlgButton(IDC_CHECK27, m_SetsPP.bVPReplaceChroma     ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK6, m_SetsPP.bInterpolateAt50pct   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK10, m_SetsPP.bUseDither           ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK17, m_SetsPP.bDeintBlend          ? BST_CHECKED : BST_UNCHECKED);

	CheckDlgButton(IDC_CHECK11, m_SetsPP.bExclusiveFS         ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK15, m_SetsPP.bVBlankBeforePresent ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK13, m_SetsPP.bAdjustPresentTime   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK16, m_SetsPP.bReinitByDisplay     ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK26, m_SetsPP.bDlssRenderAhead     ? BST_CHECKED : BST_UNCHECKED);

	SendDlgItemMessageW(IDC_COMBO6, CB_SETCURSEL, m_SetsPP.iResizeStats, 0);

	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO5, m_SetsPP.iChromaScaling);
	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO2, m_SetsPP.iUpscaling);
	SendDlgItemMessageW(IDC_COMBO3, CB_SETCURSEL, m_SetsPP.iDownscaling, 0);
	SendDlgItemMessageW(IDC_COMBO4, CB_SETCURSEL, m_SetsPP.iSwapEffect, 0);

	m_SetsPP.iHdrDisplayMaxNits = discard<int>(m_SetsPP.iHdrDisplayMaxNits, HDR_NITS_DEF, HDR_NITS_MIN, HDR_NITS_MAX);
	SetDlgItemTextW(IDC_EDIT_DISPLAYMAX, std::to_wstring(m_SetsPP.iHdrDisplayMaxNits).c_str());
}

void CVRMainPPage::EnableControls()
{
	if (!IsWindows8OrGreater()) { // Windows 7
		const BOOL bEnable = !m_SetsPP.bUseD3D11;
		GetDlgItem(IDC_STATIC1).EnableWindow(bEnable); // not working for GROUPBOX
		GetDlgItem(IDC_STATIC2).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK7).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK8).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK9).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK4).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK3).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK5).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC3).EnableWindow(bEnable);
		GetDlgItem(IDC_COMBO4).EnableWindow(bEnable);
	}
	else if (IsWindows10OrGreater()) {
		const BOOL bEnable = m_SetsPP.bUseD3D11;
		GetDlgItem(IDC_COMBO10).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC5).EnableWindow(bEnable);
		GetDlgItem(IDC_COMBO7).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC6).EnableWindow(bEnable);
		GetDlgItem(IDC_SLIDER1).EnableWindow(bEnable);
		// Super Resolution is settled below, with the rest of what the processor is
		// really doing: asking for it is not enough to make it happen.
#ifdef _WIN64
		GetDlgItem(IDC_CHECK19).EnableWindow(bEnable && m_SetsPP.bHdrPassthrough);
#endif
	}

	GetDlgItem(IDC_STATIC8).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_EDIT1).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_SLIDER2).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_EDIT_DISPLAYMAX).EnableWindow(m_SetsPP.bHdrLocalToneMapping);

	// What the video processor takes, the shaders never see. It converts the formats
	// ticked above, chroma upsampling included, and resizes as well when "Use for
	// resizing" is on -- except while DLSS 5 NR or DLSS Super Resolution runs, which
	// keeps it at the source size and hands the resizing back to the shaders.
	// DLSS Super Resolution enlarges in its place, so the Upscaling method then only
	// stands in where it cannot run.
	// "Replace VP chroma upsampling" gives the chroma of a progressive 4:2:0/4:2:2
	// picture back to the shaders, and only the chroma: the processor is handed a
	// 4:4:4 picture and goes on converting and resizing it.
	// While something plays, the renderer also says what it is really doing with it,
	// and a list is only greyed when both agree that it has nothing to do: the format
	// boxes and that option are a rule of thumb for an idle page, since Dolby Vision,
	// YCgCo and RGB on Nvidia go through the shaders whatever is ticked, and an
	// interlaced picture keeps the processor whatever the option says.
	const bool bAllVPFormats = m_SetsPP.VPFmts.bNV12 && m_SetsPP.VPFmts.bP01x
		&& m_SetsPP.VPFmts.bYUY2 && m_SetsPP.VPFmts.bOther;
	const bool bVPAvailable = !(m_SetsPP.bUseD3D11 && !IsWindows8OrGreater()); // no D3D11 VP on Windows 7
	const bool bDlssPass = m_SetsPP.bUseD3D11 && (m_SetsPP.bDlssNR || m_SetsPP.bDlssSR);
	const bool bChromaToShaders = m_SetsPP.bUseD3D11 && m_SetsPP.bVPReplaceChroma;
	// The processor can take the picture and still not be the one rebuilding its
	// chroma: with the option on, the shaders do that before handing it over, and
	// the chroma list is theirs again while the resizing lists stay its own.
	const bool bVPTakesPicture = bAllVPFormats && bVPAvailable;
	const bool bVPConverts = bVPTakesPicture && !bChromaToShaders
		&& (!m_bRendererActive || (m_uVPUse & VPUSE_Converting));
	const bool bVPResizes = bVPTakesPicture && m_SetsPP.bVPScaling && !bDlssPass
		&& (!m_bRendererActive || (m_uVPUse & VPUSE_Resizing));

	const BOOL bChromaList = !bVPConverts;
	const BOOL bDownscalingList = !bVPResizes;
	const BOOL bUpscalingList = !bVPResizes && !(m_SetsPP.bUseD3D11 && m_SetsPP.bDlssSR);
	GetDlgItem(IDC_STATIC40).EnableWindow(bChromaList);
	GetDlgItem(IDC_COMBO5).EnableWindow(bChromaList);
	GetDlgItem(IDC_STATIC39).EnableWindow(bUpscalingList);
	GetDlgItem(IDC_COMBO2).EnableWindow(bUpscalingList);
	GetDlgItem(IDC_STATIC41).EnableWindow(bDownscalingList);
	GetDlgItem(IDC_COMBO3).EnableWindow(bDownscalingList);
	GetDlgItem(IDC_CHECK6).EnableWindow(bUpscalingList || bDownscalingList);

	// RTX Video Super Resolution lives inside the processor and only does something
	// while the processor is the one enlarging the picture -- which is what "Use for
	// resizing" gives it, and what a DLSS pass takes away. It also wants a subsampled
	// picture: the driver leaves a 4:4:4 one untouched, measured here at 0.000 against
	// 4.0 on 4:2:0 (tools/dlssnr_probe, vp444_probe), so it is out of reach while
	// "Replace VP chroma upsampling" hands one over. RTX Video HDR is the other way
	// round and works on 4:4:4, which is why that box helps it and not this one.
	// Greyed rather than accepted and ignored, so the hint can say which box to untick.
#ifdef _WIN64
	const bool bChromaReplacedNow = bChromaToShaders
		&& (!m_bRendererActive || !(m_uVPUse & VPUSE_Converting));
	const BOOL bSuperRes = m_SetsPP.bUseD3D11 && IsWindows10OrGreater()
		&& bVPResizes && !bChromaReplacedNow;
#else
	const BOOL bSuperRes = FALSE; // the extension is x64 only
#endif
	GetDlgItem(IDC_STATIC7).EnableWindow(bSuperRes);
	GetDlgItem(IDC_COMBO8).EnableWindow(bSuperRes);

	// Render ahead and the chroma replacement belong to the Direct3D 11 processor.
	GetDlgItem(IDC_CHECK26).EnableWindow(m_SetsPP.bUseD3D11);
	GetDlgItem(IDC_CHECK27).EnableWindow(m_SetsPP.bUseD3D11);
}

HRESULT CVRMainPPage::OnConnect(IUnknown *pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRMainPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	if (m_SetsPP.iSDRDisplayNits != m_oldSDRDisplayNits) {
		// OK or Apply buttons were not pressed. cancel the settings.
		m_pVideoRenderer->GetSettings(m_SetsPP);
		m_SetsPP.iSDRDisplayNits = m_oldSDRDisplayNits;
		m_pVideoRenderer->SetSettings(m_SetsPP);
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

HRESULT CVRMainPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	m_pVideoRenderer->GetSettings(m_SetsPP);
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
	m_bRendererActive = m_pVideoRenderer->GetActive();
	m_uVPUse = m_bRendererActive ? m_pVideoRenderer->GetVideoProcessorUse() : 0;

	if (!IsWindows7SP1OrGreater()) {
		GetDlgItem(IDC_CHECK1).EnableWindow(FALSE);
		m_SetsPP.bUseD3D11 = false;
	}
	if (!IsWindows10OrGreater()) {
		GetDlgItem(IDC_COMBO10).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC5).EnableWindow(FALSE);
		GetDlgItem(IDC_COMBO7).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC6).EnableWindow(FALSE);
		GetDlgItem(IDC_SLIDER1).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC7).EnableWindow(FALSE);
		GetDlgItem(IDC_COMBO8).EnableWindow(FALSE);
		GetDlgItem(IDC_CHECK19).EnableWindow(FALSE);
	}

#ifndef _WIN64
	GetDlgItem(IDC_STATIC7).EnableWindow(FALSE);
	GetDlgItem(IDC_COMBO8).EnableWindow(FALSE);
	GetDlgItem(IDC_CHECK19).EnableWindow(FALSE);
#endif

	EnableControls();

	SendDlgItemMessageW(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Fixed font size");
	SendDlgItemMessageW(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Increase font by window");

	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"Auto 8/10-bit Integer",  0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"8-bit Integer",          8);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"10-bit Integer",        10);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"16-bit Floating Point", 16);

	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Enable");
	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"HACK future frames");

	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for SD");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 720p");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1080p");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1440p");

	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Do not change");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on (fullscreen)");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off (fullscreen)");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off");

	// Both lists are shown best first, as they measured on ten film references
	// brought from 1080p to 4K (tools/dlssnr_probe, --tchroma and --tupscale, the
	// tables in README-DLSS5.md). The number each entry carries is what is saved, so
	// the order can change without moving anybody's setting.
	ComboBox_AddStringData(m_hWnd, IDC_COMBO5, L"Jinc (EWA)",         CHROMA_Jinc);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO5, L"RAVU-zoom",          CHROMA_RAVU);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO5, L"Catmull-Rom",        CHROMA_CatmullRom);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO5, L"FSRCNNX 8 AR",       CHROMA_FSRCNNX8AR);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO5, L"Bilinear",           CHROMA_Bilinear);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO5, L"Nearest-neighbor",   CHROMA_Nearest);

	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"ArtCNN C4F16 DS",    UPSCALE_ArtCNN);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"RAVU-zoom",          UPSCALE_RAVUZoom);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"FSRCNNX 16 AR",      UPSCALE_FSRCNNX16AR);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"FSRCNNX 8 AR",       UPSCALE_FSRCNNX8AR);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"FSRCNNX 16",         UPSCALE_FSRCNNX16);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"FSRCNNX 8",          UPSCALE_FSRCNNX8);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"Catmull-Rom",        UPSCALE_CatmullRom);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"Lanczos2",           UPSCALE_Lanczos2);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"Mitchell-Netravali", UPSCALE_Mitchell);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"Lanczos3",           UPSCALE_Lanczos3);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"Jinc2m",             UPSCALE_Jinc2);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO2, L"Nearest-neighbor",   UPSCALE_Nearest);

	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Box");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bilinear");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Hamming");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic sharp");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Lanczos");

	SendDlgItemMessageW(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Discard");
	SendDlgItemMessageW(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Flip");

	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETRANGE, 0, MAKELONG(0, 2));
	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETTIC, 0, 1);

	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETRANGE, 0, MAKELONG(SDR_NITS_MIN / SDR_NITS_STEP, SDR_NITS_MAX / SDR_NITS_STEP));
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETTIC, 0, SDR_NITS_DEF / SDR_NITS_STEP);
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETLINESIZE, 0, 1); // arrow keys
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETPAGESIZE, 0, 5); // clicks on trackbar's channel

	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());

	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Ignore", -1);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Passthrough", 0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"ACES", 1);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Reinhard", 2);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Hable", 3);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Mobius", 4);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"BT2390/ST 2094-10", 5);

	SetControls();

	SetCursor(m_hWnd, IDC_ARROW);
	SetCursor(m_hWnd, IDC_COMBO1, IDC_HAND);

	AddHint(IDC_CHECK5,
		L"It works fast, but it's not always good.\n"
		"Disable it if you want to use shaders for resizing.\n"
		"It only covers the formats ticked above, and DLSS takes\n"
		"the resizing back from it while it runs.\n"
		"It decides the resizing and nothing else: the chroma is\n"
		"the business of the formats above and of \"Replace VP\n"
		"chroma upsampling\".\n"
		"It is also what gives \"Request Super Resolution\" something\n"
		"to work on, since the processor can only enhance a picture\n"
		"it is enlarging itself.");
	AddHint(IDC_COMBO8,
		L"Available for Direct3D 11.\n"
		"Requires hardware and driver support:\n"
		"- Intel Graphics UHD 610 or later\n"
		"- Nvidia RTX (x64 only)\n"
		"Greyed unless the processor is the one enlarging the\n"
		"picture: tick \"Use for resizing\" above. DLSS takes the\n"
		"enlarging back while it runs, and greys this too.\n"
		"Greyed as well while \"Replace VP chroma upsampling\" is on:\n"
		"the driver leaves a 4:4:4 picture untouched, measured here\n"
		"at 0.000 against 4.0 on 4:2:0, so it would do nothing at\n"
		"all. Untick that box to get it back -- one picture cannot\n"
		"have both. RTX Video HDR is the opposite case: it does work\n"
		"on 4:4:4, which is why that box helps it.");
	AddHint(IDC_CHECK19,
		L"Available for Direct3D 11.\n"
		"Requires hardware and driver support:\n"
		"- Nvidia RTX (x64 only)\n"
		"The driver only tone maps an 8-bit or a 4:4:4 picture: on a\n"
		"10-bit source it does nothing unless \"Replace VP chroma\n"
		"upsampling\" hands the processor a 4:4:4 one, and the\n"
		"statistics only claim it where it really applies.");
	AddHint(IDC_COMBO5,
		L"Used for YUV 4:2:0/4:2:2 input formats when the video\n"
		"processor does not convert them: it does its own chroma.\n"
		"Greyed while the video processor is converting what plays,\n"
		"and when every format above is ticked and the box below is\n"
		"not; it still stands in for what the video processor refuses\n"
		"(Dolby Vision, YCgCo, RGB on Nvidia).\n"
		"Listed best first, as measured on 1080p film.\n"
		"Jinc (EWA): the polar kernel madVR calls Jinc, radius\n"
		"3.2383. The best of them on film, +0.6 dB on the colour\n"
		"along luma edges against Catmull-Rom, and it stays inside\n"
		"the conversion shader: about 32 texels a pixel against 16.\n"
		"RAVU-zoom and FSRCNNX 8 AR put Cb and Cr through an mpv\n"
		"prescaler instead, for a few milliseconds: RAVU gains 0.5 dB,\n"
		"FSRCNNX loses 0.2 on film but is the best of the four on\n"
		"drawn lines, where it gains 2.3.\n"
		"All three want Direct3D 11 and 4:2:0 in planes;\n"
		"Catmull-Rom is used elsewhere.");
	AddHint(IDC_CHECK27,
		L"Available for Direct3D 11.\n"
		"The shaders rebuild the chroma of a progressive YUV\n"
		"4:2:0/4:2:2 picture with the method above and hand the video\n"
		"processor a 4:4:4 one, so the processor stays in the chain:\n"
		"RTX Video HDR goes on working, and on a 10-bit source it\n"
		"starts working, the driver leaving 10-bit 4:2:0 untouched.\n"
		"The processor's own chroma comes out about bilinear, and on\n"
		"a 10-bit source it also shifts the colour by about one level;\n"
		"measured here, this gains 5.6 dB on the colour of a 10-bit\n"
		"film and 0.6 dB on an 8-bit one.\n"
		"Interlaced video keeps the processor's chroma, since it alone\n"
		"deinterlaces, and so do 4:4:4 and RGB, which have no chroma\n"
		"to rebuild.\n"
		"What it costs: RTX Video Super Resolution. The driver leaves\n"
		"a 4:4:4 picture untouched (0.000 against 4.0 on 4:2:0,\n"
		"measured here), so it greys while this is on. The two cannot\n"
		"both apply to one picture; untick this to go back to it.");
	AddHint(IDC_COMBO2,
		L"Used to increase image size when the\n"
		"DVXA2/D3D11 Video Processor is not used for resizing.\n"
		"Listed best first, as measured on film brought to 4K.\n"
		"FSRCNNX 8/16 and ArtCNN double the luma through a small\n"
		"network, RAVU-zoom enlarges it to any size; the colour comes\n"
		"from Catmull-Rom, which also covers the rest of the scale.\n"
		"ArtCNN C4F16 DS is the best of them on grain and on\n"
		"compression -- it cleans them as it enlarges -- and the\n"
		"dearest, about 13 ms for 1080p to 4K on an RTX 3050.\n"
		"AR holds what the network invented to the range the source\n"
		"really covers: FSRCNNX rings without it, and RAVU-zoom\n"
		"carries its own.\n"
		"They need Direct3D 11; Catmull-Rom stands in elsewhere.\n"
		"Greyed while the video processor is resizing what plays, or\n"
		"while DLSS Super Resolution handles upscaling (DLSS page);\n"
		"it then only stands in where they cannot run.");
	AddHint(IDC_COMBO3,
		L"Used to reduce image size when the video processor does\n"
		"not resize. Greyed while it is resizing what plays; DLSS\n"
		"hands the resizing back to these shaders.");
	AddHint(IDC_COMBO4,
		L"'Flip' is more efficient, but 'Discard' may work\n"
		"more correctly in some rare situations.");
	AddHint(IDC_CHECK26,
		L"Available for Direct3D 11.\n"
		"Starts each picture earlier by the time the heavy passes\n"
		"take -- DLSS 5 NR, DLSS Super Resolution, FSRCNNX and\n"
		"RAVU-zoom -- and holds it until its own time, so they do\n"
		"not make the video late against the audio.\n"
		"It does nothing while none of them runs.");

	// The frame never tells this page that another one applied something, and it does
	// not always send WM_SHOWWINDOW when the tab comes back either.
	SetTimer(kRefreshTimer, 500);

	return S_OK;
}

HRESULT CVRMainPPage::OnDeactivate()
{
	KillTimer(kRefreshTimer);

	return S_OK;
}

INT_PTR CVRMainPPage::OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if ((uMsg == WM_TIMER && wParam == kRefreshTimer || uMsg == WM_SHOWWINDOW && wParam) && m_pVideoRenderer) {
		// What the DLSS page holds is read back from the renderer, and so is what it is
		// doing with the picture: both decide part of the greying here. Only those
		// fields are taken, never an edit made on this page, and nothing is touched
		// while nothing has moved.
		Settings_t current;
		m_pVideoRenderer->GetSettings(current);
		const bool bActive = m_pVideoRenderer->GetActive();
		const unsigned uVPUse = bActive ? m_pVideoRenderer->GetVideoProcessorUse() : 0;
		if (current.bDlssNR != m_SetsPP.bDlssNR || current.bDlssSR != m_SetsPP.bDlssSR
				|| bActive != m_bRendererActive || uVPUse != m_uVPUse || uMsg == WM_SHOWWINDOW) {
			CopyDlssSettings(m_SetsPP, current);
			m_bRendererActive = bActive;
			m_uVPUse = uVPUse;
			EnableControls();
		}
	}

	if (uMsg == WM_COMMAND) {
		LRESULT lValue;
		const int nID = LOWORD(wParam);
		int action = HIWORD(wParam);

		if (action == BN_CLICKED) {
			if (nID == IDC_CHECK1) {
				m_SetsPP.bUseD3D11 = IsDlgButtonChecked(IDC_CHECK1) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK2) {
				m_SetsPP.bShowStats = IsDlgButtonChecked(IDC_CHECK2) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK3) {
				m_SetsPP.bDeintDouble = IsDlgButtonChecked(IDC_CHECK3) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK5) {
				m_SetsPP.bVPScaling = IsDlgButtonChecked(IDC_CHECK5) == BST_CHECKED;
				SetDirty();
				EnableControls(); // Super Resolution follows it, and so do the shader lists
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK6) {
				m_SetsPP.bInterpolateAt50pct = IsDlgButtonChecked(IDC_CHECK6) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			// What the video processor takes decides which shader lists still serve.
			if (nID == IDC_CHECK7) {
				m_SetsPP.VPFmts.bNV12 = IsDlgButtonChecked(IDC_CHECK7) == BST_CHECKED;
				SetDirty();
				EnableControls();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK8) {
				m_SetsPP.VPFmts.bP01x = IsDlgButtonChecked(IDC_CHECK8) == BST_CHECKED;
				SetDirty();
				EnableControls();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK9) {
				m_SetsPP.VPFmts.bYUY2 = IsDlgButtonChecked(IDC_CHECK9) == BST_CHECKED;
				SetDirty();
				EnableControls();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK4) {
				m_SetsPP.VPFmts.bOther = IsDlgButtonChecked(IDC_CHECK4) == BST_CHECKED;
				SetDirty();
				EnableControls();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK10) {
				m_SetsPP.bUseDither = IsDlgButtonChecked(IDC_CHECK10) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK17) {
				m_SetsPP.bDeintBlend = IsDlgButtonChecked(IDC_CHECK17) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK11) {
				m_SetsPP.bExclusiveFS = IsDlgButtonChecked(IDC_CHECK11) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK15) {
				m_SetsPP.bVBlankBeforePresent = IsDlgButtonChecked(IDC_CHECK15) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK13) {
				m_SetsPP.bAdjustPresentTime = IsDlgButtonChecked(IDC_CHECK13) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK16) {
				m_SetsPP.bReinitByDisplay = IsDlgButtonChecked(IDC_CHECK16) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK26) {
				m_SetsPP.bDlssRenderAhead = IsDlgButtonChecked(IDC_CHECK26) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK27) {
				m_SetsPP.bVPReplaceChroma = IsDlgButtonChecked(IDC_CHECK27) == BST_CHECKED;
				SetDirty();
				EnableControls(); // the Chroma upsampling list comes back with it
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK18) {
				m_SetsPP.bHdrPreferDoVi = IsDlgButtonChecked(IDC_CHECK18) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK14) {
				m_SetsPP.bConvertToSdr = IsDlgButtonChecked(IDC_CHECK14) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK19) {
				m_SetsPP.bVPRTXVideoHDR = IsDlgButtonChecked(IDC_CHECK19) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}

			if (nID == IDC_BUTTON1) {
				m_SetsPP.SetDefault();
				SetControls();
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
		}

		if (action == CBN_SELCHANGE) {
			if (nID == IDC_COMBO6) {
				lValue = SendDlgItemMessageW(IDC_COMBO6, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iResizeStats) {
					m_SetsPP.iResizeStats = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO1) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO1);
				if (lValue != m_SetsPP.iTexFormat) {
					m_SetsPP.iTexFormat = lValue;
					SetDirty();
#ifdef _WIN64
					GetDlgItem(IDC_CHECK19).EnableWindow(m_SetsPP.bUseD3D11 && m_SetsPP.bHdrPassthrough && m_SetsPP.iTexFormat != TEXFMT_8INT);
#endif
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO9) {
				lValue = SendDlgItemMessageW(IDC_COMBO9, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iVPDeinterlacing) {
					m_SetsPP.iVPDeinterlacing = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO8) {
				lValue = SendDlgItemMessageW(IDC_COMBO8, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iVPSuperRes) {
					m_SetsPP.iVPSuperRes = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO7) {
				lValue = SendDlgItemMessageW(IDC_COMBO7, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iHdrToggleDisplay) {
					m_SetsPP.iHdrToggleDisplay = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO5) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO5);
				if (lValue != m_SetsPP.iChromaScaling) {
					m_SetsPP.iChromaScaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO2) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO2);
				if (lValue != m_SetsPP.iUpscaling) {
					m_SetsPP.iUpscaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO3) {
				lValue = SendDlgItemMessageW(IDC_COMBO3, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iDownscaling) {
					m_SetsPP.iDownscaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO4) {
				lValue = SendDlgItemMessageW(IDC_COMBO4, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iSwapEffect) {
					m_SetsPP.iSwapEffect = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO10) {
				lValue = SendDlgItemMessageW(IDC_COMBO10, CB_GETCURSEL, 0, 0);
				switch (lValue) {
					case 0:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = false;
						break;
					case 1:
						m_SetsPP.bHdrPassthrough = true;
						m_SetsPP.bHdrLocalToneMapping = false;
						break;
					case 2:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 1;
						break;
					case 3:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 2;
						break;
					case 4:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 3;
						break;
					case 5:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 4;
						break;
					case 6:
						m_SetsPP.bHdrPassthrough = false;
						m_SetsPP.bHdrLocalToneMapping = true;
						m_SetsPP.iHdrLocalToneMappingType = 5;
						break;
					default:
						break;
				}
				SetDirty();
				EnableControls();
				return (LRESULT)1;
			}
		}
		if (action == EN_CHANGE) {
			if (nID == IDC_EDIT_DISPLAYMAX) {
				SetDirty();
			}
		}
	}
	else if (uMsg == WM_HSCROLL) {
		if ((HWND)lParam == GetDlgItem(IDC_SLIDER1)) {
			LRESULT lValue = SendDlgItemMessageW(IDC_SLIDER1, TBM_GETPOS, 0, 0);
			if (lValue != m_SetsPP.iHdrOsdBrightness) {
				m_SetsPP.iHdrOsdBrightness = lValue;
				SetDirty();
			}
			return (LRESULT)1;
		}
		if ((HWND)lParam == GetDlgItem(IDC_SLIDER2)) {
			LRESULT lValue = SendDlgItemMessageW(IDC_SLIDER2, TBM_GETPOS, 0, 0);
			lValue *= SDR_NITS_STEP;
			if (lValue != m_SetsPP.iSDRDisplayNits) {
				m_SetsPP.iSDRDisplayNits = lValue;
				GetDlgItem(IDC_EDIT1).SetWindowTextW(std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());
				SetDirty();
				{
					// apply only SDRDisplayNits
					Settings_t sets;
					m_pVideoRenderer->GetSettings(sets);
					sets.iSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
					m_pVideoRenderer->SetSettings(sets);
				}
			}
			return (LRESULT)1;
		}
	}

	// Let the parent class handle the message.
	return CBasePropertyPage::OnReceiveMessage(hwnd, uMsg, wParam, lParam);
}

HRESULT CVRMainPPage::OnApplyChanges()
{
	wchar_t data[32] = {};
	GetDlgItemTextW(IDC_EDIT_DISPLAYMAX, data, 32);
	int displayMaxNits;
	try {
		displayMaxNits = std::stoi(data);
	} catch (const std::exception&) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
		return S_FALSE;
	}

	if (displayMaxNits <= HDR_NITS_MIN || displayMaxNits > HDR_NITS_MAX) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
		return S_FALSE;
	}
	// if not error then set to m_setsPP
	m_SetsPP.iHdrDisplayMaxNits = displayMaxNits;

	// The DLSS settings live on their own page, and the toggle key changes them
	// while this one is open: keep what the renderer holds for them.
	{
		Settings_t current;
		m_pVideoRenderer->GetSettings(current);
		CopyDlssSettings(m_SetsPP, current);
	}

	m_pVideoRenderer->SetSettings(m_SetsPP);
	m_pVideoRenderer->SaveSettings();

	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;

	EnableControls(); // what was just applied decides part of the greying

	return S_OK;
}

HWND CVRMainPPage::CreateHintWindow(HWND parent, int timePop, int timeInit, int timeReshow)
{
	HWND hhint = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
		WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, nullptr, nullptr);

	SetWindowPos(hhint, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(timePop, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELONG(timeInit, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_RESHOW, MAKELONG(timeReshow, 0));
	SendMessageW(hhint, TTM_SETMAXTIPWIDTH, 0, 470);
	return hhint;
}

void CVRMainPPage::AddHint(int id, const LPCWSTR text)
{
	if (!m_hHint) {
		m_hHint = CreateHintWindow(m_Dlg, 15000);
	}
	TOOLINFOW ti;
	ti.cbSize = sizeof(TOOLINFOW);
	ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
	ti.hwnd = m_Dlg;
	ti.uId = (LPARAM)GetDlgItem(id).m_hWnd;
	ti.lpszText = const_cast<LPWSTR>(text);
	SendMessageW(m_hHint, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// CVRInfoPPage

CVRInfoPPage::CVRInfoPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"InfoProp", lpunk, IDD_INFOPROPPAGE, IDS_INFOPROPPAGE_TITLE)
{
	DLog(L"CVRInfoPPage()");
}

CVRInfoPPage::~CVRInfoPPage()
{
	DLog(L"~CVRInfoPPage()");

	if (m_hMonoFont) {
		DeleteObject(m_hMonoFont);
		m_hMonoFont = 0;
	}
}

HRESULT CVRInfoPPage::OnConnect(IUnknown *pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRInfoPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

HWND GetParentOwner(HWND hwnd)
{
	HWND hWndParent = hwnd;
	HWND hWndT;
	while ((::GetWindowLongPtrW(hWndParent, GWL_STYLE) & WS_CHILD) &&
		(hWndT = ::GetParent(hWndParent)) != NULL) {
		hWndParent = hWndT;
	}

	return hWndParent;
}

static WNDPROC OldControlProc;
static LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_KEYDOWN && LOWORD(wParam) == VK_ESCAPE) {
		// fixed Esc handling when EDITTEXT control has ES_MULTILINE property and is in focus
		HWND parentOwner = GetParentOwner(control);
		if (parentOwner) {
			::PostMessageW(parentOwner, WM_COMMAND, IDCANCEL, 0);
		}
		return TRUE;
	}

	return CallWindowProcW(OldControlProc, control, message, wParam, lParam); // call edit control's own windowproc
}

HRESULT CVRInfoPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());

	// init monospace font
	LOGFONTW lf = {};
	HDC hdc = GetWindowDC();
	lf.lfHeight = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
	ReleaseDC(hdc);
	lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
	wcscpy_s(lf.lfFaceName, L"Consolas");
	m_hMonoFont = CreateFontIndirectW(&lf);

	GetDlgItem(IDC_EDIT1).SetFont(m_hMonoFont);
	ASSERT(m_pVideoRenderer);

	if (!m_pVideoRenderer->GetActive()) {
		SetDlgItemTextW(IDC_EDIT1, L"filter is not active");
		return S_OK;
	}

	std::wstring strInfo(L"Windows ");
	strInfo.append(GetWindowsVersion());
	strInfo.append(L"\r\n");

	std::wstring strVP;
	if (S_OK == m_pVideoRenderer->GetVideoProcessorInfo(strVP)) {
		str_replace(strVP, L"\n", L"\r\n");
		strInfo.append(strVP);
	}

#ifdef _DEBUG
	{
		std::vector<DisplayConfig_t> displayConfigs;

		bool ret = GetDisplayConfigs(displayConfigs);

		strInfo.append(L"\r\n");

		for (const auto& dc : displayConfigs) {
			double freq = (double)dc.refreshRate.Numerator / (double)dc.refreshRate.Denominator;
			strInfo += std::format(L"\r\n{} - {:.3f} Hz", dc.displayName, freq);

			if (dc.bitsPerChannel) { // if bitsPerChannel is not set then colorEncoding and other values are invalid
				const wchar_t* colenc = ColorEncodingToString(dc.colorEncoding);
				if (colenc) {
					strInfo += std::format(L" {}", colenc);
				}
				strInfo += std::format(L" {}-bit", dc.bitsPerChannel);
			}

			const wchar_t* output = OutputTechnologyToString(dc.outputTechnology);
			if (output) {
				strInfo += std::format(L" {}", output);
			}
		}
	}
#endif

	SetDlgItemTextW(IDC_EDIT1, strInfo.c_str());

	OldControlProc = (WNDPROC)::SetWindowLongPtrW(::GetDlgItem(m_hWnd, IDC_EDIT1), GWLP_WNDPROC, (LONG_PTR)ControlProc);

	return S_OK;
}
