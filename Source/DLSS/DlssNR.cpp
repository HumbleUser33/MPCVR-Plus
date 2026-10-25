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
 */

#include "stdafx.h"
#include <intrin.h>
#include <dxgi1_4.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <cstdarg>
#include <format>
#include "../Utils/Util.h"
#include "../../external/minhook/include/MinHook.h"
#include "NgxRuntime.h"
#include "DlssNR.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "d3d12.lib")

static const wchar_t s_SnippetName[] = L"nvngx_dlssnr.dll";
static const wchar_t s_ShimName[]    = L"nvngx.dll";

// For a test of what the renderer does when an evaluation fails, which nothing else
// provokes on demand: with MPCVR_DLSSNR_FAIL_AT=n in the environment every n-th
// Evaluate of the process gives up as if the GPU had not answered. Unset, it costs a
// comparison.
static const int s_FailAt = []() {
	wchar_t value[16] = {};
	const DWORD length = GetEnvironmentVariableW(L"MPCVR_DLSSNR_FAIL_AT", value, (DWORD)std::size(value));
	return (length > 0 && length < std::size(value)) ? _wtoi(value) : 0;
}();
static int s_Evaluations = 0;

// The snippet's parameter keys. UI, UIAlpha and BidirectionalDistortionField are
// never set. MVec, Depth and ControlMask are optional guides (SetGuides): without
// them the DLL logs "EvaluateFeature Color=... MVec=0000000000000000
// Depth=0000000000000000" and runs on colour alone.
#define P_WIDTH        "DLSSNR.Width"
#define P_HEIGHT       "DLSSNR.Height"
#define P_ENABLED      "DLSSNR.Enabled"
#define P_RESET        "DLSSNR.Reset"
#define P_STYLE        "DLSSNR.Style"
#define P_PRESET       "DLSSNR.Hint.Render.Preset"
#define P_INTENSITY    "DLSSNR.Intensity"
#define P_LOCALTONE    "DLSSNR.LocalToneStrength"
#define P_LOCALSTRUCT  "DLSSNR.LocalStructureStrength"
#define P_SKINSTRUCT   "DLSSNR.SkinStructureStrength"
#define P_AUTOMASK     "DLSSNR.UseAutoMask"
#define P_UICORRECTION "DLSSNR.UICorrection"
#define P_DEPTHINV     "DLSSNR.DepthInverted"
// The snippet names a network size apart from the output and computes a ratio per
// quality mode, but 310.8 answers 1 for every mode, and 0.75 or 0.5 set here run
// in the same time to the same bit (tools/dlssnr_probe, measured 2026-09-16).
#define P_SCALINGRATIO "DLSSNR.ScalingRatio"
#define P_MVECSCALEX   "DLSSNR.MVecScaleX"
#define P_MVECSCALEY   "DLSSNR.MVecScaleY"
#define P_COLOR        "DLSSNR.Color"
#define P_OUTPUT       "DLSSNR.Output"
#define P_BACKBUFFER   "DLSSNR.Backbuffer"
#define P_MVEC         "DLSSNR.MVec"
#define P_DEPTH        "DLSSNR.Depth"
#define P_CONTROLMASK  "DLSSNR.ControlMask"

// Every texture comes with the part of it that is meant, four keys under its name.
// They cover the whole surface: the snippet rejects any mismatch with the feature's
// dimensions ("Invalid Color/Output rect configuration").
struct SubrectKeys { const char* baseX; const char* baseY; const char* width; const char* height; };
#define SUBRECT_KEYS(name) { "DLSSNR." name "SubrectBaseX", "DLSSNR." name "SubrectBaseY", \
                             "DLSSNR." name "SubrectWidth", "DLSSNR." name "SubrectHeight" }
static const SubrectKeys s_RectColor       = SUBRECT_KEYS("Color");
static const SubrectKeys s_RectOutput      = SUBRECT_KEYS("Output");
static const SubrectKeys s_RectBackbuffer  = SUBRECT_KEYS("Backbuffer");
static const SubrectKeys s_RectMVec        = SUBRECT_KEYS("MVec");
static const SubrectKeys s_RectDepth       = SUBRECT_KEYS("Depth");
static const SubrectKeys s_RectControlMask = SUBRECT_KEYS("ControlMask");
#undef SUBRECT_KEYS

static void SetWholeSubrect(NVSDK_NGX_Parameter* p, const SubrectKeys& keys, UINT w, UINT h)
{
	p->Set(keys.baseX,  (unsigned int)0);
	p->Set(keys.baseY,  (unsigned int)0);
	p->Set(keys.width,  (unsigned int)w);
	p->Set(keys.height, (unsigned int)h);
}

const wchar_t* NgxResultName(NVSDK_NGX_Result r)
{
	switch (r) {
	case NGX_Result_Success:                        return L"Success";
	case NGX_Result_Fail:                           return L"Fail";
	case NGX_Result_FAIL_FeatureNotSupported:       return L"FeatureNotSupported";
	case NGX_Result_FAIL_PlatformError:             return L"PlatformError";
	case NGX_Result_FAIL_FeatureAlreadyExists:      return L"FeatureAlreadyExists";
	case NGX_Result_FAIL_FeatureNotFound:           return L"FeatureNotFound";
	case NGX_Result_FAIL_InvalidParameter:          return L"InvalidParameter";
	case NGX_Result_FAIL_ScratchBufferTooSmall:     return L"ScratchBufferTooSmall";
	case NGX_Result_FAIL_NotInitialized:            return L"NotInitialized";
	case NGX_Result_FAIL_UnsupportedInputFormat:    return L"UnsupportedInputFormat";
	case NGX_Result_FAIL_RWFlagMissing:             return L"RWFlagMissing";
	case NGX_Result_FAIL_MissingInput:              return L"MissingInput";
	case NGX_Result_FAIL_UnableToInitializeFeature: return L"UnableToInitializeFeature";
	case NGX_Result_FAIL_OutOfDate:                 return L"OutOfDate";
	case NGX_Result_FAIL_OutOfGPUMemory:            return L"OutOfGPUMemory";
	case NGX_Result_FAIL_UnsupportedFormat:         return L"UnsupportedFormat";
	case NGX_Result_FAIL_UnableToWriteToAppDataPath:return L"UnableToWriteToAppDataPath";
	case NGX_Result_FAIL_UnsupportedParameter:      return L"UnsupportedParameter";
	case NGX_Result_FAIL_Denied:                    return L"Denied";
	case NGX_Result_FAIL_NotImplemented:            return L"NotImplemented";
	}
	return L"Unknown";
}

// ============================================================================
// CNgxParameterStore
// ============================================================================

CNgxParameterStore::Slot& CNgxParameterStore::Put(const char* n)
{
	return m_map[n ? n : ""];
}

const CNgxParameterStore::Slot* CNgxParameterStore::Find(const char* n) const
{
	auto it = m_map.find(n ? n : "");
	return (it == m_map.end()) ? nullptr : &it->second;
}

void CNgxParameterStore::Set(const char* n, unsigned long long v) { auto& s = Put(n); s.kind = Kind::U64;   s.u64 = v; s.f64 = (double)v; }
void CNgxParameterStore::Set(const char* n, float v)              { auto& s = Put(n); s.kind = Kind::F32;   s.f64 = v; s.u64 = (uint64_t)v; }
void CNgxParameterStore::Set(const char* n, double v)             { auto& s = Put(n); s.kind = Kind::F64;   s.f64 = v; s.u64 = (uint64_t)v; }
void CNgxParameterStore::Set(const char* n, unsigned int v)       { auto& s = Put(n); s.kind = Kind::U32;   s.u64 = v; s.f64 = (double)v; }
void CNgxParameterStore::Set(const char* n, int v)                { auto& s = Put(n); s.kind = Kind::I32;   s.u64 = (uint64_t)(int64_t)v; s.f64 = (double)v; }
void CNgxParameterStore::Set(const char* n, ID3D11Resource* v)    { auto& s = Put(n); s.kind = Kind::Res11; s.ptr = v; }
void CNgxParameterStore::Set(const char* n, ID3D12Resource* v)    { auto& s = Put(n); s.kind = Kind::Res12; s.ptr = v; }
void CNgxParameterStore::Set(const char* n, void* v)              { auto& s = Put(n); s.kind = Kind::Ptr;   s.ptr = v; }

// Every Get null-checks its out-pointer, so a mislaid vtable slot produces a
// wrong value rather than memory corruption.
#define STORE_GET(expr)                                        \
	if (!o) return NGX_Result_FAIL_InvalidParameter;           \
	const Slot* v = Find(n);                                   \
	if (!v) return NGX_Result_FAIL_FeatureNotFound;            \
	*o = (expr);                                               \
	return NGX_Result_Success;

NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, unsigned long long* o) const { STORE_GET(v->u64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, float* o) const              { STORE_GET((float)v->f64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, double* o) const             { STORE_GET(v->f64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, unsigned int* o) const       { STORE_GET((unsigned int)v->u64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, int* o) const                { STORE_GET((int)v->u64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, ID3D11Resource** o) const    { STORE_GET((ID3D11Resource*)v->ptr) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, ID3D12Resource** o) const    { STORE_GET((ID3D12Resource*)v->ptr) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, void** o) const              { STORE_GET(v->ptr) }

#undef STORE_GET

void CNgxParameterStore::Reset() { m_map.clear(); }

// ============================================================================
// Architecture override
// ============================================================================
// NGXCubinGeneric::SetGPUArch asks NVAPI for the GPU architecture and the
// snippet refuses anything below its own minimum. Hooking the resolved
// function catches the query whenever it happens; the value reported is the
// minimum the snippet itself declares, so nothing is invented here.
//
// Scope: the hook is process-wide once installed, which is why it is only
// installed when the user turns the feature on, and left in place for the
// session -- the snippet reads the architecture during init and caches it, so
// arming it only around individual calls does not work (measured).
//
// The answer is only changed for calls made from the DLSS 5 NR snippet itself.
// Every NGX snippet asks the same question: DLSS Super Resolution told it was on
// a Blackwell GPU picks kernels an Ampere or Ada card cannot run, writes nothing
// and then removes the device (measured, tools/dlssnr_probe --tsr).

namespace {
	PFN_NvAPI_GPU_GetArchInfo g_pRealArchInfo = nullptr;
	unsigned int g_SpoofArch = 0;
	HMODULE g_hSpoofFor = nullptr;   // the module whose calls get the spoofed answer
	bool g_bArchHookInstalled = false;
	int g_SpoofUsers = 0;            // the sessions that asked for the override and are still up

	int __cdecl ArchInfoDetour(void* hGpu, NV_GPU_ARCH_INFO* info)
	{
		const int r = g_pRealArchInfo(hGpu, info);
		if (r == 0 && info && g_SpoofArch && g_hSpoofFor) {
			// MinHook jumps here from the function's entry, so the return address
			// is still the caller's.
			HMODULE hCaller = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCWSTR)_ReturnAddress(), &hCaller) && hCaller == g_hSpoofFor) {
				info->architecture = g_SpoofArch;
			}
		}
		return r;
	}
}

bool CDlssNR::InstallArchOverride()
{
	if (g_bArchHookInstalled) {
		g_SpoofArch = NV_GPU_ARCH_BLACKWELL;
		return true;
	}

	HMODULE hNvApi = LoadLibraryW(L"nvapi64.dll");
	if (!hNvApi) {
		Log(L"nvapi64.dll not found; architecture override unavailable");
		return false;
	}
	// Not named QI: the DirectShow base classes define that as a macro.
	auto pNvQuery = (PFN_nvapi_QueryInterface)GetProcAddress(hNvApi, "nvapi_QueryInterface");
	auto pArch = pNvQuery ? (PFN_NvAPI_GPU_GetArchInfo)pNvQuery(NVAPI_ID_GPU_GetArchInfo) : nullptr;
	if (!pArch) {
		Log(L"could not resolve NvAPI_GPU_GetArchInfo");
		return false;
	}

	// MinHook is already initialised by DllMain.
	const MH_STATUS mh = MH_Initialize();
	if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) {
		Log(L"MH_Initialize failed (%d)", (int)mh);
		return false;
	}
	if (MH_CreateHook(pArch, &ArchInfoDetour, (void**)&g_pRealArchInfo) != MH_OK
			|| MH_EnableHook(pArch) != MH_OK) {
		Log(L"could not hook NvAPI_GPU_GetArchInfo");
		return false;
	}

	g_bArchHookInstalled = true;
	g_SpoofArch = NV_GPU_ARCH_BLACKWELL;
	Log(L"architecture override active (reporting 0x%X)", g_SpoofArch);
	return true;
}

// ============================================================================
// CDlssNR -- housekeeping
// ============================================================================

CDlssNR::~CDlssNR()
{
	Shutdown();
}

void CDlssNR::Log(const wchar_t* fmt, ...)
{
	wchar_t buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(buf, std::size(buf), _TRUNCATE, fmt, ap);
	va_end(ap);

	DLog(L"CDlssNR: {}", buf);
	m_Log.emplace_back(buf);
	while (m_Log.size() > 32) {
		m_Log.pop_front();
	}
}

std::vector<std::wstring> CDlssNR::CandidateDllPaths(const wchar_t* pConfigured)
{
	std::vector<std::wstring> out;

	if (pConfigured && *pConfigured) {
		std::wstring c = pConfigured;
		if (PathIsDirectoryW(c.c_str())) {
			if (c.back() != L'\\' && c.back() != L'/') {
				c += L'\\';
			}
			c += s_SnippetName;
		}
		out.push_back(c);
	}

	const std::wstring dir = NgxRuntime::ModuleDir();
	out.push_back(dir + L"\\" + s_SnippetName);
	out.push_back(dir + L"\\dlss\\" + s_SnippetName);
	out.push_back(dir + L"\\..\\" + s_SnippetName);
	out.push_back(dir + L"\\..\\..\\" + s_SnippetName);   // repo root from _bin\Filter_x64
	return out;
}

// ============================================================================
// Library loading
// ============================================================================

bool CDlssNR::LoadCore()
{
	std::wstring path;
	m_hCore = NgxRuntime::Load(&path);
	if (!m_hCore) {
		Log(L"_nvngx.dll not found");
		return false;
	}
	Log(L"runtime: %s", path.c_str());

	m_pfnCoreAlloc   = (PFN_NGX_D3D11_AllocateParameters)GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_AllocateParameters");
	m_pfnCoreDestroy = (PFN_NGX_D3D11_DestroyParameters) GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_DestroyParameters");
	return true;
}

bool CDlssNR::LoadSnippet(const wchar_t* pConfiguredDllPath)
{
	for (const auto& cand : CandidateDllPaths(pConfiguredDllPath)) {
		if (!PathFileExistsW(cand.c_str())) {
			continue;
		}
		m_hSnippet = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (m_hSnippet) {
			wchar_t full[MAX_PATH] = {};
			GetModuleFileNameW(m_hSnippet, full, std::size(full));
			m_DllPath    = full[0] ? full : cand;
			m_SnippetDir = NgxRuntime::DirOf(m_DllPath);
			Log(L"snippet: %s", m_DllPath.c_str());
			return true;
		}
		m_State = State::DllLoadFailed;
		Log(L"LoadLibrary failed for %s (err %lu)", cand.c_str(), GetLastError());
		return false;
	}

	m_State = State::DllNotFound;
	Log(L"%s not found in any candidate location", s_SnippetName);
	return false;
}

bool CDlssNR::LoadShim()
{
	// Must be a module whose path contains "nvngx.dll": the snippet refuses any
	// other caller with PlatformError and logs "Not called from NGX runtime".
	const std::wstring dir = NgxRuntime::ModuleDir();
	const std::wstring cands[] = {
		dir + L"\\dlss\\" + s_ShimName,
		dir + L"\\" + s_ShimName,
	};

	for (const auto& c : cands) {
		if (!PathFileExistsW(c.c_str())) {
			continue;
		}
		m_hShim = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (m_hShim) {
			Log(L"shim: %s", c.c_str());
			break;
		}
	}

	if (!m_hShim) {
		m_State = State::ShimMissing;
		Log(L"%s not found -- the snippet rejects unknown callers", s_ShimName);
		return false;
	}

	m_pfnShimInit     = (PFN_ShimInit)     GetProcAddress(m_hShim, "shim_init");
	m_pfnShimPopulate = (PFN_ShimPopulate) GetProcAddress(m_hShim, "shim_populate");
	m_pfnShimCreate   = (PFN_ShimCreate)   GetProcAddress(m_hShim, "shim_create");
	m_pfnShimEval     = (PFN_ShimEval)     GetProcAddress(m_hShim, "shim_eval");
	m_pfnShimRelease  = (PFN_ShimRelease)  GetProcAddress(m_hShim, "shim_release");
	m_pfnShimShutdown = (PFN_ShimShutdown1)GetProcAddress(m_hShim, "shim_shutdown1");

	if (!m_pfnShimInit || !m_pfnShimPopulate || !m_pfnShimCreate || !m_pfnShimEval) {
		m_State = State::ShimMissing;
		Log(L"shim loaded but exports are missing");
		FreeLibrary(m_hShim);
		m_hShim = nullptr;
		return false;
	}

	m_bUseShim = true;
	return true;
}

// ============================================================================
// Guarded call routing
// ============================================================================

NVSDK_NGX_Result CDlssNR::CallPopulate(NVSDK_NGX_Parameter* p)
{
	if (m_bUseShim && m_pfnShimPopulate) {
		return m_pfnShimPopulate((void*)m_pfnPopulate, p);
	}
	return m_pfnPopulate(p);
}

NVSDK_NGX_Result CDlssNR::CallInit()
{
	if (m_bUseShim && m_pfnShimInit) {
		return m_pfnShimInit((void*)m_pfnInit, NGX_DLSSNR_APPID, m_DataPath.c_str(),
		                     m_pDev12, m_SdkVersion, nullptr);
	}
	return m_pfnInit(NGX_DLSSNR_APPID, m_DataPath.c_str(), m_pDev12, m_SdkVersion, nullptr);
}

NVSDK_NGX_Result CDlssNR::CallCreate(NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out)
{
	if (m_bUseShim && m_pfnShimCreate) {
		return m_pfnShimCreate((void*)m_pfnCreate, m_pList, NGX_FEATURE_DLSSNR, p, (void**)out);
	}
	return m_pfnCreate(m_pList, NGX_FEATURE_DLSSNR, p, out);
}

NVSDK_NGX_Result CDlssNR::CallEvaluate(const NVSDK_NGX_Parameter* p)
{
	if (m_bUseShim && m_pfnShimEval) {
		return m_pfnShimEval((void*)m_pfnEvaluate, m_pList, m_pFeature, p, nullptr);
	}
	return m_pfnEvaluate(m_pList, m_pFeature, p, nullptr);
}

NVSDK_NGX_Result CDlssNR::CallRelease(NVSDK_NGX_Handle* h)
{
	if (!m_pfnRelease) {
		return NGX_Result_FAIL_NotImplemented;
	}
	if (m_bUseShim && m_pfnShimRelease) {
		return m_pfnShimRelease((void*)m_pfnRelease, h);
	}
	return m_pfnRelease(h);
}

NVSDK_NGX_Result CDlssNR::CallShutdown1()
{
	if (!m_pfnShutdown1 || !m_pDev12) {
		return NGX_Result_FAIL_NotImplemented;
	}
	if (m_bUseShim && m_pfnShimShutdown) {
		return m_pfnShimShutdown((void*)m_pfnShutdown1, m_pDev12);
	}
	return m_pfnShutdown1(m_pDev12);
}

// ============================================================================
// The private D3D12 device
// ============================================================================

bool CDlssNR::CreateD3D12(ID3D11Device* pDevice)
{
	// Same physical adapter as the renderer, matched by LUID -- a shared handle
	// cannot cross adapters.
	CComPtr<IDXGIDevice> pDXGIDevice;
	CComPtr<IDXGIAdapter> pAdapter;
	if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice)))
			|| FAILED(pDXGIDevice->GetAdapter(&pAdapter))) {
		Log(L"could not reach the renderer's DXGI adapter");
		return false;
	}
	DXGI_ADAPTER_DESC desc = {};
	pAdapter->GetDesc(&desc);

	CComPtr<IDXGIFactory4> pFactory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)))) {
		Log(L"CreateDXGIFactory1 failed");
		return false;
	}
	CComPtr<IDXGIAdapter1> pAdapter1;
	if (FAILED(pFactory->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&pAdapter1)))) {
		Log(L"EnumAdapterByLuid failed");
		return false;
	}

	HRESULT hr = D3D12CreateDevice(pAdapter1, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDev12));
	if (FAILED(hr)) {
		Log(L"D3D12CreateDevice failed 0x%08X", hr);
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(m_pDev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_pQueue)))
			|| FAILED(m_pDev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pAlloc)))
			|| FAILED(m_pDev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pAlloc, nullptr, IID_PPV_ARGS(&m_pList)))) {
		Log(L"could not build the D3D12 command objects");
		return false;
	}
	m_bListOpen = true;   // a command list is made recording

	// The network's GPU time. Without it the network goes untimed, nothing more.
	const D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
	const D3D12_HEAP_PROPERTIES readback = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width            = 2 * sizeof(UINT64);
	rd.Height           = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(m_pQueue->GetTimestampFrequency(&m_TimestampFrequency)) || !m_TimestampFrequency
			|| FAILED(m_pDev12->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_pTimestamps)))
			|| FAILED(m_pDev12->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr, IID_PPV_ARGS(&m_pTimestampReadback)))) {
		Log(L"no timestamps on the D3D12 queue: the network goes untimed");
		m_pTimestamps.Release();
		m_pTimestampReadback.Release();
		m_TimestampFrequency = 0;
	}

	// Two shared fences, one per direction.
	if (FAILED(m_pDev11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_pFenceUp11)))) {
		Log(L"ID3D11Device5::CreateFence failed");
		return false;
	}
	HANDLE hUp = nullptr;
	if (FAILED(m_pFenceUp11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hUp)) || !hUp) {
		Log(L"could not share the D3D11 fence");
		return false;
	}
	hr = m_pDev12->OpenSharedHandle(hUp, IID_PPV_ARGS(&m_pFenceUp12));
	CloseHandle(hUp);
	if (FAILED(hr)) {
		Log(L"OpenSharedHandle(fence) failed 0x%08X", hr);
		return false;
	}

	if (FAILED(m_pDev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_pFenceDown12)))) {
		Log(L"ID3D12Device::CreateFence failed");
		return false;
	}
	HANDLE hDown = nullptr;
	if (FAILED(m_pDev12->CreateSharedHandle(m_pFenceDown12, nullptr, GENERIC_ALL, nullptr, &hDown)) || !hDown) {
		Log(L"could not share the D3D12 fence");
		return false;
	}
	hr = m_pDev11->OpenSharedFence(hDown, IID_PPV_ARGS(&m_pFenceDown11));
	CloseHandle(hDown);
	if (FAILED(hr)) {
		Log(L"OpenSharedFence failed 0x%08X", hr);
		return false;
	}

	Log(L"private D3D12 device on %s", desc.Description);
	return true;
}

// ============================================================================
// Bring-up
// ============================================================================

void CDlssNR::QueryRequirements()
{
	// Advisory. With the architecture override active this reports SUPPORTED;
	// without it, AdapterUnsupported. Either way it is only ever displayed.
	if (!m_pfnGetReq || !m_pDev11) {
		return;
	}

	CComPtr<IDXGIDevice> pDXGIDevice;
	CComPtr<IDXGIAdapter> pAdapter;
	if (FAILED(m_pDev11->QueryInterface(IID_PPV_ARGS(&pDXGIDevice)))
			|| FAILED(pDXGIDevice->GetAdapter(&pAdapter))) {
		return;
	}

	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* pathList[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = pathList;
	fci.PathListInfo.Length = 1;

	NVSDK_NGX_FeatureDiscoveryInfo fdi = {};
	fdi.SDKVersion = m_SdkVersion;
	fdi.FeatureID = NGX_FEATURE_DLSSNR;
	fdi.Identifier.IdentifierType = NGX_AppIdType_Application;
	fdi.Identifier.v.ApplicationId = NGX_DLSSNR_APPID;
	fdi.ApplicationDataPath = m_DataPath.c_str();
	fdi.FeatureInfo = &fci;

	NVSDK_NGX_FeatureRequirement req = {};
	if (NGX_SUCCEED(m_pfnGetReq(pAdapter, &fdi, &req))) {
		m_bHaveReq = true;
		m_ReqSupported = req.FeatureSupported;
		m_ReqMinArch = req.MinHWArchitecture;
		Log(L"requirements: supported=0x%X minArch=0x%X", req.FeatureSupported, req.MinHWArchitecture);
	}
}

bool CDlssNR::Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath, bool bArchOverride)
{
	if (m_bInitialised) {
		return true;
	}
	if (!pDevice) {
		return false;
	}

	m_State = State::Off;
	m_LastResult = 0;
	m_DetailError.clear();
	m_bArchOverride = bArchOverride;

	// Whatever a step that fails leaves behind goes with it, so that the next attempt
	// starts from nothing; the state says which step it was.
	const auto Abandon = [this](State state) {
		m_State = state;
		Shutdown();
		return false;
	};

	// Fences need the 11.4 interfaces.
	CComPtr<ID3D11DeviceContext> pCtx;
	pDevice->GetImmediateContext(&pCtx);
	if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&m_pDev11)))
			|| !pCtx || FAILED(pCtx->QueryInterface(IID_PPV_ARGS(&m_pCtx11)))) {
		Log(L"ID3D11Device5 or ID3D11DeviceContext4 unavailable; shared fences need Windows 10 1703+");
		return Abandon(State::NoD3D12);
	}

	// NGX writes its logs and model cache here.
	wchar_t appData[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
		m_DataPath = std::wstring(appData) + L"\\MPC-BE Filters\\MPC Video Renderer\\ngx\\";
		SHCreateDirectoryExW(nullptr, m_DataPath.c_str(), nullptr);
	} else {
		wchar_t tmp[MAX_PATH] = {};
		GetTempPathW(std::size(tmp), tmp);
		m_DataPath = tmp;
	}

	// 1. Before anything loads: the snippet reads the architecture during init and
	// caches it, so the hook has to be in place first.
	if (m_bArchOverride && InstallArchOverride()) {
		m_bHoldsArchOverride = true;
		g_SpoofUsers++;
	}

	// 2. The snippet, which says which SDK version it speaks.
	if (!LoadSnippet(pConfiguredDllPath)) {
		return Abandon(m_State);   // not found or not loaded, as LoadSnippet said
	}
	if (m_bHoldsArchOverride) {
		g_hSpoofFor = m_hSnippet;   // loaded, not yet initialised: its arch query is still to come
	}

	m_pfnInit      = (PFN_NGX_D3D12_Init_Ext)          GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_Init_Ext");
	m_pfnPopulate  = (PFN_NGX_D3D12_PopulateParameters)GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
	m_pfnCreate    = (PFN_NGX_D3D12_CreateFeature)     GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_CreateFeature");
	m_pfnEvaluate  = (PFN_NGX_D3D12_EvaluateFeature)   GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_EvaluateFeature");
	m_pfnRelease   = (PFN_NGX_D3D12_ReleaseFeature)    GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_ReleaseFeature");
	m_pfnShutdown1 = (PFN_NGX_D3D12_Shutdown1)         GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_Shutdown1");
	m_pfnGetReq    = (PFN_NGX_D3D11_GetFeatureRequirements)GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D11_GetFeatureRequirements");

	if (!m_pfnInit || !m_pfnPopulate || !m_pfnCreate || !m_pfnEvaluate) {
		Log(L"required D3D12 exports missing from %s", s_SnippetName);
		return Abandon(State::ExportsMissing);
	}

	// The snippet declares which SDK version it speaks; the core rejects newer.
	if (auto pfnApiVer = (PFN_NGX_GetU32)GetProcAddress(m_hSnippet, "NVSDK_NGX_GetAPIVersion")) {
		const uint32_t v = pfnApiVer();
		if (v >= 0x10 && v <= 0x20) {
			m_SdkVersion = v;
		}
	}
	if (auto pfnSnipVer = (PFN_NGX_GetU32)GetProcAddress(m_hSnippet, "NVSDK_NGX_GetSnippetVersion")) {
		m_SnippetVer = pfnSnipVer();
	}
	Log(L"snippet 0x%08X, SDK version 0x%02X", m_SnippetVer, m_SdkVersion);

	// 3. The caller shim: every guarded entry point of the snippet goes through it.
	if (!LoadShim()) {
		return Abandon(State::ShimMissing);
	}

	// 4. The private Direct3D 12 device and the two fences it shares with the renderer's.
	if (!CreateD3D12(pDevice)) {
		return Abandon(State::NoD3D12);
	}

	// 5. The driver's runtime, on the same device and SDK version, for the parameter
	// block it allocates. The snippet runs without it, on a block of our own.
	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* pathList[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = pathList;
	fci.PathListInfo.Length = 1;

	if (LoadCore()) {
		typedef NVSDK_NGX_Result(__cdecl* PFN_CoreInit12)(unsigned long long,
			const wchar_t*, ID3D12Device*, const void*, uint32_t);
		if (auto pfnCoreInit = (PFN_CoreInit12)GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_Init")) {
			const NVSDK_NGX_Result rc = pfnCoreInit(NGX_DLSSNR_APPID, m_DataPath.c_str(),
			                                        m_pDev12, &fci, m_SdkVersion);
			Log(L"core init: 0x%08X %s", rc, NgxResultName(rc));
		}
		if (m_pfnCoreAlloc) {
			NVSDK_NGX_Parameter* p = nullptr;
			if (NGX_SUCCEED(m_pfnCoreAlloc(&p)) && p) {
				m_pParams = p;
				m_bParamsFromCore = true;
			}
		}
	}

	if (!m_pParams) {
		m_pParams = &m_OwnParams;
		m_bParamsFromCore = false;
		Log(L"using built-in parameter block");
	}

	QueryRequirements();

	// 6. The snippet itself. The feature comes later, with the textures (CreateFeature).
	const NVSDK_NGX_Result r = CallInit();
	m_LastResult = r;
	if (NGX_FAILED(r)) {
		Log(L"Init_Ext failed: 0x%08X %s", r, NgxResultName(r));
		return Abandon(State::ApiInitFailed);
	}

	CallPopulate(m_pParams);

	m_bInitialised = true;
	m_bResetPending = true;
	m_State = State::Ready;
	Log(L"initialised");
	return true;
}

void CDlssNR::Shutdown()
{
	DrainGpu();
	ReleaseFeature();

	if (m_bInitialised) {
		CallShutdown1();
	}
	m_bInitialised = false;

	if (m_pParams && m_bParamsFromCore && m_pfnCoreDestroy) {
		m_pfnCoreDestroy(m_pParams);
	}
	m_pParams = nullptr;
	m_bParamsFromCore = false;
	m_OwnParams.Reset();

	m_pTex12In.Release();
	m_pTex12Out.Release();
	// Guides were opened on the device about to go away; a later Init must not
	// mistake them for still-valid resources.
	m_GuideMVec = {};
	m_GuideDepth = {};
	m_GuideMask = {};
	m_fMVecScaleX = 1.0f;
	m_fMVecScaleY = 1.0f;
	m_pFenceUp11.Release();
	m_pFenceUp12.Release();
	m_pFenceDown12.Release();
	m_pFenceDown11.Release();
	m_pTimestamps.Release();
	m_pTimestampReadback.Release();
	m_TimestampFrequency = 0;
	m_pList.Release();
	m_pAlloc.Release();
	m_bListOpen = false;
	m_LastSubmitted = 0;
	m_bLastTimed = false;
	m_Timing = {};
	m_pQueue.Release();
	m_pDev12.Release();
	m_pCtx11.Release();
	m_pDev11.Release();

	// The snippet is a 165 MB module holding ~500 MB of GPU allocations; do not
	// keep it mapped once the feature is off.
	if (m_hShim)    { FreeLibrary(m_hShim);    m_hShim = nullptr; }
	if (m_hSnippet) { FreeLibrary(m_hSnippet); m_hSnippet = nullptr; }
	if (m_hCore)    { FreeLibrary(m_hCore);    m_hCore = nullptr; }

	m_pfnInit = nullptr; m_pfnPopulate = nullptr; m_pfnCreate = nullptr;
	m_pfnEvaluate = nullptr; m_pfnRelease = nullptr; m_pfnShutdown1 = nullptr;
	m_pfnGetReq = nullptr; m_pfnCoreAlloc = nullptr; m_pfnCoreDestroy = nullptr;
	m_pfnShimInit = nullptr; m_pfnShimPopulate = nullptr; m_pfnShimCreate = nullptr;
	m_pfnShimEval = nullptr; m_pfnShimRelease = nullptr; m_pfnShimShutdown = nullptr;

	if (m_hFenceEvent) {
		CloseHandle(m_hFenceEvent);
		m_hFenceEvent = nullptr;
	}

	m_bUseShim = false;
	m_FenceValue = 0;

	// The architecture hook stays installed for the process: MinHook removal
	// while another thread is inside the detour is not worth the risk, and the
	// override is inert once g_SpoofArch is cleared -- which it is when the last
	// session that asked for it goes, not the first: a second renderer in the same
	// process may be about to start its own.
	if (m_bHoldsArchOverride) {
		m_bHoldsArchOverride = false;
		if (--g_SpoofUsers <= 0) {
			g_SpoofUsers = 0;
			g_SpoofArch = 0;
			g_hSpoofFor = nullptr;
		}
	}

	if (m_State == State::Ready) {
		m_State = State::Off;
	}
}

// ============================================================================
// Feature and evaluation
// ============================================================================

// Let the queue drain, and release anything the other device may still be parked on.
// Tearing the D3D12 side down while the renderer's context still holds a GPU-side Wait
// on a fence nobody will ever signal again stops the D3D11 timeline dead: the picture
// freezes while audio, on its own path, keeps playing. That is exactly what toggling
// the feature during playback once did.
void CDlssNR::DrainGpu()
{
	// Nothing may still be reading or writing the shared textures when they are
	// released: the queue is given a last signal and waited for, and a queue that will
	// not get there -- a device gone, a wait for an input that never comes -- is
	// released from the CPU instead.
	if (m_pQueue && m_pDev12 && m_pFenceDown12 && SUCCEEDED(m_pDev12->GetDeviceRemovedReason())) {
		m_FenceValue++;
		if (FAILED(m_pQueue->Signal(m_pFenceDown12, m_FenceValue)) || !WaitForFence(m_pFenceDown12, m_FenceValue)) {
			Unblock();
		}
	} else {
		Unblock();
	}
	if (m_pCtx11) {
		m_pCtx11->Flush();
	}
}

// Both fences to the highest value either was ever asked for, from the CPU: whichever
// device waits on the other goes on. Only for a session that is being given up.
void CDlssNR::Unblock()
{
	if (m_pFenceUp12) {
		m_pFenceUp12->Signal(m_FenceValue);
	}
	if (m_pFenceDown12) {
		m_pFenceDown12->Signal(m_FenceValue);
	}
}

// The list open on the allocator, once the GPU is done with everything recorded from
// it: the previous submission, which is the previous picture's network. Resetting the
// allocator before that is what once removed the device (DXGI_ERROR_DEVICE_REMOVED,
// 0x887A0005).
bool CDlssNR::OpenList()
{
	if (m_bListOpen) {
		return true;
	}
	if (m_LastSubmitted && !WaitForFence(m_pFenceDown12, m_LastSubmitted)) {
		return false;
	}
	CollectGpuTime();   // the timestamps are written over by the next submission
	if (FAILED(m_pAlloc->Reset()) || FAILED(m_pList->Reset(m_pAlloc, nullptr))) {
		return false;
	}
	m_bListOpen = true;
	return true;
}

// What was recorded, to the queue, which then signals the next fence value.
bool CDlssNR::SubmitList(const bool bTimed)
{
	m_bListOpen = false;
	if (FAILED(m_pList->Close())) {
		return false;
	}
	ID3D12CommandList* lists[] = { m_pList };
	m_pQueue->ExecuteCommandLists(1, lists);
	m_FenceValue++;
	if (FAILED(m_pQueue->Signal(m_pFenceDown12, m_FenceValue))) {
		return false;
	}
	m_LastSubmitted = m_FenceValue;
	m_bLastTimed = bTimed;
	return true;
}

// The work that has to be there before anything else is done: the feature's own
// upload. The list is left closed; the next OpenList finds the GPU done with it.
bool CDlssNR::ExecuteAndWait()
{
	return OpenList() && SubmitList(false) && WaitForFence(m_pFenceDown12, m_LastSubmitted);
}

void CDlssNR::CollectGpuTime()
{
	if (!m_bLastTimed || !m_pFenceDown12 || !m_pTimestampReadback || m_pFenceDown12->GetCompletedValue() < m_LastSubmitted) {
		return;
	}
	m_bLastTimed = false;
	const D3D12_RANGE range = { 0, 2 * sizeof(UINT64) };
	UINT64* stamps = nullptr;
	if (SUCCEEDED(m_pTimestampReadback->Map(0, &range, (void**)&stamps)) && stamps) {
		if (stamps[1] >= stamps[0]) {
			m_Timing.gpuMs = double(stamps[1] - stamps[0]) * 1000.0 / double(m_TimestampFrequency);
			m_Timing.gpuSerial++;
		}
		const D3D12_RANGE none = { 0, 0 };
		m_pTimestampReadback->Unmap(0, &none);
	}
}

// Blocks until the fence reaches the value, or the device dies.
bool CDlssNR::WaitForFence(ID3D12Fence* pFence, UINT64 value)
{
	if (!pFence) {
		return false;
	}
	if (pFence->GetCompletedValue() >= value) {
		return true;
	}
	if (!m_hFenceEvent) {
		m_hFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!m_hFenceEvent) {
			return false;
		}
	}
	if (FAILED(pFence->SetEventOnCompletion(value, m_hFenceEvent))) {
		return false;
	}
	if (WaitForSingleObject(m_hFenceEvent, 2000) != WAIT_OBJECT_0) {
		Log(L"GPU wait timed out");
		return false;
	}
	return true;
}

bool CDlssNR::CheckDeviceLost()
{
	if (!m_pDev12) {
		return true;
	}
	const HRESULT hr = m_pDev12->GetDeviceRemovedReason();
	if (SUCCEEDED(hr)) {
		return false;
	}
	m_DetailError = std::format(L"Direct3D 12 device removed (0x{:08X})", (unsigned)hr);
	Log(L"%s", m_DetailError.c_str());
	m_State = State::DeviceLost;
	Unblock();   // the renderer may be waiting for a network that will not come
	return true;
}

// Opens a D3D11 texture on the private D3D12 device through an NT handle. Each
// step names itself on failure: "could not share textures" on its own was
// useless to diagnose.
bool CDlssNR::OpenOnD3D12(const wchar_t* which, ID3D11Texture2D* pTex11, CComPtr<ID3D12Resource>& out)
{
	out.Release();

	D3D11_TEXTURE2D_DESC td = {};
	pTex11->GetDesc(&td);
	if (!(td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
		m_DetailError = std::format(L"{} texture has no share handle (misc 0x{:X})", which, td.MiscFlags);
		Log(L"%s", m_DetailError.c_str());
		return false;
	}

	CComPtr<IDXGIResource1> pRes1;
	HRESULT hr = pTex11->QueryInterface(IID_PPV_ARGS(&pRes1));
	if (FAILED(hr)) {
		m_DetailError = std::format(L"{}: no IDXGIResource1 (0x{:08X})", which, (unsigned)hr);
		Log(L"%s", m_DetailError.c_str());
		return false;
	}

	HANDLE h11 = nullptr;
	hr = pRes1->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h11);
	if (FAILED(hr) || !h11) {
		m_DetailError = std::format(L"{}: CreateSharedHandle failed (0x{:08X})", which, (unsigned)hr);
		Log(L"%s", m_DetailError.c_str());
		return false;
	}

	hr = m_pDev12->OpenSharedHandle(h11, IID_PPV_ARGS(&out));
	CloseHandle(h11);
	if (FAILED(hr)) {
		m_DetailError = std::format(L"{}: OpenSharedHandle failed (0x{:08X})", which, (unsigned)hr);
		Log(L"%s", m_DetailError.c_str());
		return false;
	}
	return true;
}

bool CDlssNR::SetGuideSlot(GuideSlot& slot, const wchar_t* which, ID3D11Texture2D* pTex11)
{
	if (!pTex11) {
		slot = {};
		return true;
	}
	if (slot.p11 == pTex11 && slot.p12) {
		return true;
	}

	slot = {};
	if (!OpenOnD3D12(which, pTex11, slot.p12)) {
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	pTex11->GetDesc(&td);
	slot.p11 = pTex11;
	slot.w = td.Width;
	slot.h = td.Height;
	return true;
}

// Guides can be set before or after the feature exists. They go into the
// parameter block straight away, so a CreateFeature that follows sees them too.
bool CDlssNR::SetGuides(const Guides& g)
{
	if (!m_pDev12) {
		return false;
	}

	const bool ok = SetGuideSlot(m_GuideMVec, L"motion vectors", g.pMVec)
		&& SetGuideSlot(m_GuideDepth, L"depth", g.pDepth)
		&& SetGuideSlot(m_GuideMask, L"control mask", g.pControlMask);
	if (!ok) {
		m_GuideMVec = {};
		m_GuideDepth = {};
		m_GuideMask = {};
	}
	m_fMVecScaleX = g.fMVecScaleX;
	m_fMVecScaleY = g.fMVecScaleY;

	if (m_pParams) {
		PushGuideParams(m_pParams);
	}
	return ok;
}

void CDlssNR::PushGuideParams(NVSDK_NGX_Parameter* p)
{
	struct Entry { const char* key; const SubrectKeys& subrect; const GuideSlot& slot; };
	const Entry entries[] = {
		{ P_MVEC,        s_RectMVec,        m_GuideMVec },
		{ P_DEPTH,       s_RectDepth,       m_GuideDepth },
		{ P_CONTROLMASK, s_RectControlMask, m_GuideMask },
	};

	for (const Entry& e : entries) {
		// Absent is a null resource rather than a missing key: a pointer set on
		// an earlier frame must not linger in the block.
		p->Set(e.key, (ID3D12Resource*)e.slot.p12.p);
		if (e.slot.p12) {
			SetWholeSubrect(p, e.subrect, e.slot.w, e.slot.h);
		}
	}
	p->Set(P_MVECSCALEX, m_fMVecScaleX);
	p->Set(P_MVECSCALEY, m_fMVecScaleY);
}

bool CDlssNR::CreateFeature(ID3D11Texture2D* pShared11In, ID3D11Texture2D* pShared11Out,
                            UINT w, UINT h, const Params& p)
{
	if (!m_bInitialised || !pShared11In || !pShared11Out || !w || !h) {
		return false;
	}
	if (MatchesFeature(w, h, p.iPreset) && MatchesTextures(pShared11In, pShared11Out)) {
		return true;
	}
	if (CheckDeviceLost()) {
		return false;
	}

	ReleaseFeature();

	// Open the renderer's textures on the D3D12 side. They were created with
	// Tex2D_DefaultShaderRTargetUAVShared, so they carry an NT share handle and
	// the UAV bind flag NGX needs for its output.
	if (!OpenOnD3D12(L"input", pShared11In, m_pTex12In) || !OpenOnD3D12(L"output", pShared11Out, m_pTex12Out)) {
		m_State = State::ShareFailed;
		return false;
	}

	m_pParams->Set(P_WIDTH,  (unsigned int)w);
	m_pParams->Set(P_HEIGHT, (unsigned int)h);
	m_pParams->Set(P_ENABLED, (int)1);
	m_pParams->Set(P_STYLE,  (int)p.iStyle);
	m_pParams->Set(P_PRESET, (int)p.iPreset);
	m_pParams->Set(P_SCALINGRATIO, 1.0f);
	m_pParams->Set("CreationNodeMask",   (unsigned int)1);
	m_pParams->Set("VisibilityNodeMask", (unsigned int)1);

	if (!OpenList()) {
		m_State = State::FeatureCreateFailed;
		Log(L"CreateFeature %ux%u: the GPU had not finished the last network after two seconds", w, h);
		Unblock();
		return false;
	}
	const NVSDK_NGX_Result r = CallCreate(m_pParams, &m_pFeature);
	m_LastResult = r;
	if (NGX_FAILED(r) || !m_pFeature) {
		m_pFeature = nullptr;
		m_State = State::FeatureCreateFailed;
		Log(L"CreateFeature %ux%u failed: 0x%08X %s", w, h, r, NgxResultName(r));
		return false;
	}

	// CreateFeature records upload work on the list; it has to run.
	if (!ExecuteAndWait()) {
		Log(L"could not submit the CreateFeature work");
		ReleaseFeature();
		m_State = State::FeatureCreateFailed;
		return false;
	}

	m_pShared11In  = pShared11In;
	m_pShared11Out = pShared11Out;
	m_featW = w;
	m_featH = h;
	m_featPreset = p.iPreset;
	m_bResetPending = true;
	m_State = State::Ready;
	Log(L"feature created %ux%u preset %d", w, h, p.iPreset);
	return true;
}

// The feature and our hold on the renderer's two textures, which it is about to
// release or has released: kept open here, two RGBA16F textures the size of the
// picture would stay allocated for as long as the feature is off.
void CDlssNR::ReleaseFeature()
{
	if (m_pFeature) {
		DrainGpu();
		CallRelease(m_pFeature);
		m_pFeature = nullptr;
	}
	m_pTex12In.Release();
	m_pTex12Out.Release();
	m_featW = m_featH = 0;
	m_featPreset = -1;
	m_pShared11In = nullptr;
	m_pShared11Out = nullptr;
}

void CDlssNR::PushEvaluateParams(NVSDK_NGX_Parameter* p, const Params& s, bool bReset)
{
	const UINT w = m_featW, h = m_featH;

	p->Set(P_WIDTH,  (unsigned int)w);
	p->Set(P_HEIGHT, (unsigned int)h);
	p->Set(P_ENABLED, (int)1);
	p->Set(P_RESET, (int)(bReset ? 1 : 0));
	p->Set(P_STYLE,  (int)s.iStyle);
	p->Set(P_PRESET, (int)s.iPreset);
	p->Set(P_INTENSITY,   s.fIntensity);
	p->Set(P_LOCALTONE,   s.fLocalTone);
	p->Set(P_LOCALSTRUCT, s.fLocalStructure);
	p->Set(P_SKINSTRUCT,  s.fSkinStructure);
	p->Set(P_AUTOMASK, (int)(s.bUseAutoMask ? 1 : 0));
	p->Set(P_UICORRECTION, (int)0);
	p->Set(P_DEPTHINV, (int)1);
	p->Set(P_SCALINGRATIO, 1.0f);
	PushGuideParams(p);

	p->Set(P_COLOR,      (ID3D12Resource*)m_pTex12In);
	p->Set(P_OUTPUT,     (ID3D12Resource*)m_pTex12Out);
	p->Set(P_BACKBUFFER, (ID3D12Resource*)m_pTex12Out);

	SetWholeSubrect(p, s_RectColor, w, h);
	SetWholeSubrect(p, s_RectOutput, w, h);
	SetWholeSubrect(p, s_RectBackbuffer, w, h);
}

bool CDlssNR::Evaluate(const Params& p)
{
	if (!m_bInitialised || !m_pFeature || !m_pTex12In || !m_pTex12Out) {
		return false;
	}

	if (CheckDeviceLost()) {
		return false;
	}

	// A wait that ran out leaves the session not to be used again as it is: whatever
	// device waits on the other is released, and the caller starts another session
	// (State says so).
	const auto GaveUp = [this](const wchar_t* what) {
		m_DetailError = what;
		m_State = State::EvaluateFailed;
		m_LastResult = NGX_Result_Fail;
		Log(L"%s", what);
		Unblock();
		return false;
	};
	if (s_FailAt > 0 && ++s_Evaluations % s_FailAt == 0) {
		return GaveUp(L"made to fail for a test (MPCVR_DLSSNR_FAIL_AT)");
	}

	static const double s_ticksPerMs = []() {
		LARGE_INTEGER qpf = {};
		QueryPerformanceFrequency(&qpf);
		return qpf.QuadPart / 1000.0;
	}();
	LARGE_INTEGER t0 = {}, t1 = {}, t2 = {};
	QueryPerformanceCounter(&t0);

	// The previous picture's network is done with before this one is recorded: the
	// allocator is free, and a queue that has hung is found here, before the next
	// Present would wait on it for good.
	if (!OpenList()) {
		return CheckDeviceLost() ? false : GaveUp(L"the GPU had not run the network of the picture before after two seconds");
	}
	QueryPerformanceCounter(&t1);

	// The renderer has just written the input texture: the queue reads it once the
	// renderer's context has got that far, which the context signals.
	m_FenceValue++;
	const UINT64 inputWritten = m_FenceValue;
	m_pCtx11->Signal(m_pFenceUp11, inputWritten);
	m_pCtx11->Flush();
	if (FAILED(m_pQueue->Wait(m_pFenceUp12, inputWritten))) {
		return GaveUp(L"the queue could not be told to wait for the input");
	}

	// No barriers: a texture shared from D3D11 comes back with
	// ALLOW_SIMULTANEOUS_ACCESS already set (measured: D3D12 reports flags
	// 0x25), so it stays usable in COMMON from both devices and transitioning
	// it would only add a way to get the state wrong.
	PushEvaluateParams(m_pParams, p, m_bResetPending || p.bNoHistory);
	if (m_pTimestamps) {
		m_pList->EndQuery(m_pTimestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
	}
	const NVSDK_NGX_Result r = CallEvaluate(m_pParams);
	if (m_pTimestamps) {
		m_pList->EndQuery(m_pTimestamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
		m_pList->ResolveQueryData(m_pTimestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_pTimestampReadback, 0);
	}
	QueryPerformanceCounter(&t2);

	m_LastResult = r;
	if (NGX_FAILED(r)) {
		m_DetailError.clear();
		m_State = State::EvaluateFailed;
		Log(L"Evaluate failed: 0x%08X %s", r, NgxResultName(r));
		SubmitList(false);   // what was recorded goes, and the queue's wait with it
		return false;
	}
	if (!SubmitList(m_pTimestamps != nullptr)) {
		return CheckDeviceLost() ? false : GaveUp(L"the network could not be handed to the queue");
	}
	// The renderer's context reads the network's output from here on: it waits for
	// the queue, on the GPU -- or, should it refuse to, here.
	if (FAILED(m_pCtx11->Wait(m_pFenceDown11, m_LastSubmitted)) && !WaitForFence(m_pFenceDown12, m_LastSubmitted)) {
		return GaveUp(L"the GPU had not run the network after two seconds");
	}

	m_Timing.inputWaitMs = double(t1.QuadPart - t0.QuadPart) / s_ticksPerMs;
	m_Timing.recordMs    = double(t2.QuadPart - t1.QuadPart) / s_ticksPerMs;
	m_Timing.gpuWaitMs   = 0;

	m_bResetPending = false;
	return true;
}

// ============================================================================
// Reporting
// ============================================================================

std::wstring CDlssNR::GetStatusLine() const
{
	switch (m_State) {
	case State::Off:            return L"disabled";
	case State::DllNotFound:    return std::wstring(s_SnippetName) + L" not found";
	case State::DllLoadFailed:  return std::wstring(s_SnippetName) + L" failed to load";
	case State::ExportsMissing: return L"unexpected DLL (D3D12 exports missing)";
	case State::ShimMissing:    return std::wstring(s_ShimName) + L" missing";
	case State::NoD3D12:        return L"no Direct3D 12 device on this adapter";
	case State::DeviceLost:
		return m_DetailError.empty() ? std::wstring(L"Direct3D 12 device lost") : m_DetailError;
	case State::ShareFailed:
		return m_DetailError.empty() ? std::wstring(L"could not share textures with Direct3D 12")
		                            : (L"share failed: " + m_DetailError);
	case State::EvaluateFailed:
		if (!m_DetailError.empty()) {
			return L"unavailable: " + m_DetailError;
		}
		[[fallthrough]];
	case State::ApiInitFailed:
	case State::FeatureCreateFailed:
		return std::format(L"unavailable: 0x{:08X} {}", m_LastResult, NgxResultName(m_LastResult));
	case State::Ready:
		return m_pFeature ? std::format(L"ready {}x{} preset {}", m_featW, m_featH, m_featPreset)
		                  : L"initialised";
	}
	return L"?";
}

std::wstring CDlssNR::GetInfoBlock() const
{
	std::wstring s = L"DLSS 5 Neural Rendering:\n";

	s += std::format(L"  DLL       : {}\n", m_DllPath.empty() ? L"<not loaded>" : m_DllPath);
	if (m_SnippetVer) {
		s += std::format(L"  Snippet   : 0x{:08X}, SDK version 0x{:02X}\n", m_SnippetVer, m_SdkVersion);
	}
	s += std::format(L"  Transport : Direct3D 12 interop{}\n", m_bUseShim ? L", nvngx.dll shim" : L"");
	s += std::format(L"  Arch      : {}\n", m_bArchOverride ? L"reported as Blackwell" : L"as reported by the driver");
	s += std::format(L"  State     : {}\n", GetStatusLine());
	s += std::format(L"  Guides    : {}\n", m_GuideMask.p12 ? L"ControlMask (temporal stabilizer)" : L"none");

	if (m_bHaveReq) {
		s += std::format(L"  Reported  : supported=0x{:X} minArch=0x{:X}", m_ReqSupported, m_ReqMinArch);
		if (m_ReqSupported & NGX_FeatureSupport_AdapterUnsupported) {
			s += L" (adapter below stock minimum)";
		}
		s += L"\n";
	}

	if (!m_Log.empty()) {
		s += L"  Log       :\n";
		size_t skip = m_Log.size() > 8 ? m_Log.size() - 8 : 0;
		for (const auto& line : m_Log) {
			if (skip) { skip--; continue; }
			s += L"    " + line + L"\n";
		}
	}
	return s;
}
