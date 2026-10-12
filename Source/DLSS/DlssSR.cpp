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
#include <shlwapi.h>
#include <shlobj.h>
#include <format>
#include <memory>
#include <mutex>
#include "Helper.h"
#include "DlssSR.h"

#pragma comment(lib, "shlwapi.lib")

namespace {

const wchar_t kSnippetName[] = L"nvngx_dlss.dll";

// Newest first: a runtime older than the SDK answers OutOfDate.
constexpr uint32_t kSdkVersions[] = { 0x15, 0x14, 0x13 };

// The parameter keys of DLSS Super Resolution.
#define P_WIDTH          "Width"
#define P_HEIGHT         "Height"
#define P_OUTWIDTH       "OutWidth"
#define P_OUTHEIGHT      "OutHeight"
#define P_PERFQUALITY    "PerfQualityValue"
#define P_CREATEFLAGS    "DLSS.Feature.Create.Flags"
#define P_OUTSUBRECTS    "DLSS.Enable.Output.Subrects"
#define P_COLOR          "Color"
#define P_OUTPUT         "Output"
#define P_DEPTH          "Depth"
#define P_MOTIONVECTORS  "MotionVectors"
#define P_JITTERX        "Jitter.Offset.X"
#define P_JITTERY        "Jitter.Offset.Y"
#define P_SHARPNESS      "Sharpness"
#define P_RESET          "Reset"
#define P_MVSCALEX       "MV.Scale.X"
#define P_MVSCALEY       "MV.Scale.Y"
#define P_RENDERWIDTH    "DLSS.Render.Subrect.Dimensions.Width"
#define P_RENDERHEIGHT   "DLSS.Render.Subrect.Dimensions.Height"
#define P_PREEXPOSURE    "DLSS.Pre.Exposure"
#define P_EXPOSURESCALE  "DLSS.Exposure.Scale"
#define P_FRAMETIME      "FrameTimeDeltaInMsec"

const char* const kPresetKeys[] = {
	"DLSS.Hint.Render.Preset.DLAA",
	"DLSS.Hint.Render.Preset.Quality",
	"DLSS.Hint.Render.Preset.Balanced",
	"DLSS.Hint.Render.Preset.Performance",
	"DLSS.Hint.Render.Preset.UltraPerformance",
	"DLSS.Hint.Render.Preset.UltraQuality",
};

// What NGX logs, from whichever thread it logs on. The callback carries no
// context, so the lines gather here for every instance.
std::mutex g_NgxLogLock;
std::deque<std::wstring> g_NgxLog;
CDlssSR::TraceFn g_Trace = nullptr;

// The model DLSS picks for each scale, as its creation log says, indexed by
// NVSDK_NGX_PerfQuality_Value.
wchar_t g_PresetByMode[6] = {};

// "... NgxDltss::FillCreationParams ... Info: (Perf) ... Preset M"
void ReadPresetLine(const std::wstring& line)
{
	if (line.find(L"NgxDltss::FillCreationParams") == std::wstring::npos) {
		return;
	}
	static const struct { const wchar_t* tag; int mode; } tags[] = {
		{ L"(Perf)", NGX_PERFQUALITY_MaxPerf }, { L"(Balanced)", NGX_PERFQUALITY_Balanced },
		{ L"(Quality)", NGX_PERFQUALITY_MaxQuality }, { L"(UltraPerf)", NGX_PERFQUALITY_UltraPerformance },
		{ L"(UltraQuality)", NGX_PERFQUALITY_UltraQuality }, { L"(DLAA)", NGX_PERFQUALITY_DLAA },
	};
	const size_t preset = line.rfind(L"Preset ");
	if (preset == std::wstring::npos || preset + 7 >= line.size()) {
		return;
	}
	for (const auto& t : tags) {
		if (line.find(t.tag) != std::wstring::npos) {
			g_PresetByMode[t.mode] = line[preset + 7];
			return;
		}
	}
}

void __cdecl NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, uint32_t)
{
	if (!message) {
		return;
	}
	std::wstring line;
	for (const char* p = message; *p; p++) {
		if (*p != '\r' && *p != '\n') {
			line += (wchar_t)(unsigned char)*p;
		}
	}
	DLog(L"NGX: {}", line);
	if (g_Trace) {
		g_Trace((L"NGX: " + line).c_str());
	}
	ReadPresetLine(line);

	// The runtime's configuration dump and model searches say nothing about why
	// something failed; the rest is kept for the information block.
	for (const wchar_t* noise : { L"NGXLoadConfig", L"NGXLoadFromPath", L"FillCreationParams", L"Container" }) {
		if (line.find(noise) != std::wstring::npos) {
			return;
		}
	}
	std::lock_guard lock(g_NgxLogLock);
	g_NgxLog.push_back(std::move(line));
	while (g_NgxLog.size() > 64) {
		g_NgxLog.pop_front();
	}
}

// Every call into NGX runs in its own pipeline state: NGX binds compute shaders,
// views and samplers on the immediate context, and the renderer's passes assume
// what they left there.
class CNgxStateScope
{
public:
	CNgxStateScope(ID3D11DeviceContext1* pContext, ID3DDeviceContextState* pState)
	{
		if (pContext && pState) {
			m_pContext = pContext;
			pContext->SwapDeviceContextState(pState, &m_pPrevious);
		}
	}
	~CNgxStateScope()
	{
		if (m_pContext && m_pPrevious) {
			m_pContext->SwapDeviceContextState(m_pPrevious, nullptr);
		}
	}

private:
	CComPtr<ID3D11DeviceContext1>   m_pContext;
	CComPtr<ID3DDeviceContextState> m_pPrevious;
};

std::wstring DirOf(const std::wstring& path)
{
	const size_t sep = path.find_last_of(L"\\/");
	return (sep == std::wstring::npos) ? std::wstring(L".") : path.substr(0, sep);
}

std::wstring ThisModuleDir()
{
	wchar_t buf[MAX_PATH] = {};
	HMODULE hMod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCWSTR)&ThisModuleDir, &hMod);
	GetModuleFileNameW(hMod, buf, (DWORD)std::size(buf));
	return DirOf(buf);
}

HRESULT ClearTexture(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Texture2D* pTexture, float value)
{
	CComPtr<ID3D11RenderTargetView> pTarget;
	HRESULT hr = pDevice->CreateRenderTargetView(pTexture, nullptr, &pTarget);
	if (SUCCEEDED(hr)) {
		const FLOAT color[4] = { value, value, value, value };
		pContext->ClearRenderTargetView(pTarget, color);
	}
	return hr;
}

} // namespace

// ============================================================================
// Housekeeping
// ============================================================================

CDlssSR::~CDlssSR()
{
	Shutdown();
}

void CDlssSR::SetTrace(TraceFn fn)
{
	g_Trace = fn;
}

void CDlssSR::Log(const std::wstring& line)
{
	DLog(L"CDlssSR: {}", line);
	if (g_Trace) {
		g_Trace(line.c_str());
	}
	m_Log.push_back(line);
	while (m_Log.size() > 16) {
		m_Log.pop_front();
	}
}

bool CDlssSR::Fail(State state, const std::wstring& why)
{
	m_State = state;
	m_Detail = why;
	Log(why);
	return false;
}

std::vector<std::wstring> CDlssSR::CandidateDllPaths(const wchar_t* pConfigured)
{
	std::vector<std::wstring> out;
	if (pConfigured && *pConfigured) {
		std::wstring c = pConfigured;
		if (PathIsDirectoryW(c.c_str())) {
			if (c.back() != L'\\' && c.back() != L'/') {
				c += L'\\';
			}
			c += kSnippetName;
		}
		out.push_back(c);
		return out;   // a path the user gave is the only one tried
	}

	const std::wstring dir = ThisModuleDir();
	out.push_back(dir + L"\\" + kSnippetName);
	out.push_back(dir + L"\\dlss\\" + kSnippetName);
	out.push_back(dir + L"\\..\\" + kSnippetName);
	out.push_back(dir + L"\\..\\..\\" + kSnippetName);   // the repository root from _bin\Filter_x64
	return out;
}

int CDlssSR::QualityModeFor(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight)
{
	if (!inWidth || !inHeight) {
		return NGX_PERFQUALITY_MaxPerf;
	}
	const double ratio = std::max((double)outWidth / inWidth, (double)outHeight / inHeight);
	return (ratio <= 1.0)  ? NGX_PERFQUALITY_DLAA
	     : (ratio < 1.6)   ? NGX_PERFQUALITY_MaxQuality
	     : (ratio < 1.85)  ? NGX_PERFQUALITY_Balanced
	     : (ratio < 2.5)   ? NGX_PERFQUALITY_MaxPerf
	     :                   NGX_PERFQUALITY_UltraPerformance;
}

const wchar_t* CDlssSR::QualityModeName(int mode)
{
	switch (mode) {
	case NGX_PERFQUALITY_MaxPerf:          return L"Performance";
	case NGX_PERFQUALITY_Balanced:         return L"Balanced";
	case NGX_PERFQUALITY_MaxQuality:       return L"Quality";
	case NGX_PERFQUALITY_UltraPerformance: return L"Ultra Performance";
	case NGX_PERFQUALITY_UltraQuality:     return L"Ultra Quality";
	case NGX_PERFQUALITY_DLAA:             return L"DLAA";
	}
	return L"?";
}

// ============================================================================
// Bring-up
// ============================================================================

bool CDlssSR::FindSnippet(const wchar_t* pConfiguredDllPath)
{
	for (const auto& candidate : CandidateDllPaths(pConfiguredDllPath)) {
		if (!PathFileExistsW(candidate.c_str()) || PathIsDirectoryW(candidate.c_str())) {
			continue;
		}
		wchar_t full[MAX_PATH] = {};
		if (!GetFullPathNameW(candidate.c_str(), (DWORD)std::size(full), full, nullptr)) {
			continue;
		}
		// The runtime loads the feature by its name from the folder it is given.
		if (_wcsicmp(PathFindFileNameW(full), kSnippetName)) {
			return Fail(State::DllNotFound, std::format(L"the DLL must be named {}", kSnippetName));
		}
		m_DllPath = full;
		m_SnippetDir = DirOf(m_DllPath);
		Log(L"snippet: " + m_DllPath);
		return true;
	}
	return Fail(State::DllNotFound, std::format(L"{} not found", kSnippetName));
}

bool CDlssSR::LoadCore()
{
	// Where the driver installed its NGX runtime.
	HKEY hKey = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		wchar_t dir[MAX_PATH] = {};
		DWORD size = sizeof(dir) - sizeof(wchar_t);
		DWORD type = 0;
		if (RegQueryValueExW(hKey, L"FullPath", nullptr, &type, (LPBYTE)dir, &size) == ERROR_SUCCESS
				&& (type == REG_SZ || type == REG_EXPAND_SZ) && dir[0]) {
			const std::wstring path = std::wstring(dir) + L"\\_nvngx.dll";
			m_hCore = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		}
		RegCloseKey(hKey);
	}

	if (!m_hCore) {
		WIN32_FIND_DATAW fd = {};
		const std::wstring root = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\";
		HANDLE hFind = FindFirstFileW((root + L"nv_disp*").c_str(), &fd);
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					const std::wstring path = root + fd.cFileName + L"\\_nvngx.dll";
					if (PathFileExistsW(path.c_str())) {
						m_hCore = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
					}
				}
			} while (!m_hCore && FindNextFileW(hFind, &fd));
			FindClose(hFind);
		}
	}

	if (!m_hCore) {
		return Fail(State::CoreNotFound, L"the driver's NGX runtime (_nvngx.dll) was not found");
	}

	m_pfnInitExt     = (PFN_NGX_D3D11_Init_Ext)               GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_Init_Ext");
	m_pfnGetCaps     = (PFN_NGX_D3D11_GetCapabilityParameters)GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_GetCapabilityParameters");
	m_pfnDestroy     = (PFN_NGX_D3D11_DestroyParameters)      GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_DestroyParameters");
	m_pfnCreate      = (PFN_NGX_D3D11_CreateFeature)          GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_CreateFeature");
	m_pfnEvaluate    = (PFN_NGX_D3D11_EvaluateFeature)        GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_EvaluateFeature");
	m_pfnRelease     = (PFN_NGX_D3D11_ReleaseFeature)         GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_ReleaseFeature");
	m_pfnShutdown    = (PFN_NGX_D3D11_Shutdown)               GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_Shutdown");

	if (!m_pfnInitExt || !m_pfnGetCaps || !m_pfnCreate || !m_pfnEvaluate || !m_pfnRelease || !m_pfnShutdown) {
		return Fail(State::CoreNotFound, L"the driver's NGX runtime lacks the Direct3D 11 entry points");
	}

	// From here on the runtime stays in the process, whatever is freed. Its Init
	// loads nvngx_dlss.dll, which no Shutdown and no FreeLibrary unloads again, and
	// gives it a function of the runtime to call on every Evaluate
	// (NVSDK_NGX_SetTelemetryEvaluateCallback). The snippet keeps the first one it
	// is ever given: 310.9.1 puts it in a static it initialises once and leaves the
	// later ones aside. So the runtime it met first has to be the one still there.
	// Unloaded with one film and loaded again for the next, the runtime comes back
	// where it was only if nothing took the address in between; when something did,
	// the first Evaluate of the second film jumped to where the old one had been
	// (0xC0000005 on an address of no module, now and then, in playback_test's
	// five graphs in a row; every time once the address is taken on purpose).
	HMODULE hPinned = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			(LPCWSTR)m_hCore, &hPinned)) {
		Log(std::format(L"the NGX runtime could not be kept loaded ({})", GetLastError()));
	}
	return true;
}

bool CDlssSR::Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath)
{
	if (m_bInitialised) {
		return true;
	}
	if (!pDevice) {
		return false;
	}
	m_State = State::Off;
	m_LastResult = 0;
	m_Detail.clear();

	auto abandon = [this]() {
		const State state = m_State;
		const std::wstring detail = m_Detail;
		Shutdown();
		m_State = state;
		m_Detail = detail;
		return false;
	};

	// A pipeline state of NGX's own; see CNgxStateScope.
	CComQIPtr<ID3D11Device1> pDevice1(pDevice);
	CComPtr<ID3D11DeviceContext> pContext;
	pDevice->GetImmediateContext(&pContext);
	if (!pDevice1 || !pContext || FAILED(pContext->QueryInterface(IID_PPV_ARGS(&m_pContext)))) {
		Fail(State::NoContextState, L"Direct3D 11.1 is not available");
		return abandon();
	}
	const D3D_FEATURE_LEVEL level = pDevice->GetFeatureLevel();
	const UINT stateFlags = (pDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
		? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
	HRESULT hr = pDevice1->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION,
		__uuidof(ID3D11Device), nullptr, &m_pNgxState);
	if (FAILED(hr)) {
		Fail(State::NoContextState, std::format(L"cannot create a device context state (0x{:08X})", (unsigned)hr));
		return abandon();
	}
	m_pDevice = pDevice;

	if (!FindSnippet(pConfiguredDllPath) || !LoadCore()) {
		return abandon();
	}

	// NGX writes its logs and caches here; the same folder as DLSS 5 NR.
	wchar_t appData[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
		m_DataPath = std::wstring(appData) + L"\\MPC-BE Filters\\MPC Video Renderer\\ngx\\";
		SHCreateDirectoryExW(nullptr, m_DataPath.c_str(), nullptr);
	} else {
		wchar_t tmp[MAX_PATH] = {};
		GetTempPathW((DWORD)std::size(tmp), tmp);
		m_DataPath = tmp;
	}

	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* paths[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = paths;
	fci.PathListInfo.Length = 1;
	fci.LoggingInfo.LoggingCallback = NgxLogCallback;
	fci.LoggingInfo.MinimumLoggingLevel = NGX_LOGGING_ON;
	fci.LoggingInfo.DisableOtherLoggingSinks = false;

	NVSDK_NGX_Result r = NGX_Result_Fail;
	{
		CNgxStateScope scope(m_pContext, m_pNgxState);
		for (const uint32_t version : kSdkVersions) {
			r = m_pfnInitExt(NGX_DLSSNR_APPID, m_DataPath.c_str(), pDevice, version, &fci);
			Log(std::format(L"NGX init, SDK 0x{:02X}: 0x{:08X} {}", version, r, NgxResultName(r)));
			m_SdkVersion = version;
			if (r != NGX_Result_FAIL_OutOfDate) {
				break;
			}
		}
	}
	m_LastResult = r;
	if (NGX_FAILED(r)) {
		Fail(State::InitFailed, std::format(L"NGX init failed: 0x{:08X} {}", r, NgxResultName(r)));
		return abandon();
	}
	m_bInitialised = true;

	r = m_pfnGetCaps(&m_pParams);
	if (NGX_FAILED(r) || !m_pParams) {
		m_pParams = nullptr;
		m_LastResult = r;
		Fail(State::InitFailed, std::format(L"no NGX parameters: 0x{:08X} {}", r, NgxResultName(r)));
		return abandon();
	}

	int available = 0;
	m_pParams->Get("SuperSampling.Available", &available);
	if (!available) {
		int initResult = 0;
		int needsDriver = 0;
		unsigned int minMajor = 0, minMinor = 0;
		m_pParams->Get("SuperSampling.FeatureInitResult", &initResult);
		m_pParams->Get("SuperSampling.NeedsUpdatedDriver", &needsDriver);
		m_pParams->Get("SuperSampling.MinDriverVersionMajor", &minMajor);
		m_pParams->Get("SuperSampling.MinDriverVersionMinor", &minMinor);
		if (needsDriver) {
			Fail(State::NotAvailable, std::format(L"DLSS Super Resolution needs driver {}.{} or newer", minMajor, minMinor));
		} else {
			Fail(State::NotAvailable, std::format(L"DLSS Super Resolution is not available: 0x{:08X} {}",
				(unsigned)initResult, NgxResultName((NVSDK_NGX_Result)initResult)));
		}
		return abandon();
	}

	m_bResetPending = true;
	m_State = State::Ready;
	Log(L"initialised");
	return true;
}

void CDlssSR::Shutdown()
{
	ReleaseFeature();

	if (m_hCore) {
		CNgxStateScope scope(m_pContext, m_pNgxState);
		if (m_pParams && m_pfnDestroy) {
			m_pfnDestroy(m_pParams);
		}
		// Shutdown1(device) crashes inside the runtime after an Init_Ext session;
		// the Direct3D 11 Shutdown without a device ends it cleanly, a later Init
		// works again and a DLSS 5 NR session in the same runtime carries on
		// (measured, tools/dlssnr_probe --tsr).
		if (m_bInitialised && m_pfnShutdown) {
			m_pfnShutdown();
		}
	}
	m_pParams = nullptr;
	m_bInitialised = false;

	if (m_hCore) {
		// LoadCore's reference, given back. The module itself stays: see there.
		FreeLibrary(m_hCore);
		m_hCore = nullptr;
	}
	m_pfnInitExt = nullptr; m_pfnGetCaps = nullptr; m_pfnDestroy = nullptr;
	m_pfnCreate = nullptr; m_pfnEvaluate = nullptr; m_pfnRelease = nullptr; m_pfnShutdown = nullptr;
	m_failedIn[0] = m_failedIn[1] = 0;
	m_failedOut[0] = m_failedOut[1] = 0;

	m_pNgxState.Release();
	m_pContext.Release();
	m_pDevice.Release();
	m_DllPath.clear();
	m_SnippetDir.clear();
	m_SdkVersion = 0;

	if (m_State == State::Ready) {
		m_State = State::Off;
	}
}

// ============================================================================
// Feature and evaluation
// ============================================================================

bool CDlssSR::CreateFeature(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight, unsigned preset)
{
	if (!m_bInitialised || !inWidth || !inHeight || !outWidth || !outHeight) {
		return false;
	}
	if (MatchesFeature(inWidth, inHeight, outWidth, outHeight, preset)) {
		return true;
	}
	if (IsRefused(inWidth, inHeight, outWidth, outHeight, preset)) {
		ReleaseFeature();
		return false;
	}
	ReleaseFeature();
	auto remember = [&]() {
		m_failedIn[0] = inWidth;
		m_failedIn[1] = inHeight;
		m_failedOut[0] = outWidth;
		m_failedOut[1] = outHeight;
		m_failedPreset = preset;
	};

	HRESULT hr = m_TexIn.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, inWidth, inHeight, Tex2D_DefaultShaderRTarget);
	if (SUCCEEDED(hr)) {
		hr = m_TexOut.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, outWidth, outHeight, Tex2D_DefaultShaderRTargetUAV);
	}
	if (SUCCEEDED(hr)) {
		hr = m_TexDepth.CheckCreate(m_pDevice, DXGI_FORMAT_R32_FLOAT, inWidth, inHeight, Tex2D_DefaultShaderRTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = m_TexNoMotion.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16_FLOAT, inWidth, inHeight, Tex2D_DefaultShaderRTarget);
	}
	// A video has no depth: one plane halfway, so nothing reads as sky or as a
	// surface right in front of the camera.
	if (SUCCEEDED(hr)) {
		hr = ClearTexture(m_pDevice, m_pContext, m_TexDepth.pTexture, 0.5f);
	}
	if (SUCCEEDED(hr)) {
		hr = ClearTexture(m_pDevice, m_pContext, m_TexNoMotion.pTexture, 0.0f);
	}
	if (FAILED(hr)) {
		ReleaseFeature();
		remember();
		return Fail(State::TexturesFailed, std::format(L"cannot create the textures for {}x{} -> {}x{} (0x{:08X})",
			inWidth, inHeight, outWidth, outHeight, (unsigned)hr));
	}

	const int mode = QualityModeFor(inWidth, inHeight, outWidth, outHeight);
	m_pParams->Set(P_WIDTH, (unsigned int)inWidth);
	m_pParams->Set(P_HEIGHT, (unsigned int)inHeight);
	m_pParams->Set(P_OUTWIDTH, (unsigned int)outWidth);
	m_pParams->Set(P_OUTHEIGHT, (unsigned int)outHeight);
	m_pParams->Set(P_PERFQUALITY, (int)mode);
	m_pParams->Set(P_CREATEFLAGS, (int)NGX_DLSS_FLAG_MVLowRes);
	m_pParams->Set(P_OUTSUBRECTS, (int)0);
	m_pParams->Set("CreationNodeMask", (unsigned int)1);
	m_pParams->Set("VisibilityNodeMask", (unsigned int)1);
	for (const char* key : kPresetKeys) {
		m_pParams->Set(key, (unsigned int)preset);
	}

	NVSDK_NGX_Result r = NGX_Result_Fail;
	g_PresetByMode[mode] = 0;
	{
		CNgxStateScope scope(m_pContext, m_pNgxState);
		r = m_pfnCreate(m_pContext, NGX_FEATURE_SUPERSAMPLING, m_pParams, &m_hFeature);
	}
	m_LastResult = r;
	if (NGX_FAILED(r) || !m_hFeature) {
		m_hFeature = nullptr;
		ReleaseFeature();
		remember();
		return Fail(State::FeatureCreateFailed, std::format(L"cannot create the feature {}x{} -> {}x{}: 0x{:08X} {}",
			inWidth, inHeight, outWidth, outHeight, r, NgxResultName(r)));
	}
	m_presetUsed = g_PresetByMode[mode];

	m_inWidth   = inWidth;
	m_inHeight  = inHeight;
	m_outWidth  = outWidth;
	m_outHeight = outHeight;
	m_preset    = preset;
	m_mode      = mode;
	m_bResetPending = true;
	m_State = State::Ready;
	m_Detail.clear();
	Log(L"feature " + GetStatusLine().substr(6));   // past "ready "
	return true;
}

void CDlssSR::ReleaseFeature()
{
	if (m_hFeature && m_pfnRelease) {
		CNgxStateScope scope(m_pContext, m_pNgxState);
		m_pfnRelease(m_hFeature);
	}
	m_hFeature = nullptr;
	m_TexIn.Release();
	m_TexOut.Release();
	m_TexDepth.Release();
	m_TexNoMotion.Release();
	m_inWidth = m_inHeight = 0;
	m_outWidth = m_outHeight = 0;
	m_preset = 0;
	m_presetUsed = 0;
}

bool CDlssSR::Evaluate(ID3D11Texture2D* pMotion, float frameTimeMs)
{
	if (!m_hFeature) {
		return false;
	}

	m_pParams->Set(P_COLOR,         (ID3D11Resource*)m_TexIn.pTexture.p);
	m_pParams->Set(P_OUTPUT,        (ID3D11Resource*)m_TexOut.pTexture.p);
	m_pParams->Set(P_DEPTH,         (ID3D11Resource*)m_TexDepth.pTexture.p);
	m_pParams->Set(P_MOTIONVECTORS, (ID3D11Resource*)(pMotion ? pMotion : m_TexNoMotion.pTexture.p));
	m_pParams->Set(P_JITTERX, 0.0f);
	m_pParams->Set(P_JITTERY, 0.0f);
	m_pParams->Set(P_SHARPNESS, 0.0f);
	m_pParams->Set(P_RESET, (int)(m_bResetPending ? 1 : 0));
	m_pParams->Set(P_MVSCALEX, 1.0f);
	m_pParams->Set(P_MVSCALEY, 1.0f);
	m_pParams->Set(P_RENDERWIDTH, (unsigned int)m_inWidth);
	m_pParams->Set(P_RENDERHEIGHT, (unsigned int)m_inHeight);
	m_pParams->Set(P_PREEXPOSURE, 1.0f);
	m_pParams->Set(P_EXPOSURESCALE, 1.0f);
	m_pParams->Set(P_FRAMETIME, frameTimeMs);

	NVSDK_NGX_Result r = NGX_Result_Fail;
	{
		CNgxStateScope scope(m_pContext, m_pNgxState);
		r = m_pfnEvaluate(m_pContext, m_hFeature, m_pParams, nullptr);
	}
	m_LastResult = r;
	if (NGX_FAILED(r)) {
		return Fail(State::EvaluateFailed, std::format(L"evaluation failed: 0x{:08X} {}", r, NgxResultName(r)));
	}
	m_bResetPending = false;
	return true;
}

// ============================================================================
// Reporting
// ============================================================================

std::wstring CDlssSR::GetStatusLine() const
{
	switch (m_State) {
	case State::Off:
		return L"disabled";
	case State::Ready:
		if (!m_hFeature) {
			return L"initialised";
		} else {
			// J..M are presets 10..13; Default names the model DLSS picked when its log said.
			std::wstring preset;
			if (m_preset >= NGX_DLSS_PRESET_J && m_preset <= NGX_DLSS_PRESET_M) {
				preset = std::wstring(1, (wchar_t)(L'J' + (m_preset - NGX_DLSS_PRESET_J)));
			} else if (m_presetUsed) {
				preset = std::wstring(1, m_presetUsed) + L" (default)";
			} else {
				preset = L"default";
			}
			return std::format(L"ready {}x{} -> {}x{}, {}, preset {}", m_inWidth, m_inHeight, m_outWidth, m_outHeight,
				QualityModeName(m_mode), preset);
		}
	default:
		return m_Detail.empty() ? std::format(L"unavailable: 0x{:08X} {}", m_LastResult, NgxResultName(m_LastResult)) : m_Detail;
	}
}

std::wstring CDlssSR::GetStatsLine() const
{
	if (m_State != State::Ready || !m_hFeature) {
		return GetStatusLine();
	}
	const wchar_t* mode = L"?";
	switch (m_mode) {
	case NGX_PERFQUALITY_MaxPerf:          mode = L"Perf";  break;
	case NGX_PERFQUALITY_Balanced:         mode = L"Bal";   break;
	case NGX_PERFQUALITY_MaxQuality:       mode = L"Qual";  break;
	case NGX_PERFQUALITY_UltraPerformance: mode = L"UPerf"; break;
	case NGX_PERFQUALITY_UltraQuality:     mode = L"UQual"; break;
	case NGX_PERFQUALITY_DLAA:             mode = L"DLAA";  break;
	}
	wchar_t preset = L'?';
	if (m_preset >= NGX_DLSS_PRESET_J && m_preset <= NGX_DLSS_PRESET_M) {
		preset = (wchar_t)(L'J' + (m_preset - NGX_DLSS_PRESET_J));
	} else if (m_presetUsed) {
		preset = m_presetUsed;
	}
	return std::format(L"{}x{}->{}x{} {}/{}", m_inWidth, m_inHeight, m_outWidth, m_outHeight, mode, preset);
}

std::wstring CDlssSR::GetInfoBlock() const
{
	std::wstring s = L"DLSS Super Resolution:\n";
	s += std::format(L"  DLL       : {}\n", m_DllPath.empty() ? L"<not found>" : m_DllPath);
	if (m_SdkVersion) {
		s += std::format(L"  Session   : Direct3D 11, driver NGX runtime, SDK 0x{:02X}\n", m_SdkVersion);
	}
	s += std::format(L"  State     : {}\n", GetStatusLine());

	std::vector<std::wstring> lines(m_Log.begin(), m_Log.end());
	{
		std::lock_guard lock(g_NgxLogLock);
		const size_t from = g_NgxLog.size() > 8 ? g_NgxLog.size() - 8 : 0;
		for (size_t i = from; i < g_NgxLog.size(); i++) {
			lines.push_back(L"NGX: " + g_NgxLog[i]);
		}
	}
	if (!lines.empty()) {
		s += L"  Log       :\n";
		const size_t from = lines.size() > 16 ? lines.size() - 16 : 0;
		for (size_t i = from; i < lines.size(); i++) {
			s += L"    " + lines[i] + L"\n";
		}
	}
	return s;
}
