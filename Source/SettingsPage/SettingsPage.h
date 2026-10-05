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

#include "../IVideoRenderer.h"

// CVRSettingsPPage
//
// The one settings page: a list of sections on the left, the section's own child
// dialog on the right. It replaces upstream's Settings page and the fork's DLSS
// page, and owns every field of Settings_t between them -- which is why neither
// the cross-page timer dance nor CopyDlssSettings is needed here.
//
// Upstream's page (CVRMainPPage, IDD_MAINPROPPAGE) is left in the tree exactly as
// it is and simply not listed by GetPages(): a merge from upstream then costs
// nothing, which is the whole point of writing a new page rather than reworking
// theirs.
//
// The sections are separate dialogs, so a control lives in one of them and not in
// the page. Item() finds it by id wherever it is, and everything below works in
// ids as the old pages did.

class __declspec(uuid("B6F4E8A2-3C71-4D59-9E80-1F2A7C4B5D63"))
	CVRSettingsPPage : public CBasePropertyPage, public CWindow
{
public:
	enum Section {
		SECTION_Source = 0,
		SECTION_Chroma,
		SECTION_Scaling,
		SECTION_Detail,
		SECTION_DlssNR,
		SECTION_Hdr,
		SECTION_Present,
		SECTION_COUNT
	};

	CVRSettingsPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRSettingsPPage();

	// Called by the section dialogs' shared window procedure.
	INT_PTR OnSectionMessage(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

	Settings_t m_SetsPP;
	int m_oldSDRDisplayNits = SDR_NITS_DEF;

	// What the renderer is doing right now, watched by the timer: it decides part of
	// the greying, and nothing else tells the page when it changes.
	unsigned m_uVPUse = 0;
	bool m_bRendererActive = false;

	bool m_bActivated = false;
	HWND m_hHint = nullptr;

	// What the renderer last held for DLSS, so the toggle key can be told apart
	// from an edit of the page's own that has not been applied yet.
	bool m_bDlssNRSeen = false;

	HWND m_hSections[SECTION_COUNT] = {};
	int m_iSection = SECTION_Source;

	static INT_PTR CALLBACK SectionProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

	// A control by id, wherever it lives: in one of the sections, or on the page
	// itself for the list and the Default button.
	HWND Item(int id) const;
	void    Enable(int id, BOOL bEnable) const;
	bool    Checked(int id) const;
	void    SetCheck(int id, bool bChecked) const;
	LRESULT Send(int id, UINT uMsg, WPARAM wParam = 0, LPARAM lParam = 0) const;
	void    SetText(int id, LPCWSTR text) const;

	void ShowSection(int section);
	void DressUp();            // fonts, colours, and the controls we draw ourselves
	void FillCombos();
	void SetControls();
	void EnableControls();

	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
	HRESULT OnDeactivate() override;
	INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
	HRESULT OnApplyChanges() override;

	// Filling the controls must not light the Apply button, and nothing before
	// OnActivate has finished is an edit of the user's.
	void SetDirty() {
		if (m_bActivated && !m_bDirty) {
			m_bDirty = TRUE;
			if (m_pPageSite) {
				m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
			}
		}
	}

	// Nothing tells a page that the renderer changed under it -- the toggle key can
	// switch DLSS while this is open, and what the processor is really doing moves
	// with the film.
	static constexpr UINT_PTR kRefreshTimer = 1;

	void AddHint(int id, LPCWSTR text);
};
