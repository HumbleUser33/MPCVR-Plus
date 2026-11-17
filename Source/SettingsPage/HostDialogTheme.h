#pragma once

// The property-page host owns the palette. Ask the outer settings dialog for
// its actual control colours; an ordinary light host returns the system palette.
namespace HostDialogTheme {
struct Palette {
	HBRUSH face = nullptr;
	COLORREF background = 0;
	COLORREF text = 0;

	static Palette From(HWND host, HDC dc)
	{
		const int saved = SaveDC(dc);
		if (!saved) return {};
		SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
		SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
		const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(host, WM_CTLCOLORDLG,
			reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(host)));
		const COLORREF bg = GetBkColor(dc), fg = GetTextColor(dc);
		RestoreDC(dc, saved);
		const auto luma = [](COLORREF c) {
			return 299 * GetRValue(c) + 587 * GetGValue(c) + 114 * GetBValue(c);
		};
		if (!brush || luma(bg) >= 128000 || luma(fg) <= luma(bg)) return {};
		return { brush, bg, fg };
	}
};

inline LRESULT CALLBACK PaintTabs(HWND tabs, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR id, DWORD_PTR)
{
	if (msg == WM_NCDESTROY) {
		RemoveWindowSubclass(tabs, PaintTabs, id);
	} else if (msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_ERASEBKGND) {
		HDC probe = msg == WM_PAINT ? GetDC(tabs) : reinterpret_cast<HDC>(wp);
		const auto colors = probe ? Palette::From(GetParent(tabs), probe) : Palette{};
		if (msg == WM_PAINT && probe) ReleaseDC(tabs, probe);
		if (colors.face) {
			if (msg == WM_ERASEBKGND) return 1;
			PAINTSTRUCT ps{};
			HDC dc = msg == WM_PAINT ? BeginPaint(tabs, &ps) : probe;
			if (dc) {
				const int saved = SaveDC(dc);
				RECT client{};
				GetClientRect(tabs, &client);
				FillRect(dc, &client, colors.face);
				const auto blend = [](COLORREF a, COLORREF b, int percent) {
					return RGB((GetRValue(a)*(100-percent)+GetRValue(b)*percent)/100,
						(GetGValue(a)*(100-percent)+GetGValue(b)*percent)/100,
						(GetBValue(a)*(100-percent)+GetBValue(b)*percent)/100);
				};
				HBRUSH border = CreateSolidBrush(blend(colors.background, colors.text, 20));
				HBRUSH inactive = CreateSolidBrush(blend(colors.background, colors.text, 8));
				SetBkMode(dc, TRANSPARENT);
				SetTextColor(dc, colors.text);
				if (const auto font = SendMessageW(tabs, WM_GETFONT, 0, 0)) {
					SelectObject(dc, reinterpret_cast<HFONT>(font));
				}
				const int selected = TabCtrl_GetCurSel(tabs);
				for (int i = 0; i < TabCtrl_GetItemCount(tabs); ++i) {
					RECT r{};
					wchar_t label[64]{};
					TCITEMW item{};
					item.mask = TCIF_TEXT;
					item.pszText = label;
					item.cchTextMax = _countof(label);
					if (!TabCtrl_GetItemRect(tabs, i, &r) ||
						!SendMessageW(tabs, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(&item))) continue;
					FillRect(dc, &r, i == selected || !inactive ? colors.face : inactive);
					if (border) FrameRect(dc, &r, border);
					DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
					if (i == TabCtrl_GetCurFocus(tabs) && GetFocus() == tabs &&
						!(SendMessageW(tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
						InflateRect(&r, -3, -3);
						DrawFocusRect(dc, &r);
					}
				}
				if (inactive) DeleteObject(inactive);
				if (border) DeleteObject(border);
				if (saved) RestoreDC(dc, saved);
			}
			if (msg == WM_PAINT) EndPaint(tabs, &ps);
			return 0;
		}
	}
	return DefSubclassProc(tabs, msg, wp, lp);
}

inline INT_PTR SectionMessage(HWND section, UINT msg, WPARAM wp, LPARAM lp)
{
	const HWND host = GetParent(section);
	if (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) {
		if (Palette::From(host, reinterpret_cast<HDC>(wp)).face) {
			return SendMessageW(host, msg, wp, lp);
		}
	} else if (msg == WM_ERASEBKGND) {
		const auto dc = reinterpret_cast<HDC>(wp);
		if (const auto palette = Palette::From(host, dc); palette.face) {
			RECT rect{};
			GetClientRect(section, &rect);
			FillRect(dc, &rect, palette.face);
			return TRUE;
		}
	} else if (msg == WM_NOTIFY && lp && reinterpret_cast<const NMHDR*>(lp)->code == NM_CUSTOMDRAW) {
		// The host draws dark checkbox/radio captions and any other controls
		// it handles this way. Their notifications normally stop at the section.
		const auto& custom = *reinterpret_cast<const NMCUSTOMDRAW*>(lp);
		if (!custom.hdc || !Palette::From(host, custom.hdc).face) return FALSE;
		const auto result = SendMessageW(host, msg, wp, lp);
		if (result) {
			SetWindowLongPtrW(section, DWLP_MSGRESULT, result);
			return TRUE;
		}
	}
	return FALSE;
}
} // namespace HostDialogTheme
