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

// CDX11VideoProcessor: the mpv prescalers on luma and chroma, and the 4:4:4 chroma pre-pass.
// All of it is this fork's, and kept out of DX11VideoProcessor.cpp so that a merge of
// upstream's file meets as little of ours as possible (MERGING-UPSTREAM.md).

#include "stdafx.h"
#include "Helper.h"
#include "Times.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"
#include "DX11VideoProcessorShared.h"
#include "Shaders.h"
#include "Upscale/MpvShaderTables.h"

const MpvShaderInfo* MpvLumaShader(const int upscaling)
{
	switch (upscaling) {
	case UPSCALE_FSRCNNX8:
	case UPSCALE_FSRCNNX8AR:  return &kMpvFSRCNNX8;
	case UPSCALE_FSRCNNX16:
	case UPSCALE_FSRCNNX16AR: return &kMpvFSRCNNX16;
	case UPSCALE_RAVUZoom:    return &kMpvRavuZoomAR3;
	case UPSCALE_ArtCNN:      return &kMpvArtCNNC4F16DS;
	default:                  return nullptr;
	}
}

// The prescaler on Cb and Cr, and whether either is held to the range the source
// really covers around each point. RAVU carries its own anti-ringing inside the
// kernel, which is what the AR of its name means; FSRCNNX and ArtCNN do not.
static const MpvShaderInfo* MpvChromaShader(const int chromaScaling)
{
	switch (chromaScaling) {
	case CHROMA_RAVU:       return &kMpvRavuZoomAR3;
	case CHROMA_FSRCNNX8AR: return &kMpvFSRCNNX8;
	default:                return nullptr;
	}
}

static bool MpvLumaAntiRing(const int upscaling)
{
	return upscaling == UPSCALE_FSRCNNX8AR || upscaling == UPSCALE_FSRCNNX16AR;
}

static bool MpvChromaAntiRing(const int chromaScaling)
{
	return chromaScaling == CHROMA_FSRCNNX8AR;
}

// The compiled passes and tables of the mpv prescalers, from the resources.
static bool MpvResource(UINT resid, const BYTE*& data, size_t& size)
{
	LPVOID pData = nullptr;
	DWORD dwSize = 0;
	if (S_OK != GetDataFromResource(pData, dwSize, resid)) {
		return false;
	}
	data = (const BYTE*)pData;
	size = dwSize;
	return true;
}

void CDX11VideoProcessor::UpdateMpvLuma()
{
	const MpvShaderInfo* pInfo = m_pDevice ? MpvLumaShader(m_iUpscalingInUse) : nullptr;
	m_bMpvLumaAntiRing = MpvLumaAntiRing(m_iUpscalingInUse);
	if (m_MpvLuma.Info() == pInfo) {
		return;
	}

	m_MpvLuma.Release();
	m_TexMpvLuma.Release();
	m_TexMpvResize.Release();
	m_TexMpvColor.Release();
	m_TexMpvOutput.Release();
	m_TexMpvLumaOut.Release();
	if (pInfo) {
		// Feature level 10.x cannot run them: Catmull-Rom stays in their place.
		const HRESULT hr = m_MpvLuma.Load(m_pDevice, *pInfo, IDF_VS_11_MPV_HOOK, MpvResource);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::UpdateMpvLuma() : {} not loaded, error {}", pInfo->name, HR2Str(hr));
	}
}

HRESULT CDX11VideoProcessor::MpvLumaPass(Tex2D_t* pInputTexture, const CRect& rSrc, const CRect& dstRect, const int rotation, Tex2D_t** ppResult)
{
	if (!pInputTexture || !ppResult) {
		return E_POINTER;
	}

	// Sizes before rotation: ResizeShaderPass turns the picture afterwards.
	const bool bTurned = (rotation == 90 || rotation == 270);
	const UINT inW = rSrc.Width(), inH = rSrc.Height();
	const UINT outW = bTurned ? dstRect.Height() : dstRect.Width();
	const UINT outH = bTurned ? dstRect.Width() : dstRect.Height();
	UINT w = 0, h = 0;
	if (!m_MpvLuma.Applies(inW, inH, outW, outH, &w, &h)) {
		return S_FALSE; // not enlarged enough for it: the resize shaders do the job
	}

	m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::MpvLuma);

	const auto Run = [&]() {
		// The luma the network enlarges...
		HRESULT hr = m_TexMpvLuma.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, inW, inH, Tex2D_DefaultShaderRTarget);
		if (S_OK == hr) {
			hr = TextureCopyRect(*pInputTexture, m_TexMpvLuma.pTexture, rSrc, CRect(0, 0, inW, inH), m_pPSMpvLuma, nullptr, 0, false);
		}
		if (S_OK == hr) {
			hr = m_MpvLuma.Process(m_pDeviceContext, m_TexMpvLuma.pShaderResource, inW, inH, outW, outH, 0.0f, 0.0f, m_TexMpvLumaOut);
		}
		if (S_OK != hr) {
			return hr;
		}

		// ... the colour Catmull-Rom enlarges to the same size ...
		const CRect rResize(0, 0, w, inH);
		const CRect rOut(0, 0, w, h);
		hr = m_TexMpvResize.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, w, inH, Tex2D_DefaultShaderRTarget);
		if (S_OK == hr) {
			hr = m_TexMpvColor.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, Tex2D_DefaultShaderRTarget);
		}
		if (S_OK == hr) {
			hr = TextureResizeShader(*pInputTexture, m_TexMpvResize.pTexture, rSrc, rResize, m_pShaderUpscaleX, 0, false);
		}
		if (S_OK == hr) {
			hr = TextureResizeShader(m_TexMpvResize, m_TexMpvColor.pTexture, rResize, rOut, m_pShaderUpscaleY, 0, false);
		}

		// ... and takes the network's luma.
		if (S_OK == hr) {
			hr = m_TexMpvOutput.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, Tex2D_DefaultShaderRTarget);
		}
		if (S_OK == hr) {
			// The anti-ringing pass also reads the plane the network was given, to know
			// what range it may keep.
			m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexMpvLumaOut.pShaderResource.p);
			if (m_bMpvLumaAntiRing) {
				m_pDeviceContext->PSSetShaderResources(2, 1, &m_TexMpvLuma.pShaderResource.p);
			}
			hr = TextureCopyRect(m_TexMpvColor, m_TexMpvOutput.pTexture, rOut, rOut,
				m_bMpvLumaAntiRing ? m_pPSMpvCombineAR : m_pPSMpvCombine, nullptr, 0, false);
			ID3D11ShaderResourceView* views[2] = {};
			m_pDeviceContext->PSSetShaderResources(1, 2, views);
		}
		return hr;
	};
	const HRESULT hr = Run();

	m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::MpvLuma);

	if (hr != S_OK) {
		// Latch off rather than failing on every picture: Catmull-Rom takes over.
		DLog(L"CDX11VideoProcessor::MpvLumaPass() : {} failed with error {}", m_MpvLuma.Info()->name, HR2Str(hr));
		m_MpvLuma.Release();
		UpdateScalingStrings();
		UpdateStatsStatic();
		return FAILED(hr) ? hr : E_FAIL;
	}

	*ppResult = &m_TexMpvOutput;
	return S_OK;
}

void CDX11VideoProcessor::UpdateMpvChroma()
{
	// The shader video processor on 4:2:0 in two or three planes; the passes need
	// feature level 11.0.
	const MpvShaderInfo* pInfo = MpvChromaShader(m_iChromaScaling);
	const bool bWanted = pInfo && !m_bMpvChromaFailed
		&& m_srcParams.Subsampling == 420
		&& m_srcParams.pDX11Planes && m_srcParams.pDX11Planes->FmtPlane2
		&& m_pDevice && m_pDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;

	if (bWanted && m_MpvChroma.Info() != pInfo) {
		m_MpvChroma.Release();
		const HRESULT hr = m_MpvChroma.Load(m_pDevice, *pInfo, IDF_VS_11_MPV_HOOK, MpvResource);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::UpdateMpvChroma() : {} not loaded, error {}", pInfo->name, HR2Str(hr));
	} else if (!bWanted && m_MpvChroma.IsLoaded()) {
		m_MpvChroma.Release();
		for (int c = 0; c < 2; c++) {
			m_TexMpvChromaIn[c].Release();
			m_TexMpvChromaOut[c].Release();
			m_TexMpvChromaAR[c].Release();
		}
	}
	m_bMpvChromaActive = bWanted && m_MpvChroma.IsLoaded();
	m_bMpvChromaAntiRing = m_bMpvChromaActive && MpvChromaAntiRing(m_iChromaScaling);
}

int CDX11VideoProcessor::ChromaScalingForShader() const
{
	if (MpvChromaShader(m_iChromaScaling)) {
		// CHROMA_RAVU is what the conversion shader knows as "the planes are already
		// at the luma size": which prescaler put them there is no business of its own.
		return m_bMpvChromaActive ? CHROMA_RAVU : CHROMA_CatmullRom;
	}
	return m_iChromaScaling;
}

HRESULT CDX11VideoProcessor::MpvChromaPass()
{
	const UINT texW = m_TexSrcVideo.desc.Width;
	const UINT texH = m_TexSrcVideo.desc.Height;
	const UINT chromaW = texW / m_srcParams.pDX11Planes->div_chroma_w;
	const UINT chromaH = texH / m_srcParams.pDX11Planes->div_chroma_h;

	// Cb and Cr where the conversion shader reads them: both in one plane, or in
	// planes of their own, Cr first for YV12.
	const bool bPlanar = m_TexSrcVideo.pShaderResource3 != nullptr;
	const bool bCrFirst = m_srcParams.cformat == CF_YV12 || m_srcParams.cformat == CF_YV16 || m_srcParams.cformat == CF_YV24;
	ID3D11ShaderResourceView* const planes[2] = {
		(bPlanar && bCrFirst) ? m_TexSrcVideo.pShaderResource3 : m_TexSrcVideo.pShaderResource2,
		!bPlanar ? m_TexSrcVideo.pShaderResource2 : bCrFirst ? m_TexSrcVideo.pShaderResource2 : m_TexSrcVideo.pShaderResource3,
	};
	const FLOAT channels[2][4] = {
		{ 1, 0, 0, 0 },
		{ bPlanar ? 1.0f : 0.0f, bPlanar ? 0.0f : 1.0f, 0, 0 },
	};

	// Where chroma sits among the luma pixels, as the conversion shader has it:
	// MPEG-2 on the left column, co-sited on the top row as well, MPEG-1 centred.
	float shiftX = 0.5f / texW;
	float shiftY = 0.0f;
	switch (m_srcExFmt.VideoChromaSubsampling) {
	case DXVA2_VideoChromaSubsampling_Cosited:
		shiftY = 0.5f / texH;
		break;
	case DXVA2_VideoChromaSubsampling_MPEG1:
		shiftX = 0.0f;
		break;
	}

	HRESULT hr = S_OK;
	for (int c = 0; c < 2 && hr == S_OK; c++) {
		if (!planes[c]) {
			return E_POINTER;
		}
		hr = m_TexMpvChromaIn[c].CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, chromaW, chromaH, Tex2D_DefaultShaderRTarget);
		if (FAILED(hr)) {
			break;
		}

		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pMpvChromaPlaneConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (FAILED(hr)) {
			break;
		}
		memcpy(mr.pData, channels[c], sizeof(channels[c]));
		m_pDeviceContext->Unmap(m_pMpvChromaPlaneConstants, 0);

		CComPtr<ID3D11RenderTargetView> pRenderTargetView;
		hr = m_pDevice->CreateRenderTargetView(m_TexMpvChromaIn[c].pTexture, nullptr, &pRenderTargetView);
		if (FAILED(hr)) {
			break;
		}
		const CRect rect(0, 0, chromaW, chromaH);
		hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, chromaW, chromaH, rect, 0, false);
		if (FAILED(hr)) {
			break;
		}
		D3D11_VIEWPORT VP = { 0.0f, 0.0f, (FLOAT)chromaW, (FLOAT)chromaH, 0.0f, 1.0f };
		TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSMpvChromaPlane,
			planes[c], m_pSamplerPoint, m_pMpvChromaPlaneConstants, m_pVertexBuffer);

		hr = m_MpvChroma.Process(m_pDeviceContext, m_TexMpvChromaIn[c].pShaderResource, chromaW, chromaH, texW, texH,
			shiftX, shiftY, m_TexMpvChromaOut[c]);
		if (hr == S_OK && (m_TexMpvChromaOut[c].width != texW || m_TexMpvChromaOut[c].height != texH)) {
			hr = E_FAIL;
		}
		if (hr != S_OK || !m_bMpvChromaAntiRing) {
			continue;
		}

		// What the network invented past the colours really around that point, taken
		// back: the same four samples the prescaler started from, at the same siting.
		hr = m_TexMpvChromaAR[c].CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, texW, texH, Tex2D_DefaultShaderRTarget);
		if (FAILED(hr)) {
			break;
		}
		const FLOAT shift[4] = { shiftX, shiftY, 0, 0 };
		hr = m_pDeviceContext->Map(m_pMpvChromaPlaneConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (FAILED(hr)) {
			break;
		}
		memcpy(mr.pData, shift, sizeof(shift));
		m_pDeviceContext->Unmap(m_pMpvChromaPlaneConstants, 0);

		CComPtr<ID3D11RenderTargetView> pArTargetView;
		hr = m_pDevice->CreateRenderTargetView(m_TexMpvChromaAR[c].pTexture, nullptr, &pArTargetView);
		if (FAILED(hr)) {
			break;
		}
		const CRect rectFull(0, 0, texW, texH);
		hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, texW, texH, rectFull, 0, false);
		if (FAILED(hr)) {
			break;
		}
		D3D11_VIEWPORT VPFull = { 0.0f, 0.0f, (FLOAT)texW, (FLOAT)texH, 0.0f, 1.0f };
		m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexMpvChromaIn[c].pShaderResource.p);
		TextureBlt11(m_pDeviceContext, pArTargetView, VPFull, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSMpvChromaAR,
			m_TexMpvChromaOut[c].pShaderResource, m_pSamplerPoint, m_pMpvChromaPlaneConstants, m_pVertexBuffer);
		ID3D11ShaderResourceView* noViews[1] = {};
		m_pDeviceContext->PSSetShaderResources(1, 1, noViews);
		m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	}

	return FAILED(hr) ? hr : (hr == S_OK ? S_OK : E_FAIL);
}

// The video processor's own chroma upsampling lands between Nearest and Bilinear, a
// decibel under Catmull-Rom on the colour along luma edges, and on a 10-bit source the
// driver reads the studio range as 16/255..235/255 whatever the depth, which shifts the
// colour by about six tenths of a level -- worth five decibels more than the chroma
// itself (tools/dlssnr_probe playback_test --chroma, --chroma10). So "Replace VP chroma
// upsampling" has a compute shader rebuild the chroma and hand the processor a 4:4:4
// picture, which it then converts with nothing left to reconstruct: RTX Video HDR and
// everything else it does still apply. Only an interlaced source keeps its own chroma:
// this driver deinterlaces NV12 alone, and 4:4:4 would come out woven.
bool CDX11VideoProcessor::ChromaToShaders(const FmtConvParams_t& params, const bool interlaced)
{
	return m_bVPReplaceChroma && !interlaced
		&& (params.Subsampling == 420 || params.Subsampling == 422)
		&& params.VP11Format != DXGI_FORMAT_UNKNOWN && params.DX11Format != DXGI_FORMAT_UNKNOWN
		&& params.pDX11Planes;
}

// The compute shader that rebuilds the chroma with the chosen method and writes the
// picture the video processor reads, in 4:4:4.
HRESULT CDX11VideoProcessor::UpdateConvertTo444Shader()
{
	m_pCSConvertTo444.Release();

	// The prescaler where it runs, Catmull-Rom in its place elsewhere, as in the
	// shader video processor.
	UpdateMpvChroma();
	const int chromaScaling = ChromaScalingForShader();

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = GetShaderConvertTo444(m_srcWidth, m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
		m_srcParams, m_srcExFmt, chromaScaling, m_VPInputFmt, &pShaderCode);
	if (S_OK == hr) {
		hr = m_pDevice->CreateComputeShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pCSConvertTo444);
		pShaderCode->Release();
	}
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::UpdateConvertTo444Shader() failed with error {}", HR2Str(hr));

	return hr;
}

// One thread per pixel, from the planes to the processor's own texture.
void CDX11VideoProcessor::ConvertTo444Pass(ID3D11UnorderedAccessView* pUav)
{
	if (!m_pCSConvertTo444 || !pUav) {
		return;
	}
	// Timed whole, the prescaler with it: the statistics give the step one figure.
	m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::Prepass);
	if (m_bMpvChromaActive && FAILED(MpvChromaPass())) {
		// The prescaler brings Cb and Cr to the luma size first; latched off if it
		// cannot, and the conversion shader goes back to reading the planes.
		m_bMpvChromaFailed = true;
		UpdateConvertTo444Shader();
		UpdateStatsStatic();
	}

	// Nothing may still hold one of these planes as a render target: this pass
	// dispatches, so it sets none of its own, and Direct3D would answer such a read
	// with zeros -- a green picture, the chroma at the bottom of its range.
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	ID3D11ShaderResourceView* srvs[3] = {
		m_TexSrcVideo.pShaderResource,
		m_bMpvChromaActive ? MpvChromaPlane(0) : m_TexSrcVideo.pShaderResource2.p,
		m_bMpvChromaActive ? MpvChromaPlane(1) : m_TexSrcVideo.pShaderResource3.p
	};
	ID3D11SamplerState* samplers[2] = { m_pSamplerPoint, m_pSamplerLinear };
	ID3D11UnorderedAccessView* uavs[1] = { pUav };

	m_pDeviceContext->CSSetShader(m_pCSConvertTo444, nullptr, 0);
	m_pDeviceContext->CSSetShaderResources(0, 3, srvs);
	m_pDeviceContext->CSSetSamplers(0, 2, samplers);
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	m_pDeviceContext->Dispatch((m_srcWidth + 7) / 8, (m_srcHeight + 7) / 8, 1);

	ID3D11ShaderResourceView* none[3] = {};
	ID3D11UnorderedAccessView* noUav[1] = {};
	m_pDeviceContext->CSSetShaderResources(0, 3, none);
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
	m_pDeviceContext->CSSetShader(nullptr, nullptr, 0);
	m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::Prepass);
}

// The chroma upsampling actually in service, whoever converts the picture.
const wchar_t* CDX11VideoProcessor::ChromaScalingName() const
{
	switch (m_iChromaScaling) {
	case CHROMA_Nearest:    return L"Nearest-neighbor";
	case CHROMA_Bilinear:   return L"Bilinear";
	case CHROMA_CatmullRom: return L"Catmull-Rom";
	case CHROMA_Jinc:       return L"Jinc (EWA)";
	case CHROMA_RAVU:       return m_bMpvChromaActive ? L"RAVU-zoom" : L"Catmull-Rom";
	case CHROMA_FSRCNNX8AR: return m_bMpvChromaActive ? L"FSRCNNX 8 AR" : L"Catmull-Rom";
	}
	return L"?";
}
