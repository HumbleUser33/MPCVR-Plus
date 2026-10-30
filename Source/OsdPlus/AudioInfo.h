// Audio connection formats, read only on the telemetry worker.
#pragma once
#include <mmreg.h>
#include "AudioSelection.h"
#include "AudioFormats.h"

// MPC-BE AudioSwitcher/StreamSwitcher.h: read-only active-input identity.
interface __declspec(uuid("DA395FA3-4A3E-4D85-805E-0BEFF53D4BCD")) IOsdStreamSwitcherInput : public IUnknown {
    STDMETHOD_(bool, IsActive)() PURE;
};

class COsdAudioInfo {
    CComPtr<IPin> m_rendererInput;
    static bool HasMpcBeProfile(IPin* pin) {
        PIN_INFO info = {};
        if (!pin || FAILED(pin->QueryPinInfo(&info)) || !info.pFilter) return false;
        CLSID clsid = {};
        const HRESULT hr = info.pFilter->GetClassID(&clsid);
        info.pFilter->Release();
        return SUCCEEDED(hr) && OsdAudio::MpcBeProfileSource(OsdAudio::FromGuid(clsid));
    }
    static std::wstring Format(const CMediaType& mt, DWORD current = 0, bool mpcBeProfile = false) {
        const auto info = OsdAudio::Parse(OsdAudio::FromGuid(mt.subtype), OsdAudio::FromGuid(mt.formattype),
            mt.pbFormat, mt.cbFormat, current, mpcBeProfile);
        std::wstring codec(info.Encoding.Name);
        if (codec.empty()) {
            codec = std::format(L"Audio 0x{:X}", info.Tag);
            if (!(info.Identity == OsdAudio::Guid{})) {
                const auto& id = info.Identity;
                codec += std::format(L" [{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}]",
                    id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
                    id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
            }
        }
        return std::format(L"{}, {}, {}, {}, {}", codec,
            info.Rate ? std::format(L"{} Hz", info.Rate) : L"sample rate N/A",
            info.Channels ? std::format(L"{} ch", info.Channels) : L"channels N/A",
            info.Bits ? std::format(L"{}-bit", info.Bits) : L"depth N/A",
            info.BitRate ? std::format(L"{:.0f} kb/s", info.BitRate / 1000.0) : L"rate N/A");
    }
    static bool IsPcm(const CMediaType& mt) {
        return OsdAudio::Parse(OsdAudio::FromGuid(mt.subtype), OsdAudio::FromGuid(mt.formattype),
            mt.pbFormat, mt.cbFormat).LinearPcm;
    }
public:
    void Reset() { m_rendererInput.Release(); }
    void Find(IFilterGraph* graph) {
        Reset();
        CComPtr<IEnumFilters> filters;
        if (!graph || FAILED(graph->EnumFilters(&filters))) return;
        CComPtr<IBaseFilter> filter;
        while (filters->Next(1, &filter, nullptr) == S_OK) {
            CComPtr<IEnumPins> pins;
            if (SUCCEEDED(filter->EnumPins(&pins))) {
                CComPtr<IPin> pin, audio;
                bool downstream = false;
                while (pins->Next(1, &pin, nullptr) == S_OK) {
                    PIN_DIRECTION direction;
                    CMediaType mt;
                    if (SUCCEEDED(pin->QueryDirection(&direction)) && SUCCEEDED(pin->ConnectionMediaType(&mt)) && mt.majortype == MEDIATYPE_Audio) {
                        if (direction == PINDIR_INPUT) audio = pin;
                        else downstream = true;
                    }
                    pin.Release();
                }
                if (audio && !downstream) { m_rendererInput = audio; return; }
            }
            filter.Release();
        }
    }
    std::wstring Text() {
        std::wstring input = L"N/A", decoded = L"N/A", render = L"N/A";
        CMediaType mt;
        if (m_rendererInput && SUCCEEDED(m_rendererInput->ConnectionMediaType(&mt))) render = Format(mt);
        CComPtr<IPin> output;
        if (m_rendererInput) m_rendererInput->ConnectedTo(&output);
        // Follow only connected audio inputs, starting at the actual renderer.
        for (unsigned depth = 0; output && depth < 16; ++depth) {
            CMediaType outType;
            output->ConnectionMediaType(&outType);
            PIN_INFO info = {};
            if (FAILED(output->QueryPinInfo(&info)) || !info.pFilter) break;
            CComPtr<IBaseFilter> filter; filter.Attach(info.pFilter);
            CComPtr<IEnumPins> pins;
            if (FAILED(filter->EnumPins(&pins))) break;
            CComPtr<IPin> pin, upstream;
            CMediaType inType;
            std::vector<CComPtr<IPin>> upstreamPins;
            std::vector<CMediaType> inputTypes;
            std::vector<int> states;
            while (pins->Next(1, &pin, nullptr) == S_OK) {
                PIN_DIRECTION direction;
                CMediaType candidate;
                if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_INPUT
                    && SUCCEEDED(pin->ConnectionMediaType(&candidate)) && candidate.majortype == MEDIATYPE_Audio) {
                    CComPtr<IPin> connected;
                    if (SUCCEEDED(pin->ConnectedTo(&connected))) {
                        CComPtr<IOsdStreamSwitcherInput> selection;
                        pin->QueryInterface(IID_PPV_ARGS(&selection));
                        states.push_back(selection ? (selection->IsActive() ? 1 : 0) : -1);
                        upstreamPins.push_back(connected);
                        inputTypes.push_back(candidate);
                    }
                }
                pin.Release();
            }
            if (states.empty()) { if (IsPcm(outType)) input = decoded = Format(outType); break; }
            const int active = SelectOsdAudioInput(states);
            if (active < 0) break;
            upstream = upstreamPins[active];
            inType = inputTypes[active];
            if (!IsPcm(inType)) {
                DWORD rate = 0;
                CComPtr<IOsdBitRateInfo> bitrate;
                if (upstream) upstream->QueryInterface(IID_PPV_ARGS(&bitrate));
                if (bitrate) rate = bitrate->GetCurrentBitRate();
                input = Format(inType, rate, HasMpcBeProfile(upstream));
                decoded = Format(outType);
                break;
            }
            output = upstream;
        }
        return L"\n-- Audio -----------------------------------------------------------------------\nAudio input   : " + input
            + L"\nAudio decoded : " + decoded + L"\nAudio render  : " + render;
    }
};

