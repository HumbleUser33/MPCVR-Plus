// OSD Plus integration facade. Keep upstream renderer hooks small.
#pragma once
#include "../DX11Helper.h"
#include "../D3DUtil/D3D11Font.h"
#include "Telemetry.h"
#include "OverlayText.h"

class COsdPlusOverlay {
    COsdTelemetry m_telemetry;
    ULONGLONG m_firstDraw = 0;
    std::wstring m_text;
    bool m_sourcePending = true;
    LUID m_luid = {};
    COsdPlusText m_draw;
public:
    void Stop() { m_telemetry.Stop(); }
    const std::wstring& Text() const { return m_text; }
    void InvalidateSource() {
        m_firstDraw = 0; m_sourcePending = true; m_text.clear();
        m_telemetry.Invalidate();
    }
    void Update(LUID luid, IPin* upstream) {
        if (luid.HighPart != m_luid.HighPart || luid.LowPart != m_luid.LowPart) {
            m_luid = luid; InvalidateSource();
        }
        const auto now = GetTickCount64();
        if (!m_firstDraw) m_firstDraw = now;
        // No monitoring in the player startup, renderer construction or media setup.
        if (now - m_firstDraw >= 3000 && m_sourcePending) {
            m_telemetry.Start(luid, upstream); m_sourcePending = false;
        }
        const auto text = m_telemetry.Text();
        if (!text.empty()) m_text = text;
    }
    HRESULT Draw(CD3D11Font& font, ID3D11RenderTargetView* target, const SIZE& size,
                 POINT origin, const std::wstring& text, D3DCOLOR white, int hdrBrightness) {
        return m_draw.Draw(font, target, size, origin, text, white, hdrBrightness);
    }
};
