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

// CDX11VideoProcessor: the HDR curve (Source/HdrCurve.h), the last change to the light of
// every picture that leaves in HDR. All of it is this fork's, and kept out of
// DX11VideoProcessor.cpp so that a merge of upstream's file meets as little of ours as
// possible (MERGING-UPSTREAM.md).

#include "stdafx.h"
#include "Helper.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"
#include "HdrCurveTune.h"
#include "Times.h"

// The picture leaves in HDR: the swap chain is PQ BT.2020, for an HDR source played as HDR
// or tone mapped to the display, and for an SDR one RTX Video HDR turns into HDR. The
// same test as the swap chain's own (InitSwapChain). HDR10+ plays as HDR10, and Dolby
// Vision's picture is PQ by the time the post-scale steps see it.
bool CDX11VideoProcessor::HdrOutput() const
{
	return m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR);
}

bool CDX11VideoProcessor::HdrCurveStepCounted() const
{
	return m_bHdrCurveStep || m_bHdrTuneStep;
}

// The constants from the settings, on this display: a preset's highlight numbers follow
// the display's peak (Display max nits, on the HDR tab).
void CDX11VideoProcessor::UpdateHdrCurve()
{
	m_HdrCurveConstants = HdrCurve::ShaderConstants(
		HdrCurve::Applied(m_iHdrCurvePreset, m_HdrCurveValues, m_iHdrDisplayMaxNits, m_iHdrCurveStrength));
}

// rRead of the input to rect of the target, through the curve. Where the picture no longer
// leaves in HDR -- the display left HDR since the steps were counted -- it is copied
// through, so that the steps after it find it where they expect it. So is it where the step
// is only there for the tune to measure what the curve receives, which it does first.
HRESULT CDX11VideoProcessor::HdrCurvePass(Tex2D_t* pInputTexture, ID3D11Texture2D* pRenderTarget, const CRect& rRead, const CRect& rect)
{
	if (m_bHdrTunePicture && HdrOutput()) {
		m_bHdrTunePicture = false;
		MeasureHdrTune(*pInputTexture, rRead);
	}
	m_bHdrCurveRan = false;
	if (m_bHdrCurveStep && HdrOutput()) {
		if (!m_pPSHdrCurve) {
			CreatePShaderFromResource(&m_pPSHdrCurve, IDF_PS_11_HDR_CURVE);
		}
		if (m_pPSHdrCurve && !m_pHdrCurveConstantBuffer) {
			D3D11_BUFFER_DESC desc = {};
			desc.ByteWidth = sizeof(HdrCurve::Constants);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			const D3D11_SUBRESOURCE_DATA data = { &m_HdrCurveConstants, 0, 0 };
			if (SUCCEEDED(m_pDevice->CreateBuffer(&desc, &data, &m_pHdrCurveConstantBuffer))) {
				m_HdrCurveUploaded = m_HdrCurveConstants;
			}
		}
		if (m_pPSHdrCurve && m_pHdrCurveConstantBuffer) {
			if (memcmp(&m_HdrCurveUploaded, &m_HdrCurveConstants, sizeof(m_HdrCurveConstants)) != 0) {
				m_pDeviceContext->UpdateSubresource(m_pHdrCurveConstantBuffer, 0, nullptr, &m_HdrCurveConstants, 0, 0);
				m_HdrCurveUploaded = m_HdrCurveConstants;
			}
			m_bHdrCurveRan = true;
			return TextureCopyRect(*pInputTexture, pRenderTarget, rRead, rect, m_pPSHdrCurve, m_pHdrCurveConstantBuffer, 0, false);
		}
	}
	return TextureCopyRect(*pInputTexture, pRenderTarget, rRead, rect, m_pPS_Simple, nullptr, 0, false);
}

// The HDR10 metadata as the display is to be told it. With the curve capping the picture,
// no peak above the cap: told the film's own, a display that tone maps from the metadata
// would compress the highlights a second time. MaxFALL is held under the cap as well.
DXGI_HDR_METADATA_HDR10 CDX11VideoProcessor::HdrMetadataToSend(const DXGI_HDR_METADATA_HDR10& hdr10)
{
	DXGI_HDR_METADATA_HDR10 sent = hdr10;
	const HdrCurve::Constants none = {};
	const HdrCurve::Constants& c = m_bHdrCurveStep ? m_HdrCurveConstants : none;
	sent.MaxMasteringLuminance     = HdrCurve::SentPeak(sent.MaxMasteringLuminance, c);
	sent.MaxContentLightLevel      = (UINT16)HdrCurve::SentPeak(sent.MaxContentLightLevel, c);
	if (sent.MaxFrameAverageLightLevel) {
		sent.MaxFrameAverageLightLevel = (UINT16)HdrCurve::SentPeak(sent.MaxFrameAverageLightLevel, c);
	}
	m_uHdrCurveSentCap = (unsigned)c.capNits;
	return sent;
}

// The tune scans a film whose picture leaves in HDR, on a GPU that can measure it.
const wchar_t* CDX11VideoProcessor::HdrTuneRefusal()
{
	if (!HdrOutput()) {
		return L"the picture does not leave in HDR";
	}
	if (!HdrMeasureSupported()) {
		return L"this GPU cannot measure the picture";
	}
	return nullptr;
}

// The tune's measurement step forced into the chain, or let go: the steps counted again.
void CDX11VideoProcessor::HdrTuneStepChanged()
{
	UpdatePostScaleTexures();
}

// The picture the curve receives, as two histograms over PQ (cs_hdr_tune_hist.hlsl), read
// back at once and handed to the tune's worker. At most once for each place the scan visits,
// so the wait on the GPU costs nothing anyone sees: the film is paused.
HRESULT CDX11VideoProcessor::MeasureHdrTune(const Tex2D_t& tex, const CRect& rect)
{
	if (!tex.pShaderResource) {
		return E_POINTER;
	}
	constexpr UINT bins = 2 * HdrCurveTune::kBins;
	HRESULT hr = S_OK;
	if (!m_pCSHdrTuneHist) {
		LPVOID data;
		DWORD size;
		hr = GetDataFromResource(data, size, IDF_CS_11_HDR_TUNE_HIST);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateComputeShader(data, size, nullptr, &m_pCSHdrTuneHist);
		}
		if (FAILED(hr)) {
			return hr;
		}
	}
	if (!m_pHdrTuneBuffer) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = bins * sizeof(UINT);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(UINT);
		hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pHdrTuneBuffer);
		if (SUCCEEDED(hr)) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = bins;
			hr = m_pDevice->CreateUnorderedAccessView(m_pHdrTuneBuffer, &uavDesc, &m_pHdrTuneUAV);
		}
		if (SUCCEEDED(hr)) {
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pHdrTuneStaging);
		}
		if (SUCCEEDED(hr)) {
			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.ByteWidth = 4 * sizeof(UINT);
			cbDesc.Usage = D3D11_USAGE_DEFAULT;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			hr = m_pDevice->CreateBuffer(&cbDesc, nullptr, &m_pHdrTuneConstants);
		}
		if (FAILED(hr)) {
			m_pHdrTuneUAV.Release();
			m_pHdrTuneBuffer.Release();
			m_pHdrTuneStaging.Release();
			m_pHdrTuneConstants.Release();
			return hr;
		}
	}

	CRect r;
	r.IntersectRect(rect, CRect(0, 0, tex.desc.Width, tex.desc.Height));
	if (r.IsRectEmpty()) {
		return S_FALSE;
	}
	const uint64_t start = GetPreciseTick();
	const UINT constants[4] = { (UINT)r.left, (UINT)r.top, (UINT)r.Width(), (UINT)r.Height() };
	m_pDeviceContext->UpdateSubresource(m_pHdrTuneConstants, 0, nullptr, constants, 0, 0);
	const UINT zeros[4] = {};
	m_pDeviceContext->ClearUnorderedAccessViewUint(m_pHdrTuneUAV, zeros);

	// The step before has just drawn into this texture: left bound as a render target, it
	// would be read as zeros (CLAUDE.md, "a compute pass sets no render target").
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11ShaderResourceView* pSRV = tex.pShaderResource;
	m_pDeviceContext->CSSetShader(m_pCSHdrTuneHist, nullptr, 0);
	m_pDeviceContext->CSSetShaderResources(0, 1, &pSRV);
	m_pDeviceContext->CSSetConstantBuffers(0, 1, &m_pHdrTuneConstants.p);
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, &m_pHdrTuneUAV.p, nullptr);
	m_pDeviceContext->Dispatch((r.Width() + 31) / 32, (r.Height() + 31) / 32, 1);
	pSRV = nullptr;
	ID3D11UnorderedAccessView* pNullUAV = nullptr;
	m_pDeviceContext->CSSetShaderResources(0, 1, &pSRV);
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, &pNullUAV, nullptr);
	m_pDeviceContext->CSSetShader(nullptr, nullptr, 0);

	m_pDeviceContext->CopyResource(m_pHdrTuneStaging, m_pHdrTuneBuffer);
	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pHdrTuneStaging, 0, D3D11_MAP_READ, 0, &mr);
	if (FAILED(hr)) {
		return hr;
	}
	HdrTunePicture picture;
	picture.hist.assign((const unsigned*)mr.pData, (const unsigned*)mr.pData + bins);
	m_pDeviceContext->Unmap(m_pHdrTuneStaging, 0);
	picture.rtFilm = m_pFilter->m_rtStartTime + m_rtStart;
	picture.ms = (double)(GetPreciseTick() - start) * 1000.0 / GetPreciseTicksPerSecond();
	picture.width = r.Width();
	picture.height = r.Height();
	m_pFilter->m_HdrTune.Deliver(std::move(picture));
	return S_OK;
}
