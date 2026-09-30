/*
* (C) 2018-2026 see Authors.txt
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

#pragma once

#include <DXGI1_2.h>
#include <dxva2api.h>
#include <dxgi1_5.h>
#include <strmif.h>
#include <map>
#include "IVideoRenderer.h"
#include "DX11Helper.h"
#include "D3D11VP.h"
#include "D3DUtil/D3D11Font.h"
#include "D3DUtil/D3D11Geometry.h"
#include "DLSS/DlssStabilizer.h"
#include "DLSS/DlssNR.h"
#include "DLSS/DlssSR.h"
#include "DLSS/DlssTiming.h"
#include "Upscale/MpvShader.h"
#include "VideoProcessor.h"
#include "SubPic/DX11SubPic.h"

#include <atomic>

#define TEST_SHADER 0

class CVideoRendererInputPin;

class CDX11VideoProcessor
	: public CVideoProcessor
{
private:
	friend class CVideoRendererInputPin;

	// Direct3D 11
	CComPtr<ID3D11Device1>        m_pDevice;
	CComPtr<ID3D11DeviceContext1> m_pDeviceContext;
	CComPtr<ID3D11SamplerState>   m_pSamplerPoint;
	CComPtr<ID3D11SamplerState>   m_pSamplerLinear;
	CComPtr<ID3D11SamplerState>   m_pSamplerDither;
	CComPtr<ID3D11BlendState>     m_pAlphaBlendState;
	CComPtr<ID3D11VertexShader>   m_pVS_Simple;
	CComPtr<ID3D11PixelShader>    m_pPS_Simple;
	CComPtr<ID3D11PixelShader>    m_pPS_BitmapToFrame;
	CComPtr<ID3D11InputLayout>    m_pVSimpleInputLayout;
	CComPtr<ID3D11Buffer>         m_pVertexBuffer;
	CComPtr<ID3D11Buffer>         m_pResizeShaderConstantBuffer;
	CComPtr<ID3D11Buffer>         m_pHalfOUtoInterlaceConstantBuffer;
	CComPtr<ID3D11Buffer>         m_pFinalPassConstantBuffer;

	DXGI_SWAP_EFFECT              m_UsedSwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	// A window that has carried a flip model swap chain keeps it: Windows never takes
	// one back to the older model, and a picture presented the old way into such a
	// window never reaches the screen.
	HWND                          m_hWndFlipModel = nullptr;

#if TEST_SHADER
	CComPtr<ID3D11PixelShader>    m_pPS_TEST;
#endif

	Tex11Video_t m_TexSrcVideo; // for copy of frame
	Tex2D_t m_TexConvertOutput;
	Tex2D_t m_TexResize;        // for intermediate result of two-pass resize
	CTex2DRing m_TexsPostScale;
	Tex2D_t m_TexDither;
	Tex2D_t m_TexDlssIn;  // RGBA16F copy of the converted frame, only when a format change is needed
	Tex2D_t m_TexDlssOut; // RGBA16F NGX output, always UAV-capable

	// for GetAlignmentSize()
	struct Alignment_t {
		Tex11Video_t texture;
		ColorFormat_t cformat = {};
		LONG cx = {};
	} m_Alignment;

	// D3D11 Video Processor
	CD3D11VP m_D3D11VP;

	CComPtr<ID3D11Buffer> m_pCorrectionConstants;
	CComPtr<ID3D11PixelShader> m_pPSCorrection;
	const wchar_t* m_strCorrection = nullptr;

	// HDR tonemapping
	struct HDRParamsConstantBuffer_t {
		float MasteringMinLuminanceNits;
		float MasteringMaxLuminanceNits;
		float maxCLL;
		float maxFALL;
		float displayMaxNits;
		UINT selection; // 1 = ACES, 2 = Reinhard, 3 = Habel, 4 = Möbius, 5 = BT2390, 6 = ST 2094-10
		float padding[2];
	};
	struct DoViDynamicConstantsBuffer_t {
		float trim_chroma_weight;
		float trim_saturation_gain;
		float trim_slope;
		float trim_offset;
		float trim_power;
		UINT enabled;
		float padding[2];
	};
	HDRParamsConstantBuffer_t m_lastHDRParamsConstantBuffer = {};
	DoViDynamicConstantsBuffer_t m_lastDoViDynamicConstantsBuffer = {};
	CComPtr<ID3D11Buffer> m_pHDR10ToneMappingConstants;
	CComPtr<ID3D11Buffer> m_pDoViDynamicConstants;
	CComPtr<ID3D11PixelShader> m_pPSHDR10ToneMapping;

	// D3D11 Shader Video Processor
	CComPtr<ID3D11PixelShader> m_pPSConvertColor;
	CComPtr<ID3D11PixelShader> m_pPSConvertColorDeint;
	struct {
		bool bEnable = false;
		ID3D11Buffer* pVertexBuffer = nullptr;
		ID3D11Buffer* pConstants = nullptr;
		void Release() {
			bEnable = false;
			SAFE_RELEASE(pVertexBuffer);
			SAFE_RELEASE(pConstants);
		}
	} m_PSConvColorData;

	CComPtr<ID3D11Buffer> m_pDoviCurvesConstantBuffer;

	CComPtr<ID3D11PixelShader> m_pShaderUpscaleX;
	CComPtr<ID3D11PixelShader> m_pShaderUpscaleY;
	CComPtr<ID3D11PixelShader> m_pShaderDownscaleX;
	CComPtr<ID3D11PixelShader> m_pShaderDownscaleY;

	std::vector<ExternalPixelShader11_t> m_pPreScaleShaders;
	std::vector<ExternalPixelShader11_t> m_pPostScaleShaders;
	CComPtr<ID3D11Buffer> m_pPostScaleConstants;
	CComPtr<ID3D11PixelShader> m_pPSHalfOUtoInterlace;
	CComPtr<ID3D11PixelShader> m_pPSFinalPass;

	CComPtr<IDXGIFactory2>   m_pDXGIFactory2;
	CComPtr<IDXGISwapChain1> m_pDXGISwapChain1;
	CComPtr<IDXGISwapChain4> m_pDXGISwapChain4;
	CComPtr<IDXGIOutput>    m_pDXGIOutput;
	DXGI_COLOR_SPACE_TYPE m_currentSwapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

	// Input parameters
	DXGI_FORMAT m_srcDXGIFormat = DXGI_FORMAT_UNKNOWN;

	// D3D11 VP texture format
	DXGI_FORMAT m_D3D11OutputFmt = DXGI_FORMAT_UNKNOWN;

	// intermediate texture format
	DXGI_FORMAT m_InternalTexFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

	// swap chain format
	DXGI_FORMAT m_SwapChainFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
	UINT32 m_DisplayBitsPerChannel = 8;

	D3D11_VIDEO_FRAME_FORMAT m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;

	CComPtr<IDXGIFactory1> m_pDXGIFactory1;

	bool m_bSubPicWasRendered = false;

	// AlphaBitmap
	Tex2D_t m_TexAlphaBitmap;
	CComPtr<ID3D11Buffer> m_pAlphaBitmapVertex;

	// Statistics
	CD3D11Rectangle m_StatsBackground;
	CD3D11Font      m_Font3D;
	CD3D11Rectangle m_Rect3D;
	CD3D11Rectangle m_Underlay;
	CD3D11Lines     m_Lines;
	CD3D11Polyline  m_SyncLine;

	bool m_bExclusiveScreen = false;
	bool m_bFullScreen = false;

	int m_iVPSuperRes = SUPERRES_Disable;
	bool m_bVPUseSuperRes = false; // but it is not exactly

	bool m_bVPRTXVideoHDR = false;
	bool m_bVPUseRTXVideoHDR = false;
	// How deep the HDR swap-chain retry in InitializeD3D11VP has gone into itself.
	int m_nHdrSwapChainRetry = 0;

	// Sharpening, a step of the post-scale chain: the method, which of the five
	// levels, and the shader that does it.
	int m_iSharpen = SHARPEN_Disabled;
	int m_iSharpenLevel = SHARPEN_LEVEL_DEF;
	CComPtr<ID3D11PixelShader> m_pPSSharpen;
	CComPtr<ID3D11Buffer> m_pSharpenConstants;

	bool m_bVPReplaceChroma = false;    // the setting
	bool m_bChromaReplacedVP = false;   // and the shaders rebuild this source's chroma
	DXGI_FORMAT m_VPInputFmt = DXGI_FORMAT_UNKNOWN;   // what the processor reads: the source, or packed 4:4:4
	CComPtr<ID3D11ComputeShader> m_pCSConvertTo444;   // what writes that 4:4:4 picture

	CDlssNR m_DlssNR;
	CDlssNR::Params m_DlssParams;
	std::wstring m_strDlssNRDllPath;
	bool m_bDlssNR = false;       // user setting
	bool m_bDlssNRActive = false; // setting AND actually working
	bool m_bDlssNRUavOk = false;  // RGBA16F typed UAV store supported
	bool m_bDlssNRAfterUpscale = false; // run at display resolution instead
	CDlssStabilizer m_DlssStabilizer;   // steadies the network's effect after it runs
	int  m_iDlssNRStabilizer = DLSSNR_STAB_DEF;
	int  m_iDlssNRMotion = DLSSNR_MOTION_DEF;
	bool m_bDlssNRMotionVectors = false; // Optical Flow vectors to the network as well
	bool m_bDlssNewPicture = false;     // the next DLSS pass sees a new picture, not a redraw

	// DLSS Super Resolution in place of the resize shaders when the picture grows.
	CDlssSR m_DlssSR;
	std::wstring m_strDlssSRDllPath;
	bool m_bDlssSR = false;             // user setting
	bool m_bDlssSRActive = false;       // setting AND the NGX session is up
	int  m_iDlssSRPreset = DLSSSR_PRESET_DEF;
	CDlssStabilizer m_DlssSRMotion;     // Optical Flow vectors for DLSS SR, drawn to the global motion
	bool m_bDlssSRNewPicture = false;   // the next pass sees a new picture, not a redraw
	std::wstring m_strDlssSRMotion;     // where the vectors came from, for the statistics

	// Render ahead while a DLSS pass runs: samples are processed early by what the
	// passes take, and each finished picture waits for its time (CRenderAhead).
	bool m_bDlssRenderAhead = true;     // user setting
	CRenderAhead m_RenderAhead;
	CPreciseSleep m_PreciseSleep;
	CComPtr<ID3D11Query> m_pPictureDoneQuery;
	uint64_t m_tickSampleStart = 0;     // GetPreciseTick() when ProcessSample began on the current sample
	// Time of each DLSS stage and of the mpv prescalers, for the statistics.
	CGpuStageTimes m_DlssStageTimes;
	CRollingMs m_DlssNRTimes;

	// mpv prescalers (Shaders/mpv). As the Upscaling method, FSRCNNX, RAVU-zoom or
	// ArtCNN enlarges the luma and Catmull-Rom the picture, which takes the network's
	// luma; the resize shaders then scale what is left. As Chroma upsampling, RAVU-zoom
	// or FSRCNNX brings Cb and Cr to the luma size before the conversion shader reads
	// them. The AR methods add anti-ringing: what the network invented beyond the range
	// the source really covers is taken back.
	CMpvShader m_MpvLuma;                     // loaded while the Upscaling method names one
	CMpvShader m_MpvChroma;                   // the prescaler the Chroma upsampling method names
	CComPtr<ID3D11PixelShader> m_pPSMpvLuma;
	CComPtr<ID3D11PixelShader> m_pPSMpvCombine;
	CComPtr<ID3D11PixelShader> m_pPSMpvCombineAR;
	CComPtr<ID3D11PixelShader> m_pPSMpvChromaPlane;
	CComPtr<ID3D11PixelShader> m_pPSMpvChromaAR;
	CComPtr<ID3D11Buffer> m_pMpvChromaPlaneConstants;
	Tex2D_t m_TexMpvLuma;                     // luma of the picture at the source size
	Tex2D_t m_TexMpvResize;                   // Catmull-Rom's first pass
	Tex2D_t m_TexMpvColor;                    // the picture by Catmull-Rom at the prescaled size
	Tex2D_t m_TexMpvOutput;                   // that picture with the prescaler's luma
	CMpvShader::Texture m_TexMpvLumaOut;      // the prescaler's luma
	Tex2D_t m_TexMpvChromaIn[2];              // Cb and Cr in red, at the chroma size
	CMpvShader::Texture m_TexMpvChromaOut[2]; // Cb and Cr at the luma size
	Tex2D_t m_TexMpvChromaAR[2];              // and those two held to the source's range
	bool m_bMpvChromaActive = false;          // the conversion shader reads the planes below
	bool m_bMpvChromaAntiRing = false;        // ... through m_TexMpvChromaAR
	bool m_bMpvLumaAntiRing = false;          // the combine pass holds the luma to that range
	bool m_bMpvChromaFailed = false;          // latched off until the format or the setting changes

	// Where the conversion shader reads Cb (0) and Cr (1) while a prescaler is running:
	// the anti-ringing pass's output where there is one, what the prescaler wrote when
	// that pass has not run.
	ID3D11ShaderResourceView* MpvChromaPlane(const int c) const {
		return (m_bMpvChromaAntiRing && m_TexMpvChromaAR[c].pShaderResource)
			? m_TexMpvChromaAR[c].pShaderResource.p : m_TexMpvChromaOut[c].pShaderResource.p;
	}

	// The chroma method the conversion shader is built for: a prescaler that runs tells
	// it to read the planes the prescaler wrote, and one that cannot run leaves
	// Catmull-Rom in its place.
	int ChromaScalingForShader() const;

	bool RenderAheadActive() const { return m_bDlssRenderAhead && (m_bDlssNRActive || m_bDlssSRActive || m_MpvLuma.IsLoaded()); }
	void MarkPictureSubmitted();
	void HoldUntilPresentTime(const REFERENCE_TIME frameStartTime, const bool bMeasure);

	CDlssStabilizer::Motion DlssMotionSource() const
	{
		return (m_iDlssNRMotion == DLSSNR_MOTION_DETECTOR)
			? CDlssStabilizer::Motion::Detector : CDlssStabilizer::Motion::OpticalFlow;
	}
	D3D_FEATURE_LEVEL m_FeatureLevel = D3D_FEATURE_LEVEL_10_0;

	bool m_bHdrPassthroughSupport             = false;
	std::atomic_bool m_bHdrDisplaySwitching   = false; // switching HDR display in progress
	bool m_bHdrDisplayModeEnabled             = false;
	bool m_bHdrAllowSwitchDisplay             = true;
	bool m_bACMEnabled                        = false;

	UINT m_srcVideoTransferFunction = 0; // need a description or rename

	std::map<std::wstring, bool> m_hdrModeSavedState;
	std::map<std::wstring, bool, std::less<>> m_hdrModeStartState;

	struct HDRMetadata {
		DXGI_HDR_METADATA_HDR10 hdr10 = {};
		bool bValid = false;
	};
	HDRMetadata m_hdr10 = {};
	HDRMetadata m_lastHdr10 = {};

	UINT m_DoviMaxMasteringLuminance = 0;
	UINT m_DoviMinMasteringLuminance = 0;
	UINT m_DoviMaxContentLightLevel = 0;
	UINT m_DoviMaxFrameAverageLightLevel = 0;

	struct DoviExtensionMetadata_t {
		struct L1_t {
			uint16_t min_pq = 0;
			uint16_t max_pq = 0;
			uint16_t avg_pq = 0;
			bool present = false;

			bool operator==(const L1_t& other) const {
				return min_pq == other.min_pq && max_pq == other.max_pq && avg_pq == other.avg_pq;
			}
			bool operator!=(const L1_t& other) const {
				return !(*this == other);
			}
		};
		L1_t L1;
		L1_t L1Cached;

		struct L2_t {
			float trim_slope = 0.f;
			float trim_offset = 0.f;
			float trim_power = 0.f;
			float trim_saturation_gain = 0.f;
			float trim_chroma_weight = 0.f;
			bool present = false;
		};
		L2_t L2;
	} m_DoviExtensionMetadata;

	HMONITOR m_lastFullscreenHMonitor = nullptr;

	D3DCOLOR m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);

	// SubPic
	CComPtr<CDX11SubPicAllocator> m_pSubPicAllocator;
	bool m_bCallbackDeviceIsSet = false;
	void SetCallbackDevice();
	void UpdateSubPic();

	std::atomic_bool m_bDisplayModeChangeAfterHDRToggle = false;
	CCritSec m_HDRToggleLock;

	bool m_bHDRModeChangeOutside = false;

	void FillDisplayParams();

public:
	CDX11VideoProcessor(CMpcVideoRenderer* pFilter, const Settings_t& config, HRESULT& hr);
	~CDX11VideoProcessor() override;

	int Type() override { return VP_DX11; }

	HRESULT Init(const HWND hwnd, const bool displayHdrChanged, bool* pChangeDevice = nullptr) override;
	bool Initialized();

private:
	void ReleaseVP();
	void ReleaseDevice();
	void ReleaseSwapChain();

	UINT GetPostScaleSteps();

	HRESULT CreatePShaderFromResource(ID3D11PixelShader** ppPixelShader, UINT resid);
	// Loads or drops the sharpening pass to match the setting, and fills its
	// constants. Returns whether the number of post-scale steps changed.
	bool UpdateSharpenShader();
	void SetShaderConvertColorParams();
	void SetShaderLuminanceParams();

	void SetHDR10ShaderParams(float, float, float, float, float, int);
	void SetDolbyVisionDynamicParams();

	HRESULT SetShaderDoviCurvesPoly();
	HRESULT SetShaderDoviCurves();

	void UpdateTexParams(int cdepth);
	void UpdateRenderRect();
	void UpdateScalingStrings();

	void CalcStatsParams() override;

	HRESULT MemCopyToTexSrcVideo(const BYTE* srcData, const int srcPitch);

	bool Preferred10BitOutput() {
		return m_DisplayBitsPerChannel >= 10 && (m_InternalTexFmt == DXGI_FORMAT_R10G10B10A2_UNORM || m_InternalTexFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
	}

	bool HandleHDRToggle();
	bool ToggleHDR(const DisplayConfig_t& displayConfig, const bool bEnableAdvancedColor);

public:
	HRESULT SetDevice(ID3D11Device *pDevice, ID3D11DeviceContext *pContext);
	HRESULT InitSwapChain(bool bWindowChanged);

	BOOL VerifyMediaType(const CMediaType* pmt) override;
	BOOL InitMediaType(const CMediaType* pmt) override;

	// Whether the shaders rebuild this source's chroma before the processor takes it.
	bool ChromaToShaders(const FmtConvParams_t& params, const bool interlaced);
	HRESULT UpdateConvertTo444Shader();
	const wchar_t* ChromaScalingName() const;
	void ConvertTo444Pass(ID3D11UnorderedAccessView* pUav);

	HRESULT InitializeD3D11VP(const FmtConvParams_t& params, const UINT width, const UINT height, const CMediaType* pmt);
	HRESULT InitializeTexVP(const FmtConvParams_t& params, const UINT width, const UINT height);
	void UpdatFrameProperties(); // use this after receiving modified frame from hardware decoder

	BOOL GetAlignmentSize(const CMediaType& mt, SIZE& Size) override;

	HRESULT ProcessSample(IMediaSample* pSample) override;
	HRESULT CopySample(IMediaSample* pSample);
	// Render: 1 - render first fied or progressive frame, 2 - render second fied, 0 or other - forced repeat of render.
	HRESULT Render(int field, const REFERENCE_TIME frameStartTime) override;
	HRESULT FillBlack() override;

	void SetVideoRect(const CRect& videoRect)      override;
	HRESULT SetWindowRect(const CRect& windowRect) override;
	HRESULT Reset(bool bDisplayModeChange) override;
	bool IsInit() const override { return m_bHdrDisplaySwitching; }

	HRESULT GetCurentImage(long *pDIBImage) override;
	HRESULT GetDisplayedImage(BYTE **ppDib, unsigned* pSize) override;
	HRESULT GetVPInfo(std::wstring& str) override;
	unsigned GetVideoProcessorUse() override
	{
		if (!m_D3D11VP.IsReady()) {
			return 0;
		}
		// It only resizes while no DLSS pass has taken that back from it (UpdateTexures).
		const bool bResizes = m_bVPScaling && !m_bVPScalingUseShaders && !m_bDlssNRActive && !m_bDlssSRActive;
		// With the chroma rebuilt before it, the processor is handed 4:4:4 and the
		// chroma list is the shaders', so it is not the one converting chroma.
		return (m_bChromaReplacedVP ? 0 : VPUSE_Converting) | (bResizes ? VPUSE_Resizing : 0);
	}

	// Settings
	void Configure(const Settings_t& config) override;

	void SetRotation(int value) override;
	void SetStereo3dTransform(int value) override;

	void Flush() override;

	void ClearPreScaleShaders() override;
	void ClearPostScaleShaders() override;

	HRESULT AddPreScaleShader(const std::wstring& name, const std::string& srcCode) override;
	HRESULT AddPostScaleShader(const std::wstring& name, const std::string& srcCode) override;

	ISubPicAllocator* GetSubPicAllocator() override;

	void SwitchFullScreen(bool set) override;

private:
	void UpdateTexures();
	void UpdatePostScaleTexures();
	void UpdateUpscalingShaders();
	void UpdateDownscalingShaders();
	HRESULT UpdateConvertColorShader();
	void UpdateBitmapShader();

	HRESULT D3D11VPPass(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second);
	HRESULT ConvertColorPass(ID3D11Texture2D* pRenderTarget);
	HRESULT ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const int rotation);
	HRESULT FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect);

	void DrawSubtitles(ID3D11Texture2D* pRenderTarget);
	// bAllowDlss is false for the screenshot path, which must not disturb the
	// network's temporal history.
	HRESULT Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second, const bool bAllowDlss = true);
	HRESULT DlssNRPass(Tex2D_t* pInputTexture, const CRect& rSrc, Tex2D_t** ppResult);
	bool DlssNRSupportedHere() const;
	void UpdateDlssNR();
	std::wstring GetDlssStatus() override;
	// Scales rSrc of the input to dstRect, before rotation, through DLSS Super
	// Resolution. S_FALSE where it does not apply -- the picture does not grow on
	// both axes, or no feature can be made for these sizes -- and the resize shaders
	// then do the whole job.
	HRESULT DlssSRPass(Tex2D_t* pInputTexture, const CRect& rSrc, const CRect& dstRect, const int rotation, Tex2D_t** ppResult);
	bool DlssSRSupportedHere() const;
	void UpdateDlssSR();
	std::wstring GetDlssSRStatus() override;

	// Loads the prescaler the Upscaling method names, or drops it.
	void UpdateMpvLuma();
	// Scales rSrc of the input towards dstRect, before rotation, through the luma
	// prescaler: the result is enlarged as far as the network takes it, for
	// ResizeShaderPass to finish. S_FALSE where it does not apply.
	HRESULT MpvLumaPass(Tex2D_t* pInputTexture, const CRect& rSrc, const CRect& dstRect, const int rotation, Tex2D_t** ppResult);
	// RAVU-zoom for Chroma upsampling where it can run, which the conversion shader follows.
	void UpdateMpvChroma();
	HRESULT MpvChromaPass();

	HRESULT AlphaBlt(ID3D11ShaderResourceView* pShaderResource, ID3D11Texture2D* pRenderTarget,
					 ID3D11Buffer* pVertexBuffer, D3D11_VIEWPORT* pViewPort,
					 ID3D11SamplerState* pSampler);
	HRESULT TextureCopyRect(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
							const CRect& srcRect, const CRect& destRect,
							ID3D11PixelShader* pPixelShader, ID3D11Buffer* pConstantBuffer,
							const int iRotation, const bool bFlip);

	HRESULT TextureResizeShader(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
								const CRect& srcRect, const CRect& destRect,
								ID3D11PixelShader* pPixelShader,
								const int iRotation, const bool bFlip);

	void UpdateStatsPresent();
	void UpdateStatsStatic() override;
	//void UpdateStatsPostProc();
	HRESULT DrawStats(ID3D11Texture2D* pRenderTarget);
	std::wstring GetStatsText() override;
	int GetRenderAhead() override;

public:
	// IMFVideoProcessor
	STDMETHODIMP SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues) override;

	// IMFVideoMixerBitmap
	STDMETHODIMP SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms) override;
	STDMETHODIMP UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms) override;
};
