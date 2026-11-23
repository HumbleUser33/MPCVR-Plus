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
	return m_bHdrCurveStep;
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
// through, so that the steps after it find it where they expect it.
HRESULT CDX11VideoProcessor::HdrCurvePass(Tex2D_t* pInputTexture, ID3D11Texture2D* pRenderTarget, const CRect& rRead, const CRect& rect)
{
	m_bHdrCurveRan = false;
	if (HdrOutput()) {
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
