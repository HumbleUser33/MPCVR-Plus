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
#include "DlssStabilizer.h"

namespace {

// Measured in tools/dlssnr_probe --tstab.
constexpr float kTolerance    = 0.02f;   // how far the history may stray from the current 3x3 neighbourhood
constexpr float kReprojection = 0.03f;   // input luma error where trust in a vector starts to fall
constexpr float kConsistency  = 1.0f;    // forward/backward mismatch, flow pixels, where trust starts to fall
constexpr float kCostLow      = 48.0f;   // matching cost where trust starts to fall
constexpr float kCostHigh     = 160.0f;  // and where it is gone
constexpr float kTrustedShare = 0.05f;   // fewer trusted blocks than this share: no global motion (--tsrstill)

// The layout of the renderer's VERTEX.
struct QuadVertex {
	float x, y, z;
	float u, v;
};

// About 540 lines of flow whatever the video: 1.6 ms on an RTX 3050 (--tflow).
UINT FlowFactor(UINT height)
{
	return height >= 1440 ? 4 : (height >= 720 ? 2 : 1);
}

} // namespace

CDlssStabilizer::FlowSettings CDlssStabilizer::ForDlssSR()
{
	FlowSettings flow;
	flow.bidirectional     = true;
	flow.cost              = true;
	flow.flowBlur          = 1;
	flow.snap              = true;
	flow.snapLow           = 0.75f;
	flow.snapHigh          = 1.5f;
	flow.snapGate          = 2;
	flow.snapDeadZone      = 0.4f;
	flow.vectorRadius      = 3;
	flow.vectorSigma       = 1.0f;
	flow.vectorShift       = 3;
	flow.vectorSupportLow  = 0.1f;
	flow.vectorSupportHigh = 0.3f;
	flow.vectorEvidence    = 4.0f;
	flow.vectorTemporal    = 0.3f;
	flow.vectorRefine      = 3;
	flow.vectorReach       = 4.0f;
	flow.vectorTexture     = 0.02f;
	return flow;
}

HRESULT CDlssStabilizer::CreateTarget(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format, Target& target)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width            = width;
	desc.Height           = height;
	desc.MipLevels        = 1;
	desc.ArraySize        = 1;
	desc.Format           = format;
	desc.SampleDesc.Count = 1;
	desc.Usage            = D3D11_USAGE_DEFAULT;
	desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	target = Target{};
	HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &target.pTexture);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateShaderResourceView(target.pTexture, nullptr, &target.pShaderResource);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(target.pTexture, nullptr, &target.pRenderTarget);
	}
	return hr;
}

bool CDlssStabilizer::Matches(UINT width, UINT height, bool bMotionOnly, const FlowSettings& flow) const
{
	return m_width == width && m_height == height && m_bMotionOnly == bMotionOnly
		&& m_FlowSettings == flow && m_pPSStabilize;
}

HRESULT CDlssStabilizer::CreateResources(ID3D11Device* pDevice, UINT width, UINT height, bool bMotionOnly, const FlowSettings& flow)
{
	HRESULT hr = S_OK;
	const struct { UINT resid; ID3D11PixelShader** ppShader; } shaders[] = {
		{ IDF_PS_11_DLSS_STAB_FLOWFRAME,  &m_pPSFlowFrame  },
		{ IDF_PS_11_DLSS_STAB_FLOWMOTION, &m_pPSFlowMotion },
		{ IDF_PS_11_DLSS_STAB_STABILIZE,  &m_pPSStabilize  },
	};
	for (const auto& s : shaders) {
		LPVOID data = nullptr;
		DWORD size = 0;
		hr = GetDataFromResource(data, size, s.resid);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreatePixelShader(data, size, nullptr, s.ppShader);
		}
		if (FAILED(hr)) {
			DLog(L"CDlssStabilizer::Create() : shader {} failed with error {}", s.resid, HR2Str(hr));
			return hr;
		}
	}

	D3D11_BUFFER_DESC bufferDesc = { 32 * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	hr = pDevice->CreateBuffer(&bufferDesc, nullptr, &m_pConstants);
	if (SUCCEEDED(hr)) {
		// FillVertices for an unrotated, unflipped blit of a whole texture: the
		// winding the renderer draws with, which survives back-face culling.
		static const QuadVertex quad[4] = {
			{ -1, -1, 0,  0, 1 },
			{ -1, +1, 0,  0, 0 },
			{ +1, -1, 0,  1, 1 },
			{ +1, +1, 0,  1, 0 },
		};
		bufferDesc = { sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
		const D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
		hr = pDevice->CreateBuffer(&bufferDesc, &init, &m_pQuad);
	}

	if (SUCCEEDED(hr)) {
		hr = m_TexMotion.CheckCreate(pDevice, DXGI_FORMAT_R16G16_FLOAT, width, height, Tex2D_DefaultShaderRTargetUAVShared);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(m_TexMotion.pTexture, nullptr, &m_pMotionTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = CreateTarget(pDevice, width, height, DXGI_FORMAT_R8_UNORM, m_Confidence);
	}
	if (bMotionOnly) {
		if (SUCCEEDED(hr) && flow.snap) {
			// The global motion, measured on the GPU and read there: nothing waits for it.
			LPVOID data = nullptr;
			DWORD size = 0;
			hr = GetDataFromResource(data, size, IDF_CS_11_DLSS_GLOBAL_MOTION);
			if (SUCCEEDED(hr)) {
				hr = pDevice->CreateComputeShader(data, size, nullptr, &m_pCSGlobalMotion);
			}
			if (SUCCEEDED(hr)) {
				hr = GetDataFromResource(data, size, IDF_PS_11_DLSS_STAB_SNAPMOTION);
			}
			if (SUCCEEDED(hr)) {
				hr = pDevice->CreatePixelShader(data, size, nullptr, &m_pPSSnapMotion);
			}
			if (SUCCEEDED(hr)) {
				hr = GetDataFromResource(data, size, IDF_PS_11_DLSS_STAB_BLOCKMOTION);
			}
			if (SUCCEEDED(hr)) {
				hr = pDevice->CreatePixelShader(data, size, nullptr, &m_pPSBlockMotion);
			}
			if (SUCCEEDED(hr)) {
				D3D11_TEXTURE2D_DESC desc = {};
				desc.Width            = 1;
				desc.Height           = 1;
				desc.MipLevels        = 1;
				desc.ArraySize        = 1;
				desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
				desc.SampleDesc.Count = 1;
				desc.Usage            = D3D11_USAGE_DEFAULT;
				desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				hr = pDevice->CreateTexture2D(&desc, nullptr, &m_pGlobalMotion);
			}
			if (SUCCEEDED(hr)) {
				hr = pDevice->CreateShaderResourceView(m_pGlobalMotion, nullptr, &m_pGlobalMotionView);
			}
			if (SUCCEEDED(hr)) {
				hr = pDevice->CreateUnorderedAccessView(m_pGlobalMotion, nullptr, &m_pGlobalMotionTarget);
			}
			DLogIf(FAILED(hr), L"CDlssStabilizer::Create() : the global motion failed with error {}", HR2Str(hr));
		}
		return hr;
	}
	for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
		hr = CreateTarget(pDevice, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_History[i]);
		if (SUCCEEDED(hr)) {
			hr = CreateTarget(pDevice, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_Input[i]);
		}
	}
	if (SUCCEEDED(hr)) {
		hr = m_TexResult.CheckCreate(pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, Tex2D_DefaultShaderRTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(m_TexResult.pTexture, nullptr, &m_pResultTarget);
	}
	return hr;
}

HRESULT CDlssStabilizer::Create(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, UINT width, UINT height,
	ID3D11InputLayout* pInputLayout, ID3D11VertexShader* pVertexShader,
	ID3D11SamplerState* pSamplerPoint, ID3D11SamplerState* pSamplerLinear, bool bMotionOnly, const FlowSettings& flow)
{
	CheckPointer(pDevice, E_POINTER);
	CheckPointer(pContext, E_POINTER);
	if (Matches(width, height, bMotionOnly, flow)) {
		return S_OK;
	}
	// What failed once fails again: no new attempt on every picture until the size
	// changes, or the owner calls Release.
	if (width == m_failedWidth && height == m_failedHeight) {
		return E_FAIL;
	}
	Release();
	if (width < 16 || height < 16) {
		return E_INVALIDARG;
	}

	HRESULT hr = CreateResources(pDevice, width, height, bMotionOnly, flow);
	if (FAILED(hr)) {
		DLog(L"CDlssStabilizer::Create() : {}x{} failed with error {}", width, height, HR2Str(hr));
		Release();
		m_failedWidth  = width;
		m_failedHeight = height;
		return hr;
	}

	m_pDevice        = pDevice;
	m_pInputLayout   = pInputLayout;
	m_pVertexShader  = pVertexShader;
	m_pSamplerPoint  = pSamplerPoint;
	m_pSamplerLinear = pSamplerLinear;
	m_width          = width;
	m_height         = height;
	m_bMotionOnly    = bMotionOnly;
	m_FlowSettings   = flow;

	m_flowFactor = flow.flowFactor ? std::clamp(flow.flowFactor, 1u, 4u) : FlowFactor(height);
	CDlssOpticalFlow::Options options;
	options.gridSize      = flow.gridSize;
	options.perfLevel     = flow.perfLevel;
	options.bidirectional = flow.bidirectional;
	options.cost          = flow.cost;
	options.temporalHints = flow.temporalHints;
	m_bFlow = m_Flow.Init(pDevice, pContext, width / m_flowFactor, height / m_flowFactor, options);
	if (m_bFlow && m_pPSBlockMotion) {
		// PASS 4's blocks, one pixel per vector, known once the engine has its grid.
		for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
			hr = CreateTarget(pDevice, m_Flow.OutputWidth(), m_Flow.OutputHeight(), DXGI_FORMAT_R32G32B32A32_FLOAT, m_BlockField[i]);
		}
		if (FAILED(hr)) {
			Release();
			m_failedWidth  = width;
			m_failedHeight = height;
			return hr;
		}
	}

	if (m_bFlow) {
		m_status = std::format(L"Optical Flow {}x{}", m_Flow.Width(), m_Flow.Height());
	} else {
		// Without motion the history cannot be trusted anywhere: every picture goes
		// through as the network left it.
		m_status = std::format(L"{} (Optical Flow: {})", bMotionOnly ? L"no motion vectors" : L"nothing steadied",
			m_Flow.GetStatusLine());
	}

	const FLOAT zero[4] = { 0, 0, 0, 0 };
	pContext->ClearRenderTargetView(m_pMotionTarget, zero);
	pContext->ClearRenderTargetView(m_Confidence.pRenderTarget, zero);
	Reset();
	return S_OK;
}

void CDlssStabilizer::Release()
{
	m_Flow.Release();
	m_bFlow = false;
	m_pPSFlowFrame.Release();
	m_pPSFlowMotion.Release();
	m_pPSStabilize.Release();
	m_pPSSnapMotion.Release();
	m_pPSBlockMotion.Release();
	m_BlockField[0] = Target{};
	m_BlockField[1] = Target{};
	m_iBlockField = 0;
	m_bBlockHistory = false;
	m_pCSGlobalMotion.Release();
	m_pGlobalMotionTarget.Release();
	m_pGlobalMotionView.Release();
	m_pGlobalMotion.Release();
	m_pConstants.Release();
	m_pQuad.Release();
	m_pMotionTarget.Release();
	m_TexMotion.Release();
	m_Confidence = Target{};
	m_History[0] = Target{};
	m_History[1] = Target{};
	m_Input[0] = Target{};
	m_Input[1] = Target{};
	m_pResultTarget.Release();
	m_TexResult.Release();
	m_pDevice.Release();
	m_pInputLayout   = nullptr;
	m_pVertexShader  = nullptr;
	m_pSamplerPoint  = nullptr;
	m_pSamplerLinear = nullptr;
	m_width  = 0;
	m_height = 0;
	m_bMotionOnly   = false;
	m_FlowSettings  = FlowSettings();
	m_iHistory      = 0;
	m_iInput        = 0;
	m_bHistoryValid = false;
	m_bHaveMotion   = false;
	m_bHaveResult   = false;
	m_bLastReset    = true;
	m_failedWidth   = 0;
	m_failedHeight  = 0;
	m_status.clear();
}

void CDlssStabilizer::Reset()
{
	m_Flow.Reset();
	m_bBlockHistory = false;
	m_bHistoryValid = false;
	m_bHaveMotion   = false;
	m_bHaveResult   = false;
}

ID3D11Texture2D* CDlssStabilizer::GetMotionVectors() const
{
	return (m_width && m_bFlow) ? m_TexMotion.pTexture.p : nullptr;
}

void CDlssStabilizer::Draw(ID3D11DeviceContext* pContext, ID3D11PixelShader* pShader,
	std::initializer_list<ID3D11RenderTargetView*> targets, UINT width, UINT height,
	std::initializer_list<ID3D11ShaderResourceView*> inputs, const float* constants, size_t count)
{
	ID3D11RenderTargetView* rtvs[2] = {};
	const UINT numTargets = (UINT)std::min<size_t>(targets.size(), std::size(rtvs));
	std::copy_n(targets.begin(), numTargets, rtvs);
	ID3D11ShaderResourceView* srvs[8] = {};
	std::copy_n(inputs.begin(), std::min<size_t>(inputs.size(), std::size(srvs)), srvs);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (constants && count && SUCCEEDED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
		float block[32] = {};
		std::copy_n(constants, std::min<size_t>(count, std::size(block)), block);
		memcpy(mr.pData, block, sizeof(block));
		pContext->Unmap(m_pConstants, 0);
	}

	const D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)width, (FLOAT)height, 0, 1 };
	const UINT stride = sizeof(QuadVertex);
	const UINT offset = 0;
	ID3D11SamplerState* samplers[2] = { m_pSamplerPoint, m_pSamplerLinear };
	ID3D11Buffer* pConstants = m_pConstants;
	ID3D11Buffer* pQuad = m_pQuad;

	pContext->IASetInputLayout(m_pInputLayout);
	pContext->OMSetRenderTargets(numTargets, rtvs, nullptr);
	pContext->RSSetViewports(1, &viewport);
	pContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	pContext->VSSetShader(m_pVertexShader, nullptr, 0);
	pContext->PSSetShader(pShader, nullptr, 0);
	pContext->PSSetShaderResources(0, (UINT)std::size(srvs), srvs);
	pContext->PSSetSamplers(0, (UINT)std::size(samplers), samplers);
	pContext->PSSetConstantBuffers(0, 1, &pConstants);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	pContext->IASetVertexBuffers(0, 1, &pQuad, &stride, &offset);
	pContext->Draw(4, 0);

	// Unbound, so the next pass can write what this one read.
	ID3D11ShaderResourceView* noViews[8] = {};
	pContext->PSSetShaderResources(0, (UINT)std::size(noViews), noViews);
	ID3D11RenderTargetView* noTargets[2] = {};
	pContext->OMSetRenderTargets((UINT)std::size(noTargets), noTargets, nullptr);
}

void CDlssStabilizer::MeasureGlobalMotion(ID3D11DeviceContext* pContext)
{
	// Only the blocks PASS 1 would trust count, by the same measures.
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (SUCCEEDED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
		const float constants[32] = {
			(float)m_Flow.GridSize(), kConsistency, kCostLow, kCostHigh,
			(float)m_Flow.Width(), (float)m_Flow.Height(), m_Flow.Bidirectional() ? 1.0f : 0.0f, m_Flow.HasCost() ? 1.0f : 0.0f,
			kTrustedShare
		};
		memcpy(mr.pData, constants, sizeof(constants));
		pContext->Unmap(m_pConstants, 0);
	}
	ID3D11ShaderResourceView* views[3] = { m_Flow.ForwardFlow(), m_Flow.BackwardFlow(), m_Flow.ForwardCost() };
	ID3D11UnorderedAccessView* pTarget = m_pGlobalMotionTarget;
	ID3D11Buffer* pConstants = m_pConstants;
	pContext->CSSetShader(m_pCSGlobalMotion, nullptr, 0);
	pContext->CSSetShaderResources(0, (UINT)std::size(views), views);
	pContext->CSSetConstantBuffers(0, 1, &pConstants);
	pContext->CSSetUnorderedAccessViews(0, 1, &pTarget, nullptr);
	pContext->Dispatch(1, 1, 1);

	// Unbound, so the next pass can read what this one wrote.
	ID3D11ShaderResourceView* noViews[3] = {};
	ID3D11UnorderedAccessView* noTarget = nullptr;
	ID3D11Buffer* noConstants = nullptr;
	pContext->CSSetShaderResources(0, (UINT)std::size(noViews), noViews);
	pContext->CSSetUnorderedAccessViews(0, 1, &noTarget, nullptr);
	pContext->CSSetConstantBuffers(0, 1, &noConstants);
	pContext->CSSetShader(nullptr, nullptr, 0);
}

void CDlssStabilizer::PrepareMotion(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pInput)
{
	if (!m_width || !pContext || !pInput) {
		return;
	}

	if (m_bFlow) {
		const float frameConstants[4] = { (float)m_flowFactor, (float)m_FlowSettings.flowBlur, 0, 0 };
		Draw(pContext, m_pPSFlowFrame, { m_Flow.FrameTarget() }, m_Flow.Width(), m_Flow.Height(),
			{ pInput }, frameConstants, std::size(frameConstants));

		m_bHaveMotion = m_Flow.Execute();
		if (m_bHaveMotion && m_bMotionOnly && m_pPSSnapMotion && m_BlockField[0].pTexture) {
			// DLSS Super Resolution: the global motion, the blocks drawn to it and
			// steadied (PASS 4), then blended into the working size (PASS 3).
			MeasureGlobalMotion(pContext);
			const float scaleX = (float)m_width / m_Flow.Width(), scaleY = (float)m_height / m_Flow.Height();
			const int write = 1 - m_iBlockField;
			const float blockConstants[28] = {
				scaleX, scaleY, (float)m_Flow.GridSize(), kConsistency,
				(float)m_Flow.Width(), (float)m_Flow.Height(), kCostLow, kCostHigh,
				m_Flow.Bidirectional() ? 1.0f : 0.0f, m_Flow.HasCost() ? 1.0f : 0.0f, m_FlowSettings.snapLow, m_FlowSettings.snapHigh,
				(float)m_FlowSettings.snapGate, m_FlowSettings.snapDeadZone, (float)m_FlowSettings.vectorRadius, m_FlowSettings.vectorSigma,
				m_FlowSettings.vectorTemporal, m_bBlockHistory ? 1.0f : 0.0f, (float)m_FlowSettings.vectorShift, m_FlowSettings.vectorSupportLow,
				m_FlowSettings.vectorSupportHigh, (float)m_FlowSettings.vectorRefine, m_FlowSettings.vectorReach, m_FlowSettings.vectorTexture,
				m_FlowSettings.vectorEvidence, 0, 0, 0
			};
			Draw(pContext, m_pPSBlockMotion, { m_BlockField[write].pRenderTarget }, m_Flow.OutputWidth(), m_Flow.OutputHeight(),
				{ m_Flow.ForwardFlow(), m_Flow.BackwardFlow(), m_Flow.ForwardCost(), m_pGlobalMotionView, m_BlockField[m_iBlockField].pShaderResource,
				  m_Flow.CurrentFrame(), m_Flow.PreviousFrame() },
				blockConstants, std::size(blockConstants));
			const float snapConstants[4] = { scaleX, scaleY, (float)m_Flow.GridSize(), m_FlowSettings.snapDeadZone };
			Draw(pContext, m_pPSSnapMotion, { m_pMotionTarget }, m_width, m_height,
				{ m_BlockField[write].pShaderResource, m_pGlobalMotionView }, snapConstants, std::size(snapConstants));
			m_iBlockField = write;
			m_bBlockHistory = true;
			return;
		}
		if (m_bHaveMotion) {
			const float motionConstants[12] = {
				(float)m_width / m_Flow.Width(), (float)m_height / m_Flow.Height(), (float)m_Flow.GridSize(), kConsistency,
				(float)m_Flow.Width(), (float)m_Flow.Height(), kCostLow, kCostHigh,
				m_Flow.Bidirectional() ? 1.0f : 0.0f, m_Flow.HasCost() ? 1.0f : 0.0f, 0, 0
			};
			Draw(pContext, m_pPSFlowMotion, { m_pMotionTarget, m_Confidence.pRenderTarget }, m_width, m_height,
				{ m_Flow.ForwardFlow(), m_Flow.BackwardFlow(), m_Flow.ForwardCost() }, motionConstants, std::size(motionConstants));
			return;
		}

		// No flow for this picture: no motion for the network, no trust in the history.
		m_bBlockHistory = false;
		const FLOAT zero[4] = { 0, 0, 0, 0 };
		pContext->ClearRenderTargetView(m_pMotionTarget, zero);
		pContext->ClearRenderTargetView(m_Confidence.pRenderTarget, zero);
	}
	// Without Optical Flow there is no motion at all, and Stabilize starts the
	// history over on every picture: what the network left goes through as it is.
}

void CDlssStabilizer::Stabilize(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pInput,
	ID3D11ShaderResourceView* pNetwork, float strength, bool bNewPicture)
{
	if (!m_width || !pContext || !pInput || !pNetwork || m_bMotionOnly) {
		return;
	}

	// A new picture reads the last history and input and writes the other slots.
	// A redraw reads what its picture read and rewrites that picture's slots, so
	// both move on with the pictures only.
	const bool bAdvance = bNewPicture || !m_bHaveResult;
	if (bAdvance) {
		m_bLastReset = !m_bHistoryValid || !m_bHaveMotion;
	}
	const int iRead     = bAdvance ? m_iHistory : 1 - m_iHistory;
	const int iPrevious = bAdvance ? m_iInput : 1 - m_iInput;

	const float constants[8] = {
		1.0f - 0.75f * std::clamp(strength, 0.0f, 1.0f),     // weight of the current frame where trusted
		kTolerance,
		0.0f,
		kReprojection,
		m_bLastReset ? 1.0f : 0.0f,                           // no history for this picture
		0.0f,                                                 // EFFECT: only out - in is steadied
		0.0f,
		0.0f
	};

	Draw(pContext, m_pPSStabilize, { m_History[1 - iRead].pRenderTarget, m_pResultTarget }, m_width, m_height,
		{ pInput, pNetwork, m_History[iRead].pShaderResource, m_TexMotion.pShaderResource, m_Input[iPrevious].pShaderResource,
		  m_Confidence.pShaderResource },
		constants, std::size(constants));

	// This input is the previous one for the next picture's reprojection check.
	const int iStore = 1 - iPrevious;
	CComPtr<ID3D11Resource> pInputResource;
	pInput->GetResource(&pInputResource);
	if (pInputResource) {
		pContext->CopyResource(m_Input[iStore].pTexture, pInputResource);
	}

	m_iHistory = 1 - iRead;
	m_iInput = iStore;
	m_bHistoryValid = true;
	m_bHaveResult = true;
}
