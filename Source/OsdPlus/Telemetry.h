// Background-only OSD sampling. Kept outside upstream's rendering implementation.
#pragma once
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <atomic>
#include <cwctype>
#include <cmath>
#pragma comment(lib, "pdh.lib")

// MPC-HC/MPC-BE splitter output pin statistics (include/IBitRateInfo.h).
interface __declspec(uuid("EB2CD9E6-BA08-4acb-AA0F-3D8D0DD521CA")) IOsdBitRateInfo : public IUnknown {
    STDMETHOD_(DWORD, GetCurrentBitRate)() PURE;
    STDMETHOD_(DWORD, GetAverageBitRate)() PURE;
};

#include "AudioInfo.h"

class COsdTelemetry {
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    std::wstring m_text;
    std::atomic<ULONGLONG> m_lastDraw = 0;
    std::atomic<unsigned> m_generation = 0;
    CComPtr<IPin> m_pendingSource;
    LUID m_pendingLuid = {};
    unsigned m_pendingGeneration = 0;
    bool m_pending = false;
    unsigned m_textGeneration = 0;
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    };
    struct QueryScope {
        PDH_HQUERY handle = nullptr;
        ~QueryScope() { if (handle) PdhCloseQuery(handle); }
    };
    static ULONGLONG Ticks(FILETIME t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

    // Sum processes on each physical engine, then take the busiest engine.
    // Adding independent engines would incorrectly exceed Task Manager's GPU usage.
    static double EngineUsage(PDH_HCOUNTER counter, const std::wstring& luid, bool decode, std::wstring& diagnostic) {
        DWORD bytes = 0, count = 0;
        if (!counter) { diagnostic = L"GPU counter unavailable"; return -1; }
        PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (status != PDH_MORE_DATA || !bytes || bytes > 16 * 1024 * 1024) {
            diagnostic = std::format(L"GPU probe 0x{:08X}", static_cast<UINT>(status)); return -1;
        }
        std::vector<BYTE> buffer;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            buffer.resize(bytes);
            status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count,
                reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
            if (status != PDH_MORE_DATA || bytes > 16 * 1024 * 1024) break;
        }
        if (status != ERROR_SUCCESS) {
            diagnostic = std::format(L"GPU array 0x{:08X}", static_cast<UINT>(status)); return -1;
        }
        auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        std::map<std::wstring, double> engines;
        for (DWORD i = 0; i < count; ++i) {
            if (!items[i].szName) continue;
            std::wstring name(items[i].szName);
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (name.find(luid + L"_") == std::wstring::npos || (decode && name.find(L"engtype_videodecode") == std::wstring::npos)) continue;
            const auto status = items[i].FmtValue.CStatus;
            if (status != PDH_CSTATUS_VALID_DATA && status != PDH_CSTATUS_NEW_DATA) continue;
            if (!std::isfinite(items[i].FmtValue.doubleValue) || items[i].FmtValue.doubleValue < 0) continue;
            const auto engine = name.find(L"_phys_");
            if (engine != std::wstring::npos) engines[name.substr(engine)] += items[i].FmtValue.doubleValue;
        }
        double peak = -1;
        for (const auto& entry : engines) peak = std::max(peak, entry.second);
        if (peak < 0 && !decode) diagnostic = L"GPU no valid engine for " + luid;
        return peak < 0 ? -1 : std::clamp(peak, 0.0, 100.0);
    }
    static std::wstring Percent(double x) { return x < 0 ? L"N/A" : std::format(L"{:.1f}%", x); }
    static std::wstring Rate(DWORD x) { return x ? std::format(L"{:.2f} Mb/s", x / 1000000.0) : L"N/A"; }

    void Run(LUID luid, CComPtr<IPin>& sourcePin, unsigned generation) {
        ComScope com;
        CComPtr<IOsdBitRateInfo> bitrate;
        std::wstring codec = L"N/A";
        DWORD declaredRate = 0;
        std::wstring sourceStatus;
        double fileAverage = 0;
        COsdAudioInfo audio;
        const auto readSource = [&](IPin* source) {
            bitrate.Release(); codec = L"N/A"; declaredRate = 0;
            sourceStatus = source ? L"decoder input not exposed" : L"no upstream video pin";
            fileAverage = 0; audio.Reset();
            CComPtr<IPin> output = source;
            // Native in-process DirectShow filters are free-threaded. Retain their
            // pin directly, as the streaming path does, instead of requiring an
            // IPin COM proxy/stub that a player's built-in filters may not provide.
            if (output) {
                PIN_INFO info = {};
                if (SUCCEEDED(output->QueryPinInfo(&info)) && info.pFilter) {
                    CComPtr<IBaseFilter> filter; filter.Attach(info.pFilter);
                    FILTER_INFO filterInfo = {};
                    if (SUCCEEDED(filter->QueryFilterInfo(&filterInfo)) && filterInfo.pGraph) {
                        CComPtr<IFilterGraph> graph; graph.Attach(filterInfo.pGraph);
                        audio.Find(graph);
                        CComPtr<IMediaSeeking> seeking;
                        graph->QueryInterface(IID_PPV_ARGS(&seeking));
                        REFERENCE_TIME duration = 0;
                        GUID timeFormat = {};
                        if (seeking && SUCCEEDED(seeking->GetTimeFormat(&timeFormat)) && timeFormat == TIME_FORMAT_MEDIA_TIME) seeking->GetDuration(&duration);
                        CComPtr<IEnumFilters> filters;
                        if (SUCCEEDED(graph->EnumFilters(&filters))) {
                            CComPtr<IBaseFilter> candidate;
                            while (filters->Next(1, &candidate, nullptr) == S_OK) {
                                CComPtr<IFileSourceFilter> fileSource;
                                candidate->QueryInterface(IID_PPV_ARGS(&fileSource));
                                if (fileSource) {
                                    LPOLESTR path = nullptr;
                                    if (SUCCEEDED(fileSource->GetCurFile(&path, nullptr)) && path) {
                                        std::wstring name(path); CoTaskMemFree(path);
                                        // Metadata only: no media scan. File average includes audio/container.
                                        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
                                        const bool localDrive = name.size() > 2 && name[1] == L':'
                                            && (GetDriveTypeW(name.substr(0, 3).c_str()) == DRIVE_FIXED
                                                || GetDriveTypeW(name.substr(0, 3).c_str()) == DRIVE_RAMDISK);
                                        if (duration > 0 && name.find(L"://") == std::wstring::npos
                                                && name.rfind(L"\\\\", 0) != 0
                                                && localDrive
                                                && GetFileAttributesExW(name.c_str(), GetFileExInfoStandard, &attributes)) {
                                            const auto bytes = (ULONGLONG(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
                                            fileAverage = double(bytes) * 8.0 * 10000000.0 / duration;
                                        }
                                        break;
                                    }
                                }
                                candidate.Release();
                            }
                        }
                    }
                }
            }
            // Skip raw-video processing filters between the decoder and renderer.
            // Traversal is bounded and happens only on this background thread.
            for (unsigned depth = 0; output && depth < 16; ++depth) {
                PIN_INFO info = {};
                const auto pinInfoHr = output->QueryPinInfo(&info);
                if (FAILED(pinInfoHr) || !info.pFilter) {
                    sourceStatus = std::format(L"pin info unavailable (0x{:08X})", static_cast<UINT>(pinInfoHr));
                    break;
                }
                CComPtr<IBaseFilter> filter;
                filter.Attach(info.pFilter);
                CComPtr<IEnumPins> pins;
                const auto enumHr = filter->EnumPins(&pins);
                if (FAILED(enumHr)) {
                    sourceStatus = std::format(L"decoder pins unavailable (0x{:08X})", static_cast<UINT>(enumHr));
                    break;
                }
                CComPtr<IPin> pin, next;
                bool encoded = false;
                while (pins->Next(1, &pin, nullptr) == S_OK) {
                    PIN_DIRECTION direction;
                    CMediaType mt;
                    if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_INPUT
                            && SUCCEEDED(pin->ConnectionMediaType(&mt)) && mt.majortype == MEDIATYPE_Video
                            && SUCCEEDED(pin->ConnectedTo(&next))) {
                        std::wstring tag;
                        for (unsigned j = 0; j < 4; ++j) {
                            const WCHAR c = (mt.subtype.Data1 >> (8 * j)) & 255;
                            if (c < 32 || c > 126) { tag.clear(); break; }
                            tag += c;
                        }
                        if (mt.subtype == MEDIASUBTYPE_MPEG2_VIDEO) tag = L"MPEG-2";
                        else if (mt.subtype == MEDIASUBTYPE_MPEG1Payload || mt.subtype == MEDIASUBTYPE_MPEG1Video) tag = L"MPEG-1";
                        static constexpr const wchar_t* raw[] = {
                            L"NV12", L"YV12", L"I420", L"IYUV", L"YUY2", L"UYVY", L"YVYU",
                            L"P010", L"P016", L"P210", L"P216", L"Y210", L"Y216", L"Y410", L"Y416",
                            L"AYUV", L"v210", L"v410", L"RGB3", L"RGB4", L"BGR3", L"BGR4"
                        };
                        encoded = !tag.empty() && std::none_of(std::begin(raw), std::end(raw), [&](const auto* item) { return tag == item; });
                        if (encoded) {
                            codec = tag;
                            if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                                declaredRate = reinterpret_cast<VIDEOINFOHEADER*>(mt.pbFormat)->dwBitRate;
                            } else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
                                declaredRate = reinterpret_cast<VIDEOINFOHEADER2*>(mt.pbFormat)->dwBitRate;
                            } else if (mt.formattype == FORMAT_MPEG2Video && mt.cbFormat >= sizeof(MPEG2VIDEOINFO)) {
                                declaredRate = reinterpret_cast<MPEG2VIDEOINFO*>(mt.pbFormat)->hdr.dwBitRate;
                            }
                            const auto rateHr = next->QueryInterface(IID_PPV_ARGS(&bitrate));
                            sourceStatus = bitrate ? L"" : std::format(L"splitter live bitrate unavailable (0x{:08X})", static_cast<UINT>(rateHr));
                        }
                        break;
                    }
                    pin.Release();
                }
                if (encoded) break;
                output = next;
            }
        };
        readSource(sourcePin);
        sourcePin.Release();
        CComPtr<IDXGIFactory4> factory;
        CComPtr<IDXGIAdapter3> adapter;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
        std::wstring key = std::format(L"luid_0x{:08x}_0x{:08x}", static_cast<UINT>(luid.HighPart), luid.LowPart);
        QueryScope queryScope;
        auto& query = queryScope.handle;
        PDH_HCOUNTER gpu = nullptr;
        PDH_HCOUNTER utility = nullptr;
        std::wstring gpuStatus;
        ULONGLONG retryAt = 0;
        const auto openCounters = [&] {
            if (query) PdhCloseQuery(query);
            query = nullptr; gpu = nullptr; utility = nullptr;
            auto status = PdhOpenQueryW(nullptr, 0, &query);
            if (status == ERROR_SUCCESS) {
                status = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu);
                PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Utility", 0, &utility);
            }
            gpuStatus = status == ERROR_SUCCESS ? L"" : std::format(L"GPU init 0x{:08X}", static_cast<UINT>(status));
            if (query) PdhCollectQueryData(query); // Baseline only; never publish this sample.
            retryAt = GetTickCount64() + 15000;
        };
        // Delay PDH setup until the OSD is actually visible.
        FILETIME idle = {}, kernel = {}, user = {}, creation, exit, pk = {}, pu = {};
        GetSystemTimes(&idle, &kernel, &user);
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &pk, &pu);
        ULONGLONG prevIdle = Ticks(idle), prevTotal = Ticks(kernel) + Ticks(user), prevProcess = Ticks(pk) + Ticks(pu);
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_wake.wait_for(lock, std::chrono::seconds(1), [&] { return m_stop; })) break;
            }
            CComPtr<IPin> pendingSource;
            LUID pendingLuid = {};
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_pending) {
                    pendingSource.Attach(m_pendingSource.Detach());
                    pendingLuid = m_pendingLuid; generation = m_pendingGeneration;
                    pending = true; m_pending = false;
                }
            }
            if (generation != m_generation.load()) {
                bitrate.Release(); audio.Reset(); codec = L"N/A"; declaredRate = 0;
                continue; // A new source gets its own three-second quiet period.
            }
            if (pending) {
                readSource(pendingSource);
                pendingSource.Release();
                if (pendingLuid.HighPart != luid.HighPart || pendingLuid.LowPart != luid.LowPart) {
                    luid = pendingLuid;
                    key = std::format(L"luid_0x{:08x}_0x{:08x}", static_cast<UINT>(luid.HighPart), luid.LowPart);
                    adapter.Release();
                    if (factory) factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
                }
            }
            // Hide the OSD: suspend expensive driver queries, but keep the worker
            // and its counters ready. Resume with a fresh CPU baseline below.
            if (GetTickCount64() - m_lastDraw.load(std::memory_order_relaxed) > 2000) {
                GetSystemTimes(&idle, &kernel, &user);
                GetProcessTimes(GetCurrentProcess(), &creation, &exit, &pk, &pu);
                prevIdle = Ticks(idle); prevTotal = Ticks(kernel) + Ticks(user); prevProcess = Ticks(pk) + Ticks(pu);
                continue;
            }
            double cpu = -1, process = -1;
            if (GetSystemTimes(&idle, &kernel, &user) && GetProcessTimes(GetCurrentProcess(), &creation, &exit, &pk, &pu)) {
                const auto total = Ticks(kernel) + Ticks(user), busy = Ticks(pk) + Ticks(pu);
                const auto delta = total - prevTotal, idleDelta = Ticks(idle) - prevIdle;
                if (delta && idleDelta <= delta) {
                    cpu = 100.0 * (delta - idleDelta) / delta;
                    process = std::clamp(100.0 * (busy - prevProcess) / delta, 0.0, 100.0);
                }
                prevIdle = Ticks(idle); prevTotal = total; prevProcess = busy;
            }
            double usage = -1, decode = -1;
            bool hasUtility = false;
            const auto collect = query ? PdhCollectQueryData(query) : PDH_INVALID_HANDLE;
            if (collect == ERROR_SUCCESS) {
                gpuStatus.clear();
                usage = EngineUsage(gpu, key, false, gpuStatus);
                std::wstring decodeStatus;
                decode = EngineUsage(gpu, key, true, decodeStatus);
                PDH_FMT_COUNTERVALUE value = {};
                if (utility && PdhGetFormattedCounterValue(utility, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &value) == ERROR_SUCCESS
                    && (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA)
                    && std::isfinite(value.doubleValue) && value.doubleValue >= 0) {
                    cpu = std::clamp(value.doubleValue, 0.0, 100.0); hasUtility = true;
                }
            } else {
                gpuStatus = std::format(L"GPU collect 0x{:08X}", static_cast<UINT>(collect));
            }
            if (usage < 0 && GetTickCount64() >= retryAt) {
                const auto failure = gpuStatus;
                openCounters();
                if (gpuStatus.empty()) gpuStatus = failure + L" (query restarted; warming up)";
            }
            DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
            const bool hasMemory = adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
            DWORD average = bitrate ? bitrate->GetAverageBitRate() : 0;
            const DWORD current = bitrate ? bitrate->GetCurrentBitRate() : 0;
            if (!average) average = declaredRate;
            std::wstring text = std::format(L"\nCodec         : {}\nVideo bitrate : avg {}, current {}\n-- Hardware --------------------------------------------------------------------\nCPU           : {} total, {} player\nGPU           : {} busiest engine, decode {}\nVRAM (player) : {}",
                codec, Rate(average), Rate(current), Percent(cpu), Percent(process), Percent(usage), Percent(decode),
                hasMemory ? std::format(L"{:.0f} MiB", memory.CurrentUsage / 1048576.0) : L"N/A");
            if (!gpuStatus.empty()) text += L"\nMonitor status: " + gpuStatus;
            if (!hasUtility) text += L"\nCPU metric    : busy time (Utility unavailable)";
            if (fileAverage > 0 || !sourceStatus.empty()) {
                std::wstring sourceText;
                if (fileAverage > 0) sourceText += std::format(L"\nFile avg      : {:.2f} Mb/s (all streams + container)", fileAverage / 1000000.0);
                if (!sourceStatus.empty()) sourceText += L"\nSource info   : " + sourceStatus;
                const auto hardware = text.find(L"\n-- Hardware");
                text.insert(hardware, sourceText);
            }
            text.insert(text.find(L"\n-- Hardware"), audio.Text());
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation == m_generation.load()) {
                m_text = std::move(text); m_textGeneration = generation;
            }
        }
        bitrate.Release(); adapter.Release(); factory.Release();
    }
public:
    ~COsdTelemetry() { Stop(); }
    void Stop() {
        { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; }
        m_wake.notify_all();
        if (m_worker.joinable()) m_worker.join();
        m_pendingSource.Release();
    }
    void Invalidate() { m_generation.fetch_add(1); }
    bool Started() const { return m_worker.joinable(); }
    void Start(LUID luid, IPin* output) {
        CComPtr<IPin> source = output;
        if (Started()) {
            CComPtr<IPin> old;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                old.Attach(m_pendingSource.Detach());
                m_pendingSource.Attach(source.Detach()); m_pendingLuid = luid;
                m_pendingGeneration = m_generation.load(); m_pending = true;
            }
            return;
        }
        const unsigned generation = m_generation.load();
        try { m_worker = std::thread([this, luid, source, generation]() mutable {
            // Monitoring is optional: a sampling failure must not terminate the player.
            try { Run(luid, source, generation); }
            catch (...) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_text = L"\nHardware      : N/A (monitor unavailable)";
                m_textGeneration = generation;
            }
        }); }
        catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_text = L"\nSource info   : monitor thread unavailable";
            m_textGeneration = generation;
        }
    }
    std::wstring Text() {
        m_lastDraw.store(GetTickCount64(), std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
        return lock.owns_lock() && m_textGeneration == m_generation.load() ? m_text : std::wstring();
    }
};

