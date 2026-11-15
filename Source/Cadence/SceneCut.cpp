#include "stdafx.h"
#include <algorithm>
#include <iterator>
#include "../resource.h"
#include "../Helper.h"
#include "SceneCut.h"

namespace {

// The frames as the stabilizer makes its flow frames for DLSS Super Resolution
// (CDlssStabilizer, FlowFactor and ForDlssSR's flowBlur): about 960 pixels wide whatever
// the picture, each a box reaching a picture pixel past its own.
UINT FrameFactor(UINT height)
{
	return height >= 1440 ? 4 : (height >= 720 ? 2 : 1);
}
constexpr UINT kReach = 1;

HRESULT LoadShader(ID3D11Device* pDevice, UINT id, ID3D11ComputeShader** ppShader)
{
	LPVOID data = nullptr;
	DWORD size = 0;
	HRESULT hr = GetDataFromResource(data, size, id);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateComputeShader(data, size, nullptr, ppShader);
	}
	return hr;
}

} // namespace

HRESULT CSceneCut::Tex::Create(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format)
{
	Release();
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &pTex);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateShaderResourceView(pTex, nullptr, &pSrv);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateUnorderedAccessView(pTex, nullptr, &pUav);
	}
	if (FAILED(hr)) {
		Release();
	}
	return hr;
}

bool CSceneCut::Init(ID3D11Device* pDevice)
{
	if (IsReady()) {
		return true;
	}
	if (!pDevice) {
		return false;
	}
	HRESULT hr = LoadShader(pDevice, IDF_CS_11_SCENE_CUT_LUMA, &m_pCSLuma);
	if (SUCCEEDED(hr)) {
		hr = LoadShader(pDevice, IDF_CS_11_DLSS_SCENE_SUMS, &m_pCSSums);
	}
	if (SUCCEEDED(hr)) {
		hr = LoadShader(pDevice, IDF_CS_11_DLSS_SCENE_CUT, &m_pCSCut);
	}
	if (SUCCEEDED(hr)) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = pDevice->CreateBuffer(&desc, nullptr, &m_pLumaConstants);
	}
	if (SUCCEEDED(hr)) {
		const float constants[8] = { kHistogram, kRatio, kResidual, kCorrelation, kLightHist, kLightRatio, 0, 0 };
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = sizeof(constants);
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		const D3D11_SUBRESOURCE_DATA data = { constants, 0, 0 };
		hr = pDevice->CreateBuffer(&desc, &data, &m_pCutConstants);
	}
	if (SUCCEEDED(hr)) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 16 * sizeof(float);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		hr = pDevice->CreateBuffer(&desc, nullptr, &m_pState);
	}
	if (SUCCEEDED(hr)) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC target = {};
		target.Format = DXGI_FORMAT_R32_FLOAT;
		target.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		target.Buffer.NumElements = 16;
		hr = pDevice->CreateUnorderedAccessView(m_pState, &target, &m_pStateUav);
	}
	if (SUCCEEDED(hr)) {
		const float zero[4] = {};
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = 1;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA data = { zero, sizeof(zero), 0 };
		hr = pDevice->CreateTexture2D(&desc, &data, &m_pGlobal);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateShaderResourceView(m_pGlobal, nullptr, &m_pGlobalSrv);
	}
	for (int i = 0; i < kSlots && SUCCEEDED(hr); i++) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 16 * sizeof(float);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		hr = pDevice->CreateBuffer(&desc, nullptr, &m_slots[i].pStaging);
	}
	if (FAILED(hr)) {
		Release();
		return false;
	}
	Reset();
	return true;
}

void CSceneCut::ReleaseTextures()
{
	for (auto& t : m_frame) {
		t.Release();
	}
	for (auto& t : m_sums) {
		t.Release();
	}
	m_w = m_h = m_fw = m_fh = 0;
	m_bHaveLast = false;
}

void CSceneCut::Release()
{
	ReleaseTextures();
	for (auto& slot : m_slots) {
		slot.pStaging.Release();
		slot.bPending = false;
	}
	m_pGlobalSrv.Release();
	m_pGlobal.Release();
	m_pStateUav.Release();
	m_pState.Release();
	m_pCutConstants.Release();
	m_pLumaConstants.Release();
	m_pCSCut.Release();
	m_pCSSums.Release();
	m_pCSLuma.Release();
}

bool CSceneCut::Ensure(ID3D11Device* pDevice, UINT w, UINT h)
{
	if (w == m_w && h == m_h && m_frame[0].pTex) {
		return true;
	}
	ReleaseTextures();
	m_factor = FrameFactor(h);
	const UINT fw = w / m_factor, fh = h / m_factor;
	if (fw < 32 || fh < 32) {
		return false;
	}
	HRESULT hr = S_OK;
	for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
		hr = m_frame[i].Create(pDevice, fw, fh, DXGI_FORMAT_R8_UNORM);
	}
	for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
		hr = m_sums[i].Create(pDevice, (fw + 15) / 16, (fh + 15) / 16, DXGI_FORMAT_R32G32B32A32_FLOAT);
	}
	if (FAILED(hr)) {
		ReleaseTextures();
		return false;
	}
	m_w = w;
	m_h = h;
	m_fw = fw;
	m_fh = fh;
	m_current = 0;
	m_bHaveLast = false;
	m_bStale = true;
	return true;
}

bool CSceneCut::Measure(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture,
	UINT w, UINT h, UINT encoding, float whiteNits, uint64_t id)
{
	if (!IsReady() || !pContext || !pPicture || !Ensure(pDevice, w, h)) {
		return false;
	}
	// A compute pass sets no render target: the picture was just drawn into the back
	// buffer, and anything still bound would have its view dropped from what reads it.
	pContext->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11ShaderResourceView* noSrv[4] = {};
	ID3D11UnorderedAccessView* noUav[2] = {};

	const int cur = m_current, last = 1 - m_current;
	{
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(pContext->Map(m_pLumaConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		struct { UINT factor, reach, encoding; float white; } c = { m_factor, kReach, encoding, whiteNits > 0 ? whiteNits : 203.0f };
		memcpy(mapped.pData, &c, sizeof(c));
		pContext->Unmap(m_pLumaConstants, 0);
	}
	pContext->CSSetShader(m_pCSLuma, nullptr, 0);
	pContext->CSSetConstantBuffers(0, 1, &m_pLumaConstants.p);
	pContext->CSSetShaderResources(0, 1, &pPicture);
	pContext->CSSetUnorderedAccessViews(0, 1, &m_frame[cur].pUav.p, nullptr);
	pContext->Dispatch((m_fw + 7) / 8, (m_fh + 7) / 8, 1);
	pContext->CSSetShaderResources(0, 1, noSrv);
	pContext->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);

	// The first picture has nothing to be compared with, and gives no residual to keep.
	const bool bCompare = m_bHaveLast;
	if (bCompare) {
		if (m_bStale) {
			const FLOAT zero[4] = {};
			pContext->ClearUnorderedAccessViewFloat(m_pStateUav, zero);
			m_bStale = false;
		}
		ID3D11ShaderResourceView* sumViews[3] = { m_frame[cur].pSrv, m_frame[last].pSrv, m_pGlobalSrv };
		ID3D11UnorderedAccessView* sumTargets[2] = { m_sums[0].pUav, m_sums[1].pUav };
		pContext->CSSetShader(m_pCSSums, nullptr, 0);
		pContext->CSSetShaderResources(0, (UINT)std::size(sumViews), sumViews);
		pContext->CSSetUnorderedAccessViews(0, (UINT)std::size(sumTargets), sumTargets, nullptr);
		pContext->Dispatch((m_fw + 15) / 16, (m_fh + 15) / 16, 1);
		pContext->CSSetShaderResources(0, (UINT)std::size(sumViews), noSrv);
		pContext->CSSetUnorderedAccessViews(0, (UINT)std::size(sumTargets), noUav, nullptr);

		ID3D11ShaderResourceView* cutViews[4] = { m_frame[cur].pSrv, m_frame[last].pSrv, m_sums[0].pSrv, m_sums[1].pSrv };
		pContext->CSSetShader(m_pCSCut, nullptr, 0);
		pContext->CSSetConstantBuffers(0, 1, &m_pCutConstants.p);
		pContext->CSSetShaderResources(0, (UINT)std::size(cutViews), cutViews);
		pContext->CSSetUnorderedAccessViews(0, 1, &m_pStateUav.p, nullptr);
		pContext->Dispatch(1, 1, 1);
		pContext->CSSetShaderResources(0, (UINT)std::size(cutViews), noSrv);
		pContext->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);

		Slot& slot = m_slots[m_nextSlot];
		m_nextSlot = (m_nextSlot + 1) % kSlots;
		pContext->CopyResource(slot.pStaging, m_pState);
		slot.id = id;
		slot.bPending = true;
	}
	ID3D11Buffer* noCb = nullptr;
	pContext->CSSetConstantBuffers(0, 1, &noCb);
	pContext->CSSetShader(nullptr, nullptr, 0);

	m_current = last;
	m_bHaveLast = true;
	return bCompare;
}

int CSceneCut::Read(ID3D11DeviceContext* pContext, uint64_t id, float* state)
{
	for (Slot& slot : m_slots) {
		if (!slot.bPending || slot.id != id) {
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (pContext->Map(slot.pStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr) != S_OK) {
			return -1;
		}
		float values[16];
		memcpy(values, mr.pData, sizeof(values));
		pContext->Unmap(slot.pStaging, 0);
		slot.bPending = false;
		if (state) {
			memcpy(state, values, sizeof(values));
		}
		return values[0] > 0.5f ? 1 : 0;
	}
	return -1;
}
