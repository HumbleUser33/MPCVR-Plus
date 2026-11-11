/*
 * NGX ABI declarations for the DLSS 5 Neural Rendering snippet (feature 18).
 *
 * Hand-declared rather than vendored: we need a fraction of the SDK surface,
 * and we implement NVSDK_NGX_Parameter instead of consuming it, because the
 * snippet exports only PopulateParameters_Impl -- the allocator lives in the
 * driver core.
 *
 * The parameter vtable layout below was verified experimentally against
 * nvngx_dlssnr.dll 310.8.0.0: PopulateParameters_Impl calls slot 7
 * (Set(const char*, void*)) twice, to register DLSSNRComputeScalingRatioCallback
 * and DLSSNRGetStatsCallback. Do not reorder, and do not add a virtual
 * destructor -- it would take slot 0 under MSVC and shift every entry.
 */

#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct IDXGIAdapter;

// ---------------------------------------------------------------- results

typedef uint32_t NVSDK_NGX_Result;

enum : NVSDK_NGX_Result {
	NGX_Result_Success                       = 0x00000001,
	NGX_Result_Fail                          = 0xBAD00000,
	NGX_Result_FAIL_FeatureNotSupported       = 0xBAD00001,
	NGX_Result_FAIL_PlatformError             = 0xBAD00002, // also: caller validation
	NGX_Result_FAIL_FeatureAlreadyExists      = 0xBAD00003,
	NGX_Result_FAIL_FeatureNotFound           = 0xBAD00004,
	NGX_Result_FAIL_InvalidParameter          = 0xBAD00005,
	NGX_Result_FAIL_ScratchBufferTooSmall     = 0xBAD00006,
	NGX_Result_FAIL_NotInitialized            = 0xBAD00007,
	NGX_Result_FAIL_UnsupportedInputFormat    = 0xBAD00008,
	NGX_Result_FAIL_RWFlagMissing             = 0xBAD00009,
	NGX_Result_FAIL_MissingInput              = 0xBAD0000A,
	NGX_Result_FAIL_UnableToInitializeFeature = 0xBAD0000B,
	NGX_Result_FAIL_OutOfDate                 = 0xBAD0000C,
	NGX_Result_FAIL_OutOfGPUMemory            = 0xBAD0000D,
	NGX_Result_FAIL_UnsupportedFormat         = 0xBAD0000E,
	NGX_Result_FAIL_UnableToWriteToAppDataPath= 0xBAD0000F,
	NGX_Result_FAIL_UnsupportedParameter      = 0xBAD00010,
	NGX_Result_FAIL_Denied                    = 0xBAD00011,
	NGX_Result_FAIL_NotImplemented            = 0xBAD00012,
};

#define NGX_SUCCEED(r) (((r) & 0xFFF00000) != 0xBAD00000)
#define NGX_FAILED(r)  (((r) & 0xFFF00000) == 0xBAD00000)

const wchar_t* NgxResultName(NVSDK_NGX_Result r);

// ---------------------------------------------------------------- constants

constexpr uint32_t NGX_FEATURE_DLSSNR = 18;

// The snippet reports API version 0x13; the driver core rejects anything newer
// with FAIL_OutOfDate. Queried at runtime, this is only the fallback.
constexpr uint32_t NGX_SDK_VERSION_FALLBACK = 0x13;

constexpr unsigned long long NGX_DLSSNR_APPID = 141959980ull;

// DLSS Super Resolution, through the driver core.
constexpr uint32_t NGX_FEATURE_SUPERSAMPLING = 1;

// NVSDK_NGX_PerfQuality_Value: the scale a feature is tuned for.
enum : int {
	NGX_PERFQUALITY_MaxPerf          = 0,   // 2x
	NGX_PERFQUALITY_Balanced         = 1,   // 1.72x
	NGX_PERFQUALITY_MaxQuality       = 2,   // 1.5x
	NGX_PERFQUALITY_UltraPerformance = 3,   // 3x
	NGX_PERFQUALITY_UltraQuality     = 4,
	NGX_PERFQUALITY_DLAA             = 5,   // 1x
};

// NVSDK_NGX_DLSS_Feature_Flags.
enum : int {
	NGX_DLSS_FLAG_IsHDR         = 1 << 0,
	NGX_DLSS_FLAG_MVLowRes      = 1 << 1,   // vectors at the input size
	NGX_DLSS_FLAG_MVJittered    = 1 << 2,
	NGX_DLSS_FLAG_DepthInverted = 1 << 3,
	NGX_DLSS_FLAG_AutoExposure  = 1 << 6,
};

// NVSDK_NGX_DLSS_Hint_Render_Preset: 0 lets DLSS choose for the scale; J and K
// are the first transformer models, L and M the second (DLSS 4.5).
enum : unsigned int {
	NGX_DLSS_PRESET_Default = 0,
	NGX_DLSS_PRESET_J = 10,
	NGX_DLSS_PRESET_K = 11,
	NGX_DLSS_PRESET_L = 12,
	NGX_DLSS_PRESET_M = 13,
};

// ------------------------------------------------------------------ structs

struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_PathListInfo {
	const wchar_t* const* Path;
	unsigned int          Length;
};

enum NVSDK_NGX_Logging_Level : uint32_t {
	NGX_LOGGING_OFF = 0, NGX_LOGGING_ON = 1, NGX_LOGGING_VERBOSE = 2
};

typedef void (__cdecl* NVSDK_NGX_AppLogCallback)(const char* message,
	NVSDK_NGX_Logging_Level level, uint32_t sourceComponent);

struct NVSDK_NGX_LoggingInfo {
	NVSDK_NGX_AppLogCallback LoggingCallback;
	NVSDK_NGX_Logging_Level  MinimumLoggingLevel;
	bool                     DisableOtherLoggingSinks;
};

struct NVSDK_NGX_FeatureCommonInfo {
	NVSDK_NGX_PathListInfo PathListInfo;
	void*                  InternalData;
	NVSDK_NGX_LoggingInfo  LoggingInfo;   // added in SDK 0x14; harmless at 0x13
};

// Feature discovery -- advisory. On a modded snippet this still reports the
// stock minimum architecture, so it is logged but never used as a gate.
enum NVSDK_NGX_Application_Identifier_Type : uint32_t {
	NGX_AppIdType_Application = 0, NGX_AppIdType_Project = 1
};

struct NVSDK_NGX_ProjectIdDescription {
	const char* ProjectId;
	uint32_t    EngineType;
	const char* EngineVersion;
};

struct NVSDK_NGX_Application_Identifier {
	NVSDK_NGX_Application_Identifier_Type IdentifierType;
	union {
		NVSDK_NGX_ProjectIdDescription ProjectDesc;
		unsigned long long             ApplicationId;
	} v;
};

struct NVSDK_NGX_FeatureDiscoveryInfo {
	uint32_t                           SDKVersion;
	uint32_t                           FeatureID;
	NVSDK_NGX_Application_Identifier   Identifier;
	const wchar_t*                     ApplicationDataPath;
	const NVSDK_NGX_FeatureCommonInfo* FeatureInfo;
};

enum : uint32_t {
	NGX_FeatureSupport_Supported            = 0,
	NGX_FeatureSupport_CheckNotPresent      = 1,
	NGX_FeatureSupport_DriverVersionUnsupported = 2,
	NGX_FeatureSupport_AdapterUnsupported   = 4,
	NGX_FeatureSupport_OSVersionBelowMinimum= 8,
	NGX_FeatureSupport_NotImplemented       = 16,
};

struct NVSDK_NGX_FeatureRequirement {
	uint32_t     FeatureSupported;   // bitfield of the above; 0 == supported
	unsigned int MinHWArchitecture;  // NV_GPU_ARCHITECTURE_ID
	char         MinOSVersion[255];
};

// --------------------------------------------------- the parameter interface

class NVSDK_NGX_Parameter
{
public:
	virtual void Set(const char* n, unsigned long long v) = 0;                    //  0
	virtual void Set(const char* n, float v) = 0;                                 //  1
	virtual void Set(const char* n, double v) = 0;                                //  2
	virtual void Set(const char* n, unsigned int v) = 0;                          //  3
	virtual void Set(const char* n, int v) = 0;                                   //  4
	virtual void Set(const char* n, ID3D11Resource* v) = 0;                       //  5
	virtual void Set(const char* n, ID3D12Resource* v) = 0;                       //  6
	virtual void Set(const char* n, void* v) = 0;                                 //  7
	virtual NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const = 0; //  8
	virtual NVSDK_NGX_Result Get(const char* n, float* o) const = 0;              //  9
	virtual NVSDK_NGX_Result Get(const char* n, double* o) const = 0;             // 10
	virtual NVSDK_NGX_Result Get(const char* n, unsigned int* o) const = 0;       // 11
	virtual NVSDK_NGX_Result Get(const char* n, int* o) const = 0;                // 12
	virtual NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const = 0;    // 13
	virtual NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const = 0;    // 14
	virtual NVSDK_NGX_Result Get(const char* n, void** o) const = 0;              // 15
	virtual void Reset() = 0;                                                     // 16
};

// ------------------------------------------------------------ entry points
// Snippet ABI (NGX_SNIPPET_BUILD flavour): version is the 4th argument.

typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_Init_Ext)(
	unsigned long long appId, const wchar_t* appDataPath, ID3D11Device* dev,
	uint32_t sdkVersion, const void* reserved);

// App-side ABI, used for the driver core: FeatureCommonInfo 4th, version 5th.
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_Init_Core)(
	unsigned long long appId, const wchar_t* appDataPath, ID3D11Device* dev,
	const NVSDK_NGX_FeatureCommonInfo* featureInfo, uint32_t sdkVersion);

typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_Shutdown1)(ID3D11Device* dev);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_PopulateParameters)(NVSDK_NGX_Parameter* p);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_AllocateParameters)(NVSDK_NGX_Parameter** out);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_DestroyParameters)(NVSDK_NGX_Parameter* p);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_CreateFeature)(
	ID3D11DeviceContext* ctx, uint32_t featureId,
	NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_EvaluateFeature)(
	ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* h,
	const NVSDK_NGX_Parameter* p, void* progressCallback);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_ReleaseFeature)(NVSDK_NGX_Handle* h);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_GetFeatureRequirements)(
	IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* info,
	NVSDK_NGX_FeatureRequirement* out);
typedef uint32_t (__cdecl* PFN_NGX_GetU32)(void);

// The driver core's D3D11 session, for DLSS Super Resolution: the core loads
// nvngx_dlss.dll itself from the paths in FeatureCommonInfo. Its Init_Ext takes
// the snippet ordering, PFN_NGX_D3D11_Init_Ext above; its plain Init answers
// OutOfDate once a current snippet is on the path, and its Init_ProjectID with the
// SDK's documented argument order crashes (measured, tools/dlssnr_probe --tsr).
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_GetCapabilityParameters)(NVSDK_NGX_Parameter** out);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_Shutdown)(void);

// D3D12 is the only backend that actually works in this snippet build: its
// D3D11 entry points refuse without ever querying the driver.
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_Init_Ext)(
	unsigned long long appId, const wchar_t* appDataPath, ID3D12Device* dev,
	uint32_t sdkVersion, const void* reserved);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_Shutdown1)(ID3D12Device* dev);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_PopulateParameters)(NVSDK_NGX_Parameter* p);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_CreateFeature)(
	ID3D12GraphicsCommandList* list, uint32_t featureId,
	NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_EvaluateFeature)(
	ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* h,
	const NVSDK_NGX_Parameter* p, void* progressCallback);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_ReleaseFeature)(NVSDK_NGX_Handle* h);
// The driver core's own Shutdown1 takes a second argument, where it writes a word the
// DLSS SDK's library reads after it (zero: nothing of NGX left to keep). Called with the
// device alone, it wrote through whatever the register held: 0xC0000005 in _nvngx.dll as
// the renderer closed, every time.
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D12_Shutdown1_Core)(ID3D12Device* dev, uint32_t* out);

// ------------------------------------------------------------------- NVAPI
// The snippet asks NVAPI for the GPU architecture and refuses anything below
// its minimum. NGXCubinGeneric::SetGPUArch reads it through GetArchInfo.

typedef void*    (__cdecl* PFN_nvapi_QueryInterface)(unsigned int id);
typedef int      (__cdecl* PFN_NvAPI_Initialize)(void);

struct NV_GPU_ARCH_INFO {
	unsigned int version;
	unsigned int architecture;    // NV_GPU_ARCHITECTURE_ID
	unsigned int implementation;
	unsigned int revision;
};
typedef int (__cdecl* PFN_NvAPI_GPU_GetArchInfo)(void* hGpu, NV_GPU_ARCH_INFO* info);

constexpr unsigned int NVAPI_ID_Initialize      = 0x0150E828;
constexpr unsigned int NVAPI_ID_GPU_GetArchInfo = 0xD8265D24;

// NV_GPU_ARCHITECTURE_ID: Turing 0x160, Ampere 0x170, Ada 0x190, Blackwell 0x1B0
constexpr unsigned int NV_GPU_ARCH_BLACKWELL = 0x1B0;

// -------------------------------------------------------------- shim ABI
// nvngx_dlssnr.dll refuses any caller whose module path does not contain
// "nvngx.dll" (0xBAD00002, logged as "Not called from NGX runtime"). These
// trampolines live in a module with that name and forward the real call.

typedef NVSDK_NGX_Result (__cdecl* PFN_ShimInit)(
	void* fn, unsigned long long appId, const wchar_t* path, void* dev,
	uint32_t ver, const void* fci);
typedef NVSDK_NGX_Result (__cdecl* PFN_ShimPopulate)(void* fn, void* params);
typedef NVSDK_NGX_Result (__cdecl* PFN_ShimCreate)(
	void* fn, void* ctx, uint32_t featureId, void* params, void** outHandle);
typedef NVSDK_NGX_Result (__cdecl* PFN_ShimEval)(
	void* fn, void* ctx, const void* handle, const void* params, void* cb);
typedef NVSDK_NGX_Result (__cdecl* PFN_ShimRelease)(void* fn, void* handle);
typedef NVSDK_NGX_Result (__cdecl* PFN_ShimShutdown1)(void* fn, void* dev);
typedef NVSDK_NGX_Result (__cdecl* PFN_ShimGetReq)(
	void* fn, void* adapter, const void* info, void* out);
