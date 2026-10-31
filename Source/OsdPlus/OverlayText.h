// The statistics' text drawn in one call a colour. Apart from Overlay.h so that
// tools\dlssnr_probe\osd_draw_test compiles this very code against the way it was drawn
// before, and requires the same pixels.
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "../DX11Helper.h"
#include "../D3DUtil/D3D11Font.h"

class COsdPlusText {
    // The value colour of each section, Source's until a heading says otherwise.
    static constexpr struct { const wchar_t* heading; D3DCOLOR value; } kSections[] = {
        { L"-- Source",   D3DCOLOR_XRGB(128, 255, 128) },
        { L"-- Pipeline", D3DCOLOR_XRGB(128, 224, 255) },
        { L"-- Timing",   D3DCOLOR_XRGB(255, 224, 128) },
        { L"-- Audio",    D3DCOLOR_XRGB(224, 176, 255) },
        { L"-- Hardware", D3DCOLOR_XRGB(255, 192, 128) },
    };
    static constexpr size_t kCount = std::size(kSections);

    // Kept from one picture to the next for their memory.
    std::wstring m_labels, m_values[kCount];
    struct Apart { float y; size_t section; size_t labelPos, labelLen, valuePos, valueLen; };
    std::vector<Apart> m_apart;
    int m_calls = 0;

    static bool OneCellEach(std::wstring_view s) {
        for (const wchar_t c : s) {
            if (c < 0x20 || c > 0x7E) return false;
        }
        return true;
    }

public:
    // How many Draw2DText calls the last Draw made.
    int Calls() const { return m_calls; }

    // A line is its label in white, up to the colon, then its value in the colour of its
    // section. Consolas gives every printable ASCII character the advance of a space, and
    // a space advances without drawing: so the labels go out in one call, then each
    // section's values in one call, with spaces where the labels and the other sections'
    // values are. Two calls a line, as it was, made 64 to 70 a picture in the bench, each
    // setting the whole pipeline again, changing the colour and mapping the vertex buffer.
    // A label with any other character keeps a call of its own for its value, placed
    // after the label's measured width as before.
    HRESULT Draw(CD3D11Font& font, ID3D11RenderTargetView* target, const SIZE& size,
                 POINT origin, const std::wstring& text, D3DCOLOR white, int hdrBrightness) {
        const auto color = [&](D3DCOLOR c) {
            if (white == D3DCOLOR_XRGB(255, 255, 255)) return c;
            return TransferPQ(c, hdrBrightness == 1 ? 50.0f : hdrBrightness == 2 ? 30.0f : 100.0f);
        };
        m_labels.clear();
        for (auto& v : m_values) v.clear();
        m_apart.clear();
        m_calls = 0;
        bool used[kCount] = {};
        const float lineHeight = static_cast<float>(font.GetMaxCharMetric().cy);
        float y = static_cast<float>(origin.y);
        size_t section = 0;
        for (size_t pos = 0; pos <= text.size();) {
            const auto end = text.find(L'\n', pos);
            const auto line = std::wstring_view(text).substr(pos, end == std::wstring::npos ? end : end - pos);
            for (size_t i = 0; i < kCount; i++) {
                if (line.starts_with(kSections[i].heading)) section = i;
            }
            const auto colon = line.find(L':');
            const size_t split = colon != std::wstring_view::npos && colon < 24 ? colon + 1
                : !line.empty() && line.front() == L' ' ? 0 : line.size();
            m_labels.append(line.substr(0, split));
            if (split < line.size()) {
                if (OneCellEach(line.substr(0, split))) {
                    m_values[section].append(split, L' ');
                    m_values[section].append(line.substr(split));
                    used[section] = true;
                } else {
                    m_apart.push_back({ y, section, pos, split, pos + split, line.size() - split });
                }
            }
            if (end == std::wstring::npos) break;
            m_labels += L'\n';
            for (auto& v : m_values) v += L'\n';
            y += lineHeight;
            pos = end + 1;
        }
        const float x = static_cast<float>(origin.x), top = static_cast<float>(origin.y);
        HRESULT hr = font.Draw2DText(target, size, x, top, white, m_labels.c_str());
        m_calls++;
        for (size_t i = 0; i < kCount && SUCCEEDED(hr); i++) {
            if (used[i]) {
                hr = font.Draw2DText(target, size, x, top, color(kSections[i].value), m_values[i].c_str());
                m_calls++;
            }
        }
        for (const auto& a : m_apart) {
            if (FAILED(hr)) break;
            const std::wstring label = text.substr(a.labelPos, a.labelLen), value = text.substr(a.valuePos, a.valueLen);
            SIZE extent = {};
            if (FAILED(hr = font.GetTextExtent(label.c_str(), &extent))) break;
            hr = font.Draw2DText(target, size, static_cast<float>(origin.x + extent.cx), a.y, color(kSections[a.section].value), value.c_str());
            m_calls++;
        }
        return hr;
    }
};
