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

// CDX11VideoProcessor: the cadence slips, placed on a scene cut or covered by a picture
// made between two (CPresentSchedule::SetCadenceSlips says when a slip is owed and where
// it may go). All of it is this fork's, and kept out of DX11VideoProcessor.cpp so that a
// merge of upstream's file meets as little of ours as possible (MERGING-UPSTREAM.md).
//
// Every picture drawn while a slip may come is copied out of the back buffer before
// anything goes over it -- the back buffer is not a texture a shader may read -- and the
// copy is what the scene cut detector measures (CSceneCut) and what a made picture is made
// from. The detector's answer is the GPU's, read when the picture's turn to be presented
// comes. A slip that no cut came in time for, with "else a made picture", is the one place
// a picture is presented that was not drawn: EchoFG's (CEchoFlow, CEchoSynth11), between
// the last picture and this one at the moment of the refresh the slip adds, with whatever
// the player draws over a picture drawn over it again, and this one follows on the next
// refresh from a copy of its own back buffer.

#include "stdafx.h"
#include "Helper.h"
#include "Times.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"

extern bool g_bPresent;

namespace {

constexpr float kWhiteNits = 203.0f;   // the SDR white an HDR picture is compared and made under (framegen's kEchoWhiteNits)
constexpr float kSoften = 0.6f;        // as the frame generation branch gives EchoFG
constexpr float kGrain = 1.0f;         // its grain matching, SDR only

// Test aids, read once. MPCVR_CADENCE_NOCUT=1: what the detector says is traced and not acted
// on, so that every slip is forced and, with "else a made picture", covered by one -- the way
// to see made pictures on a film full of cuts. MPCVR_CADENCE_DUMP=<folder>: the first
// kDumpCount made pictures written there at half size with the two they were made between
// (BMP, 8 bits a channel, a PQ picture as its code values), a picture later.
constexpr unsigned kDumpCount = 6;

bool NoCutFromEnvironment()
{
	wchar_t value[8] = {};
	return GetEnvironmentVariableW(L"MPCVR_CADENCE_NOCUT", value, (DWORD)std::size(value)) && value[0] == L'1';
}

std::wstring DumpFolderFromEnvironment()
{
	wchar_t value[MAX_PATH] = {};
	const DWORD n = GetEnvironmentVariableW(L"MPCVR_CADENCE_DUMP", value, (DWORD)std::size(value));
	return n && n < std::size(value) ? std::wstring(value) : std::wstring();
}

// A mapped texture of B8G8R8A8, R8G8B8A8 or R10G10B10A2 to a 24-bit BMP at half its size.
void WriteBmpHalf(const std::wstring& path, const D3D11_MAPPED_SUBRESOURCE& mapped, UINT w, UINT h, DXGI_FORMAT format)
{
	const UINT ow = w / 2, oh = h / 2, stride = (ow * 3 + 3) & ~3u;
	std::vector<uint8_t> data((size_t)stride * oh);
	for (UINT y = 0; y < oh; y++) {
		const uint8_t* row = (const uint8_t*)mapped.pData + (size_t)mapped.RowPitch * (2 * y);
		uint8_t* out = &data[(size_t)stride * (oh - 1 - y)];
		for (UINT x = 0; x < ow; x++) {
			uint8_t r, g, b;
			if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
				const uint32_t v = ((const uint32_t*)row)[2 * x];
				r = (uint8_t)((v & 1023) >> 2);
				g = (uint8_t)(((v >> 10) & 1023) >> 2);
				b = (uint8_t)(((v >> 20) & 1023) >> 2);
			} else {
				const uint8_t* p = row + 8 * x;
				const bool bBgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
				r = bBgra ? p[2] : p[0];
				g = p[1];
				b = bBgra ? p[0] : p[2];
			}
			out[3 * x] = b;
			out[3 * x + 1] = g;
			out[3 * x + 2] = r;
		}
	}
	BITMAPFILEHEADER file = {};
	BITMAPINFOHEADER info = {};
	file.bfType = 0x4D42;
	file.bfOffBits = sizeof(file) + sizeof(info);
	file.bfSize = file.bfOffBits + (DWORD)data.size();
	info.biSize = sizeof(info);
	info.biWidth = (LONG)ow;
	info.biHeight = (LONG)oh;
	info.biPlanes = 1;
	info.biBitCount = 24;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
		fwrite(&file, sizeof(file), 1, f);
		fwrite(&info, sizeof(info), 1, f);
		fwrite(data.data(), 1, data.size(), f);
		fclose(f);
	}
}

} // namespace

void CDX11VideoProcessor::CadenceWriteDump()
{
	m_bCadenceDumpPending = false;
	static const std::wstring s_folder = DumpFolderFromEnvironment();
	const wchar_t* names[3] = { L"a", L"made", L"c" };
	for (int i = 0; i < 3; i++) {
		Tex2D_t& t = m_TexCadenceDump[i];
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (t.pTexture && SUCCEEDED(m_pDeviceContext->Map(t.pTexture, 0, D3D11_MAP_READ, 0, &mapped))) {
			WriteBmpHalf(std::format(L"{}\\cadence_{}_{}.bmp", s_folder, m_cadenceDumped, names[i]), mapped, t.desc.Width, t.desc.Height, t.desc.Format);
			m_pDeviceContext->Unmap(t.pTexture, 0);
		}
	}
	m_cadenceDumped++;
}

bool CDX11VideoProcessor::CadenceActive() const
{
	return m_iCadenceSlips != CADENCE_SLIPS_Off && m_bDlssRenderAhead && m_pDXGISwapChain1 && m_PresentSchedule.SlipsPlaced();
}

UINT CDX11VideoProcessor::CadenceEncoding() const
{
	return m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2u : 0u;
}

void CDX11VideoProcessor::CadenceReset()
{
	m_bCadenceHaveLast = false;
	m_bCadenceHaveBefore = false;
	m_bCadenceMeasured = false;
	m_SceneCut.Reset();
	m_CadenceFlow.Forget();
	m_cadenceAhead.store(0, std::memory_order_relaxed);
}

void CDX11VideoProcessor::CadenceRelease()
{
	CadenceReset();
	m_SceneCut.Release();
	m_CadenceFlow.Release();
	m_CadenceSynth.Release();
	for (auto& t : m_TexCadencePicture) {
		t.Release();
	}
	m_pCadenceMadeUav.Release();
	m_TexCadenceMade.Release();
	m_TexCadenceKeep.Release();
	for (auto& t : m_TexCadenceDump) {
		t.Release();
	}
	m_bCadenceDumpPending = false;
	m_cadenceRect.SetRectEmpty();
	m_bCadenceMakeReady = false;
	m_bCadenceMakeFailed = false;
}

void CDX11VideoProcessor::CadenceTakePicture(ID3D11Texture2D* pBackBuffer)
{
	if (m_bCadenceDumpPending) {
		CadenceWriteDump();   // the copies of the last made picture, which the GPU has had a picture's time for
	}
	D3D11_TEXTURE2D_DESC bb = {};
	pBackBuffer->GetDesc(&bb);
	CRect rect;
	if (!rect.IntersectRect(m_videoRect, CRect(0, 0, (int)bb.Width, (int)bb.Height)) || rect.Width() < 64 || rect.Height() < 64) {
		return;
	}
	const UINT w = (UINT)rect.Width(), h = (UINT)rect.Height();
	if (rect != m_cadenceRect || m_TexCadencePicture[0].desc.Format != bb.Format || !m_TexCadencePicture[1].pTexture) {
		// Another size, another place: nothing before it to compare with or make from.
		m_cadenceRect = rect;
		CadenceReset();
		m_bCadenceMakeReady = false;
		for (auto& t : m_TexCadencePicture) {
			if (FAILED(t.Create(m_pDevice, bb.Format, w, h, Tex2D_DefaultShader))) {
				DLog(L"CDX11VideoProcessor::CadenceTakePicture() : the copies of {}x{} could not be made", w, h);
				CadenceRelease();
				return;
			}
		}
	}
	if (!m_SceneCut.IsReady() && !m_SceneCut.Init(m_pDevice)) {
		return;
	}

	const int cur = m_bCadenceHaveLast ? 1 - m_iCadencePicture : m_iCadencePicture;
	const D3D11_BOX box = { (UINT)rect.left, (UINT)rect.top, 0, (UINT)rect.right, (UINT)rect.bottom, 1 };
	m_pDeviceContext->CopySubresourceRegion(m_TexCadencePicture[cur].pTexture, 0, 0, 0, 0, pBackBuffer, 0, &box);
	m_bCadenceHaveBefore = m_bCadenceHaveLast;
	m_bCadenceHaveLast = true;
	m_iCadencePicture = cur;
	m_bCadenceMeasured = m_SceneCut.Measure(m_pDevice, m_pDeviceContext, m_TexCadencePicture[cur].pShaderResource,
		w, h, CadenceEncoding(), kWhiteNits, ++m_cadenceId);

	// What a made picture needs, made while the film plays and not when a slip needs it:
	// creating it takes longer than the refresh the picture is for.
	if (m_iCadenceSlips == CADENCE_SLIPS_OnCutOrMade && !m_bCadenceMakeReady && !m_bCadenceMakeFailed) {
		const DXGI_FORMAT madeFormat = bb.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
		bool bReady = m_CadenceFlow.Init(m_pDevice) && m_CadenceSynth.Init(m_pDevice)
			&& m_CadenceFlow.Ensure(m_pDevice, w, h) && m_CadenceSynth.Ensure(m_pDevice, w, h);
		if (bReady) {
			m_pCadenceMadeUav.Release();
			bReady = SUCCEEDED(m_TexCadenceMade.Create(m_pDevice, madeFormat, w, h, Tex2D_DefaultShaderRTargetUAV))
				&& SUCCEEDED(m_pDevice->CreateUnorderedAccessView(m_TexCadenceMade.pTexture, nullptr, &m_pCadenceMadeUav));
		}
		if (bReady) {
			m_bCadenceMakeReady = true;
		} else {
			// No made picture on this device (feature level 11_0, memory): the slips that no
			// cut comes for are plain ones, as "On a scene cut" has them.
			m_bCadenceMakeFailed = true;
			DLog(L"CDX11VideoProcessor::CadenceTakePicture() : no made pictures: {} / {}",
				m_CadenceFlow.Status(), m_CadenceSynth.Status());
			if (m_PacingTrace.Enabled()) {
				m_PacingTrace.Note(L"cadence slips: no made pictures (" + m_CadenceFlow.Status() + L" / " + m_CadenceSynth.Status() + L")");
			}
			UpdateStatsPresent();
		}
	}
}

int CDX11VideoProcessor::CadenceReadCut(CPacingTrace::Picture* pTraced)
{
	if (!m_bCadenceMeasured) {
		return -1;
	}
	float state[16] = {};
	const int cut = m_SceneCut.Read(m_pDeviceContext, m_cadenceId, state);
	if (cut >= 0) {
		m_bCadenceMeasured = false;
		// The trace's cut columns are DLSS SR's when it runs (CCutReadback).
		if (pTraced && !m_bDlssSRActive && pTraced->cutHist < 0) {
			CPacingTrace::TakeCut(*pTraced, state);
		}
		if (pTraced && cut) {
			pTraced->flags |= CPacingTrace::CadenceCut;
		}
	}
	static const bool s_bNoCut = NoCutFromEnvironment();
	return s_bNoCut && cut > 0 ? 0 : cut;
}

void CDX11VideoProcessor::CadenceUpdateAhead()
{
	// While a slip waits for its cut and may end on a made picture, each sample is asked
	// for a refresh and a made picture earlier than render ahead would: the drawn picture
	// has to be ready, and the made one too, by the refresh before its own.
	int ahead = 0;
	if (m_iCadenceSlips == CADENCE_SLIPS_OnCutOrMade && m_bCadenceMakeReady && m_PresentSchedule.SlipWaiting()) {
		ahead = (int)((m_dRefreshPeriodMs + m_cadenceMadeMs) * 10000.0);
	}
	m_cadenceAhead.store(ahead, std::memory_order_relaxed);
}

bool CDX11VideoProcessor::CadencePresentMade(const CPresentSchedule::Plan& plan, CComPtr<ID3D11Texture2D>& pBackBuffer,
	const bool bMeasure, const bool bTrace, const CPacingTrace::Picture& traced)
{
	if (!m_bCadenceMakeReady || !m_bCadenceHaveBefore || !m_bCadenceHaveLast || !m_pCadenceMadeUav) {
		m_cadenceMadeFailed++;
		return false;
	}
	const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;
	const long long tickStart = (long long)GetPreciseTick();
	double t = 0.5;
	const CPresentSchedule::Plan made = m_PresentSchedule.MadePlan(tickStart + (long long)(m_cadenceMadeMs * ticksPerMs), plan, t);
	if (!made.bValid) {
		m_cadenceMadeFailed++;   // too late for the refresh before this picture's: a plain slip
		return false;
	}

	// The motion from the picture before to this one, and the picture between them.
	const UINT w = (UINT)m_cadenceRect.Width(), h = (UINT)m_cadenceRect.Height();
	const UINT encoding = CadenceEncoding();
	ID3D11ShaderResourceView* pBefore = m_TexCadencePicture[1 - m_iCadencePicture].pShaderResource;
	ID3D11ShaderResourceView* pNow = m_TexCadencePicture[m_iCadencePicture].pShaderResource;
	m_CadenceFlow.Forget();
	m_CadenceFlow.Measure(m_pDeviceContext, pBefore, nullptr, encoding, kWhiteNits);
	m_CadenceFlow.Measure(m_pDeviceContext, pNow, pBefore, encoding, kWhiteNits);
	if (!m_CadenceSynth.Make(m_pDeviceContext, pBefore, pNow, m_CadenceFlow.MotionView(), m_pCadenceMadeUav, (float)t,
			encoding, kWhiteNits, kSoften, encoding == 0 ? kGrain : 0.0f)) {
		m_cadenceMadeFailed++;
		return false;
	}

	// This picture, with everything drawn over it, is kept for the refresh after; the made
	// one takes its place in the back buffer, and the same things are drawn over it.
	D3D11_TEXTURE2D_DESC bb = {};
	pBackBuffer->GetDesc(&bb);
	if (FAILED(m_TexCadenceKeep.CheckCreate(m_pDevice, bb.Format, bb.Width, bb.Height, Tex2D_Default))) {
		m_cadenceMadeFailed++;
		return false;
	}
	m_pDeviceContext->CopyResource(m_TexCadenceKeep.pTexture, pBackBuffer);
	{
		CComPtr<ID3D11RenderTargetView> pRTV;
		if (SUCCEEDED(m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRTV))) {
			const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			m_pDeviceContext->ClearRenderTargetView(pRTV, black);
		}
	}
	TextureCopyRect(m_TexCadenceMade, pBackBuffer, CRect(0, 0, (int)w, (int)h), m_cadenceRect, m_pPS_Simple, nullptr, 0, false);
	HRESULT hrOverlays = S_OK;
	DrawOverlays(pBackBuffer, hrOverlays);

	// Presented on its own refresh as any picture: what render ahead learns from it is
	// whether the drawn picture's sample came early enough for the made one before it.
	m_tickPictureDone = 0;
	MarkPictureSubmitted();
	CPresentSchedule::Plan forWait = made;
	forWait.tickDue = plan.tickDue;
	WaitForScheduledPresent(forWait, bMeasure);
	const uint64_t tickDone = m_tickPictureDone;
	if (tickDone) {
		const double tookMs = (double)((long long)tickDone - tickStart) / ticksPerMs;
		m_cadenceMadeMs = std::clamp(std::max(tookMs, 0.8 * m_cadenceMadeMs + 0.2 * tookMs), 1.0, 40.0);
	}

	CPacingTrace::Picture row = traced;
	if (bTrace) {
		row.flags |= CPacingTrace::Generated | CPacingTrace::Scheduled | CPacingTrace::SlipForced;
		row.flags &= ~(CPacingTrace::Repeated | CPacingTrace::Dropped);
		row.rtStart = traced.rtStart - (long long)((double)(plan.tickDue - made.tickDue) * 10000000.0 / GetPreciseTicksPerSecond());
		row.tickTarget = made.tickRefresh;
		row.tickStamp = made.tickStamp;
		row.tickDue = made.tickDue;
		row.tickGpuDone = (long long)tickDone;
		ReadShownForTrace(row.before);
	}

	m_tickPresentCall = GetPreciseTick();
	g_bPresent = true;
	const HRESULT hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	const uint64_t tickReturn = GetPreciseTick();
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::CadencePresentMade() : Present() failed with error {}", HR2Str(hr));
	if (SUCCEEDED(hr)) {
		UINT presentId = 0;
		if (SUCCEEDED(m_pDXGISwapChain1->GetLastPresentCount(&presentId))) {
			m_PresentSchedule.Presented(presentId, made, (long long)m_tickPresentCall,
				(double)(tickReturn - m_tickPresentCall) / ticksPerMs, tickDone != 0);
		}
	} else {
		m_cadenceMadeFailed++;
	}
	if (bTrace) {
		row.tickPresentCall = (long long)m_tickPresentCall;
		row.tickPresentReturn = (long long)tickReturn;
		ReadShownForTrace(row.after);
		m_PacingTrace.Add(row);
	}

	// The pair and what was made from it, copied out for MPCVR_CADENCE_DUMP and written a
	// picture later, when the GPU has done the copies.
	static const bool s_bDump = !DumpFolderFromEnvironment().empty();
	if (s_bDump && m_cadenceDumped < kDumpCount && !m_bCadenceDumpPending) {
		ID3D11Texture2D* from[3] = { m_TexCadencePicture[1 - m_iCadencePicture].pTexture, m_TexCadenceMade.pTexture, m_TexCadencePicture[m_iCadencePicture].pTexture };
		bool bOk = true;
		for (int i = 0; i < 3 && bOk; i++) {
			D3D11_TEXTURE2D_DESC d = {};
			from[i]->GetDesc(&d);
			bOk = SUCCEEDED(m_TexCadenceDump[i].CheckCreate(m_pDevice, d.Format, d.Width, d.Height, Tex2D_StagingRead));
			if (bOk) {
				m_pDeviceContext->CopyResource(m_TexCadenceDump[i].pTexture, from[i]);
			}
		}
		m_bCadenceDumpPending = bOk;
	}

	// This picture into the next buffer, as it was drawn.
	pBackBuffer.Release();
	if (FAILED(m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
		pBackBuffer.Release();
		return false;
	}
	m_pDeviceContext->CopyResource(pBackBuffer, m_TexCadenceKeep.pTexture);
	return SUCCEEDED(hr);
}

// What goes over the picture: the subtitles, the statistics, the notices, the player's own
// bitmap. Drawn again over a made picture, which takes the drawn one's place.
void CDX11VideoProcessor::DrawOverlays(ID3D11Texture2D* pBackBuffer, HRESULT& hr)
{
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
}

// Until a moment on the performance counter, as WaitForScheduledPresent waits: a slip
// taken on a cut moves the picture's Present a refresh later.
void CDX11VideoProcessor::WaitUntilTick(const long long tick)
{
	const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;
	const uint64_t tickStart = GetPreciseTick();
	for (;;) {
		const uint64_t tickNow = GetPreciseTick();
		const double remainingMs = (double)(tick - (long long)tickNow) / ticksPerMs;
		if (remainingMs <= 0 || m_pFilter->m_filterState != State_Running || (double)(tickNow - tickStart) / ticksPerMs >= 100.0) {
			break;
		}
		m_PreciseSleep.Sleep(std::min(remainingMs, 10.0));
	}
	m_rtHeld += (REFERENCE_TIME)((GetPreciseTick() - tickStart) * 10000000.0 / GetPreciseTicksPerSecond());
}
