// SPDX-License-Identifier: MIT
// From Echo-Storm/ls-addon-manager, revision 8d3eac351d42480326290b0f8965127a630b44f2
// (engine/own_synth.cpp), Copyright (c) 2026 Echo-Storm and andreiday; the licence is
// EchoStorm-LICENSE.txt beside this file.

#include "stdafx.h"
#include <algorithm>
#include <format>
#include "../../resource.h"
#include "../../Helper.h"
#include "EchoSynth11.h"

namespace {

// The shaders' constants (cs_echo_synth.hlsl), EchoStorm's own; the buffer is 64 bytes,
// the 60 of these and padding.
struct Constants {
	uint32_t w, h, fw, fh, mode, phase;
	float t, k;
	uint32_t encoding;
	float white;
	uint32_t scale;
	float soft, marks, grain;
	uint32_t seed;
};
static_assert(sizeof(Constants) == 60);

} // namespace

bool CEchoSynth11::Init(ID3D11Device* pDevice)
{
	if (IsReady()) {
		return true;
	}
	if (!pDevice || pDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
		m_status = L"EchoFG needs Direct3D feature level 11_1";
		return false;
	}
	static const UINT ids[EntryCount] = {
		IDF_CS_11_ECHO_SYNTH_CLEAR, IDF_CS_11_ECHO_SYNTH_ERR0, IDF_CS_11_ECHO_SYNTH_BOX5, IDF_CS_11_ECHO_SYNTH_ERRA,
		IDF_CS_11_ECHO_SYNTH_SPLAT, IDF_CS_11_ECHO_SYNTH_HYP1, IDF_CS_11_ECHO_SYNTH_HYP2, IDF_CS_11_ECHO_SYNTH_HOLES,
		IDF_CS_11_ECHO_SYNTH_SYNTH, IDF_CS_11_ECHO_SYNTH_MARKPREP, IDF_CS_11_ECHO_SYNTH_MARKLARGE, IDF_CS_11_ECHO_SYNTH_MARKUPDATE,
		IDF_CS_11_ECHO_SYNTH_GRAINSTAT
	};
	HRESULT hr = S_OK;
	for (int e = 0; e < EntryCount && SUCCEEDED(hr); e++) {
		LPVOID data = nullptr;
		DWORD size = 0;
		hr = GetDataFromResource(data, size, ids[e]);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateComputeShader(data, size, nullptr, &m_pCS[e]);
		}
	}
	if (SUCCEEDED(hr)) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 64;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = pDevice->CreateBuffer(&desc, nullptr, &m_pConstants);
	}
	if (SUCCEEDED(hr)) {
		D3D11_SAMPLER_DESC desc = {};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		hr = pDevice->CreateSamplerState(&desc, &m_pLinear);
	}
	if (FAILED(hr)) {
		Release();
		m_status = std::format(L"EchoFG's shaders could not be made, 0x{:08X}", (unsigned)hr);
		return false;
	}
	m_status.clear();
	return true;
}

void CEchoSynth11::ReleaseBuffers()
{
	for (int b = 0; b < kBuffers; b++) {
		m_pBufUav[b].Release();
		m_pBuf[b].Release();
	}
	m_w = m_h = m_aw = m_ah = 0;
}

void CEchoSynth11::Release()
{
	ReleaseBuffers();
	for (auto& cs : m_pCS) {
		cs.Release();
	}
	m_pConstants.Release();
	m_pLinear.Release();
}

bool CEchoSynth11::Ensure(ID3D11Device* pDevice, const uint32_t w, const uint32_t h)
{
	if (w == m_w && h == m_h && m_pBuf[0]) {
		return true;
	}
	ReleaseBuffers();
	if (!IsReady() || !w || !h) {
		return false;
	}
	const uint32_t aw = (w + kScale - 1) / kScale, ah = (h + kScale - 1) / kScale;
	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = aw * ah * 4;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = 4;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = aw * ah;
	HRESULT hr = S_OK;
	for (int b = 0; b < kBuffers && SUCCEEDED(hr); b++) {
		hr = pDevice->CreateBuffer(&desc, nullptr, &m_pBuf[b]);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateUnorderedAccessView(m_pBuf[b], &uav, &m_pBufUav[b]);
		}
	}
	if (FAILED(hr)) {
		ReleaseBuffers();
		m_status = std::format(L"EchoFG's buffers for {}x{} could not be made, 0x{:08X}", w, h, (unsigned)hr);
		return false;
	}
	m_w = w;
	m_h = h;
	m_aw = aw;
	m_ah = ah;
	m_status = std::format(L"EchoFG, {}x{} in cells of {}", w, h, kScale);
	return true;
}

bool CEchoSynth11::Make(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pBefore, ID3D11ShaderResourceView* pNow,
	ID3D11ShaderResourceView* pMotion, ID3D11UnorderedAccessView* pOut, const float t,
	const uint32_t encoding, const float whiteNits, const float soften, const float grain)
{
	if (!IsReady() || !m_pBuf[0] || !pContext || !pBefore || !pNow || !pMotion || !pOut) {
		return false;
	}
	// A compute pass sets no render target: what was drawn last is let go of, or its view
	// would be dropped from the passes that read it.
	pContext->OMSetRenderTargets(0, nullptr, nullptr);

	ID3D11ShaderResourceView* srvs[4] = { pBefore, pNow, pMotion, nullptr };
	ID3D11UnorderedAccessView* uavs[kBuffers + 1] = {};
	for (int b = 0; b < kBuffers; b++) {
		uavs[b] = m_pBufUav[b];
	}
	uavs[kBuffers] = pOut;
	pContext->CSSetShaderResources(0, (UINT)std::size(srvs), srvs);
	pContext->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);
	pContext->CSSetConstantBuffers(0, 1, &m_pConstants.p);
	pContext->CSSetSamplers(0, 1, &m_pLinear.p);

	const UINT gx = (m_aw + 7) / 8, gy = (m_ah + 7) / 8, fx = (m_w + 7) / 8, fy = (m_h + 7) / 8;
	const bool bGrain = grain > 0.001f && encoding == 0u;
	++m_seed;
	float now = 0.5f;   // the pair's passes run at the middle, as CEchoSynth's
	// Direct3D 11 orders a dispatch after the one before that wrote the same views: every
	// pass reads what the one before wrote without a barrier of its own.
	auto pass = [&](Entry e, uint32_t mode, uint32_t phase, float k, bool bFull = false) {
		const Constants c = { m_aw, m_ah, m_w, m_h, mode, phase, now, k, encoding, whiteNits > 0.0f ? whiteNits : 80.0f, kScale,
			std::clamp(soften, 0.0f, 1.0f), 0.0f, bGrain ? std::clamp(grain, 0.0f, 1.0f) : 0.0f, m_seed };
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		memcpy(mapped.pData, &c, sizeof(c));
		pContext->Unmap(m_pConstants, 0);
		pContext->CSSetShader(m_pCS[e], nullptr, 0);
		pContext->Dispatch(bFull ? fx : gx, bFull ? fy : gy, 1);
		return true;
	};
	bool bOk = true;
	// Once for the pair: the errors and the vectors onto a's cells.
	if (bGrain) {
		bOk = bOk && pass(GrainStat, 0, 0, 0.0f);
	}
	bOk = bOk && pass(Clear, 0, 0, 0.0f);
	bOk = bOk && pass(Err0, 0, 0, 0.0f);
	bOk = bOk && pass(Box5, 0, 0, 0.0f);
	bOk = bOk && pass(Splat, 1, 0, 1.0f);
	bOk = bOk && pass(Splat, 1, 1, 1.0f);
	bOk = bOk && pass(ErrA, 0, 0, 0.0f);
	bOk = bOk && pass(Box5, 1, 0, 0.0f);
	// Then the frame made, at its own time.
	now = std::clamp(t, 0.0f, 1.0f);
	bOk = bOk && pass(Clear, 1, 0, 0.0f);
	bOk = bOk && pass(Splat, 0, 0, 1.0f - now);   // c's vectors to the frame made
	bOk = bOk && pass(Splat, 0, 1, 1.0f - now);
	bOk = bOk && pass(Splat, 2, 0, now);          // a's reversed vectors to the frame made
	bOk = bOk && pass(Splat, 2, 1, now);
	bOk = bOk && pass(Hyp1, 0, 0, 0.0f);
	bOk = bOk && pass(Hyp2, 0, 0, 0.0f);
	bOk = bOk && pass(Holes, 0, 0, 0.0f);
	bOk = bOk && pass(Synth, 0, 0, 0.0f, true);

	ID3D11ShaderResourceView* noSrv[4] = {};
	ID3D11UnorderedAccessView* noUav[kBuffers + 1] = {};
	ID3D11Buffer* noCb = nullptr;
	pContext->CSSetShaderResources(0, (UINT)std::size(noSrv), noSrv);
	pContext->CSSetUnorderedAccessViews(0, (UINT)std::size(noUav), noUav, nullptr);
	pContext->CSSetConstantBuffers(0, 1, &noCb);
	pContext->CSSetShader(nullptr, nullptr, 0);
	return bOk;
}
