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

#include "stdafx.h"
#include <uuids.h>
#include <Mferror.h>
#include <Mfidl.h>
#include <optional>
#include <limits>
#include "Helper.h"
#include "Times.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "../Include/Version.h"
#include "DX11VideoProcessor.h"
#include "../Include/ID3DVideoMemoryConfiguration.h"
#include "Shaders.h"
#include "Utils/CPUInfo.h"
#include "DX11VideoProcessorShared.h"

#include <dxgi1_6.h>

#include "../external/minhook/include/MinHook.h"

bool g_bPresent = false;
bool g_bCreateSwapChain = false;
HWND g_hWnd = nullptr;

typedef BOOL(WINAPI* pSetWindowPos)(
	_In_ HWND hWnd,
	_In_opt_ HWND hWndInsertAfter,
	_In_ int X,
	_In_ int Y,
	_In_ int cx,
	_In_ int cy,
	_In_ UINT uFlags);

pSetWindowPos pOrigSetWindowPosDX11 = nullptr;
static BOOL WINAPI pNewSetWindowPosDX11(
	_In_ HWND hWnd,
	_In_opt_ HWND hWndInsertAfter,
	_In_ int X,
	_In_ int Y,
	_In_ int cx,
	_In_ int cy,
	_In_ UINT uFlags)
{
	if (g_bPresent && g_hWnd == hWnd) {
		DLog(L"call SetWindowPos() function during Present()");
		uFlags |= SWP_ASYNCWINDOWPOS;
	}
	return pOrigSetWindowPosDX11(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

typedef LONG(WINAPI* pSetWindowLongA)(
	_In_ HWND hWnd,
	_In_ int nIndex,
	_In_ LONG dwNewLong);

pSetWindowLongA pOrigSetWindowLongADX11 = nullptr;
static LONG WINAPI pNewSetWindowLongADX11(
	_In_ HWND hWnd,
	_In_ int nIndex,
	_In_ LONG dwNewLong)
{
	if (g_bCreateSwapChain && g_hWnd == hWnd) {
		DLog(L"Blocking call SetWindowLongA() function during create fullscreen swap chain");
		return 0L;
	}
	return pOrigSetWindowLongADX11(hWnd, nIndex, dwNewLong);
}

template <typename T>
inline bool HookFunc(T** ppSystemFunction, PVOID pHookFunction)
{
	return MH_CreateHook(*ppSystemFunction, pHookFunction, reinterpret_cast<LPVOID*>(ppSystemFunction)) == MH_OK;
}

static const ScalingShaderResId s_Upscaling11ResIDs[UPSCALE_COUNT] = {
	{0,                            0,                            L"Nearest-neighbor"  },
	{IDF_PS_11_INTERP_MITCHELL4_X, IDF_PS_11_INTERP_MITCHELL4_Y, L"Mitchell-Netravali"},
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"Catmull-Rom"       },
	{IDF_PS_11_INTERP_LANCZOS2_X,  IDF_PS_11_INTERP_LANCZOS2_Y,  L"Lanczos2"          },
	{IDF_PS_11_INTERP_LANCZOS3_X,  IDF_PS_11_INTERP_LANCZOS3_Y,  L"Lanczos3"          },
	{IDF_PS_11_INTERP_JINC2,       IDF_PS_11_INTERP_JINC2,       L"Jinc2m"            },
	// The mpv prescalers: Catmull-Rom for the colour, for the scale they leave, and
	// wherever they cannot run.
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"FSRCNNX 8"         },
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"FSRCNNX 16"        },
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"RAVU-zoom"         },
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"FSRCNNX 8 AR"      },
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"FSRCNNX 16 AR"     },
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"ArtCNN C4F16 DS"   },
};

static const ScalingShaderResId s_Downscaling11ResIDs[DOWNSCALE_COUNT] = {
	{IDF_PS_11_CONVOL_BOX_X,       IDF_PS_11_CONVOL_BOX_Y,       L"Box"          },
	{IDF_PS_11_CONVOL_BILINEAR_X,  IDF_PS_11_CONVOL_BILINEAR_Y,  L"Bilinear"     },
	{IDF_PS_11_CONVOL_HAMMING_X,   IDF_PS_11_CONVOL_HAMMING_Y,   L"Hamming"      },
	{IDF_PS_11_CONVOL_BICUBIC05_X, IDF_PS_11_CONVOL_BICUBIC05_Y, L"Bicubic"      },
	{IDF_PS_11_CONVOL_BICUBIC15_X, IDF_PS_11_CONVOL_BICUBIC15_Y, L"Bicubic sharp"},
	{IDF_PS_11_CONVOL_LANCZOS_X,   IDF_PS_11_CONVOL_LANCZOS_Y,   L"Lanczos"      },
	{IDF_PS_11_CONVOL_SPLINE36_X,  IDF_PS_11_CONVOL_SPLINE36_Y,  L"Spline36"     }
};

const UINT dither_size = 32;

struct VERTEX {
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT2 TexCoord;
};

struct PS_EXTSHADER_CONSTANTS {
	DirectX::XMFLOAT2 pxy; // pixel size in normalized coordinates
	DirectX::XMFLOAT2 wh;  // width and height of texture
	uint32_t counter;      // rendered frame counter
	float clock;           // some time in seconds
	float reserved1;
	float reserved2;
};

static_assert(sizeof(PS_EXTSHADER_CONSTANTS) % 16 == 0);

static void FillVertices(VERTEX (&Vertices)[4], const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	const float src_dx = 1.0f / srcW;
	const float src_dy = 1.0f / srcH;
	float src_l = src_dx * srcRect.left;
	float src_r = src_dx * srcRect.right;
	const float src_t = src_dy * srcRect.top;
	const float src_b = src_dy * srcRect.bottom;

	POINT points[4];
	switch (iRotation) {
	case 90:
		points[0] = { -1, +1 };
		points[1] = { +1, +1 };
		points[2] = { -1, -1 };
		points[3] = { +1, -1 };
		break;
	case 180:
		points[0] = { +1, +1 };
		points[1] = { +1, -1 };
		points[2] = { -1, +1 };
		points[3] = { -1, -1 };
		break;
	case 270:
		points[0] = { +1, -1 };
		points[1] = { -1, -1 };
		points[2] = { +1, +1 };
		points[3] = { -1, +1 };
		break;
	default:
		points[0] = { -1, -1 };
		points[1] = { -1, +1 };
		points[2] = { +1, -1 };
		points[3] = { +1, +1 };
	}

	if (bFlip) {
		std::swap(src_l, src_r);
	}

	// Vertices for drawing whole texture
	// 2 ___4
	//  |\ |
	// 1|_\|3
	Vertices[0] = { {(float)points[0].x, (float)points[0].y, 0}, {src_l, src_b} };
	Vertices[1] = { {(float)points[1].x, (float)points[1].y, 0}, {src_l, src_t} };
	Vertices[2] = { {(float)points[2].x, (float)points[2].y, 0}, {src_r, src_b} };
	Vertices[3] = { {(float)points[3].x, (float)points[3].y, 0}, {src_r, src_t} };
}

static HRESULT CreateVertexBuffer(ID3D11Device* pDevice, ID3D11Buffer** ppVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	ASSERT(ppVertexBuffer);
	ASSERT(*ppVertexBuffer == nullptr);

	VERTEX Vertices[4];
	FillVertices(Vertices, srcW, srcH, srcRect, iRotation, bFlip);

	D3D11_BUFFER_DESC BufferDesc = { sizeof(Vertices), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA InitData = { Vertices, 0, 0 };

	HRESULT hr = pDevice->CreateBuffer(&BufferDesc, &InitData, ppVertexBuffer);
	DLogIf(FAILED(hr), L"CreateVertexBuffer() : CreateBuffer() failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT FillVertexBuffer(ID3D11DeviceContext* pDeviceContext, ID3D11Buffer* pVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	ASSERT(pVertexBuffer);

	VERTEX Vertices[4];
	FillVertices(Vertices, srcW, srcH, srcRect, iRotation, bFlip);

	D3D11_MAPPED_SUBRESOURCE mr;
	HRESULT hr = pDeviceContext->Map(pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"FillVertexBuffer() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &Vertices, sizeof(Vertices));
	pDeviceContext->Unmap(pVertexBuffer, 0);

	return hr;
}

void TextureBlt11(
	ID3D11DeviceContext* pDeviceContext,
	ID3D11RenderTargetView* pRenderTargetView, D3D11_VIEWPORT& viewport,
	ID3D11InputLayout* pInputLayout,
	ID3D11VertexShader* pVertexShader,
	ID3D11PixelShader* pPixelShader,
	ID3D11ShaderResourceView* pShaderResourceViews,
	ID3D11SamplerState* pSampler,
	ID3D11Buffer* pConstantBuffer,
	ID3D11Buffer* pVertexBuffer)
{
	ASSERT(pDeviceContext);
	ASSERT(pRenderTargetView);
	ASSERT(pShaderResourceViews);

	const UINT Stride = sizeof(VERTEX);
	const UINT Offset = 0;

	// Set resources
	pDeviceContext->IASetInputLayout(pInputLayout);
	pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
	pDeviceContext->RSSetViewports(1, &viewport);
	pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	pDeviceContext->VSSetShader(pVertexShader, nullptr, 0);
	pDeviceContext->PSSetShader(pPixelShader, nullptr, 0);
	pDeviceContext->PSSetShaderResources(0, 1, &pShaderResourceViews);
	pDeviceContext->PSSetSamplers(0, 1, &pSampler);
	pDeviceContext->PSSetConstantBuffers(0, 1, &pConstantBuffer);
	pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	pDeviceContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	pDeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* views[1] = {};
	pDeviceContext->PSSetShaderResources(0, 1, views);
}

//
// CDX11VideoProcessor
//

HRESULT CDX11VideoProcessor::AlphaBlt(
	ID3D11ShaderResourceView* pShaderResource,
	ID3D11Texture2D* pRenderTarget,
	ID3D11Buffer* pVertexBuffer,
	D3D11_VIEWPORT* pViewPort,
	ID3D11SamplerState* pSampler)
{
	ID3D11RenderTargetView* pRenderTargetView;
	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);

	if (S_OK == hr) {
		UINT Stride = sizeof(VERTEX);
		UINT Offset = 0;

		// Set resources
		m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
		m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
		m_pDeviceContext->RSSetViewports(1, pViewPort);
		m_pDeviceContext->OMSetBlendState(m_pAlphaBlendState, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
		m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
		m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);
		m_pDeviceContext->PSSetShaderResources(0, 1, &pShaderResource);
		m_pDeviceContext->PSSetSamplers(0, 1, &pSampler);
		m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_pDeviceContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &Stride, &Offset);

		// Draw textured quad onto render target
		m_pDeviceContext->Draw(4, 0);

		pRenderTargetView->Release();
	}
	DLogIf(FAILED(hr), L"AlphaBlt() : CreateRenderTargetView() failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT CDX11VideoProcessor::TextureCopyRect(
	const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& destRect,
	ID3D11PixelShader* pPixelShader, ID3D11Buffer* pConstantBuffer,
	const int iRotation, const bool bFlip)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"TextureCopyRect() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, iRotation, bFlip);
	if (FAILED(hr)) {
		return hr;
	}

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)destRect.left;
	VP.TopLeftY = (FLOAT)destRect.top;
	VP.Width    = (FLOAT)destRect.Width();
	VP.Height   = (FLOAT)destRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, pPixelShader, Tex.pShaderResource, m_pSamplerPoint, pConstantBuffer, m_pVertexBuffer);

	return hr;
}

HRESULT CDX11VideoProcessor::TextureResizeShader(
	const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& dstRect,
	ID3D11PixelShader* pPixelShader,
	const int iRotation, const bool bFlip)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::TextureResizeShader() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, iRotation, bFlip);
	if (FAILED(hr)) {
		return hr;
	}

	const FLOAT constants[][4] = {
		{(float)Tex.desc.Width, (float)Tex.desc.Height, 1.0f / Tex.desc.Width, 1.0f / Tex.desc.Height},
		{(float)srcRect.Width() / dstRect.Width(), (float)srcRect.Height() / dstRect.Height(), 0, 0}
	};

	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pResizeShaderConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::TextureResizeShader() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &constants, sizeof(constants));
	m_pDeviceContext->Unmap(m_pResizeShaderConstantBuffer, 0);

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)dstRect.left;
	VP.TopLeftY = (FLOAT)dstRect.top;
	VP.Width = (FLOAT)dstRect.Width();
	VP.Height = (FLOAT)dstRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, pPixelShader, Tex.pShaderResource, m_pSamplerPoint, m_pResizeShaderConstantBuffer, m_pVertexBuffer);

	return hr;
}

// CDX11VideoProcessor

static CDlssNR::Params DlssParamsFromSettings(const Settings_t& config)
{
	CDlssNR::Params p;
	p.iStyle          = config.iDlssNRStyle;
	p.iPreset         = config.iDlssNRPreset;
	p.fIntensity      = (float)config.iDlssNRIntensity      / DLSSNR_STR_SCALE;
	p.fLocalTone      = (float)config.iDlssNRLocalTone      / DLSSNR_STR_SCALE;
	p.fLocalStructure = (float)config.iDlssNRLocalStructure / DLSSNR_STR_SCALE;
	p.fSkinStructure  = (float)config.iDlssNRSkinStructure  / DLSSNR_STR_SCALE;
	p.bUseAutoMask    = config.bDlssNRAutoMask;
	// The network keeps its own history: forcing DLSSNR.Reset on every picture
	// measured the same (--temporal), and the box that asked for it is gone.
	p.bNoHistory      = false;
	return p;
}

CDX11VideoProcessor::CDX11VideoProcessor(CMpcVideoRenderer* pFilter, const Settings_t& config, HRESULT& hr)
	: CVideoProcessor(pFilter)
{
	m_bShowStats           = config.bShowStats;
	m_iResizeStats         = config.iResizeStats;
	m_iTexFormat           = config.iTexFormat;
	m_VPFormats            = config.VPFmts;
	m_iVPDeinterlacing     = config.iVPDeinterlacing;
	m_bDeintDouble         = config.bDeintDouble;
	m_bVPScaling           = config.bVPScaling;
	m_iVPSuperRes          = config.iVPSuperRes;
	m_bVPRTXVideoHDR       = config.bVPRTXVideoHDR;
	m_bVPReplaceChroma     = config.bVPReplaceChroma;
	m_iSharpen             = config.iSharpen;
	m_iSharpenLevel        = config.iSharpenLevel;
	m_iChromaScaling       = config.iChromaScaling;
	m_iUpscaling           = config.iUpscaling;
	m_bUpscalingSmall      = config.bUpscalingSmall;
	m_iUpscalingSmall      = config.iUpscalingSmall;
	m_iUpscalingInUse      = config.iUpscaling;
	m_iDownscaling         = config.iDownscaling;
	m_bUseDither           = config.bUseDither;
	m_bDeintBlend          = config.bDeintBlend;
	m_iSwapEffect          = config.iSwapEffect;
	m_bVBlankBeforePresent = config.bVBlankBeforePresent;
	m_bAdjustPresentTime   = config.bAdjustPresentTime;
	m_bHdrPreferDoVi       = config.bHdrPreferDoVi;
	m_bHdrPassthrough      = config.bHdrPassthrough;
	m_bHdrLocalToneMapping = config.bHdrLocalToneMapping;
	m_iHdrLocalToneMappingType = config.iHdrLocalToneMappingType;
	m_iHdrDisplayMaxNits   = config.iHdrDisplayMaxNits;
	m_iHdrToggleDisplay    = config.iHdrToggleDisplay;
	m_iHdrOsdBrightness    = config.iHdrOsdBrightness;
	m_bConvertToSdr        = config.bConvertToSdr;
	m_iSDRDisplayNits      = config.iSDRDisplayNits;
	m_rtVideoDelay         = (REFERENCE_TIME)config.iVideoDelay * 10000;
	m_bDlssNR              = config.bDlssNR;
	m_DlssParams           = DlssParamsFromSettings(config);
	m_strDlssNRDllPath     = config.szDlssNRDllPath;
	m_bDlssNRAfterUpscale  = config.bDlssNRAfterUpscale;
	m_iDlssNRStabilizer    = config.iDlssNRStabilizer;
	m_iDlssNRFlowDivider   = config.iDlssNRFlowDivider;
	m_iDlssNRWorkingScale  = config.iDlssNRWorkingScale;
	m_bDlssSR              = config.bDlssSR;
	m_bDlssSRDlaa          = config.bDlssSRDlaa;
	m_iDlssSRPreset        = config.iDlssSRPreset;
	m_strDlssSRDllPath     = config.szDlssSRDllPath;

	m_nCurrentAdapter = -1;

	hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&m_pDXGIFactory1);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CDX11VideoProcessor() : CreateDXGIFactory1() failed with error {}", HR2Str(hr));
		return;
	}

	// set default ProcAmp ranges and values
	SetDefaultDXVA2ProcAmpRanges(m_DXVA2ProcAmpRanges);
	SetDefaultDXVA2ProcAmpValues(m_DXVA2ProcAmpValues);

	pOrigSetWindowPosDX11 = SetWindowPos;
	auto ret = HookFunc(&pOrigSetWindowPosDX11, pNewSetWindowPosDX11);
	DLogIf(!ret, L"CDX11VideoProcessor::CDX11VideoProcessor() : hook for SetWindowPos() fail");

	pOrigSetWindowLongADX11 = SetWindowLongA;
	ret = HookFunc(&pOrigSetWindowLongADX11, pNewSetWindowLongADX11);
	DLogIf(!ret, L"CDX11VideoProcessor::CDX11VideoProcessor() : hook for SetWindowLongA() fail");

	MH_EnableHook(MH_ALL_HOOKS);

	CComPtr<IDXGIAdapter> pDXGIAdapter;
	for (UINT adapter = 0; m_pDXGIFactory1->EnumAdapters(adapter, &pDXGIAdapter) != DXGI_ERROR_NOT_FOUND; ++adapter) {
		CComPtr<IDXGIOutput> pDXGIOutput;
		for (UINT output = 0; pDXGIAdapter->EnumOutputs(output, &pDXGIOutput) != DXGI_ERROR_NOT_FOUND; ++output) {
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(pDXGIOutput->GetDesc(&desc))) {
				DisplayConfig_t displayConfig = {};
				if (GetDisplayConfig(desc.DeviceName, displayConfig)) {
					m_hdrModeStartState[desc.DeviceName] = displayConfig.HDREnabled();
				}
			}

			pDXGIOutput.Release();
		}

		pDXGIAdapter.Release();
	}
}

CDX11VideoProcessor::~CDX11VideoProcessor()
{
	for (const auto& [displayName, state] : m_hdrModeSavedState) {
		DisplayConfig_t displayConfig = {};
		if (GetDisplayConfig(displayName.c_str(), displayConfig)) {
			if (displayConfig.HDRSupported() && displayConfig.HDREnabled() != state) {
				const auto ret = ToggleHDR(displayConfig, state);
				DLogIf(!ret, L"CDX11VideoProcessor::~CDX11VideoProcessor() : Toggle HDR {} for '{}' failed", state ? L"ON" : L"OFF", displayName);
			}
		}
	}

	m_pFilter->m_pSubPicQueue.Release();
	m_pSubPicAllocator.Release();

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();

	ReleaseDevice();

	m_pDXGIFactory1.Release();

	MH_RemoveHook(SetWindowPos);
	MH_RemoveHook(SetWindowLongA);
}

void CDX11VideoProcessor::FillDisplayParams()
{
	m_bHdrPassthroughSupport = false;
	m_bHdrDisplayModeEnabled = false;
	m_DisplayBitsPerChannel = 8;

	m_bACMEnabled = false;

	MONITORINFOEXW mi = { sizeof(mi) };
	GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), reinterpret_cast<LPMONITORINFO>(&mi));
	DisplayConfig_t displayConfig = {};

	if (GetDisplayConfig(mi.szDevice, displayConfig)) {
		m_bHdrDisplayModeEnabled = displayConfig.HDREnabled();
		m_bHdrPassthroughSupport = displayConfig.HDRSupported() && m_bHdrDisplayModeEnabled;
		m_DisplayBitsPerChannel = displayConfig.bitsPerChannel;

		m_bACMEnabled = !m_bHdrDisplayModeEnabled && displayConfig.ACMEnabled();

#ifdef _DEBUG
		auto displayDesc = std::format(L"\nDisplay: {} {}", mi.szDevice, DisplayConfigToString(displayConfig));
		if (displayConfig.bitsPerChannel) {
			auto colenc = ColorEncodingToString(displayConfig.colorEncoding);
			if (colenc) {
				displayDesc.append(std::format(L"\n  Color: {} {}-bit", colenc, displayConfig.bitsPerChannel));
				if (displayConfig.HDRSupported()) {
					displayDesc.append(L", HDR10: ");
					displayDesc.append(displayConfig.HDREnabled() ? L"on" : L"off");

					if (IsWindows11_24H2OrGreater()) {
						auto ColorModeToStr = [](DISPLAYCONFIG_ADVANCED_COLOR_MODE ColorMode) {
							std::wstring str;
#define UNPACK_VALUE(VALUE) case VALUE: str = L"" #VALUE; break;
							switch (ColorMode) {
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR);
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG);
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR);
								default:
									str = std::to_wstring(static_cast<int>(ColorMode));
							};
#undef UNPACK_VALUE

							return str;
						};

						const auto& colors = displayConfig.windows1124H2Colors;
						displayDesc.append(std::format(L"\n         Advanced Color: Supported: {}, Active: {}, Limited by OS policy: {}, HDR is supported: {}",
													   colors.advancedColorSupported, colors.advancedColorActive,
													   colors.advancedColorLimitedByPolicy, colors.highDynamicRangeSupported));
						displayDesc.append(std::format(L"\n                         HDR enabled: {}, Wide supported: {}, Wide enabled: {}",
													   colors.highDynamicRangeUserEnabled, colors.wideColorSupported,
													   colors.wideColorUserEnabled));
						displayDesc.append(std::format(L"\n                         Display color mode: {}", ColorModeToStr(colors.activeColorMode)));
					} else {
						const auto& colors = displayConfig.advancedColor;
						displayDesc.append(std::format(L"\n         Advanced Color: Supported: {}, Enabled: {}, Wide forced: {}, Force disabled: {}",
													   colors.advancedColorSupported, colors.advancedColorEnabled,
													   colors.wideColorEnforced, colors.advancedColorForceDisabled));
					}
				}
			}
		}

		DLog(L"CDX11VideoProcessor::FillDisplayParams():{}", displayDesc);
#endif
	}
}

HRESULT CDX11VideoProcessor::Init(const HWND hwnd, const bool displayHdrChanged, bool* pChangeDevice/* = nullptr*/)
{
	DLog(L"CDX11VideoProcessor::Init()");

	const bool bWindowChanged = displayHdrChanged || (m_hWnd != hwnd);
	g_hWnd = m_hWnd = hwnd;

	FillDisplayParams();

	if (m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen) {
		m_srcVideoTransferFunction = 0;
	}

	IDXGIAdapter* pDXGIAdapter = nullptr;
	const UINT currentAdapter = GetAdapter(hwnd, m_pDXGIFactory1, &pDXGIAdapter);
	CheckPointer(pDXGIAdapter, E_FAIL);
	if (m_nCurrentAdapter == currentAdapter) {
		SAFE_RELEASE(pDXGIAdapter);

		SetCallbackDevice();

		if (!m_pDXGISwapChain1 || m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen || bWindowChanged) {
			InitSwapChain(bWindowChanged);
			UpdateStatsStatic();
			m_pFilter->OnDisplayModeChange();
		}

		return S_OK;
	}
	m_nCurrentAdapter = currentAdapter;

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();
	ReleaseDevice();

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	D3D_FEATURE_LEVEL featurelevel;

	ID3D11Device *pDevice = nullptr;

	HRESULT hr = D3D11CreateDevice(
		pDXGIAdapter,
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
#ifdef _DEBUG
		D3D11_CREATE_DEVICE_DEBUG,
#else
		0,
#endif
		featureLevels,
		std::size(featureLevels),
		D3D11_SDK_VERSION,
		&pDevice,
		&featurelevel,
		nullptr);
	m_FeatureLevel = featurelevel;
#ifdef _DEBUG
	if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING || (hr == E_FAIL && !IsWindows8OrGreater())) {
		DLog(L"WARNING: D3D11 debugging messages will not be displayed");
		hr = D3D11CreateDevice(
			pDXGIAdapter,
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			0,
			featureLevels,
			std::size(featureLevels),
			D3D11_SDK_VERSION,
			&pDevice,
			&featurelevel,
			nullptr);
	}
#endif
	SAFE_RELEASE(pDXGIAdapter);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::Init() : D3D11CreateDevice() failed with error {}", HR2Str(hr));
		return hr;
	}

	DLog(L"CDX11VideoProcessor::Init() : D3D11CreateDevice() successfully with feature level {}.{}", (featurelevel >> 12), (featurelevel >> 8) & 0xF);

	hr = SetDevice(pDevice, nullptr);
	pDevice->Release();

	if (S_OK == hr) {
		if (pChangeDevice) {
			*pChangeDevice = true;
		}
	}

	if (m_VendorId == PCIV_INTEL && CPUInfo::HaveSSE41()) {
		m_pCopyGpuFn = CopyGpuFrame_SSE41;
	} else {
		m_pCopyGpuFn = CopyPlaneAsIs;
	}

	return hr;
}

bool CDX11VideoProcessor::Initialized()
{
	return (m_pDevice.p != nullptr && m_pDeviceContext.p != nullptr);
}

void CDX11VideoProcessor::ReleaseVP()
{
	DLog(L"CDX11VideoProcessor::ReleaseVP()");

	m_pFilter->ResetStreamingTimes2();
	m_RenderStats.Reset();

	if (m_pDeviceContext) {
		m_pDeviceContext->ClearState();
	}

	m_TexSrcVideo.Release();
	m_TexConvertOutput.Release();
	m_TexResize.Release();
	m_TexsPostScale.Release();
	ReleaseDlssNRTextures(true);
	m_DlssSR.ReleaseFeature();
	m_DlssSRMotion.Release();
	m_SRCutReadback.Release();
	m_RenderAhead.Reset();
	m_DlssNRTimes.Reset();

	m_TexMpvLuma.Release();
	m_TexMpvResize.Release();
	m_TexMpvColor.Release();
	m_TexMpvOutput.Release();
	m_TexMpvLumaOut.Release();
	for (int c = 0; c < 2; c++) {
		m_TexMpvChromaIn[c].Release();
		m_TexMpvChromaOut[c].Release();
		m_TexMpvChromaAR[c].Release();
	}
	m_bMpvChromaActive = false;
	m_bMpvChromaFailed = false;

	m_PSConvColorData.Release();
	m_pDoviCurvesConstantBuffer.Release();

	m_D3D11VP.ReleaseVideoProcessor();
	m_strCorrection = nullptr;

	m_srcParams      = {};
	m_srcDXGIFormat  = DXGI_FORMAT_UNKNOWN;
	m_pCopyPlaneFn   = CopyPlaneAsIs;
	m_srcWidth       = 0;
	m_srcHeight      = 0;
}

void CDX11VideoProcessor::ReleaseDevice()
{
	DLog(L"CDX11VideoProcessor::ReleaseDevice()");

	ReleaseVP();
	// Tear the NGX session down before the device it was bound to disappears --
	// Shutdown1 takes that device.
	m_DlssNR.Shutdown();
	m_bDlssNRActive = false;
	m_DlssSR.Shutdown();
	m_bDlssSRActive = false;
	m_DlssStageTimes.Release();
	m_pVPTimingProbe.Release();
	m_pPictureDoneQuery.Release();
	ReleaseVPScale();
	m_D3D11VPScale.ReleaseVideoDevice();
	m_D3D11VP.ReleaseVideoDevice();

	m_StatsBackground.InvalidateDeviceObjects();
	m_Font3D.InvalidateDeviceObjects();
	m_FontNotice.InvalidateDeviceObjects();
	m_NoticeBackground.InvalidateDeviceObjects();
	m_Rect3D.InvalidateDeviceObjects();

	m_Underlay.InvalidateDeviceObjects();
	m_Lines.InvalidateDeviceObjects();
	m_SyncLine.InvalidateDeviceObjects();

	m_TexDither.Release();
	m_bAlphaBitmapEnable = false;
	m_pAlphaBitmapVertex.Release();
	m_TexAlphaBitmap.Release();

	ClearPreScaleShaders();
	ClearPostScaleShaders();
	m_pPSCorrection.Release();
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();

	m_pPSHDR10ToneMapping.Release();
	m_pPSSharpen.Release();
	m_pPSSharpenDither.Release();
	m_pSharpenConstants.Release();

	m_pShaderUpscaleX.Release();
	m_pShaderUpscaleY.Release();
	m_pShaderDownscaleX.Release();
	m_pShaderDownscaleY.Release();
	m_strShaderX = nullptr;
	m_strShaderY = nullptr;
	m_pPSFinalPass.Release();

	m_MpvLuma.Release();
	m_MpvChroma.Release();
	m_pPSMpvLuma.Release();
	m_pPSMpvCombine.Release();
	m_pPSMpvCombineAR.Release();
	m_pPSMpvChromaPlane.Release();
	m_pPSMpvChromaAR.Release();
	m_pMpvChromaPlaneConstants.Release();

	m_pCorrectionConstants.Release();
	m_pPostScaleConstants.Release();

	m_pHDR10ToneMappingConstants.Release();
	m_pDoViDynamicConstants.Release();

#if TEST_SHADER
	m_pPS_TEST.Release();
#endif

	m_pVSimpleInputLayout.Release();
	m_pVS_Simple.Release();
	m_pPS_Simple.Release();
	m_pPS_BitmapToFrame.Release();
	m_pSamplerPoint.Release();
	m_pSamplerLinear.Release();
	m_pSamplerDither.Release();
	m_pAlphaBlendState.Release();

	m_Alignment.texture.Release();
	m_Alignment.cformat = {};
	m_Alignment.cx = {};

	m_pVertexBuffer.Release();
	m_pResizeShaderConstantBuffer.Release();
	m_pHalfOUtoInterlaceConstantBuffer.Release();
	m_pFinalPassConstantBuffer.Release();

	if (m_pDeviceContext) {
		// need ClearState() (see ReleaseVP()) and Flush() for ID3D11DeviceContext when using DXGI_SWAP_EFFECT_DISCARD in Windows 8/8.1
		m_pDeviceContext->Flush();
	}
	m_pDeviceContext.Release();
	m_bCallbackDeviceIsSet = false;

#if (1 && _DEBUG)
	if (m_pDevice) {
		ID3D11Debug* pDebugDevice = nullptr;
		HRESULT hr2 = m_pDevice->QueryInterface(IID_PPV_ARGS(&pDebugDevice));
		if (S_OK == hr2) {
			hr2 = pDebugDevice->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
			ASSERT(S_OK == hr2);
		}
		SAFE_RELEASE(pDebugDevice);
	}
#endif

	m_pDevice.Release();
}

void CDX11VideoProcessor::ReleaseSwapChain()
{
	if (m_pDXGISwapChain1) {
		m_pDXGISwapChain1->SetFullscreenState(FALSE, nullptr);
	}
	m_pDXGIOutput.Release();
	m_pDXGISwapChain4.Release();
	m_pDXGISwapChain1.Release();
	// The next one counts its refreshes and its Presents from nothing.
	m_PresentSchedule.Reset();
	m_bScheduled = false;

	m_MaxDisplayLuminance = 0;
}

UINT CDX11VideoProcessor::GetPostScaleSteps()
{
	UINT nSteps = m_pPostScaleShaders.size();
	if (m_bDlssNRActive && m_bDlssNRAfterUpscale) {
		nSteps++;
	}
	if (m_pPSCorrection) {
		nSteps++;
	}
	if (m_pPSHDR10ToneMapping) {
		nSteps++;
	}
	if (m_pPSSharpen) {
		nSteps++;
	}
	if (m_pPSHalfOUtoInterlace) {
		nSteps++;
	}
	if (m_bFinalPass && !SharpenTakesDither()) {
		nSteps++;
	}
	return nSteps;
}

// What level 1 to 5 is worth to each method. The two are not on the same scale,
// and neither of them is on the scale the bench measured them on: a picture the
// bench had blurred by halving it and enlarging it again is far softer than
// anything this renderer produces, and adaptive-sharpen -- which holds back where
// the edges are already steep -- then does almost nothing. So the levels are
// calibrated here instead, on a 1080p film enlarged to 3200x1334 through the
// filter itself, as the gain in mean gradient each strength really buys:
// +4, +8, +13, +20 and +28 per cent. Level 3 is the +13 the bench's own quality
// measurements settle on (tools/dlssnr_probe --tsharpen).
static const float s_SharpenStrength[SHARPEN_COUNT][SHARPEN_LEVEL_MAX] = {
	{ 0.00f, 0.00f, 0.00f, 0.00f, 0.00f },   // SHARPEN_Disabled
	{ 0.22f, 0.33f, 0.46f, 0.68f, 0.99f },   // SHARPEN_Adaptive
	{ 0.45f, 0.62f, 0.83f, 1.17f, 1.49f },   // SHARPEN_UnsharpClamp
};

// How far the unsharp mask may be pulled back into the range its neighbours
// cover: libplacebo's own figure, the one the prescalers' anti-ringing uses.
static const float s_SharpenClamp = 0.8f;

HRESULT CDX11VideoProcessor::CreatePShaderFromResource(ID3D11PixelShader** ppPixelShader, UINT resid)
{
	if (!m_pDevice || !ppPixelShader) {
		return E_POINTER;
	}

	LPVOID data;
	DWORD size;
	HRESULT hr = GetDataFromResource(data, size, resid);
	if (FAILED(hr)) {
		return hr;
	}

	return m_pDevice->CreatePixelShader(data, size, nullptr, ppPixelShader);
}

void CDX11VideoProcessor::SetShaderConvertColorParams()
{
	mp_cmat cmatrix;

	if (m_Dovi.bValid) {
		const float brightness = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Brightness) / 255;
		const float contrast   = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Contrast);

		for (int i = 0; i < 3; i++) {
			cmatrix.m[i][0] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 0] * contrast;
			cmatrix.m[i][1] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 1] * contrast;
			cmatrix.m[i][2] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 2] * contrast;
		}

		for (int i = 0; i < 3; i++) {
			cmatrix.c[i] = brightness;
			for (int j = 0; j < 3; j++) {
				cmatrix.c[i] -= cmatrix.m[i][j] * m_Dovi.msd.ColorMetadata.ycc_to_rgb_offset[j];
			}
		}

		m_PSConvColorData.bEnable = true;
	}
	else {
		mp_csp_params csp_params;
		set_colorspace(m_srcExFmt, csp_params.color);
		csp_params.brightness = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Brightness) / 255;
		csp_params.contrast   = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Contrast);
		csp_params.hue        = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Hue) / 180 * acos(-1);
		csp_params.saturation = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Saturation);
		csp_params.gray       = m_srcParams.CSType == CS_GRAY;

		csp_params.input_bits = csp_params.texture_bits = m_srcParams.CDepth;

		mp_get_csp_matrix(&csp_params, &cmatrix);

		m_PSConvColorData.bEnable =
			m_srcParams.CSType == CS_YUV ||
			m_srcParams.cformat == CF_GBRP8 || m_srcParams.cformat == CF_GBRP10 || m_srcParams.cformat == CF_GBRP16 ||
			csp_params.gray ||
			fabs(csp_params.brightness) > 1e-4f || fabs(csp_params.contrast - 1.0f) > 1e-4f;
	}

	PS_COLOR_TRANSFORM cbuffer = {
		{cmatrix.m[0][0], cmatrix.m[0][1], cmatrix.m[0][2], 0},
		{cmatrix.m[1][0], cmatrix.m[1][1], cmatrix.m[1][2], 0},
		{cmatrix.m[2][0], cmatrix.m[2][1], cmatrix.m[2][2], 0},
		{cmatrix.c[0],    cmatrix.c[1],    cmatrix.c[2],    0},
	};

	if (m_srcParams.cformat == CF_GBRP8 || m_srcParams.cformat == CF_GBRP10 || m_srcParams.cformat == CF_GBRP16) {
		std::swap(cbuffer.cm_r.x, cbuffer.cm_r.y); std::swap(cbuffer.cm_r.y, cbuffer.cm_r.z);
		std::swap(cbuffer.cm_g.x, cbuffer.cm_g.y); std::swap(cbuffer.cm_g.y, cbuffer.cm_g.z);
		std::swap(cbuffer.cm_b.x, cbuffer.cm_b.y); std::swap(cbuffer.cm_b.y, cbuffer.cm_b.z);
	}
	else if (m_srcParams.CSType == CS_GRAY) {
		cbuffer.cm_g.x = cbuffer.cm_g.y;
		cbuffer.cm_g.y = 0;
		cbuffer.cm_b.x = cbuffer.cm_b.z;
		cbuffer.cm_b.z = 0;
	}

	if (m_PSConvColorData.pConstants) {
		m_pDeviceContext->UpdateSubresource(m_PSConvColorData.pConstants, 0, nullptr, &cbuffer, 0, 0);
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_PSConvColorData.pConstants));
	}
}

// The sharpening pass, loaded to match the setting. Answers whether the number of
// post-scale steps changed, because the textures they run in are made for that
// number and the last step is the one that reaches the screen.
bool CDX11VideoProcessor::UpdateSharpenShader()
{
	const bool bWanted = (m_iSharpen != SHARPEN_Disabled) && m_pDevice;
	const bool bChanged = (bWanted != (m_pPSSharpen != nullptr));
	// Whether the dither is a step of its own changes the count as well.
	const bool bTookDither = SharpenTakesDither();

	m_pPSSharpen.Release();
	if (!bWanted) {
		m_pSharpenConstants.Release();
		UpdateSharpenDitherShader();
		return bChanged || bTookDither != SharpenTakesDither();
	}

	const UINT resid = (m_iSharpen == SHARPEN_Adaptive) ? IDF_PS_11_SHARPEN_ADAPTIVE : IDF_PS_11_SHARPEN_UNSHARP;
	if (FAILED(CreatePShaderFromResource(&m_pPSSharpen, resid))) {
		DLog(L"CDX11VideoProcessor::UpdateSharpenShader() : the sharpening shader would not load");
		m_pSharpenConstants.Release();
		UpdateSharpenDitherShader();
		return bChanged || bTookDither != SharpenTakesDither();
	}

	if (!m_pSharpenConstants) {
		const D3D11_BUFFER_DESC desc = {
			.ByteWidth = sizeof(FLOAT) * 12,   // the dither's scale after the method's own
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
		};
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&desc, nullptr, &m_pSharpenConstants));
	}
	UpdateSharpenDitherShader();
	return bChanged || bTookDither != SharpenTakesDither();
}

// The sharpening pass that ends on the dither, for the method and the swap chain at
// hand, while there is a dither to take: the picture is no longer written as half
// floats by the one and read back by the other. Adaptive-Sharpen 4 and the dither to a
// 10-bit swap chain at 4K, on an RTX 4060: 1.116 and 0.148 ms in two passes, 1.118 in
// one (RenderDoc). Without the half floats between them, 13.5 % of the 10-bit values
// come out a level apart, none by more.
void CDX11VideoProcessor::UpdateSharpenDitherShader()
{
	m_pPSSharpenDither.Release();
	if (!m_pPSSharpen || !m_bFinalPass) {
		return;
	}
	const bool b10 = (m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM);
	const UINT resid = (m_iSharpen == SHARPEN_Adaptive)
		? (b10 ? IDF_PS_11_SHARPEN_ADAPTIVE_DITHER10 : IDF_PS_11_SHARPEN_ADAPTIVE_DITHER)
		: (b10 ? IDF_PS_11_SHARPEN_UNSHARP_DITHER10 : IDF_PS_11_SHARPEN_UNSHARP_DITHER);
	if (FAILED(CreatePShaderFromResource(&m_pPSSharpenDither, resid))) {
		DLog(L"CDX11VideoProcessor::UpdateSharpenDitherShader() : no sharpening with the dither, the dither stays a step");
		m_pPSSharpenDither.Release();
	}
}

void CDX11VideoProcessor::SetShaderLuminanceParams()
{
	FLOAT cbuffer[4] = { 10000.0f / m_iSDRDisplayNits, 0, 0, 0 };

	if (m_pCorrectionConstants) {
		m_pDeviceContext->UpdateSubresource(m_pCorrectionConstants, 0, nullptr, &cbuffer, 0, 0);
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pCorrectionConstants));
	}
}

void CDX11VideoProcessor::SetHDR10ShaderParams(float masteringMinLuminanceNits, float masteringMaxLuminanceNits,
											   float maxCLL, float maxFALL,
											   float displayMaxNits, int toneMappingType)
{
	if (masteringMinLuminanceNits <= 0.f) masteringMinLuminanceNits = 0.f;
	if (masteringMaxLuminanceNits <= 10.f) masteringMaxLuminanceNits = 1000.f;
	if (maxCLL <= 10.f) maxCLL = masteringMaxLuminanceNits;
	if (maxFALL <= 1.f) maxFALL = maxCLL;
	if (displayMaxNits < 100.f || displayMaxNits > 10000.f) displayMaxNits = 1000.f;
	if (toneMappingType < 1 || toneMappingType > 6) toneMappingType = 1;

	const HDRParamsConstantBuffer_t cbuffer = {
		masteringMinLuminanceNits, masteringMaxLuminanceNits,
		maxCLL, maxFALL,
		displayMaxNits, static_cast<UINT>(toneMappingType)
	};

	if (!m_pPSHDR10ToneMapping) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHDR10ToneMapping, IDF_PS_11_HDR10_TONEMAP));
		DLogIf(m_pPSHDR10ToneMapping, L"CDX11VideoProcessor::SetHDR10ShaderParams() : m_pPSHDR10ToneMapping({}) created", m_iHdrLocalToneMappingType);

		UpdatePostScaleTexures();
	}

	if (m_pHDR10ToneMappingConstants) {
		if (memcmp(&m_lastHDRParamsConstantBuffer, &cbuffer, sizeof(cbuffer)) == 0) {
			return;
		}

		m_pDeviceContext->UpdateSubresource(m_pHDR10ToneMappingConstants, 0, nullptr, &cbuffer, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pHDR10ToneMappingConstants);
		if (FAILED(result)) {
			DLog(L"SetHDR10ShaderLuminanceParams() failed to create m_pHDR10ToneMappingConstants. Error: {}", result);
		}
		EXECUTE_ASSERT(S_OK == result);
	}

	m_lastHDRParamsConstantBuffer = cbuffer;
}

void CDX11VideoProcessor::SetDolbyVisionDynamicParams()
{
	const DoViDynamicConstantsBuffer_t cbuffer = {
		m_DoviExtensionMetadata.L2.trim_chroma_weight - 0.5f, m_DoviExtensionMetadata.L2.trim_saturation_gain - 0.5f,
		m_DoviExtensionMetadata.L2.trim_slope + 0.5f, m_DoviExtensionMetadata.L2.trim_offset - 0.5f, m_DoviExtensionMetadata.L2.trim_power + 0.5f,
		static_cast<UINT>(m_DoviExtensionMetadata.L2.present)
	};

	if (m_pDoViDynamicConstants) {
		if (memcmp(&m_lastDoViDynamicConstantsBuffer, &cbuffer, sizeof(cbuffer)) == 0) {
			return;
		}

		m_pDeviceContext->UpdateSubresource(m_pDoViDynamicConstants, 0, nullptr, &cbuffer, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoViDynamicConstants);
		if (FAILED(result))
		{
			DLog(L"SetDolbyVisionDynamicParams() failed to create m_pDoViDynamicConstants. Error: {}", result);
		}
		EXECUTE_ASSERT(S_OK == result);
	}

	m_lastDoViDynamicConstantsBuffer = cbuffer;

#ifndef NDEBUG
	UpdateStatsStatic();
#endif
}

HRESULT CDX11VideoProcessor::SetShaderDoviCurvesPoly()
{
	ASSERT(m_Dovi.bValid);

	PS_DOVI_POLY_CURVE polyCurves[3] = {};

	const float scale = 1.0f / ((1 << m_Dovi.msd.Header.bl_bit_depth) - 1);
	const float scale_coef = 1.0f / (1u << m_Dovi.msd.Header.coef_log2_denom);

	for (int c = 0; c < 3; c++) {
		const auto& curve = m_Dovi.msd.Mapping.curves[c];
		auto& out = polyCurves[c];

		const int num_coef = curve.num_pivots - 1;
		bool has_poly = false;
		bool has_mmr = false;

		for (int i = 0; i < num_coef; i++) {
			switch (curve.mapping_idc[i]) {
			case 0: // polynomial
				has_poly = true;
				out.coeffs_data[i].x = scale_coef * curve.poly_coef[i][0];
				out.coeffs_data[i].y = (curve.poly_order[i] >= 1) ? scale_coef * curve.poly_coef[i][1] : 0.0f;
				out.coeffs_data[i].z = (curve.poly_order[i] >= 2) ? scale_coef * curve.poly_coef[i][2] : 0.0f;
				out.coeffs_data[i].w = 0.0f; // order=0 signals polynomial
				break;
			case 1: // mmr
				has_mmr = true;
				// not supported, leave as is
				out.coeffs_data[i].x = 0.0f;
				out.coeffs_data[i].y = 1.0f;
				out.coeffs_data[i].z = 0.0f;
				out.coeffs_data[i].w = 0.0f;
				break;
			}
		}

		const int n = curve.num_pivots - 2;
		for (int i = 0; i < n; i++) {
			out.pivots_data[i].x = scale * curve.pivots[i + 1];
		}
		for (int i = n; i < 7; i++) {
			out.pivots_data[i].x = 1e9f;
		}
	}

	HRESULT hr;

	if (m_pDoviCurvesConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pDoviCurvesConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (SUCCEEDED(hr)) {
			memcpy(mr.pData, &polyCurves, sizeof(polyCurves));
			m_pDeviceContext->Unmap(m_pDoviCurvesConstantBuffer, 0);
		}
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = { sizeof(polyCurves), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_SUBRESOURCE_DATA InitData = { &polyCurves, 0, 0 };
		hr = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoviCurvesConstantBuffer);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::SetShaderDoviCurves()
{
	ASSERT(m_Dovi.bValid);

	PS_DOVI_CURVE cbuffer[3] = {};

	for (int c = 0; c < 3; c++) {
		const auto& curve = m_Dovi.msd.Mapping.curves[c];
		auto& out = cbuffer[c];

		bool has_poly = false, has_mmr = false, mmr_single = true;
		uint32_t mmr_idx = 0, min_order = 3, max_order = 1;

		const float scale_coef = 1.0f / (1 << m_Dovi.msd.Header.coef_log2_denom);
		const int num_coef = curve.num_pivots - 1;
		for (int i = 0; i < num_coef; i++) {
			switch (curve.mapping_idc[i]) {
			case 0: // polynomial
				has_poly = true;
				out.coeffs_data[i].x = scale_coef * curve.poly_coef[i][0];
				out.coeffs_data[i].y = (curve.poly_order[i] >= 1) ? scale_coef * curve.poly_coef[i][1] : 0.0f;
				out.coeffs_data[i].z = (curve.poly_order[i] >= 2) ? scale_coef * curve.poly_coef[i][2] : 0.0f;
				out.coeffs_data[i].w = 0.0f; // order=0 signals polynomial
				break;
			case 1: // mmr
				min_order = std::min<int>(min_order, curve.mmr_order[i]);
				max_order = std::max<int>(max_order, curve.mmr_order[i]);
				mmr_single = !has_mmr;
				has_mmr = true;
				out.coeffs_data[i].x = scale_coef * curve.mmr_constant[i];
				out.coeffs_data[i].y = static_cast<float>(mmr_idx);
				out.coeffs_data[i].w = static_cast<float>(curve.mmr_order[i]);
				for (int j = 0; j < curve.mmr_order[i]; j++) {
					// store weights per order as two packed float4s
					out.mmr_data[mmr_idx].x = scale_coef * curve.mmr_coef[i][j][0];
					out.mmr_data[mmr_idx].y = scale_coef * curve.mmr_coef[i][j][1];
					out.mmr_data[mmr_idx].z = scale_coef * curve.mmr_coef[i][j][2];
					out.mmr_data[mmr_idx].w = 0.0f; // unused
					mmr_idx++;
					out.mmr_data[mmr_idx].x = scale_coef * curve.mmr_coef[i][j][3];
					out.mmr_data[mmr_idx].y = scale_coef * curve.mmr_coef[i][j][4];
					out.mmr_data[mmr_idx].z = scale_coef * curve.mmr_coef[i][j][5];
					out.mmr_data[mmr_idx].w = scale_coef * curve.mmr_coef[i][j][6];
					mmr_idx++;
				}
				break;
			}
		}

		const float scale = 1.0f / ((1 << m_Dovi.msd.Header.bl_bit_depth) - 1);
		const int n = curve.num_pivots - 2;
		for (int i = 0; i < n; i++) {
			out.pivots_data[i].x = scale * curve.pivots[i + 1];
		}
		for (int i = n; i < 7; i++) {
			out.pivots_data[i].x = 1e9f;
		}

		if (has_poly) {
			out.params.methods = PS_RESHAPE_POLY;
		}
		if (has_mmr) {
			out.params.methods |= PS_RESHAPE_MMR;
			out.params.mmr_single = mmr_single;
			out.params.min_order = min_order;
			out.params.max_order = max_order;
		}
	}

	HRESULT hr;

	if (m_pDoviCurvesConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pDoviCurvesConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (SUCCEEDED(hr)) {
			memcpy(mr.pData, &cbuffer, sizeof(cbuffer));
			m_pDeviceContext->Unmap(m_pDoviCurvesConstantBuffer, 0);
		}
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = { sizeof(cbuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		hr = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoviCurvesConstantBuffer);
	}

	return hr;
}

void CDX11VideoProcessor::UpdateTexParams(int cdepth)
{
	if (m_bDlssNRActive) {
		// The network consumes and produces RGBA16F; keeping the whole chain in
		// float avoids a requantisation on the way in and banding on the way out.
		m_InternalTexFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
		return;
	}

	switch (m_iTexFormat) {
	case TEXFMT_AUTOINT:
		m_InternalTexFmt = (cdepth > 8 || m_bVPUseRTXVideoHDR) ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		break;
	case TEXFMT_8INT:    m_InternalTexFmt = DXGI_FORMAT_B8G8R8A8_UNORM;     break;
	case TEXFMT_10INT:   m_InternalTexFmt = DXGI_FORMAT_R10G10B10A2_UNORM;  break;
	case TEXFMT_16FLOAT: m_InternalTexFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
	default:
		ASSERT(FALSE);
	}
}

void CDX11VideoProcessor::UpdateRenderRect()
{
	m_renderRect.IntersectRect(m_videoRect, m_windowRect);
	UpdateScalingStrings();
}

// Which Upscaling method this picture gets. The user's, unless they asked for
// another one on small enlargements and this is one: nothing shrinks, something
// grows, and neither axis grows by five per cent. A network there doubles the
// source to gain a handful of pixels, which is its whole cost for nothing visible.
int CDX11VideoProcessor::UpscalingFor(int w1, int h1, int w2, int h2) const
{
	const bool bSmall = m_bUpscalingSmall && w1 > 0 && h1 > 0
		&& w2 >= w1 && h2 >= h1 && (w2 > w1 || h2 > h1)
		&& w2 * 100 < w1 * 105 && h2 * 100 < h1 * 105;
	return bSmall ? m_iUpscalingSmall : m_iUpscaling;
}

void CDX11VideoProcessor::UpdateScalingStrings()
{
	const int w2 = m_videoRect.Width();
	const int h2 = m_videoRect.Height();
	int w1, h1;
	if (m_iRotation == 90 || m_iRotation == 270) {
		w1 = m_srcRectHeight;
		h1 = m_srcRectWidth;
	} else {
		w1 = m_srcRectWidth;
		h1 = m_srcRectHeight;
	}
	// The sizes are known here and nowhere earlier, so this is where the method in
	// use follows them. Loading the shaders comes back through this function, and
	// finds the two agreeing the second time.
	const int wanted = UpscalingFor(w1, h1, w2, h2);
	if (wanted != m_iUpscalingInUse) {
		m_iUpscalingInUse = wanted;
		if (m_pDevice) {
			UpdateUpscalingShaders();
			return;
		}
	}
	// DLSS Super Resolution takes over when the picture grows on both axes, and with
	// DLAA when it does not grow at all.
	const bool bDlaa = m_bDlssSRActive && m_bDlssSRDlaa && w1 == w2 && h1 == h2;
	const bool bDlssSR = bDlaa || (m_bDlssSRActive && w2 > w1 && h2 > h1);
	const wchar_t* upscaling = bDlaa ? L"DLSS SR (DLAA)"
		: bDlssSR ? L"DLSS SR" : s_Upscaling11ResIDs[m_iUpscalingInUse].description;
	// An mpv prescaler only where it runs; Catmull-Rom does the job elsewhere.
	if (!bDlssSR && MpvLumaShader(m_iUpscalingInUse) && !m_MpvLuma.Applies(w1, h1, w2, h2)) {
		upscaling = L"Catmull-Rom";
	}
	// Nothing is normally said about an axis that does not change size; DLAA is the
	// one pass worth naming there, since the whole point of it is that it runs anyway.
	m_strShaderX = (w1 == w2) ? (bDlaa ? upscaling : nullptr)
		: (w1 > w2)
		? s_Downscaling11ResIDs[m_iDownscaling].description
		: upscaling;
	m_strShaderY = (h1 == h2) ? (bDlaa ? upscaling : nullptr)
		: (h1 > h2)
		? s_Downscaling11ResIDs[m_iDownscaling].description
		: upscaling;
}

void CDX11VideoProcessor::CalcStatsParams()
{
	if (m_pDeviceContext && !m_windowRect.IsRectEmpty()) {
		SIZE rtSize = m_windowRect.Size();

		// S_FALSE: the font is already there at this size and its metrics still stand,
		// which is the usual answer once the box has to follow a new line count.
		if (SUCCEEDED(m_Font3D.CreateFontBitmap(L"Consolas", m_StatsFontH, 0))) {
			SIZE charSize = m_Font3D.GetMaxCharMetric();
			m_StatsRect.right  = m_StatsRect.left + m_StatsColumns * charSize.cx + 5 + 3;
			m_StatsRect.bottom = m_StatsRect.top + m_StatsLines * charSize.cy + 5 + 3 + kStatsMarkerH + 1;
		}
		m_StatsBackground.Set(m_StatsRect, rtSize, D3DCOLOR_ARGB(80, 0, 0, 0));

		CalcGraphParams();
		m_Underlay.Set(m_GraphRect, rtSize, D3DCOLOR_ARGB(80, 0, 0, 0));

		m_Lines.ClearPoints(rtSize);
		POINT points[2];
		const int linestep = 20 * m_Yscale;
		for (int y = m_GraphRect.top + (m_Yaxis - m_GraphRect.top) % (linestep); y < m_GraphRect.bottom; y += linestep) {
			points[0] = { m_GraphRect.left,  y };
			points[1] = { m_GraphRect.right, y };
			m_Lines.AddPoints(points, std::size(points), (y == m_Yaxis) ? D3DCOLOR_XRGB(150, 150, 255) : D3DCOLOR_XRGB(100, 100, 255));
		}
		m_Lines.UpdateVertexBuffer();
	}
}

HRESULT CDX11VideoProcessor::MemCopyToTexSrcVideo(const BYTE* srcData, const int srcPitch)
{
	HRESULT hr = S_FALSE;
	D3D11_MAPPED_SUBRESOURCE mappedResource = {};

	if (m_TexSrcVideo.pTexture2) {
		hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			m_pCopyPlaneFn(m_srcHeight, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, srcPitch);
			m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture, 0);

			hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture2, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			if (SUCCEEDED(hr)) {
				const UINT cromaH = m_srcHeight / m_srcParams.pDX11Planes->div_chroma_h;
				const int cromaPitch = (m_TexSrcVideo.pTexture3) ? srcPitch / m_srcParams.pDX11Planes->div_chroma_w : srcPitch;
				srcData += srcPitch * m_srcHeight;
				m_pCopyPlaneFn(cromaH, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, cromaPitch);
				m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture2, 0);

				if (m_TexSrcVideo.pTexture3) {
					hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture3, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
					if (SUCCEEDED(hr)) {
						srcData += cromaPitch * cromaH;
						m_pCopyPlaneFn(cromaH, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, cromaPitch);
						m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture3, 0);
					}
				}
			}
		}
	} else {
		hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			const BYTE* src = (srcPitch < 0) ? srcData + srcPitch * (1 - (int)m_srcLines) : srcData;
			m_pCopyPlaneFn(m_srcLines, (BYTE*)mappedResource.pData, mappedResource.RowPitch, src, srcPitch);
			m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture, 0);
		}
	}

	return hr;
}

HRESULT CDX11VideoProcessor::SetDevice(ID3D11Device *pDevice, ID3D11DeviceContext *pContext)
{
	DLog(L"CDX11VideoProcessor::SetDevice()");

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();
	ReleaseDevice();

	CheckPointer(pDevice, E_POINTER);

	HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice));
	if (FAILED(hr)) {
		return hr;
	}
	if (pContext) {
		hr = pContext->QueryInterface(IID_PPV_ARGS(&m_pDeviceContext));
		if (FAILED(hr)) {
			return hr;
		}
	} else {
		m_pDevice->GetImmediateContext1(&m_pDeviceContext);
	}

	// for d3d11 subtitles
	CComQIPtr<ID3D10Multithread> pMultithread(m_pDeviceContext);
	pMultithread->SetMultithreadProtected(TRUE);

	CComPtr<IDXGIDevice> pDXGIDevice;
	hr = m_pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : QueryInterface(IDXGIDevice) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	// DXGI lets three pictures wait for a refresh unless it is told otherwise, and
	// Present returns at once while there is room. On a screen that refreshes faster
	// than the film that room is never used. On one that refreshes exactly as often
	// -- 24 fps at 24 Hz -- the first pictures of a film arrive in a burst, fill it,
	// and nothing ever empties it again: measured at 30 fps on 30 Hz, the queue sat
	// at 3 and the picture reached the screen 72 to 92 ms after its time, with the
	// Sync offset reading -7 all along because it is taken when Present returns.
	// One picture waiting is all a renderer that times its own presents needs.
	if (CComQIPtr<IDXGIDevice1> pDXGIDevice1 = pDXGIDevice.p) {
		pDXGIDevice1->SetMaximumFrameLatency(1);
	}

	CComPtr<IDXGIAdapter> pDXGIAdapter;
	hr = pDXGIDevice->GetAdapter(&pDXGIAdapter);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : GetAdapter(IDXGIAdapter) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	DXGI_ADAPTER_DESC dxgiAdapterDesc = {};
	hr = pDXGIAdapter->GetDesc(&dxgiAdapterDesc);
	if (SUCCEEDED(hr)) {
		m_VendorId = dxgiAdapterDesc.VendorId;
		m_strAdapterDescription = std::format(L"{} ({:04X}:{:04X})", dxgiAdapterDesc.Description, dxgiAdapterDesc.VendorId, dxgiAdapterDesc.DeviceId);
		DLog(L"Graphics DXGI adapter: {}", m_strAdapterDescription);
	}

	{
		// The NGX cubin backend writes its output through a typed UAV.
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2 = { DXGI_FORMAT_R16G16B16A16_FLOAT, 0 };
		m_bDlssNRUavOk = SUCCEEDED(m_pDevice->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2)))
			&& (fs2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
	}
	UpdateDlssNR();
	UpdateDlssSR();

	HRESULT hr2 = m_D3D11VP.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId);
	m_D3D11VPScale.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId);   // its second turn, after DLSS 5 NR
	DLogIf(FAILED(hr2), L"CDX11VideoProcessor::SetDevice() : InitVideoDevice failed with error {}", HR2Str(hr2));

	D3D11_SAMPLER_DESC SampDesc = {};
	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	SampDesc.MinLOD = 0;
	SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerPoint));

	SampDesc.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT; // linear interpolation for magnification
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerLinear));

	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerDither));

	D3D11_BLEND_DESC bdesc = {};
	bdesc.RenderTarget[0].BlendEnable = TRUE;
	bdesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	bdesc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
	bdesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bdesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bdesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bdesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bdesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBlendState(&bdesc, &m_pAlphaBlendState));

	LPVOID data;
	DWORD size;
	EXECUTE_ASSERT(S_OK == GetDataFromResource(data, size, IDF_VS_11_SIMPLE));
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateVertexShader(data, size, nullptr, &m_pVS_Simple));

	D3D11_INPUT_ELEMENT_DESC Layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateInputLayout(Layout, std::size(Layout), data, size, &m_pVSimpleInputLayout));

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_Simple, IDF_PS_11_SIMPLE));

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMpvLuma, IDF_PS_11_MPV_LUMA));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMpvCombine, IDF_PS_11_MPV_COMBINE));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMpvChromaPlane, IDF_PS_11_MPV_CHROMA_PLANE));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMpvCombineAR, IDF_PS_11_MPV_COMBINE_AR));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMpvChromaAR, IDF_PS_11_MPV_CHROMA_AR));

	// The sharpening pass only exists while it is asked for: it is a step of
	// the post-scale chain, and the chain's length is counted from it.
	UpdateSharpenShader();

#if TEST_SHADER
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_TEST, IDF_PS_11_TEST));
#endif

	D3D11_BUFFER_DESC BufferDesc = { sizeof(VERTEX) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pVertexBuffer));

	BufferDesc = { sizeof(FLOAT) * 4 * 2, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pResizeShaderConstantBuffer));

	BufferDesc = { sizeof(FLOAT) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pHalfOUtoInterlaceConstantBuffer));
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pFinalPassConstantBuffer));
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pMpvChromaPlaneConstants));

	BufferDesc = { sizeof(PS_EXTSHADER_CONSTANTS), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pPostScaleConstants));

	CComPtr<IDXGIFactory1> pDXGIFactory1;
	hr = pDXGIAdapter->GetParent(IID_PPV_ARGS(&pDXGIFactory1));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : GetParent(IDXGIFactory1) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	hr = pDXGIFactory1->QueryInterface(IID_PPV_ARGS(&m_pDXGIFactory2));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : QueryInterface(IDXGIFactory2) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	HRESULT hr3 = m_Font3D.InitDeviceObjects(m_pDevice, m_pDeviceContext);
	DLogIf(FAILED(hr3), L"m_Font3D.InitDeviceObjects() failed with error {}", HR2Str(hr3));
	if (SUCCEEDED(hr3)) {
		m_FontNotice.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		m_NoticeBackground.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_StatsBackground.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Rect3D.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Underlay.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Lines.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_SyncLine.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		DLogIf(FAILED(hr3), L"Geometric primitives InitDeviceObjects() failed with error {}", HR2Str(hr3));
	}
	ASSERT(S_OK == hr3);

	if (m_pFilter->m_inputMT.IsValid()) {
		if (!InitMediaType(&m_pFilter->m_inputMT)) {
			ReleaseDevice();
			return E_FAIL;
		}
	}

	if (m_hWnd) {
		hr = InitSwapChain(false);
		if (FAILED(hr)) {
			ReleaseDevice();
			return hr;
		}
	}

	SetCallbackDevice();
	UpdateSubPic();

	SetStereo3dTransform(m_iStereo3dTransform);

	HRESULT hr4 = m_TexDither.Create(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, dither_size, dither_size, Tex2D_DynamicShaderWrite);
	if (S_OK == hr4) {
		hr4 = GetDataFromResource(data, size, IDF_DITHER_32X32_FLOAT16);
		if (S_OK == hr4) {
			D3D11_MAPPED_SUBRESOURCE mappedResource;
			hr4 = m_pDeviceContext->Map(m_TexDither.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			if (S_OK == hr4) {
				uint16_t* src = (uint16_t*)data;
				BYTE* dst = (BYTE*)mappedResource.pData;
				for (UINT y = 0; y < dither_size; y++) {
					uint16_t* pUInt16 = reinterpret_cast<uint16_t*>(dst);
					for (UINT x = 0; x < dither_size; x++) {
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
					}
					src += dither_size;
					dst += mappedResource.RowPitch;
				}
				m_pDeviceContext->Unmap(m_TexDither.pTexture, 0);
			}
		}
		if (FAILED(hr4)) {
			m_TexDither.Release();
		}
	}

	m_pFilter->OnDisplayModeChange();
	UpdateStatsStatic();
	UpdateStatsByWindow();
	UpdateStatsByDisplay();

	return hr;
}

HRESULT CDX11VideoProcessor::InitSwapChain(bool bWindowChanged)
{
	DLog(L"CDX11VideoProcessor::InitSwapChain() - {}", m_pFilter->m_bExclusiveScreen ? L"fullscreen" : L"window");
	CheckPointer(m_pDXGIFactory2, E_FAIL);

	ReleaseSwapChain();

	auto bFullscreenChange = m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen;
	m_bExclusiveScreen = m_pFilter->m_bExclusiveScreen;

	if (bFullscreenChange || bWindowChanged) {
		HandleHDRToggle();
		UpdateBitmapShader();

		if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR10orHLG()) {
			m_bHdrAllowSwitchDisplay = false;
			InitMediaType(&m_pFilter->m_inputMT);
			m_bHdrAllowSwitchDisplay = true;

			if (m_pDXGISwapChain1) {
				DLog(L"CDX11VideoProcessor::InitSwapChain() - SwapChain was created during the call to InitMediaType(), exit");
				return S_OK;
			}
		}
	}

	const auto bHdrOutput = m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR);
	const auto b10BitOutput = bHdrOutput || Preferred10BitOutput();
	m_SwapChainFmt = b10BitOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;

	// HDR output needs the flip model, and Windows never takes a window back from it:
	// a swap chain made the old way on a window that has carried a flip one presents
	// picture after picture without error while the screen keeps the last flip
	// picture. That was the frozen picture, sound still running, when HDR output
	// stopped in full playback -- unticking RTX Video HDR, or anything that took it
	// away. So a window that has had the flip model keeps it: it shows everything the
	// old model showed (tools/dlssnr_probe: playback_test --toggle --gpu --hdr).
	const bool bFlipModel = (m_iSwapEffect == SWAPEFFECT_Flip && IsWindows8OrGreater())
		|| bHdrOutput || m_hWndFlipModel == m_hWnd;

	// How many buffers the flip model is given. Upstream gives an SDR picture two, the
	// one on the screen and the one presented: the next picture is then drawn into the
	// buffer still on the screen, and the GPU holds that draw, with everything queued
	// behind it on the device, until the flip. A renderer that waits for nothing never
	// sees it. One that waits for the GPU does, and waits for the next refresh with it:
	// at 25 Hz DLSS 5 NR waited 21.9 ms for its input on every picture and a picture
	// took 39.2 ms to draw, where a third buffer makes it 1.6 and 18.7 -- fullscreen,
	// exclusive and in a window alike (28.6 against 20.2 at 60 Hz), six buffers no
	// better than three. With Super Resolution after the network that was 47 pictures
	// of 517 left out and 44 repeated, against 2 and none. A shader prescaler pays it
	// too without waiting for anything: FSRCNNX 16 was ready in 39.2 ms and counted late
	// on every picture, in 17.7 with the third buffer. With nothing heavy in the chain
	// the two play the same. HDR output keeps upstream's six.
	// MPCVR_SWAPCHAIN_BUFFERS=n in the environment forces the count, to measure it again.
	static const UINT s_buffersAsked = [] {
		wchar_t value[16] = {};
		const DWORD length = GetEnvironmentVariableW(L"MPCVR_SWAPCHAIN_BUFFERS", value, (DWORD)std::size(value));
		return (length && length < std::size(value)) ? (UINT)std::clamp(_wtoi(value), 0, 16) : 0u;
	}();
	const UINT flipBuffers = s_buffersAsked >= 2 ? s_buffersAsked : (bHdrOutput ? 6u : 3u);

	HRESULT hr = S_OK;
	DXGI_SWAP_CHAIN_DESC1 desc1 = {};

	if (m_bExclusiveScreen) {
		MONITORINFOEXW mi = { sizeof(mi) };
		GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
		const CRect rc(mi.rcMonitor);

		desc1.Width = rc.Width();
		desc1.Height = rc.Height();
		desc1.Format = m_SwapChainFmt;
		desc1.SampleDesc.Count = 1;
		desc1.SampleDesc.Quality = 0;
		desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		if (bFlipModel) {
			desc1.BufferCount = flipBuffers;
			desc1.Scaling = DXGI_SCALING_NONE;
			desc1.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		} else { // SWAPEFFECT_Discard or Windows 7
			desc1.BufferCount = 1;
			desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		}
		desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
		fullscreenDesc.RefreshRate.Numerator = 0;
		fullscreenDesc.RefreshRate.Denominator = 1;
		fullscreenDesc.Windowed = FALSE;

		g_bCreateSwapChain = true;
		hr = m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc1, &fullscreenDesc, nullptr, &m_pDXGISwapChain1);
		g_bCreateSwapChain = false;
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::InitSwapChain() : CreateSwapChainForHwnd(fullscreen) failed with error {}", HR2Str(hr));

		m_lastFullscreenHMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY);
	}
	else {
		desc1.Width = std::max(8, m_windowRect.Width());
		desc1.Height = std::max(8, m_windowRect.Height());
		desc1.Format = m_SwapChainFmt;
		desc1.SampleDesc.Count = 1;
		desc1.SampleDesc.Quality = 0;
		desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		if (bFlipModel) {
			desc1.BufferCount = flipBuffers;
			desc1.Scaling = DXGI_SCALING_NONE;
			desc1.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		} else { // SWAPEFFECT_Discard or Windows 7
			desc1.BufferCount = 1;
			desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		}
		desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DLogIf(m_windowRect.Width() < 8 || m_windowRect.Height() < 8,
			L"CDX11VideoProcessor::InitSwapChain() : Invalid window size {}x{}, use {}x{}",
			m_windowRect.Width(), m_windowRect.Height(), desc1.Width, desc1.Height);

		hr = m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc1, nullptr, nullptr, &m_pDXGISwapChain1);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::InitSwapChain() : CreateSwapChainForHwnd() failed with error {}", HR2Str(hr));

		m_lastFullscreenHMonitor = nullptr;
	}

	if (m_pDXGISwapChain1) {
		if (bFlipModel) {
			m_hWndFlipModel = m_hWnd;
		}
		m_UsedSwapEffect = desc1.SwapEffect;

		HRESULT hr2 = m_pDXGISwapChain1->GetContainingOutput(&m_pDXGIOutput);

		m_currentSwapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		if (bHdrOutput) {
			hr2 = m_pDXGISwapChain1->QueryInterface(IID_PPV_ARGS(&m_pDXGISwapChain4));

			if (m_pDXGIOutput) {
				CComPtr<IDXGIOutput6> pDXGIOutput6;
				if (SUCCEEDED(m_pDXGIOutput->QueryInterface(IID_PPV_ARGS(&pDXGIOutput6)))) {
					DXGI_OUTPUT_DESC1 desc;
					if (SUCCEEDED(pDXGIOutput6->GetDesc1(&desc))) {
						m_MaxDisplayLuminance = static_cast<UINT>(desc.MaxLuminance);

						m_pFilter->UpdateDisplayInfo();
					}
				}
			}
		}
	}

	return hr;
}

BOOL CDX11VideoProcessor::VerifyMediaType(const CMediaType* pmt)
{
	const auto& FmtParams = GetFmtConvParams(pmt);
	if (FmtParams.VP11Format == DXGI_FORMAT_UNKNOWN && FmtParams.DX11Format == DXGI_FORMAT_UNKNOWN) {
		return FALSE;
	}

	const BITMAPINFOHEADER* pBIH = GetBIHfromVIHs(pmt);
	if (!pBIH) {
		return FALSE;
	}

	if (pBIH->biWidth <= 0 || !pBIH->biHeight) {
		return FALSE;
	}

	return TRUE;
}

bool CDX11VideoProcessor::HandleHDRToggle()
{
	CAutoLock cRendererLock(&m_HDRToggleLock);

	m_bHdrDisplaySwitching = true;
	bool bRet = false;
	bool bEnableHDR = false;

	if (!m_bHDRModeChangeOutside) {
		if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR()) {
			MONITORINFOEXW mi = { sizeof(mi) };
			GetMonitorInfoW(m_lastFullscreenHMonitor ? m_lastFullscreenHMonitor : MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
			DisplayConfig_t displayConfig = {};

			if (GetDisplayConfig(mi.szDevice, displayConfig)) {
				if (displayConfig.HDRSupported() && m_iHdrToggleDisplay) {
					bool bHDREnabled = false;
					std::wstring deviceName(mi.szDevice);
					if (auto it = m_hdrModeStartState.find(deviceName); it != m_hdrModeStartState.end()) {
						bHDREnabled = it->second;
					}

					const bool bNeedToggleOn = !displayConfig.HDREnabled() &&
											   (m_iHdrToggleDisplay == HDRTD_On || m_iHdrToggleDisplay == HDRTD_OnOff
											   || m_bExclusiveScreen || m_bFullScreen && (m_iHdrToggleDisplay == HDRTD_On_Fullscreen || m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen));
					const bool bNeedToggleOff = displayConfig.HDREnabled() &&
												!bHDREnabled && (!m_bExclusiveScreen || !m_bFullScreen) && m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen;
					DLog(L"HandleHDRToggle() : {}, {}", bNeedToggleOn, bNeedToggleOff);
					if (bNeedToggleOn) {
						bRet = ToggleHDR(displayConfig, true);
						DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR ON failed");

						if (bRet) {
							bEnableHDR = true;
							m_hdrModeSavedState.try_emplace(std::move(deviceName), false);
						}
					} else if (bNeedToggleOff) {
						bRet = ToggleHDR(displayConfig, false);
						DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR OFF failed");

						if (bRet) {
							m_hdrModeSavedState.try_emplace(std::move(deviceName), true);
						}
					}
				}
			}
		} else if (m_iHdrToggleDisplay) {
			MONITORINFOEXW mi = { sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
			DisplayConfig_t displayConfig = {};

			if (GetDisplayConfig(mi.szDevice, displayConfig)) {
				// check if HDR was already enabled in Windows before starting
				bool bWindowsHDREnabled = false;
				std::wstring deviceName(mi.szDevice);
				if (auto it = m_hdrModeStartState.find(deviceName); it != m_hdrModeStartState.end()) {
					bWindowsHDREnabled = it->second;
				}

				if (displayConfig.HDRSupported() && displayConfig.HDREnabled() &&
						(!bWindowsHDREnabled || m_iHdrToggleDisplay == HDRTD_OnOff || (m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen && m_bExclusiveScreen))) {
					bRet = ToggleHDR(displayConfig, false);
					DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR OFF failed");

					if (bRet) {
						m_hdrModeSavedState.try_emplace(std::move(deviceName), true);
					}
				}
			}
		}
	}

	if (bRet) {
		Sleep(100);
		FillDisplayParams();

		for (int i = 0; i < 5 && bEnableHDR && !m_bHdrDisplayModeEnabled; i++) {
			Sleep(100);
			FillDisplayParams();
		}
	}

	m_bHdrDisplaySwitching = false;

	return bRet;
}

bool CDX11VideoProcessor::ToggleHDR(const DisplayConfig_t& displayConfig, const bool bEnableAdvancedColor)
{
	DLog(L"ToggleHDR() : {} for '{}'", bEnableAdvancedColor, displayConfig.displayName);

	auto GetCurrentDisplayMode = [](LPCWSTR lpszDeviceName) -> std::optional<DEVMODEW> {
		DEVMODEW devmode = {};
		devmode.dmSize = sizeof(DEVMODEW);
		auto ret = EnumDisplaySettingsW(lpszDeviceName, ENUM_CURRENT_SETTINGS, &devmode);
		if (ret) {
			return devmode;
		}

		return {};
	};

	auto beforeModeOpt = GetCurrentDisplayMode(displayConfig.displayName);

	LONG ret = 1;

	if (IsWindows11_24H2OrGreater()) {
		DISPLAYCONFIG_SET_HDR_STATE setHdrState = {};
		setHdrState.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE);
		setHdrState.header.size = sizeof(setHdrState);
		setHdrState.header.adapterId = displayConfig.modeTarget.adapterId;
		setHdrState.header.id = displayConfig.modeTarget.id;
		setHdrState.enableHdr = bEnableAdvancedColor ? 1 : 0;

		ret = DisplayConfigSetDeviceInfo(&setHdrState.header);
		DLogIf(ERROR_SUCCESS != ret, L"ToggleHDR() : DisplayConfigSetDeviceInfo(DISPLAYCONFIG_SET_HDR_STATE) with '{}' failed with error {}", bEnableAdvancedColor, HR2Str(HRESULT_FROM_WIN32(ret)));
	} else {
		DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setColorState = {};
		setColorState.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
		setColorState.header.size = sizeof(setColorState);
		setColorState.header.adapterId = displayConfig.modeTarget.adapterId;
		setColorState.header.id = displayConfig.modeTarget.id;
		setColorState.enableAdvancedColor = bEnableAdvancedColor ? 1 : 0;

		ret = DisplayConfigSetDeviceInfo(&setColorState.header);
		DLogIf(ERROR_SUCCESS != ret, L"ToggleHDR() : DisplayConfigSetDeviceInfo(DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE) with '{}' failed with error {}", bEnableAdvancedColor, HR2Str(HRESULT_FROM_WIN32(ret)));
	}

	if (ret == ERROR_SUCCESS) {
		m_bDisplayModeChangeAfterHDRToggle = true;

		if (beforeModeOpt.has_value()) {
			auto afterModeOpt = GetCurrentDisplayMode(displayConfig.displayName);
			if (afterModeOpt.has_value()) {
				auto& beforeMode = *beforeModeOpt;
				auto& afterMode = *afterModeOpt;
				if (beforeMode.dmPelsWidth != afterMode.dmPelsWidth || beforeMode.dmPelsHeight != afterMode.dmPelsHeight
						|| beforeMode.dmBitsPerPel != afterMode.dmBitsPerPel || beforeMode.dmDisplayFrequency != afterMode.dmDisplayFrequency) {
					DLog(L"ToggleHDR() : Display mode changed from {}x{}@{} to {}x{}@{}, restoring",
						 beforeMode.dmPelsWidth, beforeMode.dmPelsHeight, beforeMode.dmDisplayFrequency,
						 afterMode.dmPelsWidth, afterMode.dmPelsHeight, afterMode.dmDisplayFrequency);

					auto ret = ChangeDisplaySettingsExW(displayConfig.displayName, &beforeMode, nullptr, CDS_FULLSCREEN, nullptr);
					DLogIf(DISP_CHANGE_SUCCESSFUL != ret, L"ToggleHDR() : ChangeDisplaySettingsExW() failed with error {}", HR2Str(HRESULT_FROM_WIN32(ret)));

					m_bDisplayModeChangeAfterHDRToggle = true;
				}
			}
		}
	}

	return ret == ERROR_SUCCESS;
}

BOOL CDX11VideoProcessor::InitMediaType(const CMediaType* pmt)
{
	DLog(L"CDX11VideoProcessor::InitMediaType()");

	if (!VerifyMediaType(pmt)) {
		return FALSE;
	}

	ReleaseVP();

	auto FmtParams = GetFmtConvParams(pmt);

	const BITMAPINFOHEADER* pBIH = nullptr;
	m_decExFmt.value = 0;

	if (pmt->formattype == FORMAT_VideoInfo2) {
		const VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)pmt->pbFormat;
		pBIH = &vih2->bmiHeader;
		m_srcRect = vih2->rcSource;
		m_srcAspectRatioX = vih2->dwPictAspectRatioX;
		m_srcAspectRatioY = vih2->dwPictAspectRatioY;
		if (FmtParams.CSType == CS_YUV && (vih2->dwControlFlags & (AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT))) {
			m_decExFmt.value = vih2->dwControlFlags;
			m_decExFmt.SampleFormat = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT; // ignore other flags
		}
		m_bInterlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced);
		m_rtAvgTimePerFrame = vih2->AvgTimePerFrame;
	}
	else if (pmt->formattype == FORMAT_VideoInfo) {
		const VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
		pBIH = &vih->bmiHeader;
		m_srcRect = vih->rcSource;
		m_srcAspectRatioX = 0;
		m_srcAspectRatioY = 0;
		m_bInterlaced = 0;
		m_rtAvgTimePerFrame = vih->AvgTimePerFrame;
	}
	else {
		return FALSE;
	}

	m_pFilter->m_FrameStats.SetStartFrameDuration(m_rtAvgTimePerFrame);
	m_pFilter->m_bValidBuffer = false;

	UINT biWidth  = pBIH->biWidth;
	UINT biHeight = labs(pBIH->biHeight);

	m_srcLines = biHeight * FmtParams.PitchCoeff / 2;
	m_srcPitch = biWidth * FmtParams.Packsize;
	switch (FmtParams.cformat) {
	case CF_Y8:
	case CF_NV12:
	case CF_RGB24:
	case CF_BGR48:
		m_srcPitch = ALIGN(m_srcPitch, 4);
		break;
	case CF_V210:
		m_srcPitch = ALIGN((biWidth + 5) / 6 * 16, 128);
	}
	if (pBIH->biCompression == BI_RGB && pBIH->biHeight > 0) {
		m_srcPitch = -m_srcPitch;
	}

	UINT origW = biWidth;
	UINT origH = biHeight;
	if (pmt->FormatLength() == VR_EXRADATA_POS + sizeof(VR_Extradata)) {
		const VR_Extradata* vrextra = reinterpret_cast<VR_Extradata*>(pmt->pbFormat + VR_EXRADATA_POS);
		if (vrextra->QueryWidth == pBIH->biWidth && vrextra->QueryHeight == pBIH->biHeight && vrextra->Compression == pBIH->biCompression) {
			origW  = vrextra->FrameWidth;
			origH = abs(vrextra->FrameHeight);
		}
	}

	if (m_srcRect.IsRectNull()) {
		m_srcRect.SetRect(0, 0, origW, origH);
	}
	m_srcRectWidth  = m_srcRect.Width();
	m_srcRectHeight = m_srcRect.Height();

	m_srcExFmt = SpecifyExtendedFormat(m_decExFmt, FmtParams, m_srcRectWidth, m_srcRectHeight);

	bool disableD3D11VP = false;
	switch (FmtParams.cformat) {
	case CF_NV12: disableD3D11VP = !m_VPFormats.bNV12; break;
	case CF_P010:
	case CF_P016: disableD3D11VP = !m_VPFormats.bP01x;  break;
	case CF_YUY2: disableD3D11VP = !m_VPFormats.bYUY2;  break;
	default:      disableD3D11VP = !m_VPFormats.bOther; break;
	}
	if (m_srcExFmt.VideoTransferMatrix == VIDEOTRANSFERMATRIX_YCgCo || m_Dovi.bValid) {
		disableD3D11VP = true;
	}
	if (FmtParams.CSType == CS_RGB && m_VendorId == PCIV_NVIDIA) {
		// D3D11 VP does not work correctly if RGB32 with odd frame width (source or target) on Nvidia adapters
		disableD3D11VP = true;
	}
	// Where the user asked for it, the shaders rebuild the chroma and the processor
	// still takes the picture, in 4:4:4. Read here, so that it means "the option
	// applies to this picture" and not "the processor was not going to take it".
	m_bChromaReplacedVP = !disableD3D11VP && ChromaToShaders(FmtParams, m_bInterlaced);
	if (disableD3D11VP) {
		FmtParams.VP11Format = DXGI_FORMAT_UNKNOWN;
	}

	const auto frm_gcd = std::gcd(m_srcRectWidth, m_srcRectHeight);
	const auto srcFrameARX = m_srcRectWidth / frm_gcd;
	const auto srcFrameARY = m_srcRectHeight / frm_gcd;

	if (!m_srcAspectRatioX || !m_srcAspectRatioY) {
		m_srcAspectRatioX = srcFrameARX;
		m_srcAspectRatioY = srcFrameARY;
		m_srcAnamorphic = false;
	}
	else {
		const auto ar_gcd = std::gcd(m_srcAspectRatioX, m_srcAspectRatioY);
		m_srcAspectRatioX /= ar_gcd;
		m_srcAspectRatioY /= ar_gcd;
		m_srcAnamorphic = (srcFrameARX != m_srcAspectRatioX || srcFrameARY != m_srcAspectRatioY);
	}

	UpdateUpscalingShaders();
	UpdateDownscalingShaders();

	m_pPSCorrection.Release();
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();
	m_PSConvColorData.bEnable = false;

	m_pPSHDR10ToneMapping.Release();
	m_pHDR10ToneMappingConstants.Release();
	m_pDoViDynamicConstants.Release();

	UpdateTexParams(FmtParams.CDepth);

	if (m_bHdrAllowSwitchDisplay && m_srcVideoTransferFunction != m_srcExFmt.VideoTransferFunction) {
		auto ret = HandleHDRToggle();
		if (!ret && ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_bHdrPassthroughSupport && SourceIsHDR10orHLG() && !m_pDXGISwapChain4)) {
			ret = true;
		}
		if (ret) {
			ReleaseSwapChain();
			Init(m_hWnd, false);
		}
	}

	if (Preferred10BitOutput() && m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
		ReleaseSwapChain();
		Init(m_hWnd, false);
	}

	m_srcVideoTransferFunction = m_srcExFmt.VideoTransferFunction;

	HRESULT hr = E_NOT_VALID_STATE;

	// D3D11 Video Processor
	if (FmtParams.VP11Format != DXGI_FORMAT_UNKNOWN) {
		hr = InitializeD3D11VP(FmtParams, origW, origH, pmt);
		if (SUCCEEDED(hr)) {
			UINT resId = 0;
			m_pCorrectionConstants.Release();
			bool bTransFunc22 = m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_22
								|| m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_709
								|| m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_240M;

			if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084 && !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) && m_bConvertToSdr) {
				resId = m_D3D11VP.IsPqSupported() ? IDF_PS_11_CONVERT_PQ_TO_SDR : IDF_PS_11_FIXCONVERT_PQ_TO_SDR;
				m_strCorrection = L"PQ to SDR";
			}
			else if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) {
				if (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
					resId = IDF_PS_11_CONVERT_HLG_TO_PQ;
					m_strCorrection = L"HLG to PQ";
				}
				else if (m_bConvertToSdr) {
					resId = IDF_PS_11_FIXCONVERT_HLG_TO_SDR;
					m_strCorrection = L"HLG to SDR";
				}
				else if (m_srcExFmt.VideoPrimaries == MFVideoPrimaries_BT2020) {
					// HLG compatible with SDR
					resId = IDF_PS_11_FIX_BT2020;
					m_strCorrection = L"Fix BT.2020";
				}
			}
			else if ((bTransFunc22 || !m_bHdrPassthroughSupport) && m_srcExFmt.VideoPrimaries == MFVideoPrimaries_BT2020) {
				resId = IDF_PS_11_FIX_BT2020;
				m_strCorrection = L"Fix BT.2020";
			}

			if (resId) {
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, resId));
				DLogIf(m_pPSCorrection, L"CDX11VideoProcessor::InitMediaType() m_pPSCorrection('{}') created", m_strCorrection);
				SetShaderLuminanceParams();
			}
		}
		else {
			ReleaseVP();
		}
	}

	// Tex Video Processor
	if (FAILED(hr) && FmtParams.DX11Format != DXGI_FORMAT_UNKNOWN) {
		m_bVPUseRTXVideoHDR = false;
		hr = InitializeTexVP(FmtParams, origW, origH);
		if (SUCCEEDED(hr)) {
			SetShaderConvertColorParams();
			SetShaderLuminanceParams();
		}
	}

	if (SUCCEEDED(hr)) {
		UpdateBitmapShader();
		UpdateTexures();
		UpdatePostScaleTexures();
		UpdateStatsStatic();

		m_pFilter->m_inputMT = *pmt;

		return TRUE;
	}

	return FALSE;
}

HRESULT CDX11VideoProcessor::InitializeD3D11VP(const FmtConvParams_t& params, const UINT width, const UINT height, const CMediaType* pmt)
{
	if (!m_D3D11VP.IsVideoDeviceOk()) {
		return E_ABORT;
	}

	// With its chroma rebuilt first, the processor reads a 4:4:4 picture: AYUV holds
	// eight bits a sample, Y410 ten, and both are 32 bits a pixel.
	m_VPInputFmt = m_bChromaReplacedVP
		? (params.CDepth > 8 ? DXGI_FORMAT_Y410 : DXGI_FORMAT_AYUV)
		: params.VP11Format;
	const auto& dxgiFormat = m_VPInputFmt;

	DLog(L"CDX11VideoProcessor::InitializeD3D11VP() started with input surface: {}, {} x {}", DXGIFormatToString(dxgiFormat), width, height);

	m_TexSrcVideo.Release();

	const bool bHdrPassthrough = m_bHdrDisplayModeEnabled && (SourceIsHDR10orHLG() || (m_bVPUseRTXVideoHDR && params.CDepth == 8));
	m_D3D11OutputFmt = m_InternalTexFmt;
	const int deinterlacing = m_bInterlaced ? m_iVPDeinterlacing : DEINT_Disable;
	HRESULT hr = m_D3D11VP.InitVideoProcessor(dxgiFormat, width, height, m_srcExFmt, deinterlacing, bHdrPassthrough, m_D3D11OutputFmt);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : InitVideoProcessor() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = m_D3D11VP.InitInputTextures(m_pDevice, m_bChromaReplacedVP);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : InitInputTextures() failed with error {}", HR2Str(hr));
		return hr;
	}

	auto rtxHDR = m_bVPRTXVideoHDR && m_bHdrPassthroughSupport && m_bHdrPassthrough && m_iTexFormat != TEXFMT_8INT && !SourceIsHDR();
	m_bVPUseRTXVideoHDR = (m_D3D11VP.SetRTXVideoHDR(rtxHDR) == S_OK);

	// The 4:4:4 pre-pass used to switch it off here, which cost a capability the
	// original has for an option of ours; the driver is left to answer for itself.
	//
	// Upstream also refuses it unless the picture is on its way to an 8-bit target
	// or RTX Video HDR is making the HDR, which rules out every real HDR film and
	// every 10-bit source -- while its own Configure() asks without that test, so
	// re-picking the same value from the list turned it on and ticking "Use for
	// resizing" did not. The two places ask the same question now, and it is the
	// one the driver answers yes to: on a PQ BT.2020 source in P010 the extension
	// changes the picture by 3.928, against 3.903 on the same frame in SDR
	// (tools/dlssnr_probe vp444_probe).
	UpdateVPSuperRes(params.CDepth);

	// Turning the driver's tone mapping on or off changes what the swap chain has to
	// be, and the swap chain is made before the media type: so the processor is set up
	// again, once, with the two agreeing. Upstream leaves that unbounded, and a state
	// the two cannot settle on then recurses through InitMediaType until the stack runs
	// out -- 0xC00000FD, seen here for real. One retry is all it ever legitimately
	// takes, measured: the picture is the same whether one is allowed or two.
	if ((m_bVPUseRTXVideoHDR && !m_pDXGISwapChain4)
			|| (!m_bVPUseRTXVideoHDR && m_pDXGISwapChain4 && !SourceIsHDR())) {
		if (m_nHdrSwapChainRetry) {
			DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : RTX Video HDR {} and the swap chain {} will not agree; left as they are",
				m_bVPUseRTXVideoHDR ? L"on" : L"off", m_pDXGISwapChain4 ? L"HDR" : L"SDR");
		} else {
			m_nHdrSwapChainRetry++;
			InitSwapChain(false);
			InitMediaType(pmt);
			m_nHdrSwapChainRetry--;
			return S_OK;
		}
	}

	// The chroma pass reads the picture plane by plane, as the shader processor does;
	// without it the processor takes the frame as it comes.
	hr = m_bChromaReplacedVP
		? m_TexSrcVideo.CreateEx(m_pDevice, params.DX11Format, params.pDX11Planes, width, height, Tex2D_DynamicShaderWrite)
		: m_TexSrcVideo.Create(m_pDevice, params.VP11Format, width, height, Tex2D_DynamicShaderWriteNoSRV);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : m_TexSrcVideo.Create() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_srcWidth       = width;
	m_srcHeight      = height;
	m_srcParams      = params;
	m_srcDXGIFormat  = params.VP11Format;   // what the decoder hands over, whatever the processor reads
	m_pCopyPlaneFn   = GetCopyPlaneFunction(params, m_bChromaReplacedVP ? VP_D3D11_SHADER : VP_D3D11);

	if (m_bChromaReplacedVP) {
		hr = UpdateConvertTo444Shader();
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : UpdateConvertTo444Shader() failed with error {}", HR2Str(hr));
			return hr;
		}
	}

	DLog(L"CDX11VideoProcessor::InitializeD3D11VP() completed successfully");

	return S_OK;
}

HRESULT CDX11VideoProcessor::InitializeTexVP(const FmtConvParams_t& params, const UINT width, const UINT height)
{
	const auto& srcDXGIFormat = params.DX11Format;

	DLog(L"CDX11VideoProcessor::InitializeTexVP() started with input surface: {}, {} x {}", DXGIFormatToString(srcDXGIFormat), width, height);

	HRESULT hr = m_TexSrcVideo.CreateEx(m_pDevice, srcDXGIFormat, params.pDX11Planes, width, height, Tex2D_DynamicShaderWrite);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeTexVP() : m_TexSrcVideo.CreateEx() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_srcWidth       = width;
	m_srcHeight      = height;
	m_srcParams      = params;
	m_srcDXGIFormat  = srcDXGIFormat;
	m_pCopyPlaneFn   = GetCopyPlaneFunction(params, VP_D3D11_SHADER);

	// set default ProcAmp ranges
	SetDefaultDXVA2ProcAmpRanges(m_DXVA2ProcAmpRanges);

	hr = UpdateConvertColorShader();

	SAFE_RELEASE(m_PSConvColorData.pVertexBuffer);
	EXECUTE_ASSERT(S_OK == CreateVertexBuffer(m_pDevice, &m_PSConvColorData.pVertexBuffer, m_srcWidth, m_srcHeight, m_srcRect, 0, false));

	DLog(L"CDX11VideoProcessor::InitializeTexVP() completed successfully");

	return S_OK;
}

void CDX11VideoProcessor::UpdatFrameProperties()
{
	m_srcPitch = m_srcWidth * m_srcParams.Packsize;
	m_srcLines = m_srcHeight * m_srcParams.PitchCoeff / 2;
}

BOOL CDX11VideoProcessor::GetAlignmentSize(const CMediaType& mt, SIZE& Size)
{
	if (VerifyMediaType(&mt)) {
		const auto& FmtParams = GetFmtConvParams(&mt);

		if (FmtParams.cformat == CF_RGB24) {
			Size.cx = ALIGN(Size.cx, 4);
		}
		else if (FmtParams.cformat == CF_RGB48 || FmtParams.cformat == CF_BGR48) {
			Size.cx = ALIGN(Size.cx, 2);
		}
		else if (FmtParams.cformat == CF_BGRA64 || FmtParams.cformat == CF_B64A) {
			// nothing
		}
		else {
			auto pBIH = GetBIHfromVIHs(&mt);
			if (!pBIH) {
				return FALSE;
			}

			auto biWidth = pBIH->biWidth;
			auto biHeight = labs(pBIH->biHeight);

			if (!m_Alignment.cx || m_Alignment.cformat != FmtParams.cformat
					|| m_Alignment.texture.desc.Width != biWidth || m_Alignment.texture.desc.Height != biHeight) {
				m_Alignment.texture.Release();
				m_Alignment.cformat = {};
				m_Alignment.cx = {};
			}

			if (!m_Alignment.texture.pTexture) {
				auto VP11Format = FmtParams.VP11Format;
				if (VP11Format != DXGI_FORMAT_UNKNOWN) {
					bool disableD3D11VP = false;
					switch (FmtParams.cformat) {
						case CF_NV12: disableD3D11VP = !m_VPFormats.bNV12;  break;
						case CF_P010:
						case CF_P016: disableD3D11VP = !m_VPFormats.bP01x;  break;
						case CF_YUY2: disableD3D11VP = !m_VPFormats.bYUY2;  break;
						default:      disableD3D11VP = !m_VPFormats.bOther; break;
					}
					if (disableD3D11VP) {
						VP11Format = DXGI_FORMAT_UNKNOWN;
					}
				}

				HRESULT hr = E_FAIL;
				if (VP11Format != DXGI_FORMAT_UNKNOWN) {
					hr = m_Alignment.texture.Create(m_pDevice, VP11Format, biWidth, biHeight, Tex2D_DynamicShaderWriteNoSRV);
				}
				if (FAILED(hr) && FmtParams.DX11Format != DXGI_FORMAT_UNKNOWN) {
					hr = m_Alignment.texture.CreateEx(m_pDevice, FmtParams.DX11Format, FmtParams.pDX11Planes, biWidth, biHeight, Tex2D_DynamicShaderWrite);
				}
				if (FAILED(hr)) {
					return FALSE;
				}

				UINT RowPitch = 0;
				D3D11_MAPPED_SUBRESOURCE mappedResource = {};
				if (SUCCEEDED(m_pDeviceContext->Map(m_Alignment.texture.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
					RowPitch = mappedResource.RowPitch;
					m_pDeviceContext->Unmap(m_Alignment.texture.pTexture, 0);
				}

				if (!RowPitch) {
					return FALSE;
				}

				m_Alignment.cformat = FmtParams.cformat;
				m_Alignment.cx = RowPitch / FmtParams.Packsize;
			}

			Size.cx = m_Alignment.cx;
		}

		if (FmtParams.cformat == CF_RGB24 || FmtParams.cformat == CF_XRGB32 || FmtParams.cformat == CF_ARGB32) {
			Size.cy = -abs(Size.cy); // only for biCompression == BI_RGB
		} else {
			Size.cy = abs(Size.cy);
		}

		return TRUE;

	}

	return FALSE;
}

HRESULT CDX11VideoProcessor::ProcessSample(IMediaSample* pSample)
{
	m_tickSampleStart = GetPreciseTick(); // render ahead measures the sample from here

	REFERENCE_TIME rtStart, rtEnd;
	if (FAILED(pSample->GetTime(&rtStart, &rtEnd))) {
		rtStart = m_pFilter->m_FrameStats.GeTimestamp();
	}
	const REFERENCE_TIME rtFrameDur = m_pFilter->m_FrameStats.GetAverageFrameDuration();
	rtEnd = rtStart + rtFrameDur;

	m_rtStart = rtStart;
	CRefTime rtClock(rtStart);

	// The schedule's way back into step when a picture costs most of a frame to draw
	// (CPresentSchedule::kMaxDropRun): this sample goes before anything is spent on it.
	// Only a film that is playing has a step to get back into.
	if (m_bSkipNextSample) {
		m_bSkipNextSample = false;
		if (m_bScheduled && m_pFilter->m_filterState == State_Running && !m_pFilter->m_bClockStalled && !m_pFilter->m_Stepping) {
			m_PresentSchedule.LeftOut();
			if (m_PacingTrace.Enabled()) {
				CPacingTrace::Picture traced;
				traced.rtStart = rtStart;
				traced.tickSample = traced.tickDrawn = (long long)m_tickSampleStart;
				traced.flags = CPacingTrace::Skipped | CPacingTrace::Scheduled;
				m_PacingTrace.Add(traced);
			}
			return S_FALSE;
		}
	}

	// The picture's GPU steps are timed from here, so that the chroma pre-pass, which
	// runs as the sample is copied, is timed with the rest (Render closes the set).
	if (m_bShowStats) {
		m_DlssStageTimes.Collect(m_pDeviceContext);
		m_DlssStageTimes.BeginFrame(m_pDevice, m_pDeviceContext);
	}

	HRESULT hr = CopySample(pSample);
	if (FAILED(hr)) {
		m_DlssStageTimes.EndFrame(m_pDeviceContext);
		m_RenderStats.failed++;
		return hr;
	}

	// always Render(1) a frame after CopySample()
	hr = Render(1, rtStart);
	if (FAILED(hr)) {
	}
	m_pFilter->m_DrawStats.Add(GetPreciseTick());
	if (m_pFilter->m_filterState == State_Running) {
		m_pFilter->StreamTime(rtClock);
	}

	// Against the time the picture is due, which is its stamp and the picture delay:
	// an offset that read +40 for a picture shown where it was asked to be would say
	// nothing of how the renderer is doing.
	m_RenderStats.syncoffset = rtClock - rtStart - m_rtVideoDelay;
	// The same offset carried forward to the refresh the picture is due on.
	m_bOnScreenKnown = m_tickOnScreen != 0;
	if (m_bOnScreenKnown) {
		m_rtOnScreenOffset = m_RenderStats.syncoffset
			+ (REFERENCE_TIME)(((double)m_tickOnScreen - (double)GetPreciseTick()) * 10000000.0 / GetPreciseTicksPerSecond());
	}
	// A picture presented for a refresh chosen in advance is long gone from here by
	// the time its own comes: when Present returned says nothing about it. What is
	// worth showing, and drawing, is where its refresh is against its time.
	if (m_bScheduled) {
		m_RenderStats.syncoffset = m_rtScheduledOffset;
	}

	// With a refresh for every frame, a picture is presented once the one before it
	// has been shown and is shown a refresh later: a film that starts late stays late
	// by exactly as much, for as long as it plays. Measured at 24 fps on 24 Hz and at
	// 25 on 25: 48 to 61 ms after its time, unchanged after 20 s. One picture left
	// out lets the next take the refresh before, and the film is back within half a
	// refresh of its time.
	//
	// What tells that state from a healthy one is not how late the picture is but
	// how long it waits: called at its time, Present has it shown on the next
	// refresh, less than one period away, and in the late chain on the one after.
	// Lateness alone will not do. A 23.976 fps film on 24 Hz slides through every
	// phase, and a threshold set on it left out a picture on each pass -- measured,
	// twenty in ten seconds. The wait must exceed a period by a margin, so that the
	// picture moved a refresh earlier still has that margin before its refresh, and
	// must have done so for eight pictures running.
	{
		const double period = m_dRefreshPeriodMs * 10000.0;
		const bool bRefreshPerFrame = period > 0 && rtFrameDur > 0.9 * period && rtFrameDur < 1.5 * period;
		const double waitMs = ((double)m_tickOnScreen - (double)m_tickPresentCall) * 1000.0 / GetPreciseTicksPerSecond();
		//
		// And it is done once, not as a habit. A picture can wait more than a period
		// for reasons a missing picture does nothing about -- a GPU that has not
		// finished it, a screen that does not refresh as it says. Leaving pictures out
		// then costs them and mends nothing: on a television at 25 Hz it went on at
		// two a second. So the pictures that follow one left out are the judges: if
		// they wait as long as before eight times running, nothing more is left out
		// until the stream is flushed. If they wait a refresh less and still too long,
		// the film was two refreshes behind, and a second picture goes.
		// None of it applies to a picture that was given its refresh in advance: the
		// schedule keeps the step itself, and knows a late chain cannot form.
		const bool bEligible = m_bOnScreenKnown && bRefreshPerFrame && !m_bDoubleFrames && !m_bScheduled
			&& m_pFilter->m_filterState == State_Running && !m_pFilter->m_bClockStalled;
		if (!bEligible) {
			m_nLateOnScreen = 0;
		} else if (waitMs > 1.15 * m_dRefreshPeriodMs) {
			if (++m_nLateOnScreen >= 8 && !m_bCatchUpFutile) {
				m_nLateOnScreen = 0;
				if (m_bCatchUpJudged && waitMs > m_dWaitAtCatchUp - 0.5 * m_dRefreshPeriodMs) {
					m_bCatchUpFutile = true;
				} else {
					m_bCatchUpJudged = true;
					m_dWaitAtCatchUp = waitMs;
					m_bSkipPresent = true;
				}
			}
		} else {
			m_nLateOnScreen = 0;
			m_bCatchUpJudged = false;
		}
	}

	int so = (int)std::clamp(m_RenderStats.syncoffset, -UNITS, UNITS);
#if SYNC_OFFSET_EX
	m_SyncDevs.Add(so - m_Syncs.Last());
#endif
	m_Syncs.Add(so);

	if (m_bDoubleFrames) {
		if (rtEnd + m_rtVideoDelay < rtClock) {
			m_RenderStats.dropped2++;
			return S_FALSE; // skip frame
		}

		rtStart += rtFrameDur / 2;

		hr = Render(2, rtStart);
		m_pFilter->m_DrawStats.Add(GetPreciseTick());
		if (m_pFilter->m_filterState == State_Running) {
			m_pFilter->StreamTime(rtClock);
		}

		m_RenderStats.syncoffset = rtClock - rtStart - m_rtVideoDelay;

		so = (int)std::clamp(m_RenderStats.syncoffset, -UNITS, UNITS);
#if SYNC_OFFSET_EX
		m_SyncDevs.Add(so - m_Syncs.Last());
#endif
		m_Syncs.Add(so);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::CopySample(IMediaSample* pSample)
{
	if (!m_pDXGISwapChain1) {
	}
	CheckPointer(m_pDXGISwapChain1, E_FAIL);

	uint64_t tick = GetPreciseTick();

	// Get frame type
	m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; // Progressive
	m_bDoubleFrames = false;
	if (m_bInterlaced) {
		if (CComQIPtr<IMediaSample2> pMS2 = pSample) {
			AM_SAMPLE2_PROPERTIES props;
			if (SUCCEEDED(pMS2->GetProperties(sizeof(props), (BYTE*)&props))) {
				if ((props.dwTypeSpecificFlags & AM_VIDEO_FLAG_WEAVE) == 0) {
					if (props.dwTypeSpecificFlags & AM_VIDEO_FLAG_FIELD1FIRST) {
						m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST; // Top-field first
					} else {
						m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST; // Bottom-field first
					}
					m_bDoubleFrames = m_iVPDeinterlacing && m_bDeintDouble && m_D3D11VP.IsReady();
				}
			}
		}
	}

	HRESULT hr = S_OK;
	m_FieldDrawn = 0;
	bool updateStats = false;

	m_hdr10 = {};
	if (CComQIPtr<IMediaSideData> pMediaSideData = pSample) {
		if (SourceIsHDR10orHLG() && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
			MediaSideDataHDR* hdr = nullptr;
			size_t size = 0;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataHDR, (const BYTE**)&hdr, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataHDR)) {
				const auto& primaries_x = hdr->display_primaries_x;
				const auto& primaries_y = hdr->display_primaries_y;
				if (primaries_x[0] > 0. && primaries_x[1] > 0. && primaries_x[2] > 0.
						&& primaries_y[0] > 0. && primaries_y[1] > 0. && primaries_y[2] > 0.
						&& hdr->white_point_x > 0. && hdr->white_point_y > 0.
						&& hdr->max_display_mastering_luminance > 0. && hdr->min_display_mastering_luminance > 0.) {
					m_hdr10.bValid = true;

					m_hdr10.hdr10.RedPrimary[0]   = static_cast<UINT16>(std::lround(primaries_x[2] * 50000.0));
					m_hdr10.hdr10.RedPrimary[1]   = static_cast<UINT16>(std::lround(primaries_y[2] * 50000.0));
					m_hdr10.hdr10.GreenPrimary[0] = static_cast<UINT16>(std::lround(primaries_x[0] * 50000.0));
					m_hdr10.hdr10.GreenPrimary[1] = static_cast<UINT16>(std::lround(primaries_y[0] * 50000.0));
					m_hdr10.hdr10.BluePrimary[0]  = static_cast<UINT16>(std::lround(primaries_x[1] * 50000.0));
					m_hdr10.hdr10.BluePrimary[1]  = static_cast<UINT16>(std::lround(primaries_y[1] * 50000.0));
					m_hdr10.hdr10.WhitePoint[0]   = static_cast<UINT16>(std::lround(hdr->white_point_x * 50000.0));
					m_hdr10.hdr10.WhitePoint[1]   = static_cast<UINT16>(std::lround(hdr->white_point_y * 50000.0));

					m_hdr10.hdr10.MaxMasteringLuminance = static_cast<UINT>(std::lround(hdr->max_display_mastering_luminance));
					m_hdr10.hdr10.MinMasteringLuminance = static_cast<UINT>(std::lround(hdr->min_display_mastering_luminance * 10000.0));
				}
			}

			MediaSideDataHDRContentLightLevel* hdrCLL = nullptr;
			size = 0;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataHDRContentLightLevel, (const BYTE**)&hdrCLL, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataHDRContentLightLevel)) {
				m_hdr10.hdr10.MaxContentLightLevel      = hdrCLL->MaxCLL;
				m_hdr10.hdr10.MaxFrameAverageLightLevel = hdrCLL->MaxFALL;
			}
		}

		size_t size = 0;
		MediaSideData3DOffset* offset = nullptr;
		hr = pMediaSideData->GetSideData(IID_MediaSideData3DOffset, (const BYTE**)&offset, &size);
		if (SUCCEEDED(hr) && size == sizeof(MediaSideData3DOffset) && offset->offset_count > 0 && offset->offset[0]) {
			m_nStereoSubtitlesOffsetInPixels = offset->offset[0];
		}

		if (m_srcParams.CSType == CS_YUV && (m_bHdrPreferDoVi || !SourceIsHDR10orHLG())) {
			MediaSideDataDOVIMetadata* pDOVIMetadata = nullptr;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataDOVIMetadataV2, (const BYTE**)&pDOVIMetadata, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataDOVIMetadata) && CheckDoviMetadata(pDOVIMetadata, 1)) {
				const bool bYCCtoRGBChanged = !m_PSConvColorData.bEnable ||
					(memcmp(
						&m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix,
						&pDOVIMetadata->ColorMetadata.ycc_to_rgb_matrix,
						sizeof(MediaSideDataDOVIMetadata::ColorMetadata.ycc_to_rgb_matrix) + sizeof(MediaSideDataDOVIMetadata::ColorMetadata.ycc_to_rgb_offset)
					) != 0);
				const bool bRGBtoLMSChanged =
					(memcmp(
						&m_Dovi.msd.ColorMetadata.rgb_to_lms_matrix,
						&pDOVIMetadata->ColorMetadata.rgb_to_lms_matrix,
						sizeof(MediaSideDataDOVIMetadata::ColorMetadata.rgb_to_lms_matrix)
					) != 0);
				const bool bMappingCurvesChanged = !m_pDoviCurvesConstantBuffer ||
					(memcmp(
						&m_Dovi.msd.Mapping.curves,
						&pDOVIMetadata->Mapping.curves,
						sizeof(MediaSideDataDOVIMetadata::Mapping.curves)
					) != 0);

				const bool bMasteringLuminanceChanged = m_Dovi.msd.ColorMetadata.source_max_pq != pDOVIMetadata->ColorMetadata.source_max_pq
					|| m_Dovi.msd.ColorMetadata.source_min_pq != pDOVIMetadata->ColorMetadata.source_min_pq;

				bool bMMRChanged = false;
				if (bMappingCurvesChanged) {
					bool has_mmr = false;
					for (const auto& curve : pDOVIMetadata->Mapping.curves) {
						for (uint8_t i = 0; i < (curve.num_pivots - 1); i++) {
							if (curve.mapping_idc[i] == 1) {
								has_mmr = true;
								break;
							}
						}
					}
					if (m_Dovi.bHasMMR != has_mmr) {
						m_Dovi.bHasMMR = has_mmr;
						m_pDoviCurvesConstantBuffer.Release();
						bMMRChanged = true;
					}
				}

				memcpy(&m_Dovi.msd, pDOVIMetadata, size);
				const bool doviStateChanged = !m_Dovi.bValid;
				m_Dovi.bValid = true;

				// based on libplacebo source code
				constexpr float
					PQ_M1 = 2610.f / (4096.f * 4.f),
					PQ_M2 = 2523.f / 4096.f * 128.f,
					PQ_C1 = 3424.f / 4096.f,
					PQ_C2 = 2413.f / 4096.f * 32.f,
					PQ_C3 = 2392.f / 4096.f * 32.f;

				auto PqToLinearNits = [](float x) {
					x = powf(x, 1.0f / PQ_M2);
					x = fmaxf(x - PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * x);
					x = powf(x, 1.0f / PQ_M1);
					return x * 10000.0f;
				};
				auto LinearNitsToPq = [](float y) {
					y /= 10000.0f;
					y = fmaxf(y, 0.0f);
					y = powf(y, PQ_M1);
					y = (PQ_C1 + PQ_C2 * y) / (1.0f + PQ_C3 * y);
					return powf(y, PQ_M2);
				};

				// Level 1 + 3
				for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
					if (pDOVIMetadata->Extensions[i].level == 1) {
						auto& Level1 = pDOVIMetadata->Extensions[i].Level1;

						m_DoviExtensionMetadata.L1.present = true;
						m_DoviExtensionMetadata.L1.min_pq = Level1.min_pq;
						m_DoviExtensionMetadata.L1.max_pq = Level1.max_pq;
						m_DoviExtensionMetadata.L1.avg_pq = Level1.avg_pq;

						for (uint32_t k = 0; k < LAV_DOVI_MAX_EXTENSIONS; ++k) {
							if (pDOVIMetadata->Extensions[k].level == 3) {
								auto& Level3 = pDOVIMetadata->Extensions[k].Level3;

								m_DoviExtensionMetadata.L1.min_pq = m_DoviExtensionMetadata.L1.min_pq + Level3.min_pq_offset - 2048;
								m_DoviExtensionMetadata.L1.max_pq = m_DoviExtensionMetadata.L1.max_pq + Level3.max_pq_offset - 2048;
								m_DoviExtensionMetadata.L1.avg_pq = m_DoviExtensionMetadata.L1.avg_pq + Level3.avg_pq_offset - 2048;

								break;
							}
						}

						m_DoviExtensionMetadata.L1.min_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.min_pq / 4095.f));
						m_DoviExtensionMetadata.L1.max_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.max_pq / 4095.f));
						m_DoviExtensionMetadata.L1.avg_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.avg_pq / 4095.f));

						if (m_bHdrPassthroughSupport && m_bHdrLocalToneMapping) {
							if (m_DoviExtensionMetadata.L1 != m_DoviExtensionMetadata.L1Cached) {
								m_DoviExtensionMetadata.L1Cached = m_DoviExtensionMetadata.L1;
								UpdateStatsStatic();
							}
						}

						break;
					}
				}

				// Level 2
				float display_pq = LinearNitsToPq(m_iHdrDisplayMaxNits);
				int lower_index = -1, upper_index = -1;
				float closest_lower_dist = 1.0f, closest_upper_dist = 1.0f;
				bool level2Present = false;
				uint16_t max_target_max_pq = std::numeric_limits<uint16_t>::max();

				for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
					if (pDOVIMetadata->Extensions[i].level == 2) {
						level2Present = true;

						auto& Level2 = pDOVIMetadata->Extensions[i].Level2;

						if (!m_bHdrPassthroughSupport) {
							if (Level2.target_max_pq < max_target_max_pq) {
								max_target_max_pq = Level2.target_max_pq;
								upper_index = i;
							}
						} else {
							float target_pq = Level2.target_max_pq / 4095.0f;
							if (target_pq <= display_pq) {
								float dist = display_pq - target_pq;
								if (dist < closest_lower_dist) {
									closest_lower_dist = dist;
									lower_index = i;
								}
							} else {
								float dist = target_pq - display_pq;
								if (dist < closest_upper_dist) {
									closest_upper_dist = dist;
									upper_index = i;
								}
							}
						}
					}
				}

				if (level2Present) {
					float t_slope = 1.0f, t_offset = 0.0f, t_power = 1.0f;
					float t_chroma = 0.0f, t_sat = 0.0f;

					// SCENARIO A: Display is BETWEEN two targets
					if (lower_index != -1 && upper_index != -1) {
						float lower_pq = pDOVIMetadata->Extensions[lower_index].Level2.target_max_pq / 4095.0f;
						float upper_pq = pDOVIMetadata->Extensions[upper_index].Level2.target_max_pq / 4095.0f;

						float weight = (upper_pq != lower_pq) ? (display_pq - lower_pq) / (upper_pq - lower_pq) : 0.0f;
						weight = std::clamp(weight, 0.0f, 1.0f);

						t_slope = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_slope),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_slope),
											weight);
						t_offset = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_offset),
												static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_offset),
												weight);
						t_power = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_power),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_power),
											weight);
						t_chroma = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_chroma_weight),
												static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_chroma_weight),
												weight);
						t_sat = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_saturation_gain),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_saturation_gain),
											weight);
					}
					// SCENARIO B: Display is BRIGHTER than all targets (Interpolate towards Master/Neutral)
					else if (lower_index != -1 && upper_index == -1) {
						float master_pq = pDOVIMetadata->ColorMetadata.source_max_pq / 4095.0f;

						float lower_pq = pDOVIMetadata->Extensions[lower_index].Level2.target_max_pq / 4095.0f;
						float weight = (master_pq > lower_pq) ? (display_pq - lower_pq) / (master_pq - lower_pq) : 0.0f;
						weight = std::clamp(weight, 0.0f, 1.0f);

						t_slope = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_slope), 2048.0f, weight);
						t_offset = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_offset), 2048.0f, weight);
						t_power = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_power), 2048.0f, weight);
						t_chroma = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_chroma_weight), 2048.0f, weight);
						t_sat = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_saturation_gain), 2048.0f, weight);
					}
					// SCENARIO C: Display is DIMMER than all targets (Clamp to the lowest available target)
					else if (lower_index == -1 && upper_index != -1) {
						t_slope = pDOVIMetadata->Extensions[upper_index].Level2.trim_slope;
						t_offset = pDOVIMetadata->Extensions[upper_index].Level2.trim_offset;
						t_power = pDOVIMetadata->Extensions[upper_index].Level2.trim_power;
						t_chroma = pDOVIMetadata->Extensions[upper_index].Level2.trim_chroma_weight;
						t_sat = pDOVIMetadata->Extensions[upper_index].Level2.trim_saturation_gain;
					}

					// Final normalization to floating point coefficients
					m_DoviExtensionMetadata.L2.present = true;
					m_DoviExtensionMetadata.L2.trim_slope = t_slope / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_offset = t_offset / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_power = t_power / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_saturation_gain = t_sat / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_chroma_weight = t_chroma / 4096.0f;
				}

				SetDolbyVisionDynamicParams();

				if (bMasteringLuminanceChanged) {
					m_DoviMaxMasteringLuminance = static_cast<UINT>(PqToLinearNits(m_Dovi.msd.ColorMetadata.source_max_pq / 4095.f));
					m_DoviMinMasteringLuminance = static_cast<UINT>(PqToLinearNits(m_Dovi.msd.ColorMetadata.source_min_pq / 4095.f) * 10000.0f);

					if (size == sizeof(MediaSideDataDOVIMetadata)) {
						for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
							if (pDOVIMetadata->Extensions[i].level == 6) {
								auto& Level6 = pDOVIMetadata->Extensions[i].Level6;

								m_DoviMaxMasteringLuminance = Level6.max_luminance;
								m_DoviMinMasteringLuminance = Level6.min_luminance;
								m_DoviMaxContentLightLevel = Level6.max_cll;
								m_DoviMaxFrameAverageLightLevel = Level6.max_fall;

								break;
							}
						}
					}
				}

				if (m_D3D11VP.IsReady()) {
					InitMediaType(&m_pFilter->m_inputMT);
				}
				else if (doviStateChanged) {
					UpdateStatsStatic();
				}

				if (bYCCtoRGBChanged) {
					DLog(L"CDX11VideoProcessor::CopySample() : DoVi ycc_to_rgb_matrix is changed");
					SetShaderConvertColorParams();
				}
				if (bRGBtoLMSChanged || bMMRChanged) {
					DLogIf(bRGBtoLMSChanged, L"CDX11VideoProcessor::CopySample() : DoVi rgb_to_lms_matrix is changed");
					DLogIf(bMMRChanged, L"CDX11VideoProcessor::CopySample() : DoVi has_mmr is changed");
					UpdateConvertColorShader();
				}
				if (bMappingCurvesChanged) {
					if (m_Dovi.bHasMMR) {
						hr = SetShaderDoviCurves();
					} else {
						hr = SetShaderDoviCurvesPoly();
					}
				}

				if (doviStateChanged && !SourceIsHDR10orHLG()) {
					ReleaseSwapChain();
					Init(m_hWnd, false);

					m_srcVideoTransferFunction = 0;
					InitMediaType(&m_pFilter->m_inputMT);
				}
			}
		}
	}

	if (CComQIPtr<IMediaSampleD3D11> pMSD3D11 = pSample) {
		if (m_iSrcFromGPU != 11) {
			m_iSrcFromGPU = 11;
			updateStats = true;
		}

		if (m_bChromaReplacedVP && m_D3D11VP.IsReady() && m_TexSrcVideo.desc.Usage == D3D11_USAGE_DYNAMIC) {
			// The frame arrives in the decoder's own texture and is copied into this
			// one, which the chroma pass then reads. A dynamic texture, the one a
			// frame from memory is mapped into, cannot take a copy.
			hr = m_TexSrcVideo.CreateEx(m_pDevice, m_srcParams.DX11Format, m_srcParams.pDX11Planes,
				m_srcWidth, m_srcHeight, Tex2D_DefaultShader);
			if (FAILED(hr)) {
				DLog(L"CDX11VideoProcessor::CopySample() : m_TexSrcVideo.CreateEx() failed with error {}", HR2Str(hr));
				return hr;
			}
		}

		CComQIPtr<ID3D11Texture2D> pD3D11Texture2D;
		UINT ArraySlice = 0;
		hr = pMSD3D11->GetD3D11Texture(0, &pD3D11Texture2D, &ArraySlice);
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::CopySample() : GetD3D11Texture() failed with error {}", HR2Str(hr));
			return hr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		pD3D11Texture2D->GetDesc(&desc);
		if (desc.Format != m_srcDXGIFormat) {
			return E_UNEXPECTED;
		}

#if 0 // fix for issue #16. disable reinitialization code.
		if (desc.Width != m_srcWidth || desc.Height != m_srcHeight) {
			if (m_D3D11VP.IsReady()) {
				hr = InitializeD3D11VP(m_srcParams, desc.Width, desc.Height);
			} else {
				hr = InitializeTexVP(m_srcParams, desc.Width, desc.Height);
			}
			if (FAILED(hr)) {
				return hr;
			}
			UpdatFrameProperties();
			updateStats = true;
		}
#endif

		if (m_D3D11VP.IsReady() && m_bChromaReplacedVP) {
			// The chroma pass reads the planes, so the frame leaves the decoder's own
			// texture and is written back in 4:4:4 into the processor's.
			D3D11_BOX srcBox = { 0, 0, 0, m_srcWidth, m_srcHeight, 1 };
			m_pDeviceContext->CopySubresourceRegion(m_TexSrcVideo.pTexture, 0, 0, 0, 0, pD3D11Texture2D, ArraySlice, &srcBox);
			m_D3D11VP.GetNextInputTexture(m_SampleFormat);
			ConvertTo444Pass(m_D3D11VP.GetInputUav());
		} else if (m_D3D11VP.IsReady()) {
			m_D3D11VP.SetInputVideoData(pD3D11Texture2D, pSample, ArraySlice, m_SampleFormat);
		} else {
			// here should be used CopySubresourceRegion instead of CopyResource
			D3D11_BOX srcBox = { 0, 0, 0, m_srcWidth, m_srcHeight, 1 };
			m_pDeviceContext->CopySubresourceRegion(m_TexSrcVideo.pTexture, 0, 0, 0, 0, pD3D11Texture2D, ArraySlice, &srcBox);
		}
	}
	else {
		if (m_iSrcFromGPU != 0) {
			m_iSrcFromGPU = 0;
			updateStats = true;
		}

		if (m_bChromaReplacedVP && m_TexSrcVideo.pTexture && m_TexSrcVideo.desc.Usage != D3D11_USAGE_DYNAMIC) {
			// A frame from memory is mapped into this texture, so it goes back to
			// dynamic, whatever a decoder texture made of it before.
			hr = m_TexSrcVideo.CreateEx(m_pDevice, m_srcParams.DX11Format, m_srcParams.pDX11Planes,
				m_srcWidth, m_srcHeight, Tex2D_DynamicShaderWrite);
			if (FAILED(hr)) {
				DLog(L"CDX11VideoProcessor::CopySample() : m_TexSrcVideo.CreateEx() failed with error {}", HR2Str(hr));
				return hr;
			}
		}

		BYTE* data = nullptr;
		const int size = pSample->GetActualDataLength();
		if (size >= abs(m_srcPitch) * (int)m_srcLines && S_OK == pSample->GetPointer(&data)) {
			// do not use UpdateSubresource for D3D11 VP here
			// because it can cause green screens and freezes on some configurations
			hr = MemCopyToTexSrcVideo(data, m_srcPitch);
			if (m_D3D11VP.IsReady()) {
				// ID3D11VideoProcessor does not use textures with D3D11_CPU_ACCESS_WRITE flag
				ID3D11Texture2D* pInput = m_D3D11VP.GetNextInputTexture(m_SampleFormat);
				if (m_bChromaReplacedVP) {
					ConvertTo444Pass(m_D3D11VP.GetInputUav());
				} else {
					m_pDeviceContext->CopyResource(pInput, m_TexSrcVideo.pTexture);
				}
			}
		}
	}

	if (updateStats) {
		UpdateStatsStatic();
	}

	m_RenderStats.copyticks = GetPreciseTick() - tick;

	return hr;
}

HRESULT CDX11VideoProcessor::Render(int field, const REFERENCE_TIME frameStartTime)
{
	CheckPointer(m_TexSrcVideo.pTexture, E_FAIL);
	CheckPointer(m_pDXGISwapChain1, E_FAIL);

	// A redraw while the film plays is a Present nobody planned. The player asks for
	// one whenever it moves the picture or changes what it writes over it -- its
	// message after a change of sound track, of subtitles, a seek -- and that Present
	// lands in the same refresh as a picture's own: the picture waits for it, goes in a
	// refresh late, and the queue is one too long from then on. The picture that is
	// due within a frame carries whatever changed, and is on time.
	if (!field && m_bScheduled && m_pFilter->m_filterState == State_Running
			&& (double)(GetPreciseTick() - m_tickPresentCall) < 0.25 * GetPreciseTicksPerSecond()) {
		return S_OK;
	}

	if (field) {
		m_FieldDrawn = field;
		m_bDlssNewPicture = true; // not a redraw: the stabilizer may advance
		m_bDlssSRNewPicture = true;
	}

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::Render() : GetBuffer() failed with error {}", HR2Str(hr));
		return hr;
	}

	uint64_t tick1 = GetPreciseTick();

	if (!m_windowRect.IsRectEmpty()) {
		// fill the BackBuffer with black
		ID3D11RenderTargetView* pRenderTargetView;
		if (S_OK == m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView)) {
			const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			m_pDeviceContext->ClearRenderTargetView(pRenderTargetView, ClearColor);
			pRenderTargetView->Release();
		}
	}

	if (m_pDXGISwapChain4) {
		if (m_Dovi.bValid) {
			if (!m_hdr10.bValid && m_lastHdr10.bValid) {
				m_hdr10.bValid = true;
				m_hdr10.hdr10 = m_lastHdr10.hdr10;
			}

			if (m_hdr10.bValid) {
				if (m_DoviMaxMasteringLuminance > m_hdr10.hdr10.MaxMasteringLuminance) {
					m_hdr10.hdr10.MaxMasteringLuminance = m_DoviMaxMasteringLuminance;
				}
				if (m_DoviMinMasteringLuminance && m_DoviMinMasteringLuminance != m_hdr10.hdr10.MinMasteringLuminance) {
					m_hdr10.hdr10.MinMasteringLuminance = m_DoviMinMasteringLuminance;
				}
				if (m_DoviMaxContentLightLevel && m_DoviMaxContentLightLevel != m_hdr10.hdr10.MaxContentLightLevel) {
					m_hdr10.hdr10.MaxContentLightLevel = m_DoviMaxContentLightLevel;
				}
				if (m_DoviMaxFrameAverageLightLevel && m_DoviMaxFrameAverageLightLevel != m_hdr10.hdr10.MaxFrameAverageLightLevel) {
					m_hdr10.hdr10.MaxFrameAverageLightLevel = m_DoviMaxFrameAverageLightLevel;
				}
			}
		}

		const DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		if (m_currentSwapChainColorSpace != colorSpace) {
			UINT colorSpaceSupport = 0;
			if (SUCCEEDED(m_pDXGISwapChain4->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport))
					&& (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) {
				hr = m_pDXGISwapChain4->SetColorSpace1(colorSpace);
				DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetColorSpace1() failed with error {}", HR2Str(hr));
				if (SUCCEEDED(hr)) {
					m_currentSwapChainColorSpace = colorSpace;

					if (!m_bVPUseRTXVideoHDR) {
						if (m_hdr10.bValid) {
							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_hdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(hdr) failed with error {}", HR2Str(hr));
							}

							m_lastHdr10 = m_hdr10;
							UpdateStatsStatic();
						} else if (m_lastHdr10.bValid) {
							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_lastHdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(lastHdr) failed with error {}", HR2Str(hr));
							}
						} else {
							m_lastHdr10.bValid = true;

							m_lastHdr10.hdr10.RedPrimary[0] = 34000; // Display P3 primaries
							m_lastHdr10.hdr10.RedPrimary[1] = 16000;
							m_lastHdr10.hdr10.GreenPrimary[0] = 13250;
							m_lastHdr10.hdr10.GreenPrimary[1] = 34500;
							m_lastHdr10.hdr10.BluePrimary[0] = 7500;
							m_lastHdr10.hdr10.BluePrimary[1] = 3000;
							m_lastHdr10.hdr10.WhitePoint[0] = 15635;
							m_lastHdr10.hdr10.WhitePoint[1] = 16450;
							m_lastHdr10.hdr10.MaxMasteringLuminance = m_DoviMaxMasteringLuminance ? m_DoviMaxMasteringLuminance : 1000; // 1000 nits
							m_lastHdr10.hdr10.MinMasteringLuminance = m_DoviMinMasteringLuminance ? m_DoviMinMasteringLuminance : 50;   // 0.005 nits
							if (m_DoviMaxContentLightLevel) {
								m_lastHdr10.hdr10.MaxContentLightLevel = m_DoviMaxContentLightLevel;
							}
							if (m_DoviMaxFrameAverageLightLevel) {
								m_lastHdr10.hdr10.MaxFrameAverageLightLevel = m_DoviMaxFrameAverageLightLevel;
							}

							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_lastHdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(Display P3 standard) failed with error {}", HR2Str(hr));
							}

							UpdateStatsStatic();
						}
					}
				}
			}
		} else if (m_currentSwapChainColorSpace == colorSpace && m_hdr10.bValid) {
			if (memcmp(&m_hdr10.hdr10, &m_lastHdr10.hdr10, sizeof(m_hdr10.hdr10)) != 0) {
				if (m_bHdrPassthrough) {
					hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_hdr10.hdr10);
					DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(hdr) failed with error {}", HR2Str(hr));
				}

				m_lastHdr10 = m_hdr10;
				UpdateStatsStatic();
			}
		}

		if (m_bHdrLocalToneMapping && m_currentSwapChainColorSpace == colorSpace) {
			if (m_DoviExtensionMetadata.L1.present) {
				SetHDR10ShaderParams(m_DoviExtensionMetadata.L1.min_pq, m_DoviExtensionMetadata.L1.max_pq,
									 m_DoviExtensionMetadata.L1.max_pq, m_DoviExtensionMetadata.L1.avg_pq,
									 m_iHdrDisplayMaxNits, m_iHdrLocalToneMappingType == 5 ? 6 : m_iHdrLocalToneMappingType);
			} else if (m_lastHdr10.bValid) {
				SetHDR10ShaderParams(m_lastHdr10.hdr10.MinMasteringLuminance, m_lastHdr10.hdr10.MaxMasteringLuminance,
									 m_lastHdr10.hdr10.MaxContentLightLevel, m_lastHdr10.hdr10.MaxFrameAverageLightLevel,
									 m_iHdrDisplayMaxNits, m_iHdrLocalToneMappingType);
			}
		}
	}

	RecoverDlssNR();

	// GPU time of each step, only while the statistics show it. A new sample has had
	// its set opened before it was copied, where the chroma pre-pass runs
	// (ProcessSample); a redraw, or a second field, opens its own here.
	if (m_bShowStats && !m_DlssStageTimes.IsRecording()) {
		m_DlssStageTimes.Collect(m_pDeviceContext);
		m_DlssStageTimes.BeginFrame(m_pDevice, m_pDeviceContext);
	}

	m_tracePasses = 0;
	m_traceNRTiming = {};
	if (!m_renderRect.IsRectEmpty()) {
		hr = Process(pBackBuffer, m_srcRect, m_videoRect, m_FieldDrawn == 2);
	}

	m_DlssStageTimes.EndFrame(m_pDeviceContext);   // nothing to do when no set is open

	if (!m_pPSHalfOUtoInterlace) {
		DrawSubtitles(pBackBuffer);
	}

	if (m_bShowStats) {
		hr = DrawStats(pBackBuffer);
	}
	DrawNotice(pBackBuffer);

	if (m_bAlphaBitmapEnable) {
		D3D11_TEXTURE2D_DESC desc;
		pBackBuffer->GetDesc(&desc);
		D3D11_VIEWPORT VP = {
			m_AlphaBitmapNRectDest.left * desc.Width,
			m_AlphaBitmapNRectDest.top * desc.Height,
			(m_AlphaBitmapNRectDest.right - m_AlphaBitmapNRectDest.left) * desc.Width,
			(m_AlphaBitmapNRectDest.bottom - m_AlphaBitmapNRectDest.top) * desc.Height,
			0.0f,
			1.0f
		};
		hr = AlphaBlt(m_TexAlphaBitmap.pShaderResource, pBackBuffer,
			m_pAlphaBitmapVertex, &VP,
			m_pSamplerLinear);
	}

#if 0
	{ // Tearing test (very non-optimal implementation, use only for tests)
		static int nTearingPos = 0;

		ID3D11RenderTargetView* pRenderTargetView;
		if (S_OK == m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView)) {
			CD3D11Rectangle d3d11rect;
			HRESULT hr2 = d3d11rect.InitDeviceObjects(m_pDevice, m_pDeviceContext);

			const SIZE szWindow = m_windowRect.Size();
			RECT rcTearing;

			rcTearing.left = nTearingPos;
			rcTearing.top = 0;
			rcTearing.right = rcTearing.left + 4;
			rcTearing.bottom = szWindow.cy;
			hr2 = d3d11rect.Set(rcTearing, szWindow, D3DCOLOR_XRGB(255, 0, 0));
			hr2 = d3d11rect.Draw(pRenderTargetView, szWindow);

			rcTearing.left = (rcTearing.right + 15) % szWindow.cx;
			rcTearing.right = rcTearing.left + 4;
			hr2 = d3d11rect.Set(rcTearing, szWindow, D3DCOLOR_XRGB(255, 0, 0));
			hr2 = d3d11rect.Draw(pRenderTargetView, szWindow);

			pRenderTargetView->Release();
			d3d11rect.InvalidateDeviceObjects();

			nTearingPos = (nTearingPos + 7) % szWindow.cx;
		}
	}
#endif

	uint64_t tick3 = GetPreciseTick();
	m_RenderStats.paintticks = tick3 - tick1;

	// The trace of this picture, when one is asked for (CPacingTrace), and what the
	// GPU has finished measuring of those before it.
	if (m_PacingTrace.Enabled()) {
		m_SRCutReadback.Collect(m_pDeviceContext, [this](long long rtStart, const float* state) {
			m_PacingTrace.SetCut(rtStart, state);
		});
	}
	const bool bTrace = m_PacingTrace.Enabled() && field && frameStartTime != INVALID_TIME;
	CPacingTrace::Picture traced;
	if (bTrace) {
		NoteSwapChainForTrace();
		NoteDlssForTrace();
		traced.rtStart = frameStartTime;
		traced.tickSample = (long long)m_tickSampleStart;
		traced.tickDrawn = (long long)tick3;
		traced.renderAhead = GetRenderAhead();
		traced.flags = (field == 2 ? CPacingTrace::SecondField : 0)
			| (m_D3D11VP.IsReady() ? CPacingTrace::HardwareVP : 0)
			| (m_bVPUseRTXVideoHDR ? CPacingTrace::RtxVideoHDR : 0)
			| (m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? CPacingTrace::HdrOutput : 0)
			| (m_pFilter->m_bIsD3DFullscreen ? CPacingTrace::Exclusive : 0)
			| m_tracePasses;
		traced.nrWaitUs   = (int)(m_traceNRTiming.inputWaitMs * 1000.0);
		traced.nrRecordUs = (int)(m_traceNRTiming.recordMs * 1000.0);
		traced.nrGpuUs    = (int)(std::max(m_traceNRTiming.gpuMs, 0.0) * 1000.0);
	}

	// A picture that has a time, and a clock running to be on time by. A redraw, a
	// paused film and a stopped clock have neither, and are presented as they come:
	// held against a clock that has stopped, every picture would wait its full bound.
	const bool bTimed = field && frameStartTime != INVALID_TIME
		&& m_pFilter->m_filterState == State_Running && !m_pFilter->m_bClockStalled;
	// When it is due: its stamp, and the picture delay of the settings. The stamp
	// itself stays what the trace and the subtitles go by.
	const REFERENCE_TIME frameDueTime = (frameStartTime != INVALID_TIME) ? frameStartTime + m_rtVideoDelay : INVALID_TIME;

	// The refresh this picture is for, and the moment to call Present to get it
	// (CPresentSchedule). It is what "render ahead" has become: every picture asked for
	// early and presented for a refresh chosen in advance, whatever drew it.
	CPresentSchedule::Plan plan;
	if (bTimed && m_bDlssRenderAhead) {
		ReadPresentSchedule();
		CRefTime rtNow;
		if (SUCCEEDED(m_pFilter->StreamTime(rtNow))) {
			const long long tickNow = (long long)GetPreciseTick();
			const long long tickDue = tickNow
				+ (long long)((double)(frameDueTime - rtNow) * GetPreciseTicksPerSecond() / 10000000.0);
			const double frameMs = m_pFilter->m_FrameStats.GetAverageFrameDuration() / 10000.0 / (m_bDoubleFrames ? 2 : 1);
			// What a picture costs before it can be presented: this one's drawing, or the
			// longest the GPU has lately taken to be done with one, which the render
			// thread does not wait for.
			const double drawMs = std::max((double)(tickNow - (long long)tick1) * 1000.0 / GetPreciseTicksPerSecond(),
				m_RenderAhead.MaxLatencyMs());
			plan = m_PresentSchedule.Choose(tickNow, tickDue, frameMs, drawMs);
			// A second field belongs to its sample, and the sample is all that can be left out.
			if (plan.bSkipNext && !m_bDoubleFrames) {
				m_bSkipNextSample = true;
			}
		}
	}
	if (field) {
		m_bScheduled = plan.bValid;
	}
	if (bTrace && plan.bValid) {
		traced.flags |= CPacingTrace::Scheduled | (plan.bRepeat ? CPacingTrace::Repeated : 0)
			| (plan.bDrop ? CPacingTrace::Dropped : 0);
		traced.tickTarget = plan.tickRefresh;
	}

	if (plan.bValid && plan.bDrop) {
		// No refresh left worth having: it is drawn, so that everything that keeps a
		// history has seen it, and not presented. What it lacked is time, which the
		// samples that follow are asked for earlier to have.
		if (field == 1) {
			const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;
			m_RenderAhead.AddLatency((double)(tick3 - m_tickSampleStart) / ticksPerMs,
				(double)(plan.tickDue - plan.tickWanted) / ticksPerMs,
				m_pFilter->m_FrameStats.GetAverageFrameDuration() / 10000.0);
		}
		m_tickOnScreen = 0;
		if (bTrace) {
			ReadShownForTrace(traced.after);
			m_PacingTrace.Add(traced);
		}
		return hr;
	}

	// Catching up a refresh (see ProcessSample): this picture is drawn, so that
	// everything that keeps a history has seen it, and not presented.
	if (!plan.bValid && m_bSkipPresent && field == 1 && frameStartTime != INVALID_TIME) {
		m_bSkipPresent = false;
		m_uCaughtUp++;
		m_tickOnScreen = 0;
		if (bTrace) {
			traced.flags |= CPacingTrace::NotPresented;
			ReadShownForTrace(traced.after);
			m_PacingTrace.Add(traced);
		}
		return hr;
	}

	// Without a refresh to aim at -- DXGI has not said where they are, or the setting
	// is off -- a picture a DLSS pass or a prescaler worked on is still held for its
	// time on the reference clock, as before.
	const bool bHold = !plan.bValid && bTimed && RenderAheadActive();
	if (bHold || plan.bValid) {
		MarkPictureSubmitted();
	}

	if (plan.bValid) {
		// Only the first picture of a sample is started early; a second field follows it.
		WaitForScheduledPresent(plan, field == 1);
	} else {
		if (m_bVBlankBeforePresent && m_pDXGIOutput) {
			hr = m_pDXGIOutput->WaitForVBlank();
			DLogIf(FAILED(hr), L"WaitForVBlank failed with error {}", HR2Str(hr));
			if (bTrace) {
				traced.tickAfterVBlank = (long long)GetPreciseTick();
				traced.flags |= CPacingTrace::VBlankWait;
			}
		}
		if (bHold) {
			HoldUntilPresentTime(frameDueTime, field == 1);
		} else if (m_bAdjustPresentTime) {
			SyncFrameToStreamTime(frameDueTime);
		}
	}

	if (bTrace) {
		traced.flags |= bHold ? CPacingTrace::Held : 0;
		ReadShownForTrace(traced.before);
		CRefTime rtClock;
		if (SUCCEEDED(m_pFilter->StreamTime(rtClock))) {
			traced.rtClockAtCall = rtClock;
		}
	}

	m_tickPresentCall = GetPreciseTick();
	g_bPresent = true;
	hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : Present() failed with error {}", HR2Str(hr));

	m_RenderStats.presentticks = GetPreciseTick() - tick3;
	// Most of that is the wait for the moment to call Present -- a refresh chosen, a
	// vertical blank, the picture's time -- which the statistics show apart.
	m_presentWaitTicks = m_tickPresentCall - tick3;
	MeasurePresentQueue();
	if (plan.bValid) {
		UINT presentId = 0;
		if (SUCCEEDED(m_pDXGISwapChain1->GetLastPresentCount(&presentId))) {
			m_PresentSchedule.Presented(presentId, plan, (long long)m_tickPresentCall,
				(double)(tick3 + m_RenderStats.presentticks - m_tickPresentCall) * 1000.0 / GetPreciseTicksPerSecond());
		}
		ReadPresentSchedule();
		// Where it will be on the screen against where its time is: the offset that
		// matters, known before it happens.
		m_rtScheduledOffset = (REFERENCE_TIME)((double)(plan.tickRefresh - plan.tickDue) * 10000000.0 / GetPreciseTicksPerSecond());
	}

	if (bTrace) {
		traced.tickPresentCall = (long long)m_tickPresentCall;
		traced.tickPresentReturn = (long long)(tick3 + m_RenderStats.presentticks);
		CRefTime rtClock;
		if (SUCCEEDED(m_pFilter->StreamTime(rtClock))) {
			traced.rtClockAtReturn = rtClock;
		}
		ReadShownForTrace(traced.after);
		m_PacingTrace.Add(traced);
	}

	if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen) {
		InitSwapChain(false);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::FillBlack()
{
	CheckPointer(m_pDXGISwapChain1, E_ABORT);

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FillBlack() : GetBuffer() failed with error {}", HR2Str(hr));
		return hr;
	}

	ID3D11RenderTargetView* pRenderTargetView;
	hr = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FillBlack() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_pDeviceContext->ClearRenderTargetView(pRenderTargetView, ClearColor);
	pRenderTargetView->Release();

	if (m_bShowStats) {
		hr = DrawStats(pBackBuffer);
	}
	DrawNotice(pBackBuffer);

	if (m_bAlphaBitmapEnable) {
		D3D11_TEXTURE2D_DESC desc;
		pBackBuffer->GetDesc(&desc);
		D3D11_VIEWPORT VP = {
			m_AlphaBitmapNRectDest.left * desc.Width,
			m_AlphaBitmapNRectDest.top * desc.Height,
			(m_AlphaBitmapNRectDest.right - m_AlphaBitmapNRectDest.left) * desc.Width,
			(m_AlphaBitmapNRectDest.bottom - m_AlphaBitmapNRectDest.top) * desc.Height,
			0.0f,
			1.0f
		};
		hr = AlphaBlt(m_TexAlphaBitmap.pShaderResource, pBackBuffer,
			m_pAlphaBitmapVertex, &VP,
			m_pSamplerLinear);
	}

	g_bPresent = true;
	hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::FillBlack() : Present() failed with error {}", HR2Str(hr));

	if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen) {
		InitSwapChain(false);
	}

	return hr;
}

void CDX11VideoProcessor::UpdateTexures()
{
	if (!m_srcWidth || !m_srcHeight) {
		return;
	}

	// TODO: try making w and h a multiple of 128.
	HRESULT hr = S_OK;

	ID3D11Texture2D* const pConvertOutputBefore = m_TexConvertOutput.pTexture;

	if (m_D3D11VP.IsReady()) {
		// With DLSS active the hardware VP must not upscale: the network is meant
		// to run at source resolution, and letting the VP scale first would both
		// cost far more and defeat the point.
		// Nor with DLSS Super Resolution, which scales from the source itself.
		if (VPResizesFirst()) {
			CSize texsize = m_videoRect.Size();
			hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, texsize.cx, texsize.cy, Tex2D_DefaultShaderRTarget);
			if (FAILED(hr)) {
				hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
			}
		} else {
			hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
		}
	}
	else {
		hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_InternalTexFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
	}

	// Toggling while paused recreates this texture and no new sample arrives to
	// fill it, so the pass ran over whatever happened to be in memory -- that is
	// what turned the paused frame green. Only on an actual recreation.
	if (m_TexConvertOutput.pTexture && m_TexConvertOutput.pTexture != pConvertOutputBefore) {
		CComPtr<ID3D11RenderTargetView> pRTV;
		if (S_OK == m_pDevice->CreateRenderTargetView(m_TexConvertOutput.pTexture, nullptr, &pRTV)) {
			const FLOAT black[4] = { 0, 0, 0, 1 };
			m_pDeviceContext->ClearRenderTargetView(pRTV, black);
		}
	}

	if (m_bDlssNRActive && m_TexConvertOutput.pTexture) {
		// After upscaling the network works on the displayed image, so the
		// textures follow the window; before it, they follow the cropped source.
		//
		// Taken from m_srcRect rather than m_TexConvertOutput: depending on when
		// this runs relative to the feature coming up, that texture can still be
		// sized to m_videoRect, and the pass would then rescale the picture on
		// its way through -- which is what clipped rows off AV1 frames and bent
		// the aspect ratio.
		// After upscaling the pass is a post-scale step, and those work on
		// dstRect clipped to the window -- m_renderRect, not the whole window.
		// Sizing from the window made the guard below reject every frame
		// whenever the video did not fill it exactly, which read as "ticking
		// the box turns DLSS off".
		const UINT w = m_bDlssNRAfterUpscale ? (UINT)m_renderRect.Width()  : (UINT)m_srcRectWidth;
		const UINT h = m_bDlssNRAfterUpscale ? (UINT)m_renderRect.Height() : (UINT)m_srcRectHeight;
		if (!w || !h) {
			ReleaseDlssNRTextures(true);
			return;
		}

		// The network may work smaller than the picture: it costs in proportion to
		// the pixels it is given.
		UINT ww = w, wh = h;
		CDlssCompose::WorkingSize(w, h, m_iDlssNRWorkingScale, ww, wh);
		const bool bReduce = (ww != w || wh != h);

		// The stabilizer follows the working size, and motion vectors of another
		// size must not sit in the parameter block when the feature is created.
		// DlssNRPass makes a new one.
		if (m_DlssStabilizer.IsCreated() && !m_DlssStabilizer.Matches(ww, wh, false, DlssNRFlow())) {
			ReleaseDlssNRTextures(false);
		}

		// Both textures are shared with the private D3D12 device, so the input
		// copy is always needed -- m_TexConvertOutput carries no share handle
		// even when its format already matches.
		hr = m_TexDlssIn.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, ww, wh, Tex2D_DefaultShaderRTargetUAVShared);
		if (SUCCEEDED(hr)) {
			hr = m_TexDlssOut.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, ww, wh, Tex2D_DefaultShaderRTargetUAVShared);
		}
		m_pDlssInTarget.Release();
		if (SUCCEEDED(hr) && bReduce) {
			hr = m_DlssCompose.Create(m_pDevice, w, h, ww, wh, CDlssCompose::Method::Difference,
				m_pVSimpleInputLayout, m_pVS_Simple);
			if (SUCCEEDED(hr)) {
				hr = m_pDevice->CreateRenderTargetView(m_TexDlssIn.pTexture, nullptr, &m_pDlssInTarget);
			}
		} else {
			m_DlssCompose.Release();
			m_TexDlssPicture.Release();
		}
		m_DlssNRPictureSize.SetSize(w, h);

		if (FAILED(hr) || !m_TexDlssIn.pTexture || !m_TexDlssOut.pTexture) {
			DLog(L"CDX11VideoProcessor::UpdateTexures() : failed to create DLSS textures");
			m_bDlssNRActive = false;
		} else if (!m_DlssNR.CreateFeature(m_TexDlssIn.pTexture, m_TexDlssOut.pTexture, ww, wh, m_DlssParams)) {
			m_bDlssNRActive = false;
		} else {
			// Until the first Evaluate lands these hold whatever was in memory;
			// sampling that is what turned the paused frame green.
			for (ID3D11Texture2D* pTex : { m_TexDlssIn.pTexture.p, m_TexDlssOut.pTexture.p }) {
				CComPtr<ID3D11RenderTargetView> pRTV;
				if (S_OK == m_pDevice->CreateRenderTargetView(pTex, nullptr, &pRTV)) {
					const FLOAT black[4] = { 0, 0, 0, 1 };
					m_pDeviceContext->ClearRenderTargetView(pRTV, black);
				}
			}
		}

		if (!m_bDlssNRActive) {
			ReleaseDlssNRTextures(true);
			// The network was counted on to take the picture at the source's size, and
			// it is not there: the processor's first turn has the resizing back.
			UpdateTexures();
			return;
		}
	} else {
		ReleaseDlssNRTextures(true);
	}

	// Whichever of the processor's turns resizes is the one with Super Resolution.
	UpdateVPScale();
	if (m_D3D11VP.IsReady()) {
		UpdateVPSuperRes(m_srcParams.CDepth);
	}
}

void CDX11VideoProcessor::UpdatePostScaleTexures()
{
	const bool needDither =
		(m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM
		|| m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM && m_InternalTexFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);

	m_bFinalPass = (m_bUseDither && needDither && m_TexDither.pTexture);
	if (m_bFinalPass) {
		m_pPSFinalPass.Release();
		m_bFinalPass = SUCCEEDED(CreatePShaderFromResource(
			&m_pPSFinalPass,
			(m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM) ? IDF_PS_11_FINAL_PASS_10 : IDF_PS_11_FINAL_PASS
		));
	}
	// Sharpening takes the dither on when it is the last step before it, which the
	// count below has to know.
	UpdateSharpenDitherShader();

	const UINT numPostScaleSteps = GetPostScaleSteps();
	HRESULT hr = m_TexsPostScale.CheckCreate(m_pDevice, m_InternalTexFmt, m_windowRect.Width(), m_windowRect.Height(), numPostScaleSteps);
	//UpdateStatsPostProc();
}

void CDX11VideoProcessor::UpdateUpscalingShaders()
{
	m_pShaderUpscaleX.Release();
	m_pShaderUpscaleY.Release();

	if (m_iUpscalingInUse != UPSCALE_Nearest) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderUpscaleX, s_Upscaling11ResIDs[m_iUpscalingInUse].shaderX));
		if (m_iUpscalingInUse == UPSCALE_Jinc2) {
			m_pShaderUpscaleY = m_pShaderUpscaleX;
		} else {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderUpscaleY, s_Upscaling11ResIDs[m_iUpscalingInUse].shaderY));
		}
	}

	UpdateMpvLuma();
	UpdateScalingStrings();
}

void CDX11VideoProcessor::UpdateDownscalingShaders()
{
	m_pShaderDownscaleX.Release();
	m_pShaderDownscaleY.Release();

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderDownscaleX, s_Downscaling11ResIDs[m_iDownscaling].shaderX));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderDownscaleY, s_Downscaling11ResIDs[m_iDownscaling].shaderY));

	UpdateScalingStrings();
}

HRESULT CDX11VideoProcessor::UpdateConvertColorShader()
{
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();
	ID3DBlob* pShaderCode = nullptr;

	int convertType = (m_bConvertToSdr && !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping))) ? SHADER_CONVERT_TO_SDR
		: (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) ? SHADER_CONVERT_TO_PQ
		: SHADER_CONVERT_NONE;

	MediaSideDataDOVIMetadata* pDOVIMetadata = m_Dovi.bValid ? &m_Dovi.msd : nullptr;

	// The prescaler where it runs, Catmull-Rom in its place elsewhere.
	UpdateMpvChroma();
	const int chromaScaling = ChromaScalingForShader();

	HRESULT hr = GetShaderConvertColor(true,
		m_srcWidth,
		m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
		m_srcRect, m_srcParams, m_srcExFmt, pDOVIMetadata,
		chromaScaling, convertType, false,
		&pShaderCode);
	if (S_OK == hr) {
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPSConvertColor);
		pShaderCode->Release();
	}

	if (m_bInterlaced && m_srcParams.Subsampling == 420 && m_srcParams.pDX11Planes) {
		hr = GetShaderConvertColor(true,
			m_srcWidth,
			m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
			m_srcRect, m_srcParams, m_srcExFmt, pDOVIMetadata,
			chromaScaling, convertType, true,
			&pShaderCode);
		if (S_OK == hr) {
			hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPSConvertColorDeint);
			pShaderCode->Release();
		}
	}

	if (FAILED(hr)) {
		ASSERT(0);
		m_bMpvChromaActive = false; // the stock shaders read the chroma planes
		UINT resid = 0;
		if (m_srcParams.cformat == CF_YUY2) {
			resid = IDF_PS_11_CONVERT_YUY2;
		}
		else if (m_srcParams.pDX11Planes) {
			if (m_srcParams.pDX11Planes->FmtPlane3) {
				if (m_srcParams.cformat == CF_YV12 || m_srcParams.cformat == CF_YV16 || m_srcParams.cformat == CF_YV24) {
					resid = IDF_PS_11_CONVERT_PLANAR_YV;
				} else {
					resid = IDF_PS_11_CONVERT_PLANAR;
				}
			} else {
				resid = IDF_PS_11_CONVERT_BIPLANAR;
			}
		}
		else {
			resid = IDF_PS_11_CONVERT_COLOR;
		}
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSConvertColor, resid));

		return S_FALSE;
	}

	return hr;
}

void CDX11VideoProcessor::UpdateBitmapShader()
{
	if (m_bHdrDisplayModeEnabled
			&& (SourceIsHDR() || m_bVPUseRTXVideoHDR)) {
		UINT resid;
		float SDR_peak_lum;
		switch (m_iHdrOsdBrightness) {
		default:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ;
			SDR_peak_lum = 100;
			break;
		case 1:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ1;
			SDR_peak_lum = 50;
			break;
		case 2:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ2;
			SDR_peak_lum = 30;
			break;
		}
		m_pPS_BitmapToFrame.Release();
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_BitmapToFrame, resid));
		m_dwStatsTextColor = TransferPQ(D3DCOLOR_XRGB(255, 255, 255), SDR_peak_lum);
	}
	else {
		m_pPS_BitmapToFrame = m_pPS_Simple;
		m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);
	}
}

HRESULT CDX11VideoProcessor::D3D11VPPass(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second)
{
	HRESULT hr = m_D3D11VP.SetRectangles(srcRect, dstRect);

	hr = m_D3D11VP.Process(pRenderTarget, m_SampleFormat, second);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::ProcessD3D11() : m_D3D11VP.Process() failed with error {}", HR2Str(hr));
	}

	return hr;
}

HRESULT CDX11VideoProcessor::ConvertColorPass(ID3D11Texture2D* pRenderTarget)
{
	// RAVU-zoom first: it sets up the pipeline its own way.
	if (m_bMpvChromaActive) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::MpvChroma);
		const HRESULT hrChroma = MpvChromaPass();
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::MpvChroma);
		if (FAILED(hrChroma)) {
			// Latched off: the conversion shader goes back to reading the planes.
			DLog(L"CDX11VideoProcessor::ConvertColorPass() : {} on chroma failed with error {}",
				m_MpvChroma.Info() ? m_MpvChroma.Info()->name : L"the prescaler", HR2Str(hrChroma));
			m_bMpvChromaFailed = true;
			UpdateConvertColorShader();
			UpdateStatsStatic();
		}
	}

	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"ConvertColorPass() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	D3D11_VIEWPORT VP;
	VP.TopLeftX = 0;
	VP.TopLeftY = 0;
	VP.Width = (FLOAT)m_TexConvertOutput.desc.Width;
	VP.Height = (FLOAT)m_TexConvertOutput.desc.Height;
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	const UINT Stride = sizeof(VERTEX);
	const UINT Offset = 0;

	// Set resources
	m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
	m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView.p, nullptr);
	m_pDeviceContext->RSSetViewports(1, &VP);
	m_pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
	if (m_bDeintBlend && m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE && m_pPSConvertColorDeint) {
		m_pDeviceContext->PSSetShader(m_pPSConvertColorDeint, nullptr, 0);
	} else {
		m_pDeviceContext->PSSetShader(m_pPSConvertColor, nullptr, 0);
	}
	m_pDeviceContext->PSSetShaderResources(0, 1, &m_TexSrcVideo.pShaderResource.p);
	if (m_bMpvChromaActive) {
		ID3D11ShaderResourceView* chroma[2] = { MpvChromaPlane(0), MpvChromaPlane(1) };
		m_pDeviceContext->PSSetShaderResources(1, 2, chroma);
	} else {
		m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexSrcVideo.pShaderResource2.p);
		m_pDeviceContext->PSSetShaderResources(2, 1, &m_TexSrcVideo.pShaderResource3.p);
	}
	m_pDeviceContext->PSSetSamplers(0, 1, &m_pSamplerPoint.p);
	m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerLinear.p);
	m_pDeviceContext->PSSetConstantBuffers(0, 1, &m_PSConvColorData.pConstants);
	m_pDeviceContext->PSSetConstantBuffers(1, 1, &m_pCorrectionConstants.p);
	m_pDeviceContext->PSSetConstantBuffers(2, 1, &m_pDoviCurvesConstantBuffer.p);
	if (m_pDoViDynamicConstants) {
		m_pDeviceContext->PSSetConstantBuffers(3, 1, &m_pDoViDynamicConstants.p);
	}
	m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_pDeviceContext->IASetVertexBuffers(0, 1, &m_PSConvColorData.pVertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	m_pDeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* views[3] = {};
	m_pDeviceContext->PSSetShaderResources(0, 3, views);

	return hr;
}

HRESULT CDX11VideoProcessor::ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const int rotation)
{
	HRESULT hr = S_OK;
	const int w2 = dstRect.Width();
	const int h2 = dstRect.Height();

	int w1, h1;
	ID3D11PixelShader* resizerX;
	ID3D11PixelShader* resizerY;
	if (rotation == 90 || rotation == 270) {
		w1 = srcRect.Height();
		h1 = srcRect.Width();
		resizerX = (w1 == w2) ? nullptr : (w1 > w2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p; // use Y scaling here
		if (resizerX) {
			resizerY = (h1 == h2) ? nullptr : (h1 > h2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p;
		} else {
			resizerY = (h1 == h2) ? nullptr : (h1 > h2) ? m_pShaderDownscaleX.p : m_pShaderUpscaleX.p; // use X scaling here
		}
	} else {
		w1 = srcRect.Width();
		h1 = srcRect.Height();
		resizerX = (w1 == w2) ? nullptr : (w1 > w2) ? m_pShaderDownscaleX.p : m_pShaderUpscaleX.p;
		resizerY = (h1 == h2) ? nullptr : (h1 > h2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p;
	}

	if (resizerX && resizerY) {
		// two pass resize

		D3D11_TEXTURE2D_DESC desc;
		pRenderTarget->GetDesc(&desc);

		if (resizerX == resizerY) {
			// one pass resize
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerX, rotation, m_bFlip);
			DLogIf(FAILED(hr), L"CDX11VideoProcessor::ResizeShaderPass() : failed with error {}", HR2Str(hr));

			return hr;
		}

		// check intermediate texture
		const UINT texWidth  = desc.Width;
		const UINT texHeight = h1;

		if (m_TexResize.pTexture) {
			if (texWidth != m_TexResize.desc.Width || texHeight != m_TexResize.desc.Height) {
				m_TexResize.Release(); // need new texture
			}
		}

		if (!m_TexResize.pTexture) {
			// use only float textures here
			hr = m_TexResize.Create(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, texWidth, texHeight, Tex2D_DefaultShaderRTarget);
			if (FAILED(hr)) {
				DLog(L"CDX11VideoProcessor::ResizeShaderPass() : m_TexResize.Create() failed with error {}", HR2Str(hr));
				return hr;
			}
		}

		CRect resizeRect(dstRect.left, 0, dstRect.right, texHeight);

		// First resize pass
		hr = TextureResizeShader(Tex, m_TexResize.pTexture, srcRect, resizeRect, resizerX, rotation, m_bFlip);
		// Second resize pass
		hr = TextureResizeShader(m_TexResize, pRenderTarget, resizeRect, dstRect, resizerY, 0, false);
	}
	else {
		if (resizerX) {
			// one pass resize for width
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerX, rotation, m_bFlip);
		}
		else if (resizerY) {
			// one pass resize for height
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerY, rotation, m_bFlip);
		}
		else {
			// no resize
			hr = TextureCopyRect(Tex, pRenderTarget, srcRect, dstRect, m_pPS_Simple, nullptr, rotation, m_bFlip);
		}
	}

	DLogIf(FAILED(hr), L"CDX11VideoProcessor::ResizeShaderPass() : failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT CDX11VideoProcessor::FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FinalPass() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, 0, false);
	if (FAILED(hr)) {
		return hr;
	}

	const FLOAT constants[4] = { (float)Tex.desc.Width / dither_size, (float)Tex.desc.Height / dither_size, 0, 0 };
	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pFinalPassConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FinalPass() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &constants, sizeof(constants));
	m_pDeviceContext->Unmap(m_pFinalPassConstantBuffer, 0);

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)dstRect.left;
	VP.TopLeftY = (FLOAT)dstRect.top;
	VP.Width    = (FLOAT)dstRect.Width();
	VP.Height   = (FLOAT)dstRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	// Set resources
	m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexDither.pShaderResource.p);
	m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerDither.p);

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSFinalPass, Tex.pShaderResource, m_pSamplerPoint, m_pFinalPassConstantBuffer, m_pVertexBuffer);

	ID3D11ShaderResourceView* views[1] = {};
	m_pDeviceContext->PSSetShaderResources(1, 1, views);

	return hr;
}

void CDX11VideoProcessor::DrawSubtitles(ID3D11Texture2D* pRenderTarget)
{
	HRESULT hr = S_OK;

	CComPtr<ISubPic> pSubPic = m_pFilter->GetSubPic(m_rtStart);
	if (pSubPic) {
		RECT rcSource, rcDest;
		hr = pSubPic->GetSourceAndDest(m_windowRect, m_videoRect, &rcSource, &rcDest, FALSE, {}, 0, FALSE);
		if (SUCCEEDED(hr)) {
			ID3D11RenderTargetView* pRenderTargetView;
			hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
			if (SUCCEEDED(hr)) {
				// Set render target and shaders
				m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
				m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
				m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
				m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);

				// call the function for drawing subtitles
				hr = pSubPic->AlphaBlt(&rcSource, &rcDest, nullptr);

				pRenderTargetView->Release();
			}
		}
		return;
	}

	if (m_pFilter->m_pSub11CallBack) {
		ID3D11RenderTargetView* pRenderTargetView;
		hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
		if (SUCCEEDED(hr)) {
			const CRect rSrcPri(POINT(0, 0), m_windowRect.Size());
			const CRect rDstVid(m_videoRect);
			const auto rtStart = m_pFilter->m_rtStartTime + m_rtStart;

			// Set render target and shaders
			m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
			m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
			m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
			m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);

			// call the function for drawing subtitles
			hr = m_pFilter->m_pSub11CallBack->Render11(rtStart, 0, m_rtAvgTimePerFrame, rDstVid, rDstVid, rSrcPri,
													   1., m_iStereo3dTransform == 1 ? m_nStereoSubtitlesOffsetInPixels : 0);

			pRenderTargetView->Release();
		}
	}
}

HRESULT CDX11VideoProcessor::Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second, const bool bAllowDlss)
{
	HRESULT hr = S_OK;
	m_bDitherUsed = false;
	int rotation = m_iRotation;

	CRect rSrc = srcRect;
	Tex2D_t* pInputTexture = nullptr;

	const UINT numSteps = GetPostScaleSteps();

	if (m_D3D11VP.IsReady()) {
		if (!(m_iSwapEffect == SWAPEFFECT_Discard && (m_VendorId == PCIV_AMDATI || m_VendorId == PCIV_INTEL))) {
			const bool bNeedShaderTransform =
				(m_TexConvertOutput.desc.Width != dstRect.Width() || m_TexConvertOutput.desc.Height != dstRect.Height() || m_bFlip
				|| dstRect.right > m_windowRect.right || dstRect.bottom > m_windowRect.bottom)
				|| (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) // At least on Nvidia we can sometimes get the "D3D11: Removing Device" error here when HDR Passthrough.
				|| ((m_bDlssNRActive || m_bDlssSRActive) && bAllowDlss); // the DLSS passes need the intermediate texture
			if (!bNeedShaderTransform && !numSteps) {
				m_bVPScalingUseShaders = false;
				BeginVPForTiming(CGpuStageTimes::VP);
				hr = D3D11VPPass(pRenderTarget, rSrc, dstRect, second);
				WaitForVPForTiming(pRenderTarget);
				m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::VP);

				return hr;
			}
		}

		CRect rect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
		BeginVPForTiming(CGpuStageTimes::VP);
		hr = D3D11VPPass(m_TexConvertOutput.pTexture, rSrc, rect, second);
		WaitForVPForTiming(m_TexConvertOutput.pTexture);
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::VP);
		pInputTexture = &m_TexConvertOutput;
		rSrc = rect;
		rotation = 0;
	}
	else if (m_PSConvColorData.bEnable) {
		// An mpv prescaler's chroma included: the statistics give the step one figure.
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::ShaderConvert);
		ConvertColorPass(m_TexConvertOutput.pTexture);
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::ShaderConvert);
		pInputTexture = &m_TexConvertOutput;
		rSrc.SetRect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
	}
	else {
		pInputTexture = &m_TexSrcVideo;
	}

	const bool bDlssHere = m_bDlssNRActive && bAllowDlss && !m_bDlssNRAfterUpscale;
	if (bDlssHere && pInputTexture) {
		Tex2D_t* pDlssResult = nullptr;
		if (S_OK == DlssNRPass(pInputTexture, rSrc, &pDlssResult) && pDlssResult) {
			pInputTexture = pDlssResult;
			rSrc.SetRect(0, 0, pDlssResult->desc.Width, pDlssResult->desc.Height);
		}
	}

	// "Use the video processor for resizing" with DLSS 5 NR before the resizing: the
	// processor's second turn, on what the network left. It is made while the network is
	// counted on (UpdateVPScale) and resizes for as long as it is there: a picture the
	// network gave up, and the two seconds a session takes to come back, go through it
	// all the same, so that who resizes does not change from one picture to the next.
	bool bEnlarged = false;
	bool bVPScaled = false;
	if (bAllowDlss && pInputTexture && m_D3D11VPScale.IsReady() && !m_bVPScaleFailed) {
		Tex2D_t* pScaled = nullptr;
		BeginVPForTiming(CGpuStageTimes::VPScale);
		const HRESULT hrScale = VPScalePass(pInputTexture, rSrc, &pScaled);
		if (S_OK == hrScale && pScaled) {
			WaitForVPForTiming(pScaled->pTexture);
		}
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::VPScale);
		if (S_OK == hrScale && pScaled) {
			pInputTexture = pScaled;
			rSrc.SetRect(0, 0, pScaled->desc.Width, pScaled->desc.Height);
			bEnlarged = true;
			bVPScaled = true;
			m_tracePasses |= CPacingTrace::VPAfterNR;
		}
	}
	if (bAllowDlss) {
		m_bVPScaled = bVPScaled;   // a snapshot for the player is not the film
	}

	// DLSS Super Resolution in place of the resize shaders. ResizeShaderPass below
	// is then left with rotating, flipping and placing the picture, and with any
	// scale DLSS does not cover.
	if (m_bDlssSRActive && bAllowDlss && pInputTexture) {
		Tex2D_t* pSRResult = nullptr;
		if (S_OK == DlssSRPass(pInputTexture, rSrc, dstRect, rotation, &pSRResult) && pSRResult) {
			pInputTexture = pSRResult;
			rSrc.SetRect(0, 0, pSRResult->desc.Width, pSRResult->desc.Height);
			bEnlarged = true;
		}
	}

	// An mpv prescaler chosen as the Upscaling method, where DLSS SR has not
	// enlarged the picture; ResizeShaderPass then finishes the scale, as above.
	bool bMpvLumaRan = false;
	if (!bEnlarged && m_MpvLuma.IsLoaded() && pInputTexture) {
		Tex2D_t* pMpvResult = nullptr;
		if (S_OK == MpvLumaPass(pInputTexture, rSrc, dstRect, rotation, &pMpvResult) && pMpvResult) {
			pInputTexture = pMpvResult;
			rSrc.SetRect(0, 0, pMpvResult->desc.Width, pMpvResult->desc.Height);
			bMpvLumaRan = true;
		}
	}
	if (bAllowDlss) {
		m_bMpvLumaRan = bMpvLumaRan;   // a snapshot for the player is not the film
	}

	if (numSteps) {
		UINT step = 0;
		Tex2D_t* pTex = m_TexsPostScale.GetFirstTex();
		ID3D11Texture2D* pRT = pTex->pTexture;

		auto StepSetting = [&]() {
			step++;
			pInputTexture = pTex;
			if (step < numSteps) {
				pTex = m_TexsPostScale.GetNextTex();
				pRT = pTex->pTexture;
			} else {
				pRT = pRenderTarget;
			}
		};

		CRect rect;
		rect.IntersectRect(dstRect, CRect(0, 0, pTex->desc.Width, pTex->desc.Height));

		if (m_D3D11VP.IsReady()) {
			m_bVPScalingUseShaders = rSrc.Width() != dstRect.Width() || rSrc.Height() != dstRect.Height();
		}

		// A picture that only has to be put in its place -- the processor, DLSS SR or a
		// prescaler made it the size it is shown at, and it has black bars or sits in a
		// window -- is read where it lies by the first step after, and the copy that
		// placed it goes: 0.10 ms for a 3840x1600 picture between the bars of a 4K
		// window on an RTX 4060 (RenderDoc), and the statistics gave the processor's
		// such copy 0.5 ms at 4K. The step also reads the picture as it was made, not an
		// 8-bit copy of it, and its edge clamped where it used to read what lay under the
		// bars. Not where that step is not a plain draw of the
		// picture: DLSS 5 NR after the resize, or the 3D conversion, which draws the
		// subtitles first on what it reads. rRead is the part of its input a step reads:
		// the picture where it lies, the first time, and where it was put from then on.
		const bool bFirstStepDraws = m_pPSCorrection || m_pPSHDR10ToneMapping || m_pPSSharpen
			|| !m_pPostScaleShaders.empty() || (m_bFinalPass && !m_pPSHalfOUtoInterlace);
		const bool bPlaceInFirstStep = rSrc != dstRect && rSrc.Size() == dstRect.Size() && rotation == 0 && !m_bFlip
			&& rect == dstRect && bFirstStepDraws && !(m_bDlssNRActive && bAllowDlss && m_bDlssNRAfterUpscale);
		CRect rRead = rect;

		if (bPlaceInFirstStep) {
			pTex = pInputTexture;
			rRead = rSrc;
		} else if (rSrc != dstRect || rotation != 0) {
			m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::Resize);
			hr = ResizeShaderPass(*pInputTexture, pRT, rSrc, dstRect, rotation);
			m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::Resize);
		} else {
			pTex = pInputTexture; // Hmm
		}

		if (m_bDlssNRActive && bAllowDlss && m_bDlssNRAfterUpscale) {
			StepSetting();
			Tex2D_t* pDlssResult = nullptr;
			if (S_OK == DlssNRPass(pInputTexture, rect, &pDlssResult) && pDlssResult) {
				// The network writes into its own shared texture, so the result
				// still has to reach this step's render target.
				//
				// The source rectangle is the whole DLSS texture, not rect:
				// rect is an absolute region of the post-scale texture and does
				// not start at the origin, so using it here sampled past the
				// bottom of the DLSS output. Clamp addressing then repeated the
				// last valid row -- the band of duplicated lines.
				const CRect rDlss(0, 0, pDlssResult->desc.Width, pDlssResult->desc.Height);
				hr = TextureCopyRect(*pDlssResult, pRT, rDlss, rect, m_pPS_Simple, nullptr, 0, false);
			} else {
				hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPS_Simple, nullptr, 0, false);
			}
		}

		// The finishing steps, as one figure in the statistics.
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::Finish);

		if (m_pPSCorrection) {
			StepSetting();
			hr = TextureCopyRect(*pInputTexture, pRT, rRead, rect, m_pPSCorrection, m_pCorrectionConstants, 0, false);
			rRead = rect;
		}

		if (m_pPSHDR10ToneMapping) {
			StepSetting();

			if (m_pDoViDynamicConstants) {
				m_pDeviceContext->PSSetConstantBuffers(1, 1, &m_pDoViDynamicConstants.p);
			}

			hr = TextureCopyRect(*pInputTexture, pRT, rRead, rect, m_pPSHDR10ToneMapping, m_pHDR10ToneMappingConstants, 0, false);
			rRead = rect;
		}

		if (m_pPSSharpen) {
			// The picture is the size it will be shown at and already carries the
			// values the display is given: that is where a sharpener belongs, and
			// it is also the only place a film already at the screen's resolution
			// can be reached at all. The block is the resize shaders' own, so the
			// neighbouring texels can be worked out from the texel size -- the
			// sampler on this path is a point one and samples nothing by itself.
			const int method = std::clamp(m_iSharpen, 0, SHARPEN_COUNT - 1);
			const int level = std::clamp(m_iSharpenLevel, SHARPEN_LEVEL_MIN, SHARPEN_LEVEL_MAX);
			// Taking the dither on, it reads the dither where FinalPass would: the scale
			// FinalPass is given, for the picture it reads, at the same coordinates.
			const bool bDither = SharpenTakesDither();
			const FLOAT constants[12] = {
				(float)pTex->desc.Width, (float)pTex->desc.Height,
				1.0f / pTex->desc.Width, 1.0f / pTex->desc.Height,
				1.0f, 1.0f,
				s_SharpenStrength[method][level - 1], s_SharpenClamp,
				(float)pTex->desc.Width / dither_size, (float)pTex->desc.Height / dither_size, 0, 0
			};
			D3D11_MAPPED_SUBRESOURCE mr = {};
			if (SUCCEEDED(m_pDeviceContext->Map(m_pSharpenConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
				memcpy(mr.pData, constants, sizeof(constants));
				m_pDeviceContext->Unmap(m_pSharpenConstants, 0);
			}

			StepSetting();
			if (bDither) {
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexDither.pShaderResource.p);
				m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerDither.p);
				hr = TextureCopyRect(*pInputTexture, pRT, rRead, rect, m_pPSSharpenDither, m_pSharpenConstants, 0, false);
				ID3D11ShaderResourceView* views[1] = {};
				m_pDeviceContext->PSSetShaderResources(1, 1, views);
				m_bDitherUsed = true;
			} else {
				hr = TextureCopyRect(*pInputTexture, pRT, rRead, rect, m_pPSSharpen, m_pSharpenConstants, 0, false);
			}
			rRead = rect;
		}

		if (m_pPostScaleShaders.size()) {
			static __int64 counter = 0;
			static long start = GetTickCount();

			long stop = GetTickCount();
			long diff = stop - start;
			if (diff >= 10 * 60 * 1000) {
				start = stop;    // reset after 10 min (ps float has its limits in both range and accuracy)
			}

			PS_EXTSHADER_CONSTANTS ConstData = {
				{1.0f / pTex->desc.Width, 1.0f / pTex->desc.Height },
				{(float)pTex->desc.Width, (float)pTex->desc.Height},
				counter++,
				(float)diff / 1000,
				0, 0
			};
			m_pDeviceContext->UpdateSubresource(m_pPostScaleConstants, 0, nullptr, &ConstData, 0, 0);

			for (UINT idx = 0; idx < m_pPostScaleShaders.size(); idx++) {
				StepSetting();
				hr = TextureCopyRect(*pInputTexture, pRT, rRead, rect, m_pPostScaleShaders[idx].shader, m_pPostScaleConstants, 0, false);
				rRead = rect;
			}
		}

		if (m_pPSHalfOUtoInterlace) {
			DrawSubtitles(pRT);

			StepSetting();
			FLOAT ConstData[] = {
				(float)pTex->desc.Height, 0,
				(float)dstRect.top / pTex->desc.Height, (float)dstRect.bottom / pTex->desc.Height,
			};
			D3D11_MAPPED_SUBRESOURCE mr;
			hr = m_pDeviceContext->Map(m_pHalfOUtoInterlaceConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
			if (SUCCEEDED(hr)) {
				memcpy(mr.pData, &ConstData, sizeof(ConstData));
				m_pDeviceContext->Unmap(m_pHalfOUtoInterlaceConstantBuffer, 0);
			}
			hr = TextureCopyRect(*pInputTexture, pRT, rRead, rect, m_pPSHalfOUtoInterlace, m_pHalfOUtoInterlaceConstantBuffer, 0, false);
			rRead = rect;
		}

		if (m_bFinalPass && !SharpenTakesDither()) {
			StepSetting();
			hr = FinalPass(*pTex, pRT, rRead, rect);
			m_bDitherUsed = true;
		}
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::Finish);
	}
	else {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::Resize);
		hr = ResizeShaderPass(*pInputTexture, pRenderTarget, rSrc, dstRect, rotation);
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::Resize);
	}

	DLogIf(FAILED(hr), L"CDX11VideoProcessor::Process() : failed with error {}", HR2Str(hr));

	return hr;
}

void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	m_videoRect = videoRect;
	UpdateRenderRect();
	UpdateTexures();
}

HRESULT CDX11VideoProcessor::SetWindowRect(const CRect& windowRect)
{
	m_windowRect = windowRect;
	UpdateRenderRect();
	// After upscaling, DLSS 5 NR works on the part of the picture the window shows, so
	// its textures follow the window as well as the picture. Coming back to fullscreen
	// gives the picture its new place first and the window its size afterwards: the
	// textures were made for what the two had in common in between (2122x1022 on the
	// way to 3840x2160), the pass then refused every picture for not fitting them, and
	// the network stayed off for the rest of the film with its lines still in the
	// statistics.
	if (m_bDlssNRActive && m_bDlssNRAfterUpscale) {
		UpdateTexures();
	}

	HRESULT hr = S_OK;
	const UINT w = m_windowRect.Width();
	const UINT h = m_windowRect.Height();

	if (m_pDXGISwapChain1 && !m_bExclusiveScreen) {
		hr = m_pDXGISwapChain1->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
		if (FAILED(hr)) {
			// Nobody was reading this. A refused resize leaves the window one size
			// and the swap chain another, and a flip model swap chain that does not
			// match its window presents every picture without any of them reaching
			// the screen -- which from inside the renderer looks like nothing at
			// all: the frames are drawn, the times are right, and the picture is
			// frozen. So it is built again instead of left disagreeing.
			DLog(L"CDX11VideoProcessor::SetWindowRect() : ResizeBuffers({}x{}) failed with error {}, rebuilding",
				w, h, HR2Str(hr));
			m_nSwapChainResizeFailed++;
			hr = InitSwapChain(false);
		}
	}

	UpdateStatsByWindow();

	UpdatePostScaleTexures();

	return hr;
}

HRESULT CDX11VideoProcessor::Reset(bool bDisplayModeChange)
{
	DLog(L"CDX11VideoProcessor::Reset({})", bDisplayModeChange);

	// The renderer lock first, the toggle lock second, which is the order the other
	// way in takes them: a setting changed in full playback holds the renderer lock
	// all the way from SetSettings to HandleHDRToggle. Taken here the other way
	// round, a display change and a setting change landing together each hold the
	// lock the other waits for, and the player never comes out of it. The lock is
	// recursive, so the callers that already hold it lose nothing.
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);
	CAutoLock cHDRToggleLock(&m_HDRToggleLock);

	if (bDisplayModeChange) {
		if (m_bDisplayModeChangeAfterHDRToggle) {
			m_bDisplayModeChangeAfterHDRToggle = false;
			return S_OK;
		}
	}

	if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR10orHLG()) {
		MONITORINFOEXW mi = { sizeof(mi) };
		GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
		DisplayConfig_t displayConfig = {};

		if (GetDisplayConfig(mi.szDevice, displayConfig)) {
			const auto bHdrPassthroughSupport = displayConfig.HDRSupported() && displayConfig.HDREnabled();
			if ((bHdrPassthroughSupport && !m_bHdrPassthroughSupport) || (!displayConfig.HDREnabled() && m_bHdrPassthroughSupport)) {
				m_bHDRModeChangeOutside = true;

				if (m_pFilter->m_inputMT.IsValid()) {
					ReleaseSwapChain();

					if (m_iSwapEffect == SWAPEFFECT_Discard && !displayConfig.HDREnabled()) {
						m_pFilter->Init(true);
					} else {
						Init(m_hWnd, true);
					}
				}
			}
		}
	}

	return S_OK;
}

HRESULT CDX11VideoProcessor::GetCurentImage(long *pDIBImage)
{
	UINT w = m_srcRectWidth;
	UINT h = m_srcRectHeight;
	if (m_srcAnamorphic) {
		w = MulDiv(h, m_srcAspectRatioX, m_srcAspectRatioY);
	}
	if (m_iRotation == 90 || m_iRotation == 270) {
		std::swap(w, h);
	}
	const CRect imageRect(0, 0, w, h);

	const UINT dib_bitdepth = 32;
	const UINT dib_pitch    = CalcDibRowPitch(w, dib_bitdepth);

	BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)pDIBImage;
	ZeroMemory(pBIH, sizeof(BITMAPINFOHEADER));
	pBIH->biSize      = sizeof(BITMAPINFOHEADER);
	pBIH->biWidth     = w;
	pBIH->biHeight    = -(LONG)h; // top-down RGB bitmap
	pBIH->biPlanes    = 1;
	pBIH->biBitCount  = dib_bitdepth;
	pBIH->biSizeImage = dib_pitch * h;

	HRESULT hr = S_OK;
	CComPtr<ID3D11Texture2D> pRGB32Texture2D;
	D3D11_TEXTURE2D_DESC texdesc = CreateTex2DDesc(DXGI_FORMAT_B8G8R8X8_UNORM, w, h, Tex2D_DefaultRTarget);

	hr = m_pDevice->CreateTexture2D(&texdesc, nullptr, &pRGB32Texture2D);
	if (FAILED(hr)) {
		return hr;
	}

	const bool bHdrPassthrough = (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR));
	const bool bSavedHdrPassthrough = m_bHdrPassthrough;
	const bool bSavedHdrLocalToneMapping = m_bHdrLocalToneMapping;

	if (bHdrPassthrough) {
		m_pPSHDR10ToneMapping.Release();
		m_pHDR10ToneMappingConstants.Release();

		if (m_D3D11VP.IsReady()) {
			m_pPSCorrection.Release();

			auto resId = (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084 || m_bVPUseRTXVideoHDR) ? IDF_PS_11_CONVERT_PQ_TO_SDR : IDF_PS_11_FIXCONVERT_HLG_TO_SDR;
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, resId));
			SetShaderLuminanceParams();
		} else {
			m_bHdrPassthrough = false;
			m_bHdrLocalToneMapping = false;
			UpdateConvertColorShader();
		}
	}

	const auto backupVidRect = m_videoRect;
	const auto backupWndRect = m_windowRect;
	m_videoRect  = imageRect;
	m_windowRect = imageRect;
	UpdateTexures();
	UpdatePostScaleTexures();

	auto pSub11CallBack = m_pFilter->m_pSub11CallBack;
	m_pFilter->m_pSub11CallBack = nullptr;

	hr = Process(pRGB32Texture2D, m_srcRect, imageRect, false, false);

	m_pFilter->m_pSub11CallBack = pSub11CallBack;

	m_videoRect  = backupVidRect;
	m_windowRect = backupWndRect;
	UpdateTexures();
	UpdatePostScaleTexures();

	if (bHdrPassthrough) {
		if (m_D3D11VP.IsReady()) {
			m_pCorrectionConstants.Release();
			m_pPSCorrection.Release();

			if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) {
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, IDF_PS_11_CONVERT_HLG_TO_PQ));
			}
		} else {
			m_bHdrPassthrough = bSavedHdrPassthrough;
			m_bHdrLocalToneMapping = bSavedHdrLocalToneMapping;
			UpdateConvertColorShader();
		}

		if (m_bHdrPassthroughSupport && m_bHdrLocalToneMapping) {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHDR10ToneMapping, IDF_PS_11_HDR10_TONEMAP));
			DLogIf(m_pPSHDR10ToneMapping, L"CDX11VideoProcessor::InitMediaType() m_pPSHDR10ToneMapping(type: '{}') created", m_iHdrLocalToneMappingType);
		}
	}

	if (FAILED(hr)) {
		return hr;
	}

	CComPtr<ID3D11Texture2D> pRGB32Texture2D_Shared;
	texdesc = CreateTex2DDesc(DXGI_FORMAT_B8G8R8X8_UNORM, w, h, Tex2D_StagingRead);

	hr = m_pDevice->CreateTexture2D(&texdesc, nullptr, &pRGB32Texture2D_Shared);
	if (FAILED(hr)) {
		return hr;
	}
	m_pDeviceContext->CopyResource(pRGB32Texture2D_Shared, pRGB32Texture2D);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (S_OK == m_pDeviceContext->Map(pRGB32Texture2D_Shared, 0, D3D11_MAP_READ, 0, &mr)) {
		CopyPlaneAsIs(h, (BYTE*)(pBIH + 1), dib_pitch, (BYTE*)mr.pData, mr.RowPitch);
		m_pDeviceContext->Unmap(pRGB32Texture2D_Shared, 0);
	} else {
		return E_FAIL;
	}

	return S_OK;
}

HRESULT CDX11VideoProcessor::GetDisplayedImage(BYTE **ppDib, unsigned* pSize)
{
	if (!m_pDXGISwapChain1 || !m_pDevice || !m_pDeviceContext) {
		return E_ABORT;
	}

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() failed with error {}", HR2Str(hr));
		return hr;
	}

	D3D11_TEXTURE2D_DESC desc;
	pBackBuffer->GetDesc(&desc);

	if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() backbuffer format not supported");
		return E_FAIL;
	}

	D3D11_TEXTURE2D_DESC desc2 = CreateTex2DDesc(desc.Format, desc.Width, desc.Height, Tex2D_StagingRead);
	CComPtr<ID3D11Texture2D> pTexture2DShared;
	hr = m_pDevice->CreateTexture2D(&desc2, nullptr, &pTexture2DShared);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_pDeviceContext->CopyResource(pTexture2DShared, pBackBuffer);

	UINT dib_bitdepth;
	CopyFrameDataFn pConvertToDibFunc;
	if (desc2.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
		if (m_bAllowDeepColorBitmaps) {
			dib_bitdepth = 48;
			pConvertToDibFunc = ConvertR10G10B10A2toBGR48;
		} else {
			dib_bitdepth = 32;
			pConvertToDibFunc = ConvertR10G10B10A2toBGR32;
		}
	} else {
		dib_bitdepth = 32;
		pConvertToDibFunc = CopyPlaneAsIs;
	}
	const UINT dib_pitch    = CalcDibRowPitch(desc.Width, dib_bitdepth);
	const UINT dib_size     = dib_pitch * desc.Height;

	*pSize = sizeof(BITMAPINFOHEADER) + dib_size;
	BYTE* p = (BYTE*)LocalAlloc(LMEM_FIXED, *pSize); // only this allocator can be used
	if (!p) {
		return E_OUTOFMEMORY;
	}

	BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)p;
	ZeroMemory(pBIH, sizeof(BITMAPINFOHEADER));
	pBIH->biSize      = sizeof(BITMAPINFOHEADER);
	pBIH->biWidth     = desc.Width;
	pBIH->biHeight    = -(LONG)desc.Height; // top-down RGB bitmap
	pBIH->biBitCount  = dib_bitdepth;
	pBIH->biPlanes    = 1;
	pBIH->biSizeImage = dib_size;

	D3D11_MAPPED_SUBRESOURCE mappedResource = {};
	hr = m_pDeviceContext->Map(pTexture2DShared, 0, D3D11_MAP_READ, 0, &mappedResource);
	if (SUCCEEDED(hr)) {
		pConvertToDibFunc(desc.Height, (BYTE*)(pBIH + 1), dib_pitch, (BYTE*)mappedResource.pData, mappedResource.RowPitch);
		m_pDeviceContext->Unmap(pTexture2DShared, 0);
		*ppDib = p;
	} else {
		LocalFree(p);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::GetVPInfo(std::wstring& str)
{
	str = L"DirectX 11";
	str += std::format(L"\nGraphics adapter: {}", m_strAdapterDescription);
	str.append(L"\nVideoProcessor  : ");
	if (m_D3D11VP.IsReady()) {
		D3D11_VIDEO_PROCESSOR_CAPS caps;
		UINT rateConvIndex;
		D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rateConvCaps;
		m_D3D11VP.GetVPParams(caps, rateConvIndex, rateConvCaps);

		str += std::format(L"D3D11, RateConversion_{}", rateConvIndex);

		str.append(L"\nDeinterlaceTech.:");
		if (rateConvCaps.ProcessorCaps) {
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND)               str.append(L" Blend,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB)                 str.append(L" Bob,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE)            str.append(L" Adaptive,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION) str.append(L" Motion Compensation,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE)                str.append(L" Inverse Telecine,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION)           str.append(L" Frame Rate Conversion");
		} else {
			str.append(L" none");
		}
		str_trim_end(str, ',');
		str += std::format(L"\nReference Frames: Past {}, Future {}", rateConvCaps.PastFrames, rateConvCaps.FutureFrames);
	} else {
		str.append(L"Shaders");
	}

	if (m_bDlssNR) {
		str.append(L"\n\n");
		str.append(m_DlssNR.GetInfoBlock());
	}

	str.append(m_strStatsDispInfo);

	if (m_pPostScaleShaders.size()) {
		str.append(L"\n\nPost scale pixel shaders:");
		for (const auto& pshader : m_pPostScaleShaders) {
			str += std::format(L"\n  {}", pshader.name);
		}
	}

#ifdef _DEBUG
	str.append(L"\n\nDEBUG info:");
	str += std::format(L"\nSource tex size: {}x{}", m_srcWidth, m_srcHeight);
	str += std::format(L"\nSource rect    : {},{},{},{} - {}x{}", m_srcRect.left, m_srcRect.top, m_srcRect.right, m_srcRect.bottom, m_srcRect.Width(), m_srcRect.Height());
	str += std::format(L"\nVideo rect     : {},{},{},{} - {}x{}", m_videoRect.left, m_videoRect.top, m_videoRect.right, m_videoRect.bottom, m_videoRect.Width(), m_videoRect.Height());
	str += std::format(L"\nWindow rect    : {},{},{},{} - {}x{}", m_windowRect.left, m_windowRect.top, m_windowRect.right, m_windowRect.bottom, m_windowRect.Width(), m_windowRect.Height());

	if (m_pDevice) {
		std::vector<std::pair<const DXGI_FORMAT, UINT>> formatsYUV = {
			{ DXGI_FORMAT_NV12,               0 },
			{ DXGI_FORMAT_P010,               0 },
			{ DXGI_FORMAT_P016,               0 },
			{ DXGI_FORMAT_YUY2,               0 },
			{ DXGI_FORMAT_Y210,               0 },
			{ DXGI_FORMAT_Y216,               0 },
			{ DXGI_FORMAT_AYUV,               0 },
			{ DXGI_FORMAT_Y410,               0 },
			{ DXGI_FORMAT_Y416,               0 },
		};
		std::vector<std::pair<const DXGI_FORMAT, UINT>> formatsRGB = {
			{ DXGI_FORMAT_B8G8R8X8_UNORM,     0 },
			{ DXGI_FORMAT_B8G8R8A8_UNORM,     0 },
			{ DXGI_FORMAT_R10G10B10A2_UNORM,  0 },
			{ DXGI_FORMAT_R16G16B16A16_UNORM, 0 },
		};
		for (auto& [format, formatSupport] : formatsYUV) {
			m_pDevice->CheckFormatSupport(format, &formatSupport);
		}
		for (auto& [format, formatSupport] : formatsRGB) {
			m_pDevice->CheckFormatSupport(format, &formatSupport);
		}

		int count = 0;
		str += L"\nD3D11 VP input formats  :";
		for (const auto& [format, formatSupport] : formatsYUV) {
			if (formatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
				count++;
			}
		}
		if (count) {
			str += L"\n ";
		}
		for (const auto& [format, formatSupport] : formatsRGB) {
			if (formatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
			}
		}

		count = 0;
		str += L"\nShader Texture2D formats:";
		for (const auto& [format, formatSupport] : formatsYUV) {
			if (formatSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
				count++;
			}
		}
		if (count) {
			str += L"\n ";
		}
		for (const auto& [format, formatSupport] : formatsRGB) {
			if (formatSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
			}
		}
	}
#endif

	return S_OK;
}

void CDX11VideoProcessor::Configure(const Settings_t& config)
{
	bool changeWindow            = false;
	bool changeDevice            = false;
	bool changeVP                = false;
	bool changeHDR               = false;
	bool changeTextures          = false;
	bool changeConvertShader     = false;
	bool changeBitmapShader      = false;
	bool changeUpscalingShader   = false;
	bool changeDowndcalingShader = false;
	bool changeNumTextures       = false;
	bool changeResizeStats       = false;
	bool changeSuperRes          = false;
	bool changeRTXVideoHDR       = false;
	bool changeLuminanceParams   = false;
	bool changeDlssSession       = false;
	bool changeDlssReload        = false;
	bool changeDlssFeature       = false;

	const bool bDlssNRActiveBefore = m_bDlssNRActive;
	const bool bDlssSRActiveBefore = m_bDlssSRActive;
	const bool bRenderAheadBefore  = m_bDlssRenderAhead;

	// settings that do not require preparation
	m_bShowStats           = config.bShowStats;
	m_bDeintDouble         = config.bDeintDouble;
	m_bVBlankBeforePresent = config.bVBlankBeforePresent;
	m_bAdjustPresentTime   = config.bAdjustPresentTime;
	m_bDeintBlend          = config.bDeintBlend;

	// A picture delay that changes is taken at the next picture. Left to itself the
	// schedule would keep the step it has for as long as the picture sat within three
	// quarters of a refresh of its new time, and a slider moved by 20 ms on a screen
	// at 24 Hz would do nothing at all.
	if (const REFERENCE_TIME rtVideoDelay = (REFERENCE_TIME)config.iVideoDelay * 10000; rtVideoDelay != m_rtVideoDelay) {
		m_rtVideoDelay = rtVideoDelay;
		m_PresentSchedule.Flush();
	}

	// checking what needs to be changed

	{
		// Style and the four strengths are pushed on every Evaluate, so they
		// need no rebuild; the snippet resets its own temporal history when the
		// style changes. The preset is baked into the feature.
		const CDlssNR::Params newParams = DlssParamsFromSettings(config);
		if (newParams.iPreset != m_DlssParams.iPreset) {
			changeDlssFeature = true;
		}
		m_DlssParams = newParams;
		// Nor does the stabilizer: DlssNRPass creates, changes or drops it on
		// the next picture.
		m_iDlssNRStabilizer    = config.iDlssNRStabilizer;
		m_iDlssNRFlowDivider   = config.iDlssNRFlowDivider;

		// Only a different DLL justifies tearing the whole session down.
		if (m_strDlssNRDllPath != config.szDlssNRDllPath) {
			m_strDlssNRDllPath = config.szDlssNRDllPath;
			changeDlssReload = true;
		}
		if (config.bDlssNR != m_bDlssNR) {
			m_bDlssNR = config.bDlssNR;
			changeDlssSession = true;
			// The hardware video processor has to be rebuilt: toggling DLSS
			// changes the size it renders into, and a VP still configured for
			// the old size leaves the bottom rows of the new texture unwritten
			// -- which shows up as a band of repeated lines. The picture on
			// screen is carried across that rebuild in the changeTextures block.
			changeTextures = true;
		}
		if (config.bDlssNRAfterUpscale != m_bDlssNRAfterUpscale) {
			m_bDlssNRAfterUpscale = config.bDlssNRAfterUpscale;
			changeDlssFeature = true;   // the working resolution changes
			changeTextures = true;
		}
		if (config.iDlssNRWorkingScale != m_iDlssNRWorkingScale) {
			m_iDlssNRWorkingScale = config.iDlssNRWorkingScale;
			changeDlssFeature = true;   // so does this
			changeTextures = true;
		}
	}

	{
		// DLSS Super Resolution. The preset is baked into the feature, and
		// DlssSRPass makes a new one on the next picture; only another DLL ends
		// the session.
		bool bUpdateSR = false;
		if (m_strDlssSRDllPath != config.szDlssSRDllPath) {
			m_strDlssSRDllPath = config.szDlssSRDllPath;
			m_DlssSR.Shutdown();
			m_bDlssSRActive = false;
			bUpdateSR = true;
		}
		if (config.bDlssSR != m_bDlssSR) {
			m_bDlssSR = config.bDlssSR;
			bUpdateSR = true;
		}
		if (config.bDlssSRDlaa != m_bDlssSRDlaa) {
			// The feature was made for one pair of sizes, and DLAA is another pair.
			m_bDlssSRDlaa = config.bDlssSRDlaa;
			m_DlssSR.ReleaseFeature();
			bUpdateSR = true;
		}
		m_iDlssSRPreset = config.iDlssSRPreset;
		if (bUpdateSR && m_pDevice) {
			const bool bWasActive = m_bDlssSRActive;
			UpdateDlssSR();
			if (m_bDlssSRActive != bWasActive) {
				// The hardware video processor stops or starts scaling: the same
				// rebuild as a DLSS 5 NR toggle.
				changeTextures = true;
			}
			UpdateScalingStrings();
		}
	}

	if (config.iResizeStats != m_iResizeStats) {
		m_iResizeStats = config.iResizeStats;
		changeResizeStats = true;
	}

	if (changeDlssReload) {
		m_DlssNR.Shutdown();
		m_bDlssNRActive = false;
	}
	if (changeDlssReload || changeDlssSession) {
		m_nDlssNRRecoveries = 0;   // asked for again, by hand
		m_tickDlssNRRecover = 0;
		UpdateDlssNR();
	} else if (changeDlssFeature) {
		m_DlssNR.ReleaseFeature();
		changeTextures = true;
	}

	m_bDlssRenderAhead = config.bDlssRenderAhead;
	if (m_bDlssRenderAhead != bRenderAheadBefore || m_bDlssNRActive != bDlssNRActiveBefore
			|| m_bDlssSRActive != bDlssSRActiveBefore || changeDlssFeature) {
		// What render ahead measures has changed: it starts over.
		m_RenderAhead.Reset();
		m_DlssNRTimes.Reset();
	}
	if (m_bDlssRenderAhead != bRenderAheadBefore) {
		// So does the step the pictures were keeping, and the line that says how they are timed.
		m_PresentSchedule.Flush();
		m_bScheduled = false;
		UpdateStatsPresent();
	}

	if (config.iTexFormat != m_iTexFormat) {
		m_iTexFormat = config.iTexFormat;
		changeTextures = true;
	}

	if (m_srcParams.cformat == CF_NV12) {
		changeVP = config.VPFmts.bNV12 != m_VPFormats.bNV12;
	}
	else if (m_srcParams.cformat == CF_P010 || m_srcParams.cformat == CF_P016) {
		changeVP = config.VPFmts.bP01x != m_VPFormats.bP01x;
	}
	else if (m_srcParams.cformat == CF_YUY2) {
		changeVP = config.VPFmts.bYUY2 != m_VPFormats.bYUY2;
	}
	else {
		changeVP = config.VPFmts.bOther != m_VPFormats.bOther;
	}
	m_VPFormats = config.VPFmts;

	if (config.iVPDeinterlacing != m_iVPDeinterlacing) {
		m_iVPDeinterlacing = config.iVPDeinterlacing;
		if (m_bInterlaced) {
			changeVP = true;
		}
	}

	if (config.bVPScaling != m_bVPScaling) {
		m_bVPScaling = config.bVPScaling;
		changeTextures = true;
		changeVP = true; // temporary solution
	}

	if (config.iSharpen != m_iSharpen) {
		m_iSharpen = config.iSharpen;
		// Switching it on or off adds or removes a step; changing the method does
		// not, and only swaps the shader.
		changeNumTextures = UpdateSharpenShader() || changeNumTextures;
	}
	m_iSharpenLevel = config.iSharpenLevel;   // read afresh with every picture

	if (config.bVPReplaceChroma != m_bVPReplaceChroma) {
		m_bVPReplaceChroma = config.bVPReplaceChroma;
		if (!m_bInterlaced && m_srcParams.VP11Format != DXGI_FORMAT_UNKNOWN
				&& (m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422)) {
			changeVP = true;   // only InitMediaType chooses which processor takes the picture
		}
	}

	if (config.iChromaScaling != m_iChromaScaling) {
		m_iChromaScaling = config.iChromaScaling;
		m_bMpvChromaFailed = false;
		changeConvertShader = (m_PSConvColorData.bEnable || m_bChromaReplacedVP)
			&& (m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422);
	}

	if (config.iHdrOsdBrightness != m_iHdrOsdBrightness) {
		m_iHdrOsdBrightness = config.iHdrOsdBrightness;
		changeBitmapShader = true;
	}

	if (config.iUpscaling != m_iUpscaling || config.bUpscalingSmall != m_bUpscalingSmall
			|| config.iUpscalingSmall != m_iUpscalingSmall) {
		m_iUpscaling = config.iUpscaling;
		m_bUpscalingSmall = config.bUpscalingSmall;
		m_iUpscalingSmall = config.iUpscalingSmall;
		// Whichever of the two the picture at hand gets is settled where the sizes
		// are known; this only has to be right until the shaders are loaded again.
		m_iUpscalingInUse = UpscalingFor(
			(m_iRotation == 90 || m_iRotation == 270) ? m_srcRectHeight : m_srcRectWidth,
			(m_iRotation == 90 || m_iRotation == 270) ? m_srcRectWidth : m_srcRectHeight,
			m_videoRect.Width(), m_videoRect.Height());
		changeUpscalingShader = true;
	}
	if (config.iDownscaling != m_iDownscaling) {
		m_iDownscaling = config.iDownscaling;
		changeDowndcalingShader = true;
	}

	if (config.bUseDither != m_bUseDither) {
		m_bUseDither = config.bUseDither;
		changeNumTextures = m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM;
	}

	if (config.iSwapEffect != m_iSwapEffect) {
		m_iSwapEffect = config.iSwapEffect;
		changeWindow = !m_pFilter->m_bExclusiveScreen;
	}

	if (config.bHdrPreferDoVi != m_bHdrPreferDoVi) {
		if (m_Dovi.bValid && !config.bHdrPreferDoVi && SourceIsHDR10orHLG()) {
			m_Dovi = {};
			m_DoviExtensionMetadata = {};
			changeVP = true;
		}
		m_bHdrPreferDoVi = config.bHdrPreferDoVi;
	}

	if (config.bHdrPassthrough != m_bHdrPassthrough) {
		m_bHdrPassthrough = config.bHdrPassthrough;
		changeHDR = true;
	}

	if (config.bHdrLocalToneMapping != m_bHdrLocalToneMapping ||
			config.iHdrLocalToneMappingType != m_iHdrLocalToneMappingType) {
		m_bHdrLocalToneMapping = config.bHdrLocalToneMapping;
		m_iHdrLocalToneMappingType = config.iHdrLocalToneMappingType;
		changeHDR = true;
	}

	if (config.iHdrDisplayMaxNits != m_iHdrDisplayMaxNits) {
		m_iHdrDisplayMaxNits = config.iHdrDisplayMaxNits;
		//changeHDR = true;
	}

	if (config.iHdrToggleDisplay != m_iHdrToggleDisplay) {
		if (config.iHdrToggleDisplay == HDRTD_Disabled || m_iHdrToggleDisplay == HDRTD_Disabled) {
			changeHDR = true;
		}
		m_iHdrToggleDisplay = config.iHdrToggleDisplay;
	}

	if (config.bConvertToSdr != m_bConvertToSdr) {
		m_bConvertToSdr = config.bConvertToSdr;
		if (SourceIsHDR()) {
			if (m_D3D11VP.IsReady()) {
				changeNumTextures = true;
				changeVP = true; // temporary solution
			} else {
				changeConvertShader = true;
			}
		}
	}

	if (config.iVPSuperRes != m_iVPSuperRes) {
		m_iVPSuperRes = config.iVPSuperRes;
		changeSuperRes = true;
	}

	if (config.bVPRTXVideoHDR != m_bVPRTXVideoHDR) {
		m_bVPRTXVideoHDR = config.bVPRTXVideoHDR;
		changeRTXVideoHDR = true;
	}

	if (config.iSDRDisplayNits != m_iSDRDisplayNits) {
		m_iSDRDisplayNits = config.iSDRDisplayNits;
		if (SourceIsHDR()) {
			changeLuminanceParams = true;
		}
	}

	if (!m_pFilter->GetActive()) {
		return;
	}

	// apply new settings

	if (changeWindow) {
		ReleaseSwapChain();
		EXECUTE_ASSERT(S_OK == m_pFilter->Init(true));

		if (changeHDR && (SourceIsHDR10orHLG() || m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR) || m_iHdrToggleDisplay) {
			m_srcVideoTransferFunction = 0;
			InitMediaType(&m_pFilter->m_inputMT);
		}
		return;
	}

	if (changeHDR) {
		if (SourceIsHDR10orHLG() || m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR || m_iHdrToggleDisplay) {
			if (m_iSwapEffect == SWAPEFFECT_Discard) {
				ReleaseSwapChain();
				m_pFilter->Init(true);
			}

			m_srcVideoTransferFunction = 0;
			InitMediaType(&m_pFilter->m_inputMT);

			return;
		}
	}

	if (m_Dovi.bValid) {
		changeVP = false;
	}
	if (changeVP) {
		InitMediaType(&m_pFilter->m_inputMT);
		if (m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR) {
			InitSwapChain(false);
		}

		return; // need some test
	}

	if (changeRTXVideoHDR) {
		InitMediaType(&m_pFilter->m_inputMT);

		return;
	}

	// changes that do not require a global rebuild of the video processor

	if (changeTextures) {
		UpdateTexParams(m_srcParams.CDepth);
		if (m_D3D11VP.IsReady()) {
			// The rebuild drops the picture the processor holds, and its new input
			// textures show whatever memory they were given -- green when that is
			// zeroes. While paused nothing replaces it: the renderer releases each
			// sample once drawn, so SetSettings() often has none to re-feed. The
			// input format and size do not change here, so hand the picture back.
			// Measured in tools/dlssnr_probe/vp_rebuild_test.cpp.
			const CD3D11VP::HeldFrame heldFrame = m_D3D11VP.HoldFrame();
			// update m_D3D11OutputFmt
			EXECUTE_ASSERT(S_OK == InitializeD3D11VP(m_srcParams, m_srcWidth, m_srcHeight, &m_pFilter->m_inputMT));
			m_D3D11VP.RestoreFrame(heldFrame, m_pDeviceContext, m_SampleFormat);
		}
		UpdateTexures();
		UpdatePostScaleTexures();
	}

	if (changeConvertShader) {
		if (m_bChromaReplacedVP) {
			UpdateConvertTo444Shader();   // the chroma the video processor no longer does
		} else {
			UpdateConvertColorShader();
		}
	}

	if (changeBitmapShader) {
		UpdateBitmapShader();
	}

	if (changeUpscalingShader) {
		UpdateUpscalingShaders();
	}
	if (changeDowndcalingShader) {
		UpdateDownscalingShaders();
	}

	if (changeLuminanceParams) {
		SetShaderLuminanceParams();
	}

	if (changeNumTextures) {
		UpdatePostScaleTexures();
	}

	if (changeResizeStats) {
		UpdateStatsByWindow();
		UpdateStatsByDisplay();
	}

	if (changeSuperRes) {
		UpdateVPSuperRes(m_srcParams.CDepth);
	}
	// The second turn is made for Super Resolution as asked and for the chroma as
	// kept, and neither of the two always rebuilds anything above.
	UpdateVPScale();

	UpdateStatsStatic();
}

void CDX11VideoProcessor::SetRotation(int value)
{
	m_iRotation = value;
	if (m_D3D11VP.IsReady()) {
		m_D3D11VP.SetRotation(static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(value / 90));
	}
}

void CDX11VideoProcessor::SetStereo3dTransform(int value)
{
	m_iStereo3dTransform = value;

	if (m_iStereo3dTransform == 1) {
		if (!m_pPSHalfOUtoInterlace) {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHalfOUtoInterlace, IDF_PS_11_HALFOU_TO_INTERLACE));
		}
	}
	else {
		m_pPSHalfOUtoInterlace.Release();
	}
}

void CDX11VideoProcessor::Flush()
{
	if (m_D3D11VP.IsReady()) {
		m_D3D11VP.ResetFrameOrder();
	}

	m_rtStart = 0;
	m_nLateOnScreen = 0;
	m_bSkipPresent = false;
	m_bSkipNextSample = false;
	m_bCatchUpJudged = false;
	m_bCatchUpFutile = false;
	m_PresentSchedule.Flush();
	if (m_PacingTrace.Enabled()) {
		m_PacingTrace.Note(L"flush");
	}
	m_DlssNR.RequestReset();
	m_DlssStabilizer.Reset();
	// Another place in the film, another chance for a session that gave up on this one.
	m_nDlssNRRecoveries = 0;
	if (m_bDlssNR && !m_bDlssNRActive && !m_tickDlssNRRecover
			&& (m_DlssNR.GetState() == CDlssNR::State::EvaluateFailed || m_DlssNR.GetState() == CDlssNR::State::DeviceLost)) {
		m_nDlssNRRecoveries = 1;
		m_tickDlssNRRecover = GetPreciseTick();
	}
	m_DlssSR.RequestReset();
	m_DlssSRMotion.Reset();

	m_DoviExtensionMetadata = {};
#ifndef NDEBUG
	UpdateStatsStatic();
#endif
}

void CDX11VideoProcessor::ClearPreScaleShaders()
{
	for (auto& pExtShader : m_pPreScaleShaders) {
		pExtShader.shader.Release();
	}
	m_pPreScaleShaders.clear();
	DLog(L"CDX11VideoProcessor::ClearPreScaleShaders().");
}


void CDX11VideoProcessor::ClearPostScaleShaders()
{
	for (auto& pExtShader : m_pPostScaleShaders) {
		pExtShader.shader.Release();
	}
	m_pPostScaleShaders.clear();
	//UpdateStatsPostProc();
	DLog(L"CDX11VideoProcessor::ClearPostScaleShaders().");
}

HRESULT CDX11VideoProcessor::AddPreScaleShader(const std::wstring& name, const std::string& srcCode)
{
#ifdef _DEBUG
	if (!m_pDevice) {
		return E_ABORT;
	}

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = CompileShader(srcCode, nullptr, "ps_4_0", &pShaderCode);
	if (S_OK == hr) {
		m_pPreScaleShaders.emplace_back();
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPreScaleShaders.back().shader);
		if (S_OK == hr) {
			m_pPreScaleShaders.back().name = name;
			//UpdatePreScaleTexures(); //TODO
			DLog(L"CDX11VideoProcessor::AddPreScaleShader() : \"{}\" pixel shader added successfully.", name);
		}
		else {
			DLog(L"CDX11VideoProcessor::AddPreScaleShader() : create pixel shader \"{}\" FAILED!", name);
			m_pPreScaleShaders.pop_back();
		}
		pShaderCode->Release();
	}

	if (S_OK == hr && m_D3D11VP.IsReady() && m_bVPScaling) {
		return S_FALSE;
	}

	return hr;
#else
	return E_NOTIMPL;
#endif
}

HRESULT CDX11VideoProcessor::AddPostScaleShader(const std::wstring& name, const std::string& srcCode)
{
	if (!m_pDevice) {
		return E_ABORT;
	}

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = CompileShader(srcCode, nullptr, "ps_4_0", &pShaderCode);
	if (S_OK == hr) {
		m_pPostScaleShaders.emplace_back();
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPostScaleShaders.back().shader);
		if (S_OK == hr) {
			m_pPostScaleShaders.back().name = name;
			UpdatePostScaleTexures();
			DLog(L"CDX11VideoProcessor::AddPostScaleShader() : \"{}\" pixel shader added successfully.", name);
		}
		else {
			DLog(L"CDX11VideoProcessor::AddPostScaleShader() : create pixel shader \"{}\" FAILED!", name);
			m_pPostScaleShaders.pop_back();
		}
		pShaderCode->Release();
	}

	return hr;
}

ISubPicAllocator* CDX11VideoProcessor::GetSubPicAllocator()
{
	if (!m_pSubPicAllocator && m_pDevice) {
		m_pSubPicAllocator = new CDX11SubPicAllocator(m_pDevice, { 1280, 720 });
	}
	return m_pSubPicAllocator;
}

void CDX11VideoProcessor::UpdateStatsPresent()
{
	DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
	if (m_pDXGISwapChain1 && S_OK == m_pDXGISwapChain1->GetDesc1(&swapchain_desc)) {
		m_strStatsPresent.assign(L"\nSwap chain    : ");
		if (m_nSwapChainResizeFailed) {
			m_strStatsPresent.append(std::format(L"rebuilt after {} refused resize(s), ", m_nSwapChainResizeFailed));
		}
		switch (swapchain_desc.SwapEffect) {
		case DXGI_SWAP_EFFECT_DISCARD:
			m_strStatsPresent.append(L"Discard");
			break;
		case DXGI_SWAP_EFFECT_SEQUENTIAL:
			m_strStatsPresent.append(L"Sequential");
			break;
		case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
			m_strStatsPresent.append(L"Flip sequential");
			break;
		case DXGI_SWAP_EFFECT_FLIP_DISCARD:
			m_strStatsPresent.append(L"Flip discard");
			break;
		}
		m_strStatsPresent.append(L", ");
		m_strStatsPresent.append(DXGIFormatToString(swapchain_desc.Format));
		// How many pictures it can hold decides whether a heavy chain waits a refresh
		// before it can draw the next (InitSwapChain, MPCVR_SWAPCHAIN_BUFFERS).
		m_strStatsPresent += std::format(L", {} buffers", swapchain_desc.BufferCount);

		// With render ahead and a flip model the two settings below are not what times
		// a Present: each picture is given a refresh, and the line says that instead.
		if (m_bDlssRenderAhead && swapchain_desc.SwapEffect != DXGI_SWAP_EFFECT_DISCARD
				&& swapchain_desc.SwapEffect != DXGI_SWAP_EFFECT_SEQUENTIAL) {
			m_strStatsPresent.append(L"\nFrame sync    : a refresh chosen for each picture");
		} else
		if ((m_bVBlankBeforePresent && m_pDXGIOutput) || m_bAdjustPresentTime) {
			m_strStatsPresent.append(L"\nFrame sync    :");
			if (m_bVBlankBeforePresent && m_pDXGIOutput) {
				m_strStatsPresent.append(L" wait VBlank");
			}
			if (m_bAdjustPresentTime) {
				if (m_strStatsPresent.back() != ':') {
					m_strStatsPresent += ',';
				}
				m_strStatsPresent.append(L" adjust present time");
			}
		}
	}
}

void CDX11VideoProcessor::UpdateStatsStatic()
{
	if (!m_bShowStats) {
		return;
	}

	if (m_srcParams.cformat) {
		m_strStatsHeader = std::format(L"MPC VR {}, Direct3D 11, Windows {}", _CRT_WIDE(VERSION_STR), GetWindowsVersion());

		UpdateStatsInputFmt();

		// The conversion, DLSS and the resizing are steps of the pipeline, which
		// GetStatsText lays out in their order with what each costs.
		m_strStatsVProc.clear();

		// The HDR step's text, which GetStatsText gives its label.
		if (SourceIsHDR() || m_bVPUseRTXVideoHDR) {
			m_strStatsHDR.clear();
			if (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
				if (m_bHdrPassthrough) {
					m_strStatsHDR.append(L"Passthrough");
				} else if (m_bHdrLocalToneMapping) {
					m_strStatsHDR.append(L"Local tone mapping:");
					switch (m_iHdrLocalToneMappingType) {
						case 1:
							m_strStatsHDR.append(L" ACES");
							break;
						case 2:
							m_strStatsHDR.append(L" Reinhard");
							break;
						case 3:
							m_strStatsHDR.append(L" Habel");
							break;
						case 4:
							m_strStatsHDR.append(L" Mobius");
							break;
						case 5:
							if (m_DoviExtensionMetadata.L1.present) {
								m_strStatsHDR.append(L" ST 2094-10");
							} else {
								m_strStatsHDR.append(L" BT2390");
							}
							break;
						default:
							break;
					}
					m_strStatsHDR.append(std::format(L"\n Display Max Nits: {} nits", m_iHdrDisplayMaxNits));
				}
				if (m_bVPUseRTXVideoHDR) {
					m_strStatsHDR.append(L", RTX Video HDR*");
				}
				if (m_bHdrLocalToneMapping && m_DoviExtensionMetadata.L1.present) {
					m_strStatsHDR += std::format(L", {} nits", m_DoviExtensionMetadata.L1.max_pq);
#ifndef NDEBUG
					if (m_DoviExtensionMetadata.L1.present || m_DoviExtensionMetadata.L2.present) {
						m_strStatsHDR.append(L"\n Dolby Vision metadata:");

						if (m_DoviExtensionMetadata.L1.present) {
							m_strStatsHDR.append(std::format(L"\n  L1: min/max/avg pq : {}/{}/{}",
															 m_DoviExtensionMetadata.L1.min_pq,
															 m_DoviExtensionMetadata.L1.max_pq,
															 m_DoviExtensionMetadata.L1.avg_pq));
						}

						if (m_DoviExtensionMetadata.L2.present) {
							m_strStatsHDR.append(
								std::format(L"\n  L2: trim_slope: {:.2f}\n      trim_offset: {:.2f}\n      trim_power: {:.2f}\n      trim_saturation_gain: {:.2f}\n      trim_chroma_weight: {:.2f}",
											m_DoviExtensionMetadata.L2.trim_slope,
											m_DoviExtensionMetadata.L2.trim_offset,
											m_DoviExtensionMetadata.L2.trim_power,
											m_DoviExtensionMetadata.L2.trim_saturation_gain,
											m_DoviExtensionMetadata.L2.trim_chroma_weight));
						}
					}
#endif
				} else if (m_lastHdr10.bValid) {
					if (m_bHdrLocalToneMapping) {
						m_strStatsHDR += std::format(L"\n HDR10 metadata  : Max Luminance: {}", m_lastHdr10.hdr10.MaxMasteringLuminance);
						if (m_lastHdr10.hdr10.MaxContentLightLevel) {
							m_strStatsHDR += std::format(L", MaxCLL: {}", m_lastHdr10.hdr10.MaxContentLightLevel);
						}
						m_strStatsHDR += L" nits";
					} else {
						m_strStatsHDR += std::format(L", {} nits", m_lastHdr10.hdr10.MaxMasteringLuminance);
					}
				}
			} else if (m_bConvertToSdr) {
				m_strStatsHDR.append(L"Convert to SDR");
#ifndef NDEBUG
				if (m_DoviExtensionMetadata.L2.present) {
					m_strStatsHDR.append(L"\n Dolby Vision metadata:");
					m_strStatsHDR.append(
						std::format(L"\n  L2: trim_slope: {:.2f}\n      trim_offset: {:.2f}\n      trim_power: {:.2f}\n      trim_saturation_gain: {:.2f}\n      trim_chroma_weight: {:.2f}",
									m_DoviExtensionMetadata.L2.trim_slope,
									m_DoviExtensionMetadata.L2.trim_offset,
									m_DoviExtensionMetadata.L2.trim_power,
									m_DoviExtensionMetadata.L2.trim_saturation_gain,
									m_DoviExtensionMetadata.L2.trim_chroma_weight));
				}
#endif
			} else {
				m_strStatsHDR.append(L"Not used");
			}
		} else {
			m_strStatsHDR.clear();
		}

		UpdateStatsPresent();
	}
	else {
		m_strStatsHeader = L"Error";
		m_strStatsVProc.clear();
		m_strStatsInputFmt.clear();
		//m_strStatsPostProc.clear();
		m_strStatsHDR.clear();
		m_strStatsPresent.clear();
	}
}

/*
void CDX11VideoProcessor::UpdateStatsPostProc()
{
	if (m_strCorrection || m_pPostScaleShaders.size() || m_bFinalPass) {
		m_strStatsPostProc.assign(L"\nPostProcessing:");
		if (m_strCorrection) {
			m_strStatsPostProc += std::format(L" {},", m_strCorrection);
		}
		if (m_pPostScaleShaders.size()) {
			m_strStatsPostProc += std::format(L" shaders[{}],", m_pPostScaleShaders.size());
		}
		if (m_bFinalPass) {
			m_strStatsPostProc.append(L" dither");
		}
		str_trim_end(m_strStatsPostProc, ',');
	}
	else {
		m_strStatsPostProc.clear();
	}
}
*/

HRESULT CDX11VideoProcessor::DrawStats(ID3D11Texture2D* pRenderTarget)
{
	if (m_windowRect.IsRectEmpty()) {
		return E_ABORT;
	}

	const std::wstring str = GetStatsText();

	// A step's lines come and go with the step: the box is made to fit them.
	if (UpdateStatsLayout(str)) {
		CalcStatsParams();
	}

	ID3D11RenderTargetView* pRenderTargetView = nullptr;
	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (S_OK == hr) {
		SIZE rtSize = m_windowRect.Size();

		m_StatsBackground.Draw(pRenderTargetView, rtSize);

		hr = m_Font3D.Draw2DText(pRenderTargetView, rtSize, m_StatsTextPoint.x, m_StatsTextPoint.y, m_dwStatsTextColor, str.c_str());
		// The marker scrolls along the bottom of the box, and starts over when the box
		// has moved under it.
		if (--m_StatsMarkerX < m_StatsRect.left || m_StatsMarkerX > m_StatsRect.right) {
			m_StatsMarkerX = m_StatsRect.right;
		}
		m_Rect3D.Set({ m_StatsMarkerX, m_StatsRect.bottom - kStatsMarkerH - 1, m_StatsMarkerX + 5, m_StatsRect.bottom - 1 },
			rtSize, D3DCOLOR_XRGB(128, 255, 128));
		m_Rect3D.Draw(pRenderTargetView, rtSize);


		if (CheckGraphPlacement()) {
			m_Underlay.Draw(pRenderTargetView, rtSize);

			m_Lines.Draw();

			m_SyncLine.ClearPoints(rtSize);
			m_SyncLine.AddGFPoints(m_GraphRect.left, m_Xstep, m_Yaxis, m_Yscale,
				m_Syncs.Data(), m_Syncs.OldestIndex(), m_Syncs.Size(), D3DCOLOR_XRGB(100, 200, 100));
			m_SyncLine.UpdateVertexBuffer();
			m_SyncLine.Draw();
		}
		pRenderTargetView->Release();
	}

	return hr;
}

// IMFVideoProcessor

STDMETHODIMP CDX11VideoProcessor::SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues)
{
	CheckPointer(pValues, E_POINTER);
	if (m_srcParams.cformat == CF_NONE) {
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	}

	if (dwFlags & DXVA2_ProcAmp_Brightness) {
		m_DXVA2ProcAmpValues.Brightness.ll = std::clamp(pValues->Brightness.ll, m_DXVA2ProcAmpRanges[0].MinValue.ll, m_DXVA2ProcAmpRanges[0].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Contrast) {
		m_DXVA2ProcAmpValues.Contrast.ll = std::clamp(pValues->Contrast.ll, m_DXVA2ProcAmpRanges[1].MinValue.ll, m_DXVA2ProcAmpRanges[1].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Hue) {
		m_DXVA2ProcAmpValues.Hue.ll = std::clamp(pValues->Hue.ll, m_DXVA2ProcAmpRanges[2].MinValue.ll, m_DXVA2ProcAmpRanges[2].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Saturation) {
		m_DXVA2ProcAmpValues.Saturation.ll = std::clamp(pValues->Saturation.ll, m_DXVA2ProcAmpRanges[3].MinValue.ll, m_DXVA2ProcAmpRanges[3].MaxValue.ll);
	}

	if (dwFlags & DXVA2_ProcAmp_Mask) {
		CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

		m_D3D11VP.SetProcAmpValues(&m_DXVA2ProcAmpValues);

		if (!m_D3D11VP.IsReady()) {
			SetShaderConvertColorParams();
		}
	}

	return S_OK;
}

// IMFVideoMixerBitmap

STDMETHODIMP CDX11VideoProcessor::SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

	HRESULT hr = S_FALSE;

	if (pBmpParms->GetBitmapFromDC && pBmpParms->bitmap.hdc) {
		HBITMAP hBitmap = (HBITMAP)GetCurrentObject(pBmpParms->bitmap.hdc, OBJ_BITMAP);
		if (!hBitmap) {
			return E_INVALIDARG;
		}
		DIBSECTION info = {0};
		if (!::GetObjectW(hBitmap, sizeof(DIBSECTION), &info)) {
			return E_INVALIDARG;
		}
		BITMAP& bm = info.dsBm;
		if (!bm.bmWidth || !bm.bmHeight || bm.bmBitsPixel != 32 || !bm.bmBits) {
			return E_INVALIDARG;
		}

		hr = m_TexAlphaBitmap.CheckCreate(m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM, bm.bmWidth, bm.bmHeight, Tex2D_DefaultShader);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::SetAlphaBitmap() : CheckCreate() failed with error {}", HR2Str(hr));
		if (S_OK == hr) {
			m_pDeviceContext->UpdateSubresource(m_TexAlphaBitmap.pTexture, 0, nullptr, bm.bmBits, bm.bmWidthBytes, 0);
		}
	} else {
		return E_INVALIDARG;
	}

	m_bAlphaBitmapEnable = SUCCEEDED(hr) && m_TexAlphaBitmap.pShaderResource;

	if (m_bAlphaBitmapEnable) {
		m_AlphaBitmapRectSrc = { 0, 0, (LONG)m_TexAlphaBitmap.desc.Width, (LONG)m_TexAlphaBitmap.desc.Height };
		m_AlphaBitmapNRectDest = { 0, 0, 1, 1 };

		m_pAlphaBitmapVertex.Release();

		hr = UpdateAlphaBitmapParameters(&pBmpParms->params);
	}

	return hr;
}

STDMETHODIMP CDX11VideoProcessor::UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

	if (m_bAlphaBitmapEnable) {
		if (pBmpParms->dwFlags & MFVideoAlphaBitmap_SrcRect) {
			m_AlphaBitmapRectSrc = pBmpParms->rcSrc;
			m_pAlphaBitmapVertex.Release();
		}
		if (!m_pAlphaBitmapVertex) {
			HRESULT hr = CreateVertexBuffer(m_pDevice, &m_pAlphaBitmapVertex, m_TexAlphaBitmap.desc.Width, m_TexAlphaBitmap.desc.Height, m_AlphaBitmapRectSrc, 0, false);
			if (FAILED(hr)) {
				m_bAlphaBitmapEnable = false;
				return hr;
			}
		}
		if (pBmpParms->dwFlags & MFVideoAlphaBitmap_DestRect) {
			m_AlphaBitmapNRectDest = pBmpParms->nrcDest;
		}
		DWORD validFlags = MFVideoAlphaBitmap_SrcRect|MFVideoAlphaBitmap_DestRect;

		return ((pBmpParms->dwFlags & validFlags) == validFlags) ? S_OK : S_FALSE;
	} else {
		return MF_E_NOT_INITIALIZED;
	}
}

void CDX11VideoProcessor::SetCallbackDevice()
{
	if (!m_bCallbackDeviceIsSet && m_pDevice && m_pFilter->m_pSub11CallBack) {
		m_bCallbackDeviceIsSet = SUCCEEDED(m_pFilter->m_pSub11CallBack->SetDevice11(m_pDevice));
	}
}

void CDX11VideoProcessor::UpdateSubPic()
{
	ASSERT(m_pDevice);

	if (m_pFilter->m_pSubPicProvider) {
		if (m_pSubPicAllocator) {
			m_pSubPicAllocator->ChangeDevice(m_pDevice);
		}

		if (m_pFilter->m_pSubPicQueue) {
			m_pFilter->m_pSubPicQueue->Invalidate();
			m_pFilter->m_pSubPicQueue->SetSubPicProvider(m_pFilter->m_pSubPicProvider);
		}
	}
}

void CDX11VideoProcessor::SwitchFullScreen(bool set)
{
	m_bFullScreen = set;

	if (HandleHDRToggle()) {
		InitMediaType(&m_pFilter->m_inputMT);
		InitSwapChain(false);
	}
}
