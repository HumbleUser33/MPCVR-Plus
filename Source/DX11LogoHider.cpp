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

#include "stdafx.h"
#include <algorithm>
#include <format>
#include "resource.h"
#include "Helper.h"
#include "IVideoRenderer.h"
#include "DX11LogoHider.h"
#include "Logo/LogoLibrary.h"

static_assert(LOGO_MARGIN_MIN == Logo::kMarginLeast && LOGO_MARGIN_MAX == Logo::kMarginMost
	&& LOGO_MARGIN_DEF == Logo::kMarginDefault, "the margin's setting and the analysis's agree");

HRESULT CDX11LogoHider::InitShaders(ID3D11Device* pDevice)
{
	if (!pDevice) {
		return E_POINTER;
	}
	if (m_pDevice && m_pDevice != pDevice) {
		Release(true);
	}
	m_pDevice = pDevice;
	auto create = [&](CComPtr<ID3D11ComputeShader>& cs, UINT resid) {
		if (cs) {
			return S_OK;
		}
		LPVOID data = nullptr;
		DWORD size = 0;
		HRESULT hr = GetDataFromResource(data, size, resid);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateComputeShader(data, size, nullptr, &cs);
		}
		return hr;
	};
	HRESULT hr = create(m_pCSLuma, IDF_CS_11_LOGO_LUMA);
	if (SUCCEEDED(hr)) hr = create(m_pCSCrop[0], IDF_CS_11_LOGO_CROP1);
	if (SUCCEEDED(hr)) hr = create(m_pCSCrop[1], IDF_CS_11_LOGO_CROP2);
	if (SUCCEEDED(hr)) hr = create(m_pCSCopy[0], IDF_CS_11_LOGO_COPY1);
	if (SUCCEEDED(hr)) hr = create(m_pCSCopy[1], IDF_CS_11_LOGO_COPY2);
	if (SUCCEEDED(hr)) hr = create(m_pCSFill[0], IDF_CS_11_LOGO_FILL1);
	if (SUCCEEDED(hr)) hr = create(m_pCSFill[1], IDF_CS_11_LOGO_FILL2);
	if (SUCCEEDED(hr)) hr = create(m_pCSRemove[0], IDF_CS_11_LOGO_REMOVE1);
	if (SUCCEEDED(hr)) hr = create(m_pCSRemove[1], IDF_CS_11_LOGO_REMOVE2);

	auto constants = [&](CComPtr<ID3D11Buffer>& b, UINT size) {
		if (b) {
			return S_OK;
		}
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = size;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		return pDevice->CreateBuffer(&desc, nullptr, &b);
	};
	if (SUCCEEDED(hr)) hr = constants(m_pPackConstants, sizeof(Logo::PackConstants));
	if (SUCCEEDED(hr)) hr = constants(m_pRemoveConstants, sizeof(Logo::RemoveConstants));

	// The logo's strength, kept on the GPU from one picture to the next with the bars the
	// luma pass found, and a copy the statistics read now and then.
	if (SUCCEEDED(hr) && !m_pState) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = Logo::kStateBytes;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		const float one[Logo::kStateBytes / 4] = { 1 };
		D3D11_SUBRESOURCE_DATA init = { one };
		hr = pDevice->CreateBuffer(&desc, &init, &m_pState);
		if (SUCCEEDED(hr)) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
			u.Format = DXGI_FORMAT_R32_TYPELESS;
			u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			u.Buffer.NumElements = Logo::kStateBytes / 4;
			u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			hr = pDevice->CreateUnorderedAccessView(m_pState, &u, &m_pStateUav);
		}
		if (SUCCEEDED(hr)) {
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			hr = pDevice->CreateBuffer(&desc, nullptr, &m_pStateStage);
		}
	}
	DLogIf(FAILED(hr), L"CDX11LogoHider::InitShaders() failed with error {}", HR2Str(hr));
	return hr;
}

void CDX11LogoHider::Release(const bool bShaders)
{
	for (int k = 0; k < 2; k++) {
		m_Out[k].Release();
		m_pOutUav[k].Release();
		m_Map[k].Release();
		m_Mirror[k].Release();
		m_pPyramid[k].Release();
		m_pPyramidUav[k].Release();
	}
	for (Readback& r : m_Read) {
		for (int k = 0; k < 2; k++) {
			r.pack[k].Release();
			r.pUav[k].Release();
			r.pStage[k].Release();
		}
		r.bPending = false;
	}
	m_bUploaded = false;
	m_bStatePending = false;
	if (bShaders) {
		m_Worker.Stop();
		m_pCSLuma.Release();
		for (int k = 0; k < 2; k++) {
			m_pCSCrop[k].Release();
			m_pCSCopy[k].Release();
			m_pCSFill[k].Release();
			m_pCSRemove[k].Release();
		}
		m_pPackConstants.Release();
		m_pRemoveConstants.Release();
		m_pStateUav.Release();
		m_pState.Release();
		m_pStateStage.Release();
		m_pDevice.Release();
	}
}

void CDX11LogoHider::SetFormat(int W, int H, int divW, int divH, const std::wstring& film)
{
	if (W == m_W && H == m_H && divW == m_divW && divH == m_divH && (m_bMaps || Working())) {
		return;
	}
	Stop();
	m_W = W;
	m_H = H;
	m_divW = divW;
	m_divH = divH;
	m_film = film;
	m_Chosen.clear();
	m_rtSampled = kNoTime;
	m_fShown = 1;
	std::vector<Logo::Stored> kept = Logo::KeptLogos(W, H);
	if (!kept.empty()) {
		m_Worker.Recognise(W, H, divW, divH, std::move(kept));
	}
}

void CDX11LogoHider::SetMargin(int margin)
{
	margin = std::clamp(margin, Logo::kMarginLeast, Logo::kMarginMost);
	if (margin != m_Opt.halo) {
		m_Opt.halo = margin;
		m_bUploaded = false;   // the maps say which pixels are worked
	}
}

void CDX11LogoHider::Find()
{
	if (m_W > 0 && m_H > 0) {
		m_rtSampled = kNoTime;
		m_Chosen.clear();
		m_Worker.Start(m_W, m_H, m_divW, m_divH, m_film);
	}
}

void CDX11LogoHider::Forget()
{
	Logo::ForgetLibrary();
	m_Chosen.clear();
	Stop();
}

std::wstring CDX11LogoHider::List()
{
	m_Listed.clear();
	if (m_W <= 0 || m_H <= 0) {
		return {};
	}
	m_Listed = Logo::ListKept(m_W, m_H);
	std::wstring out;
	std::vector<std::wstring> names;
	for (const Logo::Listed& l : m_Listed) {
		// Two of the same size in the same place told apart by their order.
		const std::wstring name = Logo::Describe(l.box, l.videoW, l.videoH);
		const size_t same = std::count(names.begin(), names.end(), name);
		names.push_back(name);
		out += (same ? std::format(L"{} ({})", name, same + 1) : name) + L"\n";
	}
	return out;
}

int CDX11LogoHider::Choice() const
{
	if (m_Chosen.empty()) {
		return 0;
	}
	for (size_t i = 0; i < m_Listed.size(); i++) {
		if (m_Listed[i].path == m_Chosen) {
			return (int)i + 1;
		}
	}
	return 0;
}

void CDX11LogoHider::Choose(int i)
{
	if (m_W <= 0 || m_H <= 0) {
		return;
	}
	if (i <= 0 || i > (int)m_Listed.size()) {
		// Back to the logo recognised by itself, as when the film began.
		m_Chosen.clear();
		Stop();
		std::vector<Logo::Stored> kept = Logo::KeptLogos(m_W, m_H);
		if (!kept.empty()) {
			m_Worker.Recognise(m_W, m_H, m_divW, m_divH, std::move(kept));
		}
		return;
	}
	Logo::Stored s;
	if (!Logo::LoadLogo(m_Listed[i - 1].path, s) || s.videoW != m_W || s.videoH != m_H
			|| s.maps.divW != m_divW || s.maps.divH != m_divH) {
		return;
	}
	Stop();
	m_Chosen = s.path;
	Logo::TouchStored(s.path);
	m_Worker.Use(m_W, m_H, m_divW, m_divH, std::move(s));
}

void CDX11LogoHider::Stop()
{
	m_Worker.Stop();
	m_Maps = Logo::Maps();
	m_bMaps = false;
	m_bUploaded = false;
	for (Readback& r : m_Read) {
		r.bPending = false;
	}
}

bool CDX11LogoHider::Working() const
{
	const auto phase = m_Worker.GetPhase();
	return phase == Logo::CWorker::Phase::Recognising || phase == Logo::CWorker::Phase::Locating
		|| phase == Logo::CWorker::Phase::Fitting;
}

std::wstring CDX11LogoHider::Status()
{
	const std::wstring what = m_Worker.Status();
	if (m_bMaps) {
		const wchar_t* shown = m_fShown < 0.05f ? L"not on screen" : L"hidden";
		return what.empty() ? std::wstring(shown) : what + L", " + shown;
	}
	return what.empty() ? L"none yet: Find the logo" : what;
}

void CDX11LogoHider::Dispatch(ID3D11DeviceContext* pContext, ID3D11ComputeShader* pShader, ID3D11Buffer* pConstants,
	const void* data, UINT size, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
	ID3D11UnorderedAccessView* const* uavs, UINT nUav, UINT gx, UINT gy)
{
	D3D11_MAPPED_SUBRESOURCE mr;
	if (FAILED(pContext->Map(pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
		return;
	}
	memcpy(mr.pData, data, size);
	pContext->Unmap(pConstants, 0);
	pContext->CSSetShader(pShader, nullptr, 0);
	pContext->CSSetShaderResources(0, nSrv, srvs);
	pContext->CSSetUnorderedAccessViews(0, nUav, uavs, nullptr);
	pContext->CSSetConstantBuffers(0, 1, &pConstants);
	pContext->Dispatch(gx, gy, 1);
	// What a pass wrote is read by the next: unbound before it is.
	ID3D11ShaderResourceView* noSrv[3] = {};
	ID3D11UnorderedAccessView* noUav[3] = {};
	pContext->CSSetShaderResources(0, 3, noSrv);
	pContext->CSSetUnorderedAccessViews(0, 3, noUav, nullptr);
}

HRESULT CDX11LogoHider::Targets()
{
	const UINT w[2] = { (UINT)m_W, (UINT)(m_W / m_divW) }, h[2] = { (UINT)m_H, (UINT)(m_H / m_divH) };
	const DXGI_FORMAT fmt[2] = { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM };
	for (int k = 0; k < 2; k++) {
		if (m_Out[k].pTexture && m_Out[k].desc.Width == w[k] && m_Out[k].desc.Height == h[k]) {
			continue;
		}
		m_pOutUav[k].Release();
		HRESULT hr = m_Out[k].Create(m_pDevice, fmt[k], w[k], h[k], Tex2D_DefaultShaderRTargetUAV);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateUnorderedAccessView(m_Out[k].pTexture, nullptr, &m_pOutUav[k]);
		}
		if (FAILED(hr)) {
			return hr;
		}
	}
	return S_OK;
}

HRESULT CDX11LogoHider::Upload(ID3D11DeviceContext* pContext, const Logo::Maps& maps)
{
	const Logo::Rect& b = maps.box;
	if (!maps.valid() || b.x < 0 || b.y < 0 || b.x + b.w > m_W || b.y + b.h > m_H
			|| maps.divW != m_divW || maps.divH != m_divH) {
		return E_INVALIDARG;
	}
	std::vector<uint16_t> data[2], mirror[2];
	Logo::ShaderMaps(maps, m_Opt, data[0], data[1], mirror[0], mirror[1]);
	for (int k = 0; k < 2; k++) {
		const int w = maps.alpha[k].w, h = maps.alpha[k].h;
		HRESULT hr = m_Map[k].Create(m_pDevice, DXGI_FORMAT_R16G16B16A16_UNORM, w, h, Tex2D_DefaultShader);
		if (SUCCEEDED(hr)) {
			hr = m_Mirror[k].Create(m_pDevice, DXGI_FORMAT_R16G16_UINT, w, h, Tex2D_DefaultShader);
		}
		if (FAILED(hr)) {
			return hr;
		}
		pContext->UpdateSubresource(m_Map[k].pTexture, 0, nullptr, data[k].data(), 8 * w, 0);
		pContext->UpdateSubresource(m_Mirror[k].pTexture, 0, nullptr, mirror[k].data(), 4 * w, 0);
		m_pPyramidUav[k].Release();
		m_pPyramid[k].Release();
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 16 * Logo::PyramidBufferEntries(w, h);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = 16;
		hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pPyramid[k]);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateUnorderedAccessView(m_pPyramid[k], nullptr, &m_pPyramidUav[k]);
		}
		if (FAILED(hr)) {
			return hr;
		}
	}
	m_bUploaded = true;
	return S_OK;
}

void CDX11LogoHider::Sample(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pLuma, ID3D11ShaderResourceView* pChroma,
	REFERENCE_TIME rt)
{
	// One picture every kSampleSeconds of the film, so that they differ; by the count
	// where the pictures carry no time.
	const REFERENCE_TIME every = (REFERENCE_TIME)(Logo::kSampleSeconds * 10000000);
	m_nSinceSample++;
	if (rt != kNoTime && m_rtSampled != kNoTime) {
		if (rt >= m_rtSampled && rt - m_rtSampled < every) {
			return;
		}
	} else if (m_rtSampled != kNoTime && m_nSinceSample < 10) {
		return;
	}
	int scale = 1;
	Logo::Rect box;
	const bool bLuma = m_Worker.WantsLuma(&scale);
	const bool bBox = !bLuma && m_Worker.WantsBox(&box);
	if (!bLuma && !bBox) {
		return;
	}
	Readback* slot = nullptr;
	for (Readback& r : m_Read) {
		if (!r.bPending) {
			slot = &r;
			break;
		}
	}
	if (!slot) {
		return;   // the GPU has not caught up with the last ones yet
	}

	auto ensure = [&](int k, DXGI_FORMAT fmt, UINT w, UINT h) {
		if (slot->pack[k].pTexture && slot->pack[k].desc.Width == w && slot->pack[k].desc.Height == h && slot->pack[k].desc.Format == fmt) {
			return S_OK;
		}
		slot->pUav[k].Release();
		slot->pStage[k].Release();
		HRESULT hr = slot->pack[k].Create(m_pDevice, fmt, w, h, Tex2D_DefaultShaderRTargetUAV);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateUnorderedAccessView(slot->pack[k].pTexture, nullptr, &slot->pUav[k]);
		}
		if (SUCCEEDED(hr)) {
			D3D11_TEXTURE2D_DESC desc = CreateTex2DDesc(fmt, w, h, Tex2D_StagingRead);
			hr = m_pDevice->CreateTexture2D(&desc, nullptr, &slot->pStage[k]);
		}
		return hr;
	};

	Logo::PackConstants pc = {};
	if (bLuma) {
		const UINT w = m_W / scale, h = m_H / scale;
		if (FAILED(ensure(0, DXGI_FORMAT_R16_UNORM, w, h))) {
			return;
		}
		pc.width = w;
		pc.height = h;
		pc.scale = scale;
		Dispatch(pContext, m_pCSLuma, m_pPackConstants, &pc, sizeof(pc), &pLuma, 1, &slot->pUav[0].p, 1, (w + 15) / 16, (h + 15) / 16);
		pContext->CopyResource(slot->pStage[0], slot->pack[0].pTexture);
		slot->w = w;
		slot->h = h;
	} else {
		const UINT cw = box.w / m_divW, ch = box.h / m_divH;
		if (FAILED(ensure(0, DXGI_FORMAT_R16_UNORM, box.w, box.h)) || FAILED(ensure(1, DXGI_FORMAT_R16G16_UNORM, cw, ch))) {
			return;
		}
		pc.offsetX = box.x;
		pc.offsetY = box.y;
		pc.width = box.w;
		pc.height = box.h;
		Dispatch(pContext, m_pCSCrop[0], m_pPackConstants, &pc, sizeof(pc), &pLuma, 1, &slot->pUav[0].p, 1, (box.w + 15) / 16, (box.h + 15) / 16);
		pc.offsetX = box.x / m_divW;
		pc.offsetY = box.y / m_divH;
		pc.width = cw;
		pc.height = ch;
		Dispatch(pContext, m_pCSCrop[1], m_pPackConstants, &pc, sizeof(pc), &pChroma, 1, &slot->pUav[1].p, 1, (cw + 15) / 16, (ch + 15) / 16);
		pContext->CopyResource(slot->pStage[0], slot->pack[0].pTexture);
		pContext->CopyResource(slot->pStage[1], slot->pack[1].pTexture);
		slot->box = box;
	}
	pContext->CSSetShader(nullptr, nullptr, 0);
	slot->bLuma = bLuma;
	slot->bPending = true;
	slot->order = ++m_nOrder;
	m_rtSampled = rt;
	m_nSinceSample = 0;
}

void CDX11LogoHider::Collect(ID3D11DeviceContext* pContext)
{
	Readback* oldest = nullptr;
	for (Readback& r : m_Read) {
		if (r.bPending && (!oldest || r.order < oldest->order)) {
			oldest = &r;
		}
	}
	if (!oldest) {
		return;
	}
	// Never waited for: a copy not done yet is looked at again on the next picture.
	D3D11_MAPPED_SUBRESOURCE mr[2] = {};
	if (pContext->Map(oldest->pStage[0], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr[0]) != S_OK) {
		return;
	}
	if (!oldest->bLuma && pContext->Map(oldest->pStage[1], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr[1]) != S_OK) {
		pContext->Unmap(oldest->pStage[0], 0);
		return;
	}
	auto read = [](const D3D11_MAPPED_SUBRESOURCE& m, int w, int h, int channels, int channel, Logo::Plane& out) {
		out.resize(w, h);
		for (int y = 0; y < h; y++) {
			const uint16_t* row = (const uint16_t*)((const BYTE*)m.pData + (size_t)m.RowPitch * y);
			float* o = &out.v[(size_t)y * w];
			for (int x = 0; x < w; x++) {
				o[x] = row[channels * x + channel] * (1.0f / 65535.0f);
			}
		}
	};
	if (oldest->bLuma) {
		Logo::Plane luma;
		read(mr[0], oldest->w, oldest->h, 1, 0, luma);
		pContext->Unmap(oldest->pStage[0], 0);
		m_Worker.PushLuma(std::move(luma));
	} else {
		Logo::Crop crop;
		const Logo::Rect& b = oldest->box;
		read(mr[0], b.w, b.h, 1, 0, crop.p[0]);
		read(mr[1], b.w / m_divW, b.h / m_divH, 2, 0, crop.p[1]);
		read(mr[1], b.w / m_divW, b.h / m_divH, 2, 1, crop.p[2]);
		pContext->Unmap(oldest->pStage[0], 0);
		pContext->Unmap(oldest->pStage[1], 0);
		m_Worker.PushBox(std::move(crop));
	}
	oldest->bPending = false;
}

void CDX11LogoHider::ReadStrength(ID3D11DeviceContext* pContext)
{
	if (m_bStatePending) {
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (pContext->Map(m_pStateStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr) == S_OK) {
			m_fShown = *(const float*)mr.pData;
			pContext->Unmap(m_pStateStage, 0);
			m_bStatePending = false;
		}
		return;
	}
	if (++m_nStateWait >= 12) {
		m_nStateWait = 0;
		pContext->CopyResource(m_pStateStage, m_pState);
		m_bStatePending = true;
	}
}

bool CDX11LogoHider::Run(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pLuma, ID3D11ShaderResourceView* pChroma,
	REFERENCE_TIME rt, bool newPicture)
{
	if (!m_pCSRemove[0] || !pLuma || !pChroma || m_W <= 0) {
		return false;
	}
	if (newPicture) {
		Collect(pContext);
		Sample(pContext, pLuma, pChroma, rt);
	}

	Logo::Maps fresh;
	bool final = false;
	if (m_Worker.TakeMaps(fresh, &final)) {
		// A better fit of the same logo keeps its strength; another logo starts on screen.
		const bool same = m_bMaps && fresh.box.x == m_Maps.box.x && fresh.box.y == m_Maps.box.y
			&& fresh.box.w == m_Maps.box.w && fresh.box.h == m_Maps.box.h;
		m_Maps = std::move(fresh);
		m_bMaps = true;
		m_bUploaded = false;
		if (!same && m_pState) {
			const float one[Logo::kStateBytes / 4] = { 1 };
			pContext->UpdateSubresource(m_pState, 0, nullptr, one, 0, 0);
			m_fShown = 1;
		}
	}
	if (!m_bMaps) {
		return false;
	}
	if (FAILED(Targets()) || (!m_bUploaded && FAILED(Upload(pContext, m_Maps)))) {
		return false;
	}

	// The whole picture into the hider's planes, then the logo's box worked over in them.
	Logo::PackConstants pc = {};
	for (int k = 0; k < 2; k++) {
		pc.width = m_Out[k].desc.Width;
		pc.height = m_Out[k].desc.Height;
		ID3D11ShaderResourceView* src = k ? pChroma : pLuma;
		Dispatch(pContext, m_pCSCopy[k], m_pPackConstants, &pc, sizeof(pc), &src, 1, &m_pOutUav[k].p, 1,
			(pc.width + 15) / 16, (pc.height + 15) / 16);
	}
	for (int k = 0; k < 2; k++) {
		const Logo::RemoveConstants rc = Logo::MakeRemoveConstants(m_Maps, k, m_Opt, newPicture);
		ID3D11ShaderResourceView* srvs[3] = { k ? pChroma : pLuma, m_Map[k].pShaderResource, m_Mirror[k].pShaderResource };
		ID3D11UnorderedAccessView* uavs[3] = { m_pOutUav[k], m_pPyramidUav[k], m_pStateUav };
		// The fill and the strength in one group; then every pixel of the box in its own thread.
		Dispatch(pContext, m_pCSFill[k], m_pRemoveConstants, &rc, sizeof(rc), srvs, 3, uavs, 3, 1, 1);
		Dispatch(pContext, m_pCSRemove[k], m_pRemoveConstants, &rc, sizeof(rc), srvs, 3, uavs, 3,
			(rc.boxW + 15) / 16, (rc.boxH + 15) / 16);
	}
	pContext->CSSetShader(nullptr, nullptr, 0);
	if (newPicture) {
		ReadStrength(pContext);
	}
	return true;
}
