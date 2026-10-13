/*
 * CDlssNR -- DLSS 5 Neural Rendering (NGX feature 18).
 *
 * Self-contained: knows nothing about MPC Video Renderer. Never throws, never
 * lets a failure reach the render path -- if anything goes wrong the object
 * reports Unavailable and the caller simply skips the pass.
 *
 * Why there is a D3D12 device in here
 * -----------------------------------
 * The snippet exports a complete D3D11 surface -- Init/CreateFeature/Evaluate,
 * an NGXCubinD3D11 class, NVAPI cubin calls -- and every one of them refuses
 * with FAIL_FeatureNotSupported. Measured, not assumed: during nine D3D11 init
 * attempts the snippet issued ZERO NVAPI queries, so it is not a driver gap. It
 * bails before initialising its own logging. The D3D12 path, on the same device
 * in the same process, initialises, creates the feature and evaluates.
 *
 * So the renderer keeps its D3D11 textures and we open them a second time on a
 * private D3D12 device, sharing through NT handles.
 *
 * Synchronisation is CPU-side on purpose
 * --------------------------------------
 * An earlier version let the two devices wait on each other's fences on the
 * GPU. That made it impossible to know when the command allocator was free,
 * and resetting it while the GPU was still executing removed the device
 * (DXGI_ERROR_DEVICE_REMOVED) -- which then showed up as black frames, frozen
 * pictures and sharing failures. Both directions now block on the CPU: two
 * short stalls per frame, one ordering rule, no way to get it subtly wrong.
 *
 * Bring-up order, all of it established empirically, and the order Init keeps:
 *   1. hook NvAPI_GPU_GetArchInfo before anything loads (see m_bArchOverride)
 *   2. load the snippet (nvngx_dlssnr.dll); it says which SDK version it speaks
 *   3. load the caller shim (nvngx.dll); every guarded entry point goes through it
 *   4. make the private D3D12 device and the two fences shared with the renderer's
 *   5. load the driver's runtime (_nvngx.dll), initialise it on that device and
 *      take a parameter block from it, else use our own
 *   6. initialise the snippet on the D3D12 device
 * The feature is created afterwards, when the renderer has its textures.
 *
 * What is unloaded and what is not
 * --------------------------------
 * Shutdown frees all three libraries: the snippet is a 165 MB module with half a
 * gigabyte of GPU memory behind it, and nothing of it is kept once the feature is
 * off for good. That is safe here and was not for DLSS Super Resolution (see
 * CDlssSR::LoadCore): this snippet is loaded and freed by us, so nothing outlives
 * the runtime it was given. Checked in MPC-HC with each of the four modules made to
 * come back at another address at every film -- _nvngx.dll, nvngx_dlssnr.dll,
 * nvngx.dll and nvofapi64.dll, six or seven times each: no fault.
 */

#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include "NGXTypes.h"

// ---------------------------------------------------------- parameter store
// The snippet exports PopulateParameters_Impl but no allocator -- that lives in
// the driver core. This is the fallback for when the core will not hand us one.
// Deliberately type-tolerant: a wrong vtable slot then yields a wrong value
// rather than a bad write, which keeps the failure diagnosable.

class CNgxParameterStore final : public NVSDK_NGX_Parameter
{
public:
	enum class Kind : uint8_t { None, U64, F32, F64, U32, I32, Res11, Res12, Ptr };

	void Set(const char* n, unsigned long long v) override;
	void Set(const char* n, float v) override;
	void Set(const char* n, double v) override;
	void Set(const char* n, unsigned int v) override;
	void Set(const char* n, int v) override;
	void Set(const char* n, ID3D11Resource* v) override;
	void Set(const char* n, ID3D12Resource* v) override;
	void Set(const char* n, void* v) override;

	NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override;
	NVSDK_NGX_Result Get(const char* n, float* o) const override;
	NVSDK_NGX_Result Get(const char* n, double* o) const override;
	NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override;
	NVSDK_NGX_Result Get(const char* n, int* o) const override;
	NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override;
	NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override;
	NVSDK_NGX_Result Get(const char* n, void** o) const override;

	void Reset() override;
	size_t Count() const { return m_map.size(); }

private:
	struct Slot { Kind kind = Kind::None; uint64_t u64 = 0; double f64 = 0; void* ptr = nullptr; };
	Slot& Put(const char* n);
	const Slot* Find(const char* n) const;

	std::map<std::string, Slot> m_map;
};

// --------------------------------------------------------------- the wrapper

class CDlssNR
{
public:
	// Tunables pushed on every Evaluate. Floats here, scaled ints in Settings_t.
	struct Params {
		int   iStyle           = 0;     // 0 default, 1 natural, 2 cinematic
		int   iPreset          = 0;     // 0..3 (this DLL build only ships one)
		float fIntensity       = 1.0f;  // 0..2
		float fLocalTone       = 1.0f;  // 0..2
		float fLocalStructure  = 1.0f;  // 0..2
		float fSkinStructure   = 1.0f;  // -1..2
		bool  bUseAutoMask     = true;
		// Force DLSSNR.Reset every frame. The network blends with its own
		// previous output; with no motion vectors to align that history, the
		// mismatch reads as a luminance shimmer on anything that moves.
		bool  bNoHistory       = true;

		bool operator==(const Params&) const = default;
	};

	// Where the time of the last Evaluate went, in ms: waiting for Direct3D 11 to
	// finish writing the input, recording the network's work, and waiting for the
	// GPU to run it.
	struct Timing {
		double inputWaitMs = 0;
		double recordMs    = 0;
		double gpuWaitMs   = 0;
	};
	const Timing& LastTiming() const { return m_Timing; }

	enum class State {
		Off, DllNotFound, DllLoadFailed, ExportsMissing, ShimMissing,
		NoD3D12, ApiInitFailed, ShareFailed, FeatureCreateFailed,
		EvaluateFailed, DeviceLost, Ready
	};

	CDlssNR() = default;
	~CDlssNR();
	CDlssNR(const CDlssNR&) = delete;
	CDlssNR& operator=(const CDlssNR&) = delete;

	// Loads the libraries, builds the private D3D12 device on the same adapter
	// as pDevice, and brings up the NGX session. false on any failure.
	bool Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath, bool bArchOverride);
	void Shutdown();

	// Opens the caller's D3D11 textures on the D3D12 side and creates the
	// resolution-bound feature. Both textures must have been created with
	// Tex2D_DefaultShaderRTargetUAVShared at exactly w x h, RGBA16F.
	bool CreateFeature(ID3D11Texture2D* pShared11In, ID3D11Texture2D* pShared11Out,
	                   UINT w, UINT h, const Params& p);
	void ReleaseFeature();

	// One frame. The caller must already have written the input texture on the
	// D3D11 timeline; this handles the cross-device synchronisation both ways.
	bool Evaluate(const Params& p);

	// Next Evaluate will set DLSSNR.Reset -- call on seek/flush.
	void RequestReset() { m_bResetPending = true; }

	// Optional guide inputs, pushed on every Evaluate. Each texture needs an NT
	// share handle (Tex2D_DefaultShaderRTargetUAVShared) and should match the
	// feature size; a null pointer means "not provided".
	//   pMVec         RG16F, per-pixel motion from the current frame to the
	//                 previous one, multiplied by fMVecScaleX/Y
	//   pDepth        R32F
	//   pControlMask  R8 -- a private snippet input whose meaning is measured, not
	//                 documented anywhere
	struct Guides {
		ID3D11Texture2D* pMVec        = nullptr;
		ID3D11Texture2D* pDepth       = nullptr;
		ID3D11Texture2D* pControlMask = nullptr;
		float fMVecScaleX = 1.0f;
		float fMVecScaleY = 1.0f;
	};
	bool SetGuides(const Guides& g);   // false if a texture could not be shared
	bool HasMotionVectors() const { return m_GuideMVec.p12 != nullptr; }
	bool HasControlMask() const { return m_GuideMask.p12 != nullptr; }

	bool IsInitialised() const { return m_bInitialised; }
	bool IsFeatureReady() const { return m_pFeature != nullptr; }
	bool MatchesFeature(UINT w, UINT h, int preset) const {
		return m_pFeature && m_featW == w && m_featH == h && m_featPreset == preset;
	}
	// Same dimensions is not the same texture: identity has to match too.
	bool MatchesTextures(ID3D11Texture2D* pIn, ID3D11Texture2D* pOut) const {
		return m_pShared11In == pIn && m_pShared11Out == pOut && m_pTex12In && m_pTex12Out;
	}

	State            GetState() const { return m_State; }
	NVSDK_NGX_Result GetLastResult() const { return m_LastResult; }
	std::wstring     GetStatusLine() const;   // one line, for the property page
	std::wstring     GetInfoBlock() const;    // multi-line, for the Information page
	const std::wstring& GetDllPath() const { return m_DllPath; }

	static std::vector<std::wstring> CandidateDllPaths(const wchar_t* pConfigured);

private:
	bool LoadCore();
	bool LoadSnippet(const wchar_t* pConfiguredDllPath);
	bool LoadShim();
	bool CreateD3D12(ID3D11Device* pDevice);
	bool InstallArchOverride();
	void QueryRequirements();
	void PushEvaluateParams(NVSDK_NGX_Parameter* p, const Params& s, bool bReset);
	bool ExecuteAndWait();
	void DrainGpu();
	bool WaitForFence(ID3D12Fence* pFence, UINT64 value);
	bool CheckDeviceLost();
	void Log(const wchar_t* fmt, ...);

	// guarded calls -- always via the shim when it is loaded
	NVSDK_NGX_Result CallPopulate(NVSDK_NGX_Parameter* p);
	NVSDK_NGX_Result CallInit();
	NVSDK_NGX_Result CallCreate(NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out);
	NVSDK_NGX_Result CallEvaluate(const NVSDK_NGX_Parameter* p);
	NVSDK_NGX_Result CallRelease(NVSDK_NGX_Handle* h);
	NVSDK_NGX_Result CallShutdown1();

	HMODULE m_hCore    = nullptr;   // _nvngx.dll
	HMODULE m_hSnippet = nullptr;   // nvngx_dlssnr.dll
	HMODULE m_hShim    = nullptr;   // nvngx.dll

	PFN_NGX_D3D12_Init_Ext            m_pfnInit      = nullptr;
	PFN_NGX_D3D12_PopulateParameters  m_pfnPopulate  = nullptr;
	PFN_NGX_D3D12_CreateFeature       m_pfnCreate    = nullptr;
	PFN_NGX_D3D12_EvaluateFeature     m_pfnEvaluate  = nullptr;
	PFN_NGX_D3D12_ReleaseFeature      m_pfnRelease   = nullptr;
	PFN_NGX_D3D12_Shutdown1           m_pfnShutdown1 = nullptr;
	PFN_NGX_D3D11_GetFeatureRequirements m_pfnGetReq = nullptr;

	PFN_NGX_D3D11_AllocateParameters m_pfnCoreAlloc   = nullptr;
	PFN_NGX_D3D11_DestroyParameters  m_pfnCoreDestroy = nullptr;

	PFN_ShimInit      m_pfnShimInit     = nullptr;
	PFN_ShimPopulate  m_pfnShimPopulate = nullptr;
	PFN_ShimCreate    m_pfnShimCreate   = nullptr;
	PFN_ShimEval      m_pfnShimEval     = nullptr;
	PFN_ShimRelease   m_pfnShimRelease  = nullptr;
	PFN_ShimShutdown1 m_pfnShimShutdown = nullptr;

	NVSDK_NGX_Parameter* m_pParams     = nullptr;  // core-owned or m_OwnParams
	CNgxParameterStore   m_OwnParams;
	bool                 m_bParamsFromCore = false;
	NVSDK_NGX_Handle*    m_pFeature = nullptr;

	// D3D11 side (borrowed from the renderer, addref'd)
	CComPtr<ID3D11Device5>        m_pDev11;
	CComPtr<ID3D11DeviceContext4> m_pCtx11;

	// private D3D12 side
	CComPtr<ID3D12Device>              m_pDev12;
	CComPtr<ID3D12CommandQueue>        m_pQueue;
	CComPtr<ID3D12CommandAllocator>    m_pAlloc;
	CComPtr<ID3D12GraphicsCommandList> m_pList;

	// shared textures: created on D3D11, opened here
	CComPtr<ID3D12Resource> m_pTex12In;
	CComPtr<ID3D12Resource> m_pTex12Out;
	// Not owned, never dereferenced -- kept only to notice that the renderer
	// swapped its textures out from under us.
	ID3D11Texture2D* m_pShared11In  = nullptr;
	ID3D11Texture2D* m_pShared11Out = nullptr;

	// Optional guide inputs, shared the same way as the colour pair.
	struct GuideSlot {
		CComPtr<ID3D12Resource> p12;
		ID3D11Texture2D* p11 = nullptr;   // identity only, never dereferenced
		UINT w = 0, h = 0;
	};
	GuideSlot m_GuideMVec;
	GuideSlot m_GuideDepth;
	GuideSlot m_GuideMask;
	float m_fMVecScaleX = 1.0f;
	float m_fMVecScaleY = 1.0f;

	bool OpenOnD3D12(const wchar_t* which, ID3D11Texture2D* pTex11, CComPtr<ID3D12Resource>& out);
	bool SetGuideSlot(GuideSlot& slot, const wchar_t* which, ID3D11Texture2D* pTex11);
	void PushGuideParams(NVSDK_NGX_Parameter* p);

	// Two shared fences, one per direction. Each pair is a single fence object
	// seen from both devices.
	CComPtr<ID3D11Fence> m_pFenceUp11;     // created on D3D11, signalled there
	CComPtr<ID3D12Fence> m_pFenceUp12;     // the same fence, waited on by D3D12
	CComPtr<ID3D12Fence> m_pFenceDown12;   // created on D3D12, signalled there
	CComPtr<ID3D11Fence> m_pFenceDown11;   // the same fence, waited on by D3D11
	UINT64 m_FenceValue = 0;
	HANDLE m_hFenceEvent = nullptr;

	std::wstring m_DllPath;
	std::wstring m_DataPath;
	std::wstring m_SnippetDir;

	uint32_t m_SdkVersion   = NGX_SDK_VERSION_FALLBACK;
	uint32_t m_SnippetVer   = 0;
	uint32_t m_ReqSupported = 0;
	uint32_t m_ReqMinArch   = 0;
	bool     m_bHaveReq     = false;

	UINT m_featW = 0, m_featH = 0;
	int  m_featPreset = -1;
	Timing m_Timing;

	bool m_bInitialised  = false;
	bool m_bUseShim      = false;
	bool m_bResetPending = true;
	bool m_bArchOverride = false;
	bool m_bHoldsArchOverride = false;   // this session armed the hook and has yet to give it back

	std::wstring     m_DetailError;   // what exactly went wrong, for the UI
	State            m_State      = State::Off;
	NVSDK_NGX_Result m_LastResult = 0;

	std::deque<std::wstring> m_Log;
};
