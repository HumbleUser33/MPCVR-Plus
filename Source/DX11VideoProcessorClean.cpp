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

// CDX11VideoProcessor: the artifact removal -- deblocking and debanding on the decoded
// planes, as the sample is copied, before anything converts or resizes the picture.
// All of it is this fork's, and kept out of DX11VideoProcessor.cpp so that a merge of
// upstream's file meets as little of ours as possible (MERGING-UPSTREAM.md).
//
// The planes are where the codec made the blocks and the bands: luma in 8 x 8 blocks at
// its own size, chroma in 8 x 8 blocks at its own, the steps in the code values it
// coded. So that is where they are taken out, each plane at its size, and whatever
// converts the picture afterwards reads the cleaned planes in place of the decoder's
// (SrcPlane): the conversion shader, the 4:4:4 pre-pass, or, for the processor keeping
// its own chroma, the planes written back into its input (CleanToVPInput). Nothing
// changes size, so nothing changes who resizes, and the chroma is upsampled after the
// cleaning by whoever upsamples it -- an upsampler that sharpens would otherwise
// sharpen the block edges first (tools/dlssnr_probe --tclean --study).

#include "stdafx.h"
#include "Helper.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"
#include "DX11VideoProcessorShared.h"
#include "SettingsPage/Greying.h"
#include "Clean/CleanParams.h"

// The page's rule, asked the same way: RTX Video Super Resolution, chosen and able to
// run, cleans the picture itself, and the two step aside (Greying.h).
bool CDX11VideoProcessor::CleaningAllowed() const
{
	Settings_t s;
	s.bUseD3D11 = true;
	s.VPFmts = m_VPFormats;
	s.bVPScaling = m_bVPScaling;
	s.iVPSuperRes = m_iVPSuperRes;
	s.bVPReplaceChroma = m_bVPReplaceChroma;
	s.bDlssSR = m_bDlssSR;
	return GreyingFor(s, IsWindows8OrGreater() != FALSE, IsWindows10OrGreater() != FALSE, m_Dovi.bValid).bCleaning;
}

// Video in planes: luma in one, chroma in one (Cb and Cr together) or two. Packed
// formats hold luma and chroma in one texture, and RGB has no codec planes at all.
bool CDX11VideoProcessor::CleanFormatOk(const FmtConvParams_t& params) const
{
	return params.CSType == CS_YUV && params.pDX11Planes
		&& params.pDX11Planes->FmtPlane2 != DXGI_FORMAT_UNKNOWN
		&& m_pDevice && m_pDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;
}

// The processor keeping its own chroma takes the cleaned planes back in its own format,
// each plane written through a render target view of its own (NV12: R8 and R8G8).
bool CDX11VideoProcessor::CleanPlaneTargetsOk(const DXGI_FORMAT fmt) const
{
	if (fmt != DXGI_FORMAT_NV12 && fmt != DXGI_FORMAT_P010 && fmt != DXGI_FORMAT_P016) {
		return false;
	}
	UINT support = 0;
	return SUCCEEDED(m_pDevice->CheckFormatSupport(fmt, &support)) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
}

HRESULT CDX11VideoProcessor::UpdateCleanShaders()
{
	if (!m_pDevice) {
		return E_POINTER;
	}
	auto create = [&](CComPtr<ID3D11ComputeShader>& cs, UINT resid) {
		if (cs) {
			return S_OK;
		}
		LPVOID data = nullptr;
		DWORD size = 0;
		HRESULT hr = GetDataFromResource(data, size, resid);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateComputeShader(data, size, nullptr, &cs);
		}
		return hr;
	};
	// The deblocking of the method chosen, and only that one; the debanding.
	HRESULT hr = S_OK;
	if (m_iDeblock == DEBLOCK_SPP) {
		hr = create(m_pCSDeblock[1][0], IDF_CS_11_DEBLOCK_SPP1);
		if (SUCCEEDED(hr)) hr = create(m_pCSDeblock[1][1], IDF_CS_11_DEBLOCK_SPP2);
	} else if (m_iDeblock == DEBLOCK_PP7) {
		hr = create(m_pCSDeblock[0][0], IDF_CS_11_DEBLOCK1);
		if (SUCCEEDED(hr)) hr = create(m_pCSDeblock[0][1], IDF_CS_11_DEBLOCK2);
	}
	if (SUCCEEDED(hr) && m_bDeband) {
		hr = create(m_pCSDeband[0], IDF_CS_11_DEBAND1);
		if (SUCCEEDED(hr)) hr = create(m_pCSDeband[1], IDF_CS_11_DEBAND2);
	}
	if (SUCCEEDED(hr) && !m_pCleanConstants) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = sizeof(Clean::Constants);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pCleanConstants);
	}
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::UpdateCleanShaders() failed with error {}", HR2Str(hr));
	return hr;
}

void CDX11VideoProcessor::ReleaseClean(const bool bShaders)
{
	for (int i = 0; i < 3; i++) {
		for (CleanPlane* set : { m_CleanA, m_CleanB }) {
			set[i].tex.Release();
			set[i].pUav.Release();
		}
	}
	m_CleanTargets.clear();
	m_iCleanResult = 0;
	if (bShaders) {
		for (int c = 0; c < 2; c++) {
			m_pCSDeblock[0][c].Release();
			m_pCSDeblock[1][c].Release();
			m_pCSDeband[c].Release();
		}
		m_pCleanConstants.Release();
	}
}

// Whether this media type is cleaned, and how; called from InitMediaType once it knows
// which processor took the picture.
void CDX11VideoProcessor::UpdateClean()
{
	ReleaseClean(false);
	if (!m_D3D11VP.IsReady()) {
		m_bCleanVP = false;   // the shaders took the picture after all
	}
	const bool bWanted = (m_iDeblock != DEBLOCK_Off || m_bDeband) && CleaningAllowed()
		&& CleanFormatOk(m_srcParams) && !(m_bInterlaced && m_bInterlacedSeen);
	m_bCleanActive = bWanted
		&& (!m_D3D11VP.IsReady() || m_bChromaReplacedVP || m_bCleanVP)
		&& SUCCEEDED(UpdateCleanShaders());
	if (!m_bCleanActive) {
		m_bCleanVP = false;
	}
	UpdateCleanStatus();
}

void CDX11VideoProcessor::UpdateCleanStatus()
{
	if (m_iDeblock == DEBLOCK_Off && !m_bDeband) {
		m_strCleanStatus.clear();
		return;
	}
	std::wstring what;
	if (m_iDeblock != DEBLOCK_Off) {
		what = std::format(L"deblock {} {}", m_iDeblock == DEBLOCK_SPP ? L"SPP" : L"PP7", m_iDeblockLevel);
	}
	if (m_bDeband) {
		what += std::format(L"{}deband {}", what.empty() ? L"" : L", ", m_iDebandLevel);
	}

	if (m_bCleanActive) {
		m_strCleanStatus = what + (m_bCleanVP ? L", back into the processor's planes" : L", on the planes");
		return;
	}
	const wchar_t* why = L"not applied";
	if (!CleaningAllowed()) {
		why = L"left to RTX Video Super Resolution";
	} else if (m_srcParams.CSType != CS_YUV) {
		why = L"not on RGB";
	} else if (!m_srcParams.pDX11Planes || m_srcParams.pDX11Planes->FmtPlane2 == DXGI_FORMAT_UNKNOWN) {
		why = L"not in packed formats";
	} else if (m_bInterlaced && m_bInterlacedSeen) {
		why = L"not on interlaced pictures";
	} else if (m_bCleanFailed) {
		why = L"the processor's planes take no render target here";
	}
	m_strCleanStatus = what + L": " + why;
}

// One plane of the source as the cleaning reads it, and its size.
void CDX11VideoProcessor::CleanPlaneInfo(const int i, UINT& w, UINT& h, int& channels) const
{
	const DX11PlaneConfig_t* planes = m_srcParams.pDX11Planes;
	w = m_TexSrcVideo.desc.Width;
	h = m_TexSrcVideo.desc.Height;
	channels = 1;
	if (i > 0) {
		w /= planes->div_chroma_w;
		h /= planes->div_chroma_h;
		// The second plane holds Cb and Cr together when there is no third.
		channels = (planes->FmtPlane3 == DXGI_FORMAT_UNKNOWN) ? 2 : 1;
	}
}

int CDX11VideoProcessor::CleanPlaneCount() const
{
	const DX11PlaneConfig_t* planes = m_srcParams.pDX11Planes;
	return !planes ? 0 : planes->FmtPlane3 != DXGI_FORMAT_UNKNOWN ? 3 : 2;
}

ID3D11ShaderResourceView* CDX11VideoProcessor::SrcPlane(const int i) const
{
	if (m_iCleanResult) {
		const CleanPlane* set = (m_iCleanResult == 2) ? m_CleanB : m_CleanA;
		if (set[i].tex.pShaderResource) {
			return set[i].tex.pShaderResource;
		}
	}
	return i == 0 ? m_TexSrcVideo.pShaderResource.p : i == 1 ? m_TexSrcVideo.pShaderResource2.p : m_TexSrcVideo.pShaderResource3.p;
}

// What the planes of a set are written into: 16 bits whatever the source, so that the
// smoothing keeps what it computed until the conversion reads it.
HRESULT CDX11VideoProcessor::CleanTargets(CleanPlane* set)
{
	const int count = CleanPlaneCount();
	for (int i = 0; i < count; i++) {
		UINT w, h;
		int channels;
		CleanPlaneInfo(i, w, h, channels);
		const DXGI_FORMAT fmt = channels == 2 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
		if (set[i].tex.pTexture && set[i].tex.desc.Width == w && set[i].tex.desc.Height == h && set[i].tex.desc.Format == fmt) {
			continue;
		}
		set[i].pUav.Release();
		HRESULT hr = set[i].tex.Create(m_pDevice, fmt, w, h, Tex2D_DefaultShaderRTargetUAV);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateUnorderedAccessView(set[i].tex.pTexture, nullptr, &set[i].pUav);
		}
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::CleanTargets() : plane {} failed with error {}", i, HR2Str(hr));
			return hr;
		}
		set[i].channels = channels;
	}
	return S_OK;
}

// The picture in m_TexSrcVideo, cleaned into the sets: the decoder's planes, deblocked
// into A, debanded into B. The last pass dithers to the processor's depth when the
// planes are to be written back into its input; the shaders read them at 16 bits.
void CDX11VideoProcessor::CleanRun()
{
	m_iCleanResult = 0;
	const bool bDeblock = m_iDeblock != DEBLOCK_Off;
	CComPtr<ID3D11ComputeShader>* const deblock = m_pCSDeblock[m_iDeblock == DEBLOCK_SPP ? 1 : 0];
	if (!m_bCleanActive || !m_pCleanConstants || (bDeblock && !deblock[0]) || (m_bDeband && !m_pCSDeband[0])) {
		return;
	}
	if ((bDeblock && FAILED(CleanTargets(m_CleanA))) || (m_bDeband && FAILED(CleanTargets(m_CleanB)))) {
		return;
	}

	// Nothing may still hold a plane as a render target: these passes dispatch, so they
	// set none of their own, and Direct3D would answer the read with zeros.
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	const bool bLimited = m_srcExFmt.NominalRange != DXVA2_NominalRange_0_255;
	const bool bDeep = m_srcParams.CDepth > 8;
	// Grain fades out towards black for luma and towards the middle for chroma.
	const float black = bLimited ? (bDeep ? 64.0f / 1023.0f : 16.0f / 255.0f) : 0.0f;
	const float middle = bDeep ? 512.0f / 1023.0f : 128.0f / 255.0f;
	// The step the processor's input keeps: NV12 eight bits, P010 ten in the top of 16.
	float step = 0.0f;
	if (m_bCleanVP) {
		step = (m_VPInputFmt == DXGI_FORMAT_NV12) ? 1.0f / 255.0f
			: (m_VPInputFmt == DXGI_FORMAT_P010) ? 64.0f / 65535.0f : 0.0f;
	}

	const int count = CleanPlaneCount();
	ID3D11ShaderResourceView* in[3] = { SrcPlane(0), SrcPlane(1), SrcPlane(2) };

	auto dispatch = [&](ID3D11ComputeShader* cs, const Clean::Constants& c, ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav) {
		D3D11_MAPPED_SUBRESOURCE mr;
		if (FAILED(m_pDeviceContext->Map(m_pCleanConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			return;
		}
		memcpy(mr.pData, &c, sizeof(c));
		m_pDeviceContext->Unmap(m_pCleanConstants, 0);
		m_pDeviceContext->CSSetShader(cs, nullptr, 0);
		m_pDeviceContext->CSSetShaderResources(0, 1, &srv);
		m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		m_pDeviceContext->CSSetConstantBuffers(0, 1, &m_pCleanConstants.p);
		m_pDeviceContext->Dispatch((c.width + 15) / 16, (c.height + 15) / 16, 1);
		// A plane written here is read by the next pass: unbound before it is.
		ID3D11ShaderResourceView* noSrv = nullptr;
		ID3D11UnorderedAccessView* noUav = nullptr;
		m_pDeviceContext->CSSetShaderResources(0, 1, &noSrv);
		m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
	};

	if (bDeblock) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::Deblock);
		for (int i = 0; i < count; i++) {
			const CleanPlane& p = m_CleanA[i];
			const Clean::Constants c = Clean::DeblockConstants(m_iDeblock, m_iDeblockLevel, p.tex.desc.Width, p.tex.desc.Height,
				m_bDeband ? 0.0f : step);
			dispatch(deblock[p.channels - 1], c, in[i], p.pUav);
			in[i] = p.tex.pShaderResource;
		}
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::Deblock);
		m_iCleanResult = 1;
	}
	if (m_bDeband) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::Deband);
		for (int i = 0; i < count; i++) {
			const CleanPlane& p = m_CleanB[i];
			const Clean::Constants c = Clean::DebandConstants(m_iDebandLevel, p.tex.desc.Width, p.tex.desc.Height,
				i == 0 ? black : middle, m_uCleanSeed, step);
			dispatch(m_pCSDeband[p.channels - 1], c, in[i], p.pUav);
		}
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::Deband);
		m_iCleanResult = 2;
	}
	m_pDeviceContext->CSSetShader(nullptr, nullptr, 0);
}

// As the sample is copied: a progressive picture is cleaned, an interlaced one is not
// (a woven 8 x 8 block holds two fields, and the processor needs them as they came).
void CDX11VideoProcessor::CleanPass(IMediaSample* pSample)
{
	m_iCleanResult = 0;
	if (!m_bCleanActive || m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) {
		return;
	}
	// The grain's seed is the picture's own time, so that the same picture always gets
	// the same grain: a redraw, a step or a pause does not make it shimmer.
	REFERENCE_TIME rtStart = 0, rtEnd = 0;
	if (pSample && SUCCEEDED(pSample->GetTime(&rtStart, &rtEnd))) {
		m_uCleanSeed = (uint32_t)(rtStart / 10000) ^ (uint32_t)((uint64_t)rtStart >> 32);
	} else {
		m_uCleanSeed++;
	}
	CleanRun();
}

// The cleaned planes into the processor's next input, a plane at a time, each through
// a render target view of its own; the picture as it came when it was not cleaned.
HRESULT CDX11VideoProcessor::CleanToVPInput(ID3D11Texture2D* pInput)
{
	if (!pInput) {
		return E_POINTER;
	}
	if (!m_iCleanResult) {
		m_pDeviceContext->CopyResource(pInput, m_TexSrcVideo.pTexture);
		return S_OK;
	}

	auto targets = std::find_if(m_CleanTargets.begin(), m_CleanTargets.end(),
		[&](const CleanTarget& t) { return t.pTexture == pInput; });
	if (targets == m_CleanTargets.end()) {
		CleanTarget made;
		made.pTexture = pInput;
		const bool b8 = m_VPInputFmt == DXGI_FORMAT_NV12;
		D3D11_RENDER_TARGET_VIEW_DESC desc = {};
		desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		desc.Format = b8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
		HRESULT hr = m_pDevice->CreateRenderTargetView(pInput, &desc, &made.pLuma);
		if (SUCCEEDED(hr)) {
			desc.Format = b8 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
			hr = m_pDevice->CreateRenderTargetView(pInput, &desc, &made.pChroma);
		}
		if (FAILED(hr)) {
			// Latched off: the processor goes back to reading the decoder's picture in
			// place, from the next media type on, and the statistics say why.
			DLog(L"CDX11VideoProcessor::CleanToVPInput() : no render target on a plane ({})", HR2Str(hr));
			m_bCleanFailed = true;
			m_pDeviceContext->CopyResource(pInput, m_TexSrcVideo.pTexture);
			UpdateCleanStatus();
			return hr;
		}
		m_CleanTargets.push_back(made);
		targets = m_CleanTargets.end() - 1;
	}

	const CleanPlane* set = (m_iCleanResult == 2) ? m_CleanB : m_CleanA;
	ID3D11RenderTargetView* views[2] = { targets->pLuma, targets->pChroma };
	for (int i = 0; i < 2; i++) {
		const UINT w = set[i].tex.desc.Width, h = set[i].tex.desc.Height;
		const CRect rect(0, 0, w, h);
		HRESULT hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, w, h, rect, 0, false);
		if (FAILED(hr)) {
			return hr;
		}
		D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)w, (FLOAT)h, 0, 1 };
		TextureBlt11(m_pDeviceContext, views[i], viewport, m_pVSimpleInputLayout, m_pVS_Simple, m_pPS_Simple,
			set[i].tex.pShaderResource, m_pSamplerPoint, nullptr, m_pVertexBuffer);
	}
	// The processor reads what was just drawn into.
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11ShaderResourceView* noViews[1] = {};
	m_pDeviceContext->PSSetShaderResources(0, 1, noViews);
	return S_OK;
}

// A level changed while a picture is on the screen: the same picture cleaned again,
// and written where the converter reads it, so that a paused redraw shows the change.
void CDX11VideoProcessor::CleanAgain()
{
	if (!m_bCleanActive || !m_TexSrcVideo.pTexture || m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) {
		return;
	}
	CleanRun();
	if (m_D3D11VP.IsReady()) {
		if (m_bChromaReplacedVP) {
			ConvertTo444Pass(m_D3D11VP.GetInputUav());
		} else if (m_bCleanVP) {
			// The newest texture of the processor's ring, the one it shows now.
			CleanToVPInput(m_D3D11VP.HoldFrame().pTexture);
		}
	}
}
