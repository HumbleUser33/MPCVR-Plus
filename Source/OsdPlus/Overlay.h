// OSD Plus integration facade. Keep upstream renderer hooks small.
#pragma once
#include "../DX11Helper.h"
#include "../D3DUtil/D3D11Font.h"
#include "Telemetry.h"

class COsdPlusOverlay {
    COsdTelemetry m_telemetry;
    ULONGLONG m_firstDraw = 0;
    std::wstring m_text;
    bool m_sourcePending = true;
    LUID m_luid = {};
public:
    static constexpr bool ShowSyncGraph() { return false; }
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
        float y = static_cast<float>(origin.y);
        D3DCOLOR valueColor = D3DCOLOR_XRGB(128, 255, 128);
        const auto color = [&](D3DCOLOR c) {
            if (white == D3DCOLOR_XRGB(255, 255, 255)) return c;
            return TransferPQ(c, hdrBrightness == 1 ? 50 : hdrBrightness == 2 ? 30 : 100);
        };
        HRESULT hr = S_OK;
        for (size_t pos = 0; pos <= text.size();) {
            const auto end = text.find(L'\n', pos);
            const auto line = text.substr(pos, end == std::wstring::npos ? end : end - pos);
            if (line.find(L"-- Source") == 0) valueColor = D3DCOLOR_XRGB(128, 255, 128);
            else if (line.find(L"-- Pipeline") == 0) valueColor = D3DCOLOR_XRGB(128, 224, 255);
            else if (line.find(L"-- Timing") == 0) valueColor = D3DCOLOR_XRGB(255, 224, 128);
            else if (line.find(L"-- Audio") == 0) valueColor = D3DCOLOR_XRGB(224, 176, 255);
            else if (line.find(L"-- Hardware") == 0) valueColor = D3DCOLOR_XRGB(255, 192, 128);
            const auto colon = line.find(L':');
            const size_t split = colon != std::wstring::npos && colon < 24 ? colon + 1
                : !line.empty() && line.front() == L' ' ? 0 : line.size();
            const auto label = line.substr(0, split);
            SIZE extent = {};
            if (FAILED(hr = font.GetTextExtent(label.c_str(), &extent))) return hr;
            if (FAILED(hr = font.Draw2DText(target, size, static_cast<float>(origin.x), y, white, label.c_str()))) return hr;
            if (split < line.size()) {
                const auto value = line.substr(split);
                if (FAILED(hr = font.Draw2DText(target, size, static_cast<float>(origin.x + extent.cx), y, color(valueColor), value.c_str()))) return hr;
            }
            y += font.GetMaxCharMetric().cy;
            if (end == std::wstring::npos) break;
            pos = end + 1;
        }
        return hr;
    }
};
