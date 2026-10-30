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

// CDX11VideoProcessor: DLSS 5 NR, DLSS Super Resolution and the video processor's second turn.
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

std::wstring CDX11VideoProcessor::GetDlssStatus()
{
	if (!m_bDlssNR) {
		return {};
	}
	if (!DlssNRSupportedHere()) {
		return L"not available on this adapter";
	}
	// The page cannot show the statistics, so it says which motion the stabilizer
	// really uses: Optical Flow may have given way to the detector.
	std::wstring status = m_DlssNR.GetStatusLine();
	if (m_iDlssNRStabilizer > 0 && m_DlssStabilizer.IsCreated()) {
		status += L"; stabilizer: " + m_DlssStabilizer.GetStatusLine();
	}
	return status;
}

bool CDX11VideoProcessor::DlssNRSupportedHere() const
{
#ifndef _WIN64
	return false; // the snippet is x64 only
#else
	return m_pDevice
		&& m_VendorId == PCIV_NVIDIA
		&& m_FeatureLevel >= D3D_FEATURE_LEVEL_11_0
		&& m_bDlssNRUavOk;
#endif
}

// Load or unload the NGX session to match the current setting. Cheap to call:
// it returns immediately when nothing needs to change.
void CDX11VideoProcessor::UpdateDlssNR()
{
	const bool bWanted = m_bDlssNR && DlssNRSupportedHere();

	if (!bWanted) {
		// Only drop the feature and its half-gigabyte of allocations. Unloading
		// the 165 MB snippet and rebuilding the D3D12 session on every toggle is
		// what stalled the picture for seconds; the session is torn down when
		// the device goes away or the DLL path changes, not here.
		m_DlssNR.ReleaseFeature();
		m_bDlssNRActive = false;
		return;
	}

	// A removed device leaves the session unusable; drop it so this rebuilds
	// rather than failing for the rest of the run.
	if (m_DlssNR.GetState() == CDlssNR::State::DeviceLost) {
		m_DlssNR.Shutdown();
	}
	if (!m_DlssNR.IsInitialised()) {
		// The architecture override is what lets a pre-Blackwell card past the
		// snippet's own adapter check; without it Init refuses outright.
		m_DlssNR.Init(m_pDevice, m_strDlssNRDllPath.c_str(), true);
	}
	m_bDlssNRActive = m_DlssNR.IsInitialised();
}

// A session that failed while the film played, started again from nothing once its
// time has come: the libraries, the device, the feature. Called between two pictures.
// What the session works on has not changed -- the video processor was never told
// DLSS had stopped, and its second turn went on resizing -- so there is nothing else
// to rebuild.
void CDX11VideoProcessor::RecoverDlssNR()
{
	if (!m_tickDlssNRRecover || GetPreciseTick() < m_tickDlssNRRecover) {
		return;
	}
	m_tickDlssNRRecover = 0;
	if (!m_bDlssNR || m_bDlssNRActive) {
		return;
	}
	DLog(L"CDX11VideoProcessor::RecoverDlssNR() : attempt {} of {}", m_nDlssNRRecoveries, kDlssNRRecoveries);
	ReleaseDlssNRTextures(true);
	m_DlssNR.Shutdown();
	UpdateDlssNR();
	// Back or not, the textures follow: with the network back they are its own again,
	// and with it gone the processor's first turn takes the resizing its second had.
	UpdateTexures();
	if (m_bDlssNRActive) {
		m_bDlssNewPicture = true;
	}
	UpdateStatsStatic();
}

// One neural-rendering pass at source resolution. On any failure the caller's
// input texture is left untouched and the chain runs exactly as before.
HRESULT CDX11VideoProcessor::DlssNRPass(Tex2D_t* pInputTexture, const CRect& rSrc, Tex2D_t** ppResult)
{
	if (!pInputTexture || !ppResult || !m_TexDlssIn.pTexture || !m_TexDlssOut.pTexture
			|| !m_DlssNR.IsFeatureReady()) {
		return E_FAIL;
	}

	// The network's working size, and the picture's: the same unless the working
	// size is set below 100.
	const UINT w = m_TexDlssOut.desc.Width;
	const UINT h = m_TexDlssOut.desc.Height;
	const UINT pw = (UINT)m_DlssNRPictureSize.cx;
	const UINT ph = (UINT)m_DlssNRPictureSize.cy;
	const bool bReduce = m_DlssCompose.IsCreated();

	// The pass must never change the geometry. If the region we were handed is
	// not exactly the picture's size, blitting it would rescale the picture; skip
	// instead and let UpdateTexures catch up on the next frame.
	if ((UINT)rSrc.Width() != pw || (UINT)rSrc.Height() != ph) {
		DLogIf(m_bDlssNRActive, L"CDX11VideoProcessor::DlssNRPass() : {}x{} source into {}x{} textures, skipping",
			rSrc.Width(), rSrc.Height(), pw, ph);
		return E_FAIL;
	}

	// Into the shared RGBA16F input. This blit is unconditional: the snippet
	// reads the texture from a separate D3D12 device, and m_TexConvertOutput
	// carries no share handle whatever its format.
	//
	// At a working size below the picture's, the reduction writes it instead. The
	// reduction and the composition read the picture as a whole texture, which it
	// is not when it is a part of a larger one -- after the resize, a letterboxed
	// film -- so it is copied out first.
	HRESULT hr = S_OK;
	const Tex2D_t* pPicture = pInputTexture;
	if (bReduce) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::NRReduce);
		if (rSrc.left || rSrc.top || pInputTexture->desc.Width != pw || pInputTexture->desc.Height != ph) {
			hr = m_TexDlssPicture.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, pw, ph, Tex2D_DefaultShaderRTarget);
			if (SUCCEEDED(hr)) {
				hr = TextureCopyRect(*pInputTexture, m_TexDlssPicture.pTexture, rSrc, CRect(0, 0, pw, ph), m_pPS_Simple, nullptr, 0, false);
			}
			pPicture = &m_TexDlssPicture;
		}
		if (SUCCEEDED(hr)) {
			m_DlssCompose.Reduce(m_pDeviceContext, pPicture->pShaderResource, m_pDlssInTarget);
		}
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::NRReduce);
	} else {
		hr = TextureCopyRect(*pInputTexture, m_TexDlssIn.pTexture, rSrc, CRect(0, 0, w, h), m_pPS_Simple, nullptr, 0, false);
	}
	if (FAILED(hr)) {
		return hr;
	}

	// The stabilizer steadies the network's effect after it runs; DLSSNR.ControlMask
	// strips the effect instead (tools/dlssnr_probe --teffect). Motion is measured
	// on new pictures only, and a redraw is steadied against the same history again.
	bool bStabilize = false;
	if (m_iDlssNRStabilizer > 0) {
		const CDlssStabilizer::FlowSettings flow = DlssNRFlow();
		if (!m_DlssStabilizer.Matches(w, h, false, flow)) {
			const bool bWasCreated = m_DlssStabilizer.IsCreated();
			if (m_DlssNR.HasMotionVectors()) {
				m_DlssNR.SetGuides(CDlssNR::Guides{}); // before the old vectors go away
			}
			const HRESULT hrStab = m_DlssStabilizer.Create(m_pDevice, m_pDeviceContext, w, h,
				m_pVSimpleInputLayout, m_pVS_Simple, m_pSamplerPoint, m_pSamplerLinear, false, flow);
			if (SUCCEEDED(hrStab)) {
				m_bDlssNewPicture = true; // a new stabilizer has to see this picture
			}
			if (SUCCEEDED(hrStab) || bWasCreated) {
				UpdateStatsStatic();
			}
		}
		bStabilize = m_DlssStabilizer.IsCreated();
	} else if (m_DlssStabilizer.IsCreated()) {
		ReleaseDlssNRTextures(false);
		UpdateStatsStatic();
	}

	const bool bNewPicture = m_bDlssNewPicture;
	m_bDlssNewPicture = false;
	if (bStabilize && bNewPicture) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::NRMotion);
		m_DlssStabilizer.PrepareMotion(m_pDeviceContext, m_TexDlssIn.pShaderResource);
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::NRMotion);
	}

	// The stabilizer's Optical Flow reaches the network as well, its vectors cleaned
	// the way DLSS SR's are (CDlssStabilizer::ForDlssNR): steadier than without them,
	// and nothing lost behind what moves (--tstab). It used to take a box on the page.
	ID3D11Texture2D* pVectors = bStabilize ? m_DlssStabilizer.GetMotionVectors() : nullptr;
	if ((pVectors != nullptr) != m_DlssNR.HasMotionVectors()) {
		CDlssNR::Guides guides;
		guides.pMVec = pVectors;
		m_DlssNR.SetGuides(guides);
	}

	// Unbind before handing the textures to the other device.
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	m_pDeviceContext->PSSetShaderResources(0, std::size(nullSRVs), nullSRVs);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	if (!m_DlssNR.Evaluate(m_DlssParams)) {
		// Off rather than failing on every picture, and given another go a moment
		// later (RecoverDlssNR): one evaluation the GPU did not answer in time used to
		// leave the rest of the film without it.
		m_bDlssNRActive = false;
		if (m_nDlssNRRecoveries < kDlssNRRecoveries) {
			m_nDlssNRRecoveries++;
			m_tickDlssNRRecover = GetPreciseTick() + 2 * GetPreciseTicksPerSecondI();
		}
		UpdateStatsStatic();
		return E_FAIL;
	}
	// The network's own GPU time, on its queue: the picture before's, which the queue is
	// done with by now. Nothing here waits for it.
	const CDlssNR::Timing& timing = m_DlssNR.LastTiming();
	if (timing.gpuMs >= 0 && timing.gpuSerial != m_uDlssNRTimed) {
		m_uDlssNRTimed = timing.gpuSerial;
		m_DlssNRTimes.Add(timing.gpuMs);
	}
	m_traceNRTiming = timing;
	m_tracePasses |= CPacingTrace::DlssNR | (m_DlssNR.HasMotionVectors() ? CPacingTrace::NRVectors : 0)
		| ((bStabilize && m_DlssStabilizer.HasFlow()) ? CPacingTrace::NRFlow : 0);

	if (bStabilize) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::NRStabilize);
		m_DlssStabilizer.Stabilize(m_pDeviceContext, m_TexDlssIn.pShaderResource, m_TexDlssOut.pShaderResource,
			(float)m_iDlssNRStabilizer / DLSSNR_STAB_MAX, bNewPicture);
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::NRStabilize);
		*ppResult = m_DlssStabilizer.GetResult();
	} else {
		*ppResult = &m_TexDlssOut;
	}

	// What the network and the stabilizer changed, carried back onto the picture at
	// its own size: it keeps its own detail and grain (tools/dlssnr_probe --tnrscale).
	if (bReduce) {
		m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::NRCompose);
		m_DlssCompose.Compose(m_pDeviceContext, pPicture->pShaderResource, m_TexDlssIn.pShaderResource,
			(*ppResult)->pShaderResource);
		m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::NRCompose);
		*ppResult = m_DlssCompose.GetResult();
	}
	return S_OK;
}

// Everything DLSS 5 NR holds that is the size of the picture: the feature and its
// guides first, which are open on the other device, then what they were made of.
// bFeature false leaves the feature for a stabilizer that alone is being replaced.
void CDX11VideoProcessor::ReleaseDlssNRTextures(const bool bFeature)
{
	m_DlssNR.SetGuides(CDlssNR::Guides{}); // before the shared motion vectors go away
	m_DlssStabilizer.Release();
	if (bFeature) {
		ReleaseVPScale();   // it enlarges what the network leaves
		m_DlssNR.ReleaseFeature();
		m_DlssCompose.Release();
		m_pDlssInTarget.Release();
		m_TexDlssPicture.Release();
		m_TexDlssIn.Release();
		m_TexDlssOut.Release();
		m_DlssNRPictureSize = {};
	}
}

std::wstring CDX11VideoProcessor::GetDlssSRStatus()
{
	if (!m_bDlssSR) {
		return {};
	}
	if (!DlssSRSupportedHere()) {
		return L"not available on this adapter";
	}
	return m_DlssSR.GetStatusLine();
}

bool CDX11VideoProcessor::DlssSRSupportedHere() const
{
#ifndef _WIN64
	return false; // the NGX runtime is x64 only
#else
	// NGX writes the upscaled picture through a typed UAV, as for DLSS 5 NR.
	return m_pDevice
		&& m_VendorId == PCIV_NVIDIA
		&& m_FeatureLevel >= D3D_FEATURE_LEVEL_11_0
		&& m_bDlssNRUavOk;
#endif
}

// Load or drop the DLSS Super Resolution session to match the setting. Toggling
// only drops the feature; the session goes with the device or another DLL.
void CDX11VideoProcessor::UpdateDlssSR()
{
	if (!m_bDlssSR || !DlssSRSupportedHere()) {
		m_DlssSR.ReleaseFeature();
		m_DlssSRMotion.Release();
		m_strDlssSRMotion.clear();
		m_bDlssSRActive = false;
		return;
	}
	if (!m_DlssSR.IsInitialised()) {
		m_DlssSR.Init(m_pDevice, m_strDlssSRDllPath.c_str());
	}
	m_bDlssSRActive = m_DlssSR.IsInitialised();
}

HRESULT CDX11VideoProcessor::DlssSRPass(Tex2D_t* pInputTexture, const CRect& rSrc, const CRect& dstRect, const int rotation, Tex2D_t** ppResult)
{
	if (!pInputTexture || !ppResult) {
		return E_POINTER;
	}

	// Sizes before rotation: ResizeShaderPass turns the picture afterwards.
	const bool bTurned = (rotation == 90 || rotation == 270);
	const UINT inW = rSrc.Width(), inH = rSrc.Height();
	UINT outW = bTurned ? dstRect.Height() : dstRect.Width();
	UINT outH = bTurned ? dstRect.Width() : dstRect.Height();
	// DLAA: the picture is already the size it will be shown at, and DLSS runs on it
	// anyway -- same network, same cost, nothing enlarged. CDlssSR::QualityModeFor
	// already asks NGX for NGX_PERFQUALITY_DLAA at a ratio of one; what was missing
	// was letting the pass get that far.
	const bool bDlaa = m_bDlssSRDlaa && outW == inW && outH == inH;
	if (!inW || !inH || (!bDlaa && (outW <= inW || outH <= inH))) {
		return S_FALSE;   // DLSS enlarges, or holds the size; the downscalers do the rest
	}

	if (!bDlaa) {
		// Past four times the source the output stops there, and the resize shaders
		// cover what is left (measured: 4.5x still runs, but nothing films need).
		const double reach = std::min({ 4.0 * inW / outW, 4.0 * inH / outH, 7680.0 / outW, 4320.0 / outH, 1.0 });
		outW = std::max<UINT>(inW + 1, (UINT)std::lround(outW * reach));
		outH = std::max<UINT>(inH + 1, (UINT)std::lround(outH * reach));
	}

	const unsigned preset = (unsigned)m_iDlssSRPreset;
	if (!m_DlssSR.MatchesFeature(inW, inH, outW, outH, preset)) {
		if (m_DlssSR.IsRefused(inW, inH, outW, outH, preset)) {
			return S_FALSE;   // failed before for these sizes: the resize shaders do the job
		}
		if (!m_DlssSR.CreateFeature(inW, inH, outW, outH, preset)) {
			UpdateStatsStatic();
			return S_FALSE;
		}
		m_bDlssSRNewPicture = true;   // a new feature starts its history on this picture
		m_DlssSRMotion.Reset();
		UpdateScalingStrings();
		UpdateStatsStatic();
	}

	Tex2D_t* pInput = m_DlssSR.GetInput();
	HRESULT hr = TextureCopyRect(*pInputTexture, pInput->pTexture, rSrc, CRect(0, 0, inW, inH), m_pPS_Simple, nullptr, 0, false);
	if (FAILED(hr)) {
		return hr;
	}

	// Motion only for a new picture: a redraw shows the same one, which has not
	// moved. Optical Flow of its own, about 540 lines, on the very picture DLSS
	// enlarges -- after DLSS 5 NR when that runs, so on a cleaner one -- each block
	// drawn to the picture's global motion where it lies within the noise of it
	// (CDlssStabilizer::ForDlssSR): a still background gets the motion of the whole
	// picture, none or the film's own weave, and a pan the pan. DLSS 5 NR's raw
	// vectors are no longer lent: they made still backgrounds shimmer and left the
	// grain along the frame's edges (--tsrstill). Finer flow -- the source size, a
	// vector per pixel, the slowest search -- measured no better, on still pictures
	// (--tsrq) as on films (--tsrclip), for 3 ms more.
	std::wstring motion = L"no vectors";
	ID3D11Texture2D* pMotion = nullptr;
	const bool bNewPicture = m_bDlssSRNewPicture;
	m_bDlssSRNewPicture = false;
	if (bNewPicture) {
		const CDlssStabilizer::FlowSettings flow = CDlssStabilizer::ForDlssSR();
		if (!m_DlssSRMotion.Matches(inW, inH, true, flow)) {
			m_DlssSRMotion.Create(m_pDevice, m_pDeviceContext, inW, inH,
				m_pVSimpleInputLayout, m_pVS_Simple, m_pSamplerPoint, m_pSamplerLinear, true, flow);
		}
		if (m_DlssSRMotion.IsCreated()) {
			m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::SRMotion);
			m_DlssSRMotion.PrepareMotion(m_pDeviceContext, pInput->pShaderResource);
			m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::SRMotion);
			if (m_PacingTrace.Enabled() && m_DlssSRMotion.CutMeasured()) {
				m_SRCutReadback.Copy(m_pDevice, m_pDeviceContext, m_DlssSRMotion.GetCutState(), m_rtStart);
			}
			pMotion = m_DlssSRMotion.GetMotionVectors();
			motion = pMotion ? std::wstring(L"Optical Flow, global motion") : m_DlssSRMotion.GetStatusLine();
		}
		if (motion != m_strDlssSRMotion) {
			m_strDlssSRMotion = motion;
			UpdateStatsStatic();
		}
	}

	// NGX binds the textures on its own pipeline state; nothing of the renderer's
	// may still hold them.
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	m_pDeviceContext->PSSetShaderResources(0, std::size(nullSRVs), nullSRVs);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	const float frameMs = (m_rtAvgTimePerFrame > 0) ? (float)(m_rtAvgTimePerFrame / 10000.0) : 41.7f;
	m_DlssStageTimes.Begin(m_pDeviceContext, CGpuStageTimes::SR);
	const bool bEvaluated = m_DlssSR.Evaluate(pMotion, frameMs);
	m_DlssStageTimes.End(m_pDeviceContext, CGpuStageTimes::SR);
	if (!bEvaluated) {
		// Latch off rather than failing on every picture.
		m_bDlssSRActive = false;
		UpdateScalingStrings();
		UpdateStatsStatic();
		return E_FAIL;
	}

	m_tracePasses |= CPacingTrace::DlssSR;
	*ppResult = m_DlssSR.GetOutput();
	return S_OK;
}

void CDX11VideoProcessor::ReleaseVPScale()
{
	m_VPScalePlanes.clear();
	m_D3D11VPScale.ReleaseVideoProcessor();
	m_TexVPScaleOut.Release();
	m_pPSRgbToLuma.Release();
	m_pPSRgbToChroma.Release();
	m_pCSRgbToPacked.Release();
	m_VPScaleKey = {};
	m_VPScaleInputFmt = DXGI_FORMAT_UNKNOWN;
	m_bVPScaleSuperRes = false;
	m_bVPScaleFailed = false;
	m_bVPScaled = false;
}

// The video processor's second turn, made for the picture DLSS 5 NR leaves.
//
// "Use the video processor for resizing" says the processor resizes, and with DLSS 5 NR
// on it did not: the network needs the picture at the source's size, the processor
// comes before it, and the shaders enlarged afterwards whatever the box said. So the
// processor is given a second turn. The network's R'G'B' is written back as the
// Y'CbCr a processor takes (GetShaderRgbToVideo) and enlarged by it, in the space the
// first turn left it in -- PQ BT.2020 or SDR -- so that nothing is converted but the
// matrix, there and back.
//
// As P010 when Super Resolution is asked for, since the driver only acts on 4:2:0;
// as Y410 otherwise, and whenever the chroma was asked to be kept from the processor.
// Measured on film frames (vp444_probe --after): the second trip costs 2.2 to 3.1 dB
// in P010, which is 4:2:0 met twice, and under half a decibel in Y410.
void CDX11VideoProcessor::UpdateVPScale()
{
	const UINT w = m_TexDlssOut.pTexture ? (UINT)m_DlssNRPictureSize.cx : 0;
	const UINT h = m_TexDlssOut.pTexture ? (UINT)m_DlssNRPictureSize.cy : 0;
	const UINT outW = m_videoRect.Width(), outH = m_videoRect.Height();
	// A picture already the size it is shown at is left alone: there is nothing to
	// resize, and a second turn would only take it through Y'CbCr and back.
	if (!VPResizesAfterNR() || !m_D3D11VP.IsReady() || !m_D3D11VPScale.IsVideoDeviceOk()
			|| !w || !h || !outW || !outH || (w == outW && h == outH) || !m_DlssNR.IsFeatureReady()) {
		ReleaseVPScale();
		return;
	}

	VPScaleKey key;
	key.w = w;
	key.h = h;
	key.superRes = VPSuperResAllowed(m_srcParams.CDepth) ? m_iVPSuperRes : SUPERRES_Disable;
	key.bPq = m_D3D11VP.IsPqSupported() != FALSE;
	key.bKeepChroma = m_bVPReplaceChroma;
	key.outFmt = m_D3D11OutputFmt;
	if (key == m_VPScaleKey) {
		if (!m_bVPScaleFailed
				&& FAILED(m_TexVPScaleOut.CheckCreate(m_pDevice, m_TexVPScaleOut.desc.Format, outW, outH, Tex2D_DefaultShaderRTarget))) {
			DLog(L"CDX11VideoProcessor::UpdateVPScale() : no {}x{} texture to enlarge into", outW, outH);
			m_bVPScaleFailed = true;
		}
		return;   // made for this already, or refused for it: not tried again on every resize
	}

	ReleaseVPScale();
	m_VPScaleKey = key;
	m_bVPScaleFailed = true;   // until everything below has been made

	// What the first turn leaves, said as the Y'CbCr it is written back in.
	DXVA2_ExtendedFormat exFmt = {};
	exFmt.SampleFormat           = DXVA2_SampleProgressiveFrame;
	exFmt.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG2;
	exFmt.NominalRange           = DXVA2_NominalRange_16_235;
	exFmt.VideoLighting          = DXVA2_VideoLighting_dim;
	if (key.bPq) {
		exFmt.VideoTransferMatrix   = (DXVA2_VideoTransferMatrix)MFVideoTransferMatrix_BT2020_10;
		exFmt.VideoPrimaries        = (DXVA2_VideoPrimaries)MFVideoPrimaries_BT2020;
		exFmt.VideoTransferFunction = (DXVA2_VideoTransferFunction)MFVideoTransFunc_2084;
	} else {
		exFmt.VideoTransferMatrix   = DXVA2_VideoTransferMatrix_BT709;
		exFmt.VideoPrimaries        = DXVA2_VideoPrimaries_BT709;
		exFmt.VideoTransferFunction = DXVA2_VideoTransFunc_709;
	}
	// Where this driver reads the studio range: from the full scale for an SDR picture
	// whatever its depth, from 64..940 for a PQ one (vp444_probe --after: 0.65 of a
	// level either way, 1.1 with the other).
	const double scale10 = key.bPq ? 4.0 : 1023.0 / 255.0;

	// P010 first when Super Resolution is wanted; if the driver will not have it on
	// this picture after all, Y410, which keeps the chroma whole.
	const bool bWantSuperRes = key.superRes != SUPERRES_Disable && !key.bKeepChroma;
	for (const DXGI_FORMAT fmt : { DXGI_FORMAT_P010, DXGI_FORMAT_Y410 }) {
		const bool bPlanar = (fmt == DXGI_FORMAT_P010);
		if (bPlanar && !bWantSuperRes) {
			continue;
		}
		// A 4:2:0 texture has even sides; a picture a pixel short of it is written with
		// its last column or row repeated, and only the picture is taken from it.
		const UINT texW = bPlanar ? (w + 1) & ~1u : w;
		const UINT texH = bPlanar ? (h + 1) & ~1u : h;
		// Ten bits out whatever the first turn gave, since the picture comes in ten:
		// the processor chooses, and outFmt says what it chose.
		DXGI_FORMAT outFmt = m_D3D11OutputFmt;
		HRESULT hr = m_D3D11VPScale.InitVideoProcessor(fmt, texW, texH, exFmt, DEINT_Disable, key.bPq, outFmt);
		if (SUCCEEDED(hr) && key.bPq && !m_D3D11VPScale.IsPqSupported()) {
			hr = E_FAIL;   // it would not give back the PQ the first turn gave
		}
		if (SUCCEEDED(hr)) {
			hr = m_D3D11VPScale.InitInputTextures(m_pDevice, true);
		}
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::UpdateVPScale() : no second turn in {} ({})", DXGIFormatToString(fmt), HR2Str(hr));
			continue;
		}
		m_D3D11VPScale.SetRTXVideoHDR(false);   // the first turn's, if anyone's
		m_bVPScaleSuperRes = bPlanar && (m_D3D11VPScale.SetSuperRes(key.superRes) == S_OK);
		if (bPlanar && !m_bVPScaleSuperRes) {
			continue;   // 4:2:0 for nothing
		}

		CComPtr<ID3DBlob> pLuma, pChroma, pPacked;
		if (bPlanar) {
			hr = GetShaderRgbToVideo(RGBTOVIDEO_Luma, key.bPq, w, h, scale10, &pLuma);
			if (SUCCEEDED(hr)) {
				hr = GetShaderRgbToVideo(RGBTOVIDEO_Chroma, key.bPq, w, h, scale10, &pChroma);
			}
			if (SUCCEEDED(hr)) {
				hr = m_pDevice->CreatePixelShader(pLuma->GetBufferPointer(), pLuma->GetBufferSize(), nullptr, &m_pPSRgbToLuma);
			}
			if (SUCCEEDED(hr)) {
				hr = m_pDevice->CreatePixelShader(pChroma->GetBufferPointer(), pChroma->GetBufferSize(), nullptr, &m_pPSRgbToChroma);
			}
		} else {
			hr = GetShaderRgbToVideo(RGBTOVIDEO_Packed, key.bPq, w, h, scale10, &pPacked);
			if (SUCCEEDED(hr)) {
				hr = m_pDevice->CreateComputeShader(pPacked->GetBufferPointer(), pPacked->GetBufferSize(), nullptr, &m_pCSRgbToPacked);
			}
		}
		if (SUCCEEDED(hr)) {
			hr = m_TexVPScaleOut.CheckCreate(m_pDevice, outFmt, outW, outH, Tex2D_DefaultShaderRTarget);
		}
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::UpdateVPScale() : {} could not be set up ({})", DXGIFormatToString(fmt), HR2Str(hr));
			continue;
		}
		m_VPScaleInputFmt = fmt;
		m_bVPScaleFailed = false;
		DLog(L"CDX11VideoProcessor::UpdateVPScale() : {}x{} in {} to {}x{}{}", w, h, DXGIFormatToString(fmt), outW, outH,
			m_bVPScaleSuperRes ? L", Super Resolution" : L"");
		return;
	}

	// The shaders enlarge, as they did, and the statistics say so.
	m_D3D11VPScale.ReleaseVideoProcessor();
	m_pPSRgbToLuma.Release();
	m_pPSRgbToChroma.Release();
	m_pCSRgbToPacked.Release();
	m_bVPScaleSuperRes = false;
}

// The second turn itself: DLSS 5 NR's picture in, the picture at the size it is shown
// at out. Anything that goes wrong leaves the caller's texture as it was, for the
// shaders to enlarge.
HRESULT CDX11VideoProcessor::VPScalePass(Tex2D_t* pInputTexture, const CRect& rSrc, Tex2D_t** ppResult)
{
	if (!pInputTexture || !ppResult || m_bVPScaleFailed || !m_D3D11VPScale.IsReady() || !m_TexVPScaleOut.pTexture
			|| (UINT)rSrc.Width() != m_VPScaleKey.w || (UINT)rSrc.Height() != m_VPScaleKey.h || rSrc.left || rSrc.top) {
		return E_FAIL;
	}
	const UINT w = m_VPScaleKey.w, h = m_VPScaleKey.h;
	ID3D11Texture2D* pInput = m_D3D11VPScale.GetNextInputTexture(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	if (!pInput) {
		return E_FAIL;
	}
	HRESULT hr = S_OK;

	if (m_VPScaleInputFmt == DXGI_FORMAT_P010) {
		// A plane at a time, each through the view that makes it a render target.
		auto planes = std::find_if(m_VPScalePlanes.begin(), m_VPScalePlanes.end(),
			[&](const VPScalePlanes& p) { return p.pTexture == pInput; });
		if (planes == m_VPScalePlanes.end()) {
			VPScalePlanes made;
			made.pTexture = pInput;
			D3D11_RENDER_TARGET_VIEW_DESC desc = {};
			desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			desc.Format = DXGI_FORMAT_R16_UNORM;
			hr = m_pDevice->CreateRenderTargetView(pInput, &desc, &made.pLuma);
			if (SUCCEEDED(hr)) {
				desc.Format = DXGI_FORMAT_R16G16_UNORM;
				hr = m_pDevice->CreateRenderTargetView(pInput, &desc, &made.pChroma);
			}
			if (FAILED(hr)) {
				DLog(L"CDX11VideoProcessor::VPScalePass() : no render target on a plane ({})", HR2Str(hr));
				m_bVPScaleFailed = true;
				UpdateStatsStatic();
				return hr;
			}
			m_VPScalePlanes.push_back(made);
			planes = m_VPScalePlanes.end() - 1;
		}
		D3D11_TEXTURE2D_DESC texdesc = {};
		pInput->GetDesc(&texdesc);
		hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, pInputTexture->desc.Width, pInputTexture->desc.Height, rSrc, 0, false);
		if (FAILED(hr)) {
			return hr;
		}
		D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)texdesc.Width, (FLOAT)texdesc.Height, 0, 1 };
		TextureBlt11(m_pDeviceContext, planes->pLuma, viewport, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSRgbToLuma,
			pInputTexture->pShaderResource, m_pSamplerPoint, nullptr, m_pVertexBuffer);
		viewport.Width /= 2;
		viewport.Height /= 2;
		TextureBlt11(m_pDeviceContext, planes->pChroma, viewport, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSRgbToChroma,
			pInputTexture->pShaderResource, m_pSamplerPoint, nullptr, m_pVertexBuffer);
		// The processor reads what was just drawn into.
		m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	} else {
		ID3D11UnorderedAccessView* pUav = m_D3D11VPScale.GetInputUav();
		if (!pUav) {
			m_bVPScaleFailed = true;
			UpdateStatsStatic();
			return E_FAIL;
		}
		// A dispatch sets no render target of its own, and the network's picture may
		// still be one: Direct3D would answer the read with zeros.
		m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* srvs[1] = { pInputTexture->pShaderResource };
		ID3D11UnorderedAccessView* uavs[1] = { pUav };
		m_pDeviceContext->CSSetShader(m_pCSRgbToPacked, nullptr, 0);
		m_pDeviceContext->CSSetShaderResources(0, 1, srvs);
		m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		m_pDeviceContext->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
		ID3D11ShaderResourceView* none[1] = {};
		ID3D11UnorderedAccessView* noUav[1] = {};
		m_pDeviceContext->CSSetShaderResources(0, 1, none);
		m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
		m_pDeviceContext->CSSetShader(nullptr, nullptr, 0);
	}

	const RECT src = { 0, 0, (LONG)w, (LONG)h };
	const RECT dst = { 0, 0, (LONG)m_TexVPScaleOut.desc.Width, (LONG)m_TexVPScaleOut.desc.Height };
	m_D3D11VPScale.SetRectangles(&src, &dst);
	hr = m_D3D11VPScale.Process(m_TexVPScaleOut.pTexture, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, false);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::VPScalePass() : the processor refused the picture ({})", HR2Str(hr));
		m_bVPScaleFailed = true;
		UpdateStatsStatic();
		return hr;
	}

	*ppResult = &m_TexVPScaleOut;
	return S_OK;
}

// Upstream's test for the driver's Super Resolution, as 0.10.9 has it (7ba4618, bc509ba):
// refused only with Windows' automatic colour management on, an 8-bit source, a target
// wider than 8 bits and no RTX Video HDR. Before, upstream also refused it for any
// picture bound for a target wider than 8 bits, every real HDR film among them, which
// the driver does take: on a PQ BT.2020 source in P010 the extension changes the
// picture by 3.928, against 3.903 on the same frame in SDR (tools/dlssnr_probe
// vp444_probe). Both turns of the processor ask it with the source's own facts.
bool CDX11VideoProcessor::VPSuperResAllowed(const int depth) const
{
	return !(m_bACMEnabled && !m_bVPUseRTXVideoHDR && depth == 8 && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM);
}

// The size the processor's first turn writes where it does not resize: the source's, or
// for RTX Video HDR (kRtxHdrMinPixels) the source enlarged, keeping its shape, just past
// 640x360 pixels -- 640x270 to 740x312 -- for the shaders to enlarge the rest. A small
// picture the shaders enlarged was otherwise shown without RTX Video HDR. It costs some
// of the shaders' enlargement: Fight Club reduced to 640x266 and enlarged back to 1920x800
// by FSRCNNX 16 AR came to 35.70 dB of luma PSNR against the original with it, 37.32
// without, and the processor enlarging alone to 35.40. Not for DLSS 5 NR before the
// resizing, nor DLSS SR: both start from the source's own size.
CSize CDX11VideoProcessor::VPConvertTexSize() const
{
	CSize size((int)m_srcRectWidth, (int)m_srcRectHeight);
	const UINT64 pixels = (UINT64)m_srcRectWidth * m_srcRectHeight;
	const bool bShadersResize = !m_bVPScaling && !m_bDlssSRActive && !(m_bDlssNRActive && !m_bDlssNRAfterUpscale);
	if (m_bVPUseRTXVideoHDR && bShadersResize && pixels && pixels < kRtxHdrMinPixels) {
		const double scale = std::sqrt((double)kRtxHdrMinPixels / (double)pixels);
		size.cx = (int)std::ceil(m_srcRectWidth * scale);
		size.cy = (int)std::ceil(m_srcRectHeight * scale);
	}
	return size;
}

// The driver's Super Resolution, on an 8-bit source the processor writes in ten bits on an
// HDR desktop, writes HDR10 there -- PQ, BT.2020 -- whatever output colour space it was
// given, and without RTX Video HDR the swap chain is SDR and shows it as such: a picture
// under 115 nits with a tenth of its colour (playback_test --vpturns, an XviD 640x270 and
// Fight Club re-encoded in 8 bits, at every size tried; the 10-bit original was right).
// Given eight bits to write, it writes what it was asked: mean, peak and colour within
// 0.001 of the processor's without Super Resolution, and its detail kept (0.0168 against
// 0.0119). Eight bits lose nothing of an 8-bit source. Seen on an HDR desktop only, so
// asked for there only.
bool CDX11VideoProcessor::VPSuperResOutputsEightBits(const int depth)
{
	// The same question as the rtxHDR of InitializeD3D11VP, local tone mapping included.
	const bool bRtxAsked = m_bVPRTXVideoHDR && m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)
		&& m_iTexFormat != TEXFMT_8INT && !SourceIsHDR();
	return m_bHdrDisplayModeEnabled && !bRtxAsked && depth == 8
		&& VPResizesFirst() && m_iVPSuperRes != SUPERRES_Disable;
}

// The driver's Super Resolution on the processor's first turn, asked for in one place.
// It belongs to the resizing and to nothing else, so it is asked for on the turn that
// resizes. While a DLSS pass held the first turn at the source's size it stayed asked
// for there, and the driver ran it on every picture with nothing to enlarge: 2.29
// levels of change on a 960x540 picture (vp444_probe) and 16 ms a picture at 1080p on
// an RTX 4060, before the network, which never asked for it. With DLSS 5 NR it is now
// the second turn that has it (UpdateVPScale). Without DLSS nothing is changed here:
// the question is upstream's.
void CDX11VideoProcessor::UpdateVPSuperRes(const int depth)
{
	const int superRes = (VPResizesFirst() && VPSuperResAllowed(depth)) ? m_iVPSuperRes : SUPERRES_Disable;
	m_bVPUseSuperRes = (m_D3D11VP.SetSuperRes(superRes) == S_OK);
}
