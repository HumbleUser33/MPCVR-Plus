// SPDX-License-Identifier: MIT
// From Echo-Storm/ls-addon-manager, revision 8d3eac351d42480326290b0f8965127a630b44f2
// (engine/flow_estimator.cpp, engine/flow_fix.cpp), Copyright (c) 2026 Echo-Storm and
// andreiday; the licence is EchoStorm-LICENSE.txt beside this file.

#include "stdafx.h"
#include <algorithm>
#include <format>
#include "../../resource.h"
#include "../../Helper.h"
#include "EchoFlow.h"

namespace {

// The estimator's constants (cs_echo_flow.hlsl), EchoStorm's own.
struct FlowConstants {
	UINT w, h, gw, gh, cw, ch, radius, flags;
	float lambda, bias, stability, strayCap, fastMotion, meanWeight, gradWeight;
	UINT inScale;
};
static_assert(sizeof(FlowConstants) == 64);

// FlowFix's (cs_echo_flowfix.hlsl).
struct FixConstants {
	UINT fw, fh, cw, ch, tx, ty, tile, reach, encoding;
	float white;
	UINT cs, S, fullW, fullH, fixOn, rules;
};
static_assert(sizeof(FixConstants) == 64);

constexpr float kLambda = 0.003f;   // match cost per pixel of straying from the coarser size's guess
constexpr float kBias = 0.004f;     // a pixel's extra cost for another block's vector (or none) over its own
constexpr float kStrayCap = 2.0f;   // pixels of each size past which straying costs no more
constexpr UINT kFixRules = 7;       // FlowFix's three rules, all on: plain cells, repeating cells, "clearly better"

UINT Groups(UINT n, UINT size) { return (n + size - 1) / size; }

} // namespace

void (*CEchoFlow::s_passHook)(ID3D11DeviceContext*, int, bool) = nullptr;

HRESULT CEchoFlow::Tex::Create(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format)
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

bool CEchoFlow::Init(ID3D11Device* pDevice)
{
	if (IsReady()) {
		return true;
	}
	static const UINT ids[ShaderCount] = {
		IDF_CS_11_ECHO_LUMA, IDF_CS_11_ECHO_DOWN, IDF_CS_11_ECHO_GLOBAL, IDF_CS_11_ECHO_SEARCH, IDF_CS_11_ECHO_MEDIAN,
		IDF_CS_11_ECHO_PIXEL, IDF_CS_11_ECHO_FIX_LUMA, IDF_CS_11_ECHO_FIX_TILES, IDF_CS_11_ECHO_FIX_DECIDE, IDF_CS_11_ECHO_FIX_APPLY
	};
	HRESULT hr = S_OK;
	for (int i = 0; i < ShaderCount && SUCCEEDED(hr); i++) {
		LPVOID data = nullptr;
		DWORD size = 0;
		hr = GetDataFromResource(data, size, ids[i]);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateComputeShader(data, size, nullptr, &m_pCS[i]);
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
		m_status = std::format(L"EchoStorm's shaders could not be made, 0x{:08X}", (unsigned)hr);
		return false;
	}
	m_status.clear();
	return true;
}

void CEchoFlow::ReleaseTextures()
{
	for (auto& set : m_luma) {
		for (auto& t : set) {
			t.Release();
		}
	}
	for (auto& t : m_grid) {
		t.Release();
	}
	m_filtered.Release();
	m_global.Release();
	m_motion.Release();
	m_distrust.Release();
	m_fixLumaA.Release();
	m_fixLumaC.Release();
	m_modes.Release();
	m_cells.Release();
	m_out.Release();
	m_width = m_height = 0;
	m_levels = 0;
	m_bPrevious = false;
}

void CEchoFlow::Release()
{
	ReleaseTextures();
	for (auto& cs : m_pCS) {
		cs.Release();
	}
	m_pConstants.Release();
	m_pLinear.Release();
}

bool CEchoFlow::Ensure(ID3D11Device* pDevice, UINT width, UINT height)
{
	const UINT scale = m_wantScale ? m_wantScale : width > 2560 ? 2 : 1;
	if (width == m_width && height == m_height && scale == m_scale && m_out.pTex) {
		return true;
	}
	ReleaseTextures();
	if (!width || !height) {
		return false;
	}

	// The analysis size, and the sizes of the pyramid: halved while the next is still at
	// least 64x32, and down to half size at least.
	m_scale = scale;
	m_lw[0] = (width + m_scale - 1) / m_scale;
	m_lh[0] = (height + m_scale - 1) / m_scale;
	m_levels = 1;
	while (m_levels < kMaxLevels) {
		const UINT nw = (m_lw[m_levels - 1] + 1) / 2, nh = (m_lh[m_levels - 1] + 1) / 2;
		if (m_levels >= 2 && (nw < 64 || nh < 32)) {
			break;
		}
		m_lw[m_levels] = nw;
		m_lh[m_levels] = nh;
		m_levels++;
	}
	for (int k = 0; k < m_levels; k++) {
		m_gw[k] = Groups(m_lw[k], 4);
		m_gh[k] = Groups(m_lh[k], 4);
	}

	// FlowFix's cells and tiles, at the analysis size.
	const UINT aw = m_lw[0], ah = m_lh[0];
	m_cs = aw >= 1920 ? 4 : 2;
	m_cw = Groups(aw, m_cs);
	m_ch = Groups(ah, m_cs);
	m_tile = aw >= 1920 ? 64 : 32;
	m_tx = Groups(aw, m_tile);
	m_ty = Groups(ah, m_tile);

	HRESULT hr = S_OK;
	for (int s = 0; s < 2 && SUCCEEDED(hr); s++) {
		for (int k = 0; k < m_levels && SUCCEEDED(hr); k++) {
			hr = m_luma[s][k].Create(pDevice, m_lw[k], m_lh[k], DXGI_FORMAT_R16_FLOAT);
		}
	}
	for (int k = 1; k < m_levels && SUCCEEDED(hr); k++) {
		hr = m_grid[k].Create(pDevice, m_gw[k], m_gh[k], DXGI_FORMAT_R16G16_FLOAT);
	}
	if (SUCCEEDED(hr)) hr = m_filtered.Create(pDevice, m_gw[1], m_gh[1], DXGI_FORMAT_R16G16_FLOAT);
	if (SUCCEEDED(hr)) hr = m_global.Create(pDevice, 2 * kGlobalRadius + 1, 2 * kGlobalRadius + 1, DXGI_FORMAT_R32_FLOAT);
	if (SUCCEEDED(hr)) hr = m_motion.Create(pDevice, aw, ah, DXGI_FORMAT_R16G16_FLOAT);
	if (SUCCEEDED(hr)) hr = m_distrust.Create(pDevice, aw, ah, DXGI_FORMAT_R8_UNORM);
	if (SUCCEEDED(hr)) hr = m_fixLumaA.Create(pDevice, m_cw, m_ch, DXGI_FORMAT_R32_FLOAT);
	if (SUCCEEDED(hr)) hr = m_fixLumaC.Create(pDevice, m_cw, m_ch, DXGI_FORMAT_R32_FLOAT);
	if (SUCCEEDED(hr)) hr = m_modes.Create(pDevice, m_tx, m_ty, DXGI_FORMAT_R16G16B16A16_FLOAT);
	if (SUCCEEDED(hr)) hr = m_cells.Create(pDevice, m_cw, m_ch, DXGI_FORMAT_R16G16B16A16_FLOAT);
	if (SUCCEEDED(hr)) hr = m_out.Create(pDevice, width, height, DXGI_FORMAT_R16G16_FLOAT);
	if (FAILED(hr)) {
		ReleaseTextures();
		m_status = std::format(L"EchoStorm's textures for {}x{} could not be made, 0x{:08X}", width, height, (unsigned)hr);
		return false;
	}
	m_width = width;
	m_height = height;
	m_current = 0;
	m_bPrevious = false;
	m_status = std::format(L"EchoStorm, {}x{} analysed at {}x{}, {} sizes", width, height, aw, ah, m_levels);
	return true;
}

void CEchoFlow::Run(ID3D11DeviceContext* pContext, int shader, std::initializer_list<ID3D11ShaderResourceView*> srvs,
	std::initializer_list<ID3D11UnorderedAccessView*> uavs, const void* constants, UINT size, UINT groupsX, UINT groupsY)
{
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		return;
	}
	memcpy(mapped.pData, constants, size);
	pContext->Unmap(m_pConstants, 0);

	ID3D11ShaderResourceView* srv[8] = {};
	std::copy(srvs.begin(), srvs.end(), srv);
	ID3D11UnorderedAccessView* uav[5] = {};
	std::copy(uavs.begin(), uavs.end(), uav);
	pContext->CSSetShader(m_pCS[shader], nullptr, 0);
	pContext->CSSetConstantBuffers(0, 1, &m_pConstants.p);
	pContext->CSSetSamplers(0, 1, &m_pLinear.p);
	pContext->CSSetShaderResources(0, 8, srv);
	pContext->CSSetUnorderedAccessViews(0, 5, uav, nullptr);
	if (s_passHook) {
		s_passHook(pContext, shader, false);
	}
	pContext->Dispatch(groupsX, groupsY, 1);
	if (s_passHook) {
		s_passHook(pContext, shader, true);
	}

	// Unbound: what a pass writes the next one reads.
	ID3D11ShaderResourceView* noSrv[8] = {};
	ID3D11UnorderedAccessView* noUav[5] = {};
	pContext->CSSetShaderResources(0, 8, noSrv);
	pContext->CSSetUnorderedAccessViews(0, 5, noUav, nullptr);
}

// The frame's pyramid, into the slot of the frame now (m_current).
void CEchoFlow::Pyramid(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pFrame, UINT encoding, float white)
{
	const int cur = m_current;
	{
		FlowConstants c = { m_lw[0], m_lh[0] };
		c.radius = encoding;
		c.lambda = white;
		c.inScale = m_scale;
		Run(pContext, Luma, { pFrame }, { m_luma[cur][0].pUav }, &c, sizeof(c), Groups(m_lw[0], 8), Groups(m_lh[0], 8));
	}
	for (int k = 1; k < m_levels; k++) {
		const FlowConstants c = { m_lw[k], m_lh[k], m_lw[k - 1], m_lh[k - 1] };
		Run(pContext, Down, { m_luma[cur][k - 1].pSrv }, { m_luma[cur][k].pUav }, &c, sizeof(c), Groups(m_lw[k], 8), Groups(m_lh[k], 8));
	}
}

void CEchoFlow::Prime(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pFrame, UINT encoding, float whiteNits)
{
	if (!IsReady() || !m_out.pTex || !pFrame) {
		return;
	}
	pContext->OMSetRenderTargets(0, nullptr, nullptr);
	Pyramid(pContext, pFrame, encoding, whiteNits > 0 ? whiteNits : 80.0f);
	m_current = 1 - m_current;
	m_bPrevious = true;
}

void CEchoFlow::Measure(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pFrame, ID3D11ShaderResourceView* pBefore,
	UINT encoding, float whiteNits)
{
	if (!IsReady() || !m_out.pTex || !pFrame) {
		return;
	}
	if (!pBefore) {
		m_bPrevious = false;
	}
	// A compute pass sets no render target: whatever was drawn last is let go of, or its
	// view would be dropped from the passes that read it.
	pContext->OMSetRenderTargets(0, nullptr, nullptr);

	const int cur = m_current, prev = 1 - m_current;
	const float white = whiteNits > 0 ? whiteNits : 80.0f;
	Pyramid(pContext, pFrame, encoding, white);

	if (m_bPrevious) {
		const int top = m_levels - 1;
		// The whole picture's shift, at the smallest size.
		{
			const UINT n = 2 * kGlobalRadius + 1;
			const FlowConstants c = { m_lw[top], m_lh[top], 0, 0, 0, 0, (UINT)kGlobalRadius };
			Run(pContext, Global, { m_luma[cur][top].pSrv, m_luma[prev][top].pSrv }, { m_global.pUav }, &c, sizeof(c), Groups(n, 8), Groups(n, 8));
		}
		// The search, coarse to fine.
		for (int k = top; k >= 1; k--) {
			const bool bCoarser = k + 1 < m_levels;
			const FlowConstants c = {
				m_lw[k], m_lh[k], m_gw[k], m_gh[k],
				bCoarser ? m_gw[k + 1] : (UINT)kGlobalRadius, bCoarser ? m_gh[k + 1] : 0u,
				bCoarser ? 2u : 4u, (bCoarser ? 1u : 4u) | (k == 1 ? 2u : 0u),
				kLambda, kBias, 0.0f, kStrayCap, 0.0f, 1.0f, 0.0f
			};
			Run(pContext, Search, { m_luma[cur][k].pSrv, m_luma[prev][k].pSrv, bCoarser ? m_grid[k + 1].pSrv.p : nullptr, bCoarser ? nullptr : m_global.pSrv.p },
				{ m_grid[k].pUav }, &c, sizeof(c), Groups(m_gw[k], 8), Groups(m_gh[k], 8));
		}
		const FlowConstants c = { m_gw[1], m_gh[1] };
		Run(pContext, Median, { m_grid[1].pSrv }, { m_filtered.pUav }, &c, sizeof(c), Groups(m_gw[1], 8), Groups(m_gh[1], 8));
	}
	// Every pixel at the analysis size; zero without a frame before.
	{
		const FlowConstants c = { m_lw[0], m_lh[0], m_gw[1], m_gh[1], 0, 0, 0, m_bPrevious ? 1u : 0u, kLambda, kBias, 0.0f, 0.0f, 0.0f };
		Run(pContext, Pixel, { m_luma[cur][0].pSrv, m_luma[prev][0].pSrv, m_filtered.pSrv }, { m_motion.pUav, m_distrust.pUav },
			&c, sizeof(c), Groups(m_lw[0], 8), Groups(m_lh[0], 8));
	}

	// FlowFix, and the vectors brought to the frames' size. Without a frame before there is
	// nothing to correct: the zeros are only taken up.
	const bool bFix = m_bPrevious;
	const FixConstants f = {
		m_lw[0], m_lh[0], m_cw, m_ch, m_tx, m_ty, m_tile, m_lw[0] >= 1920 ? 2u : 3u, encoding, white,
		m_cs, m_scale, m_width, m_height, bFix ? 1u : 0u, kFixRules
	};
	if (bFix) {
		Run(pContext, FixLuma, { pBefore, pFrame }, { m_fixLumaA.pUav, m_fixLumaC.pUav }, &f, sizeof(f), Groups(m_cw, 8), Groups(m_ch, 8));
		Run(pContext, FixTiles, { nullptr, nullptr, m_motion.pSrv, m_distrust.pSrv }, { nullptr, nullptr, m_modes.pUav }, &f, sizeof(f), m_tx, m_ty);
		Run(pContext, FixDecide, { nullptr, nullptr, m_motion.pSrv, nullptr, m_fixLumaA.pSrv, m_fixLumaC.pSrv, m_modes.pSrv },
			{ nullptr, nullptr, nullptr, m_cells.pUav }, &f, sizeof(f), Groups(m_cw, 8), Groups(m_ch, 8));
	}
	Run(pContext, FixApply, { nullptr, nullptr, m_motion.pSrv, nullptr, nullptr, nullptr, nullptr, m_cells.pSrv },
		{ nullptr, nullptr, nullptr, nullptr, m_out.pUav }, &f, sizeof(f), Groups(m_width, 8), Groups(m_height, 8));

	m_current = prev;
	m_bPrevious = true;
}
