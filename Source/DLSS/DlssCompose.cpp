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
#include "resource.h"
#include "Helper.h"
#include "DlssCompose.h"

namespace {

// What ps_dlss_compose.hlsl reads from b0, in its order.
struct Constants {
	float fullW, fullH, workW, workH;
	float sigma, epsilon, radius, plain;
	float srcW, srcH, dstW, dstH;
};

} // namespace

void CDlssCompose::WorkingSize(UINT fullW, UINT fullH, int percent, UINT& workW, UINT& workH)
{
	const auto Scale = [percent](UINT full) {
		const UINT even = 2 * (UINT)std::lround(full * std::clamp(percent, 1, 100) / 200.0);
		return std::min(full, std::max(even, std::min(full, 64u)));
	};
	workW = Scale(fullW);
	workH = Scale(fullH);
}

bool CDlssCompose::Matches(UINT fullW, UINT fullH, UINT workW, UINT workH, Method method) const
{
	return m_fullW == fullW && m_fullH == fullH && m_workW == workW && m_workH == workH && m_Method == method;
}

HRESULT CDlssCompose::Create(ID3D11Device* pDevice, UINT fullW, UINT fullH, UINT workW, UINT workH, Method method,
	ID3D11InputLayout* pInputLayout, ID3D11VertexShader* pVertexShader)
{
	if (Matches(fullW, fullH, workW, workH, method)) {
		return S_OK;
	}
	Release();
	if (!pDevice || !pInputLayout || !pVertexShader || !fullW || !fullH || !workW || !workH) {
		return E_INVALIDARG;
	}

	// The reduction always; the passes of the method only.
	std::vector<int> passes = { 7, 8 };
	switch (method) {
	case Method::Plain:
	case Method::Difference: passes.push_back(0); break;
	case Method::Joint:      passes.push_back(1); break;
	case Method::Guided:     passes.insert(passes.end(), { 2, 3, 4, 5, 6 }); break;
	}
	HRESULT hr = S_OK;
	for (const int pass : passes) {
		LPVOID data = nullptr;
		DWORD size = 0;
		hr = GetDataFromResource(data, size, IDF_PS_11_DLSS_COMPOSE_0 + pass);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreatePixelShader(data, size, nullptr, &m_pPS[pass]);
		}
		if (FAILED(hr)) {
			DLog(L"CDlssCompose::Create() : PASS {} failed with error {}", pass, HR2Str(hr));
			Release();
			return hr;
		}
	}

	hr = m_Pass.Create(pDevice, 16);
	if (SUCCEEDED(hr)) {
		hr = CPassTarget::Create(pDevice, workW, fullH, DXGI_FORMAT_R16G16B16A16_FLOAT, m_ReduceX);
	}
	if (SUCCEEDED(hr)) {
		hr = m_TexResult.Create(pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, fullW, fullH, Tex2D_DefaultShaderRTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(m_TexResult.pTexture, nullptr, &m_pResultTarget);
	}
	// The guided filter's sums want more than half floats: a variance is the difference
	// of two numbers about a quarter each, and on a smooth area it is a thousandth.
	if (SUCCEEDED(hr) && method == Method::Guided) {
		for (Target& t : m_Sums) {
			if (SUCCEEDED(hr)) {
				hr = CPassTarget::Create(pDevice, workW, workH, DXGI_FORMAT_R32G32B32A32_FLOAT, t);
			}
		}
		for (Target& t : m_AB) {
			if (SUCCEEDED(hr)) {
				hr = CPassTarget::Create(pDevice, workW, workH, DXGI_FORMAT_R32G32B32A32_FLOAT, t);
			}
		}
	}
	if (FAILED(hr)) {
		DLog(L"CDlssCompose::Create() : {}x{} from {}x{} failed with error {}", fullW, fullH, workW, workH, HR2Str(hr));
		Release();
		return hr;
	}

	m_pInputLayout  = pInputLayout;
	m_pVertexShader = pVertexShader;
	m_fullW = fullW;
	m_fullH = fullH;
	m_workW = workW;
	m_workH = workH;
	m_Method = method;
	return S_OK;
}

void CDlssCompose::Release()
{
	for (auto& ps : m_pPS) {
		ps.Release();
	}
	m_Pass.Release();
	m_ReduceX = Target{};
	for (Target& t : m_Sums) {
		t = Target{};
	}
	for (Target& t : m_AB) {
		t = Target{};
	}
	m_pResultTarget.Release();
	m_TexResult.Release();
	m_pInputLayout = nullptr;
	m_pVertexShader = nullptr;
	m_fullW = m_fullH = m_workW = m_workH = 0;
}

void CDlssCompose::Reduce(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture, ID3D11RenderTargetView* pNetworkInput)
{
	if (!IsCreated() || !pPicture || !pNetworkInput) {
		return;
	}
	Constants c = {};
	c.fullW = (float)m_fullW; c.fullH = (float)m_fullH;
	c.workW = (float)m_workW; c.workH = (float)m_workH;

	c.srcW = (float)m_fullW; c.srcH = (float)m_fullH;
	c.dstW = (float)m_workW; c.dstH = (float)m_fullH;
	Draw(pContext, m_pPS[7], { m_ReduceX.pRenderTarget }, m_workW, m_fullH, { pPicture }, &c.fullW, sizeof(c) / sizeof(float));

	c.srcW = (float)m_workW; c.srcH = (float)m_fullH;
	c.dstW = (float)m_workW; c.dstH = (float)m_workH;
	Draw(pContext, m_pPS[8], { pNetworkInput }, m_workW, m_workH, { m_ReduceX.pShaderResource }, &c.fullW, sizeof(c) / sizeof(float));
}

void CDlssCompose::Compose(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture,
	ID3D11ShaderResourceView* pNetworkInput, ID3D11ShaderResourceView* pNetworkOutput, const Tuning& tuning)
{
	if (!IsCreated() || !pPicture || !pNetworkInput || !pNetworkOutput) {
		return;
	}
	Constants c = {};
	c.fullW = (float)m_fullW; c.fullH = (float)m_fullH;
	c.workW = (float)m_workW; c.workH = (float)m_workH;
	c.sigma = tuning.sigma;
	c.epsilon = tuning.epsilon;
	c.radius = (float)std::clamp(tuning.radius, 1, 16);
	const size_t count = sizeof(c) / sizeof(float);

	switch (m_Method) {
	case Method::Plain:
	case Method::Difference:
		c.plain = (m_Method == Method::Plain) ? 1.0f : 0.0f;
		Draw(pContext, m_pPS[0], { m_pResultTarget }, m_fullW, m_fullH, { pPicture, pNetworkInput, pNetworkOutput }, &c.fullW, count);
		break;
	case Method::Joint:
		Draw(pContext, m_pPS[1], { m_pResultTarget }, m_fullW, m_fullH, { pPicture, pNetworkInput, pNetworkOutput }, &c.fullW, count);
		break;
	case Method::Guided:
		Draw(pContext, m_pPS[2], { m_Sums[0].pRenderTarget, m_Sums[1].pRenderTarget, m_Sums[2].pRenderTarget, m_Sums[3].pRenderTarget },
			m_workW, m_workH, { nullptr, pNetworkInput, pNetworkOutput }, &c.fullW, count);
		Draw(pContext, m_pPS[3], { m_AB[0].pRenderTarget, m_AB[1].pRenderTarget }, m_workW, m_workH,
			{ m_Sums[0].pShaderResource, m_Sums[1].pShaderResource, m_Sums[2].pShaderResource, m_Sums[3].pShaderResource }, &c.fullW, count);
		Draw(pContext, m_pPS[4], { m_Sums[0].pRenderTarget, m_Sums[1].pRenderTarget }, m_workW, m_workH,
			{ m_AB[0].pShaderResource, m_AB[1].pShaderResource }, &c.fullW, count);
		Draw(pContext, m_pPS[5], { m_AB[0].pRenderTarget, m_AB[1].pRenderTarget }, m_workW, m_workH,
			{ m_Sums[0].pShaderResource, m_Sums[1].pShaderResource }, &c.fullW, count);
		Draw(pContext, m_pPS[6], { m_pResultTarget }, m_fullW, m_fullH,
			{ pPicture, m_AB[0].pShaderResource, m_AB[1].pShaderResource }, &c.fullW, count);
		break;
	}
}

void CDlssCompose::Draw(ID3D11DeviceContext* pContext, ID3D11PixelShader* pShader,
	std::initializer_list<ID3D11RenderTargetView*> targets, UINT width, UINT height,
	std::initializer_list<ID3D11ShaderResourceView*> inputs, const float* constants, size_t count)
{
	if (!pShader) {
		return;
	}
	// The passes read with Load: no sampler has a say.
	m_Pass.Draw(pContext, m_pInputLayout, m_pVertexShader, pShader, targets, width, height, inputs, {}, constants, count);
}
