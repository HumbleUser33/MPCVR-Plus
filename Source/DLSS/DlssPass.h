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

#pragma once

#include <algorithm>
#include <initializer_list>
#include <d3d11.h>
#include <atlbase.h>

// What the passes around DLSS draw with: textures that are a render target and a
// shader resource at once, and one quad over the whole of a target, drawn with the
// renderer's input layout and vertex shader. CDlssStabilizer and CDlssCompose each
// had a copy of both.

struct CPassTarget
{
	CComPtr<ID3D11Texture2D>          pTexture;
	CComPtr<ID3D11ShaderResourceView> pShaderResource;
	CComPtr<ID3D11RenderTargetView>   pRenderTarget;

	static HRESULT Create(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format, CPassTarget& target)
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

		target = CPassTarget{};
		HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &target.pTexture);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateShaderResourceView(target.pTexture, nullptr, &target.pShaderResource);
		}
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateRenderTargetView(target.pTexture, nullptr, &target.pRenderTarget);
		}
		return hr;
	}
};

class CQuadPass
{
public:
	static constexpr UINT kMaxTargets = 4;
	static constexpr UINT kMaxInputs  = 8;

	// floats: how many the passes' constants hold, four at a time.
	HRESULT Create(ID3D11Device* pDevice, UINT floats)
	{
		Release();
		m_floats = (floats + 3) & ~3u;
		D3D11_BUFFER_DESC desc = { m_floats * (UINT)sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		HRESULT hr = pDevice->CreateBuffer(&desc, nullptr, &m_pConstants);
		if (SUCCEEDED(hr)) {
			// FillVertices for an unrotated, unflipped blit of a whole texture: the
			// winding the renderer draws with, which survives back-face culling. The
			// layout of the renderer's VERTEX.
			static const float quad[4][5] = {
				{ -1, -1, 0,  0, 1 },
				{ -1, +1, 0,  0, 0 },
				{ +1, -1, 0,  1, 1 },
				{ +1, +1, 0,  1, 0 },
			};
			desc = { sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
			const D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
			hr = pDevice->CreateBuffer(&desc, &init, &m_pQuad);
		}
		if (FAILED(hr)) {
			Release();
		}
		return hr;
	}

	void Release()
	{
		m_pConstants.Release();
		m_pQuad.Release();
		m_floats = 0;
	}

	// The constants, for a compute pass that maps them itself.
	ID3D11Buffer* Constants() const { return m_pConstants; }

	// The constants written, the rest of the block zero.
	void SetConstants(ID3D11DeviceContext* pContext, const float* constants, size_t count)
	{
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (constants && count && SUCCEEDED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			float* block = (float*)mr.pData;
			std::fill_n(block, m_floats, 0.0f);
			std::copy_n(constants, std::min<size_t>(count, m_floats), block);
			pContext->Unmap(m_pConstants, 0);
		}
	}

	// Up to four targets, all width x height, and eight inputs; the samplers go to s0
	// and on, and none are set when none are given. What the pass writes may still be
	// bound for reading from the pass before, and is unbound first; everything is
	// unbound after, so the next pass can write what this one read.
	void Draw(ID3D11DeviceContext* pContext, ID3D11InputLayout* pInputLayout, ID3D11VertexShader* pVertexShader,
		ID3D11PixelShader* pShader, std::initializer_list<ID3D11RenderTargetView*> targets, UINT width, UINT height,
		std::initializer_list<ID3D11ShaderResourceView*> inputs, std::initializer_list<ID3D11SamplerState*> samplers,
		const float* constants, size_t count)
	{
		if (!pShader) {
			return;
		}
		ID3D11RenderTargetView* rtvs[kMaxTargets] = {};
		const UINT numTargets = (UINT)std::min<size_t>(targets.size(), kMaxTargets);
		std::copy_n(targets.begin(), numTargets, rtvs);
		ID3D11ShaderResourceView* srvs[kMaxInputs] = {};
		std::copy_n(inputs.begin(), std::min<size_t>(inputs.size(), kMaxInputs), srvs);

		SetConstants(pContext, constants, count);

		ID3D11ShaderResourceView* noViews[kMaxInputs] = {};
		pContext->PSSetShaderResources(0, kMaxInputs, noViews);

		const D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)width, (FLOAT)height, 0, 1 };
		const UINT stride = 5 * sizeof(float);
		const UINT offset = 0;
		ID3D11Buffer* pConstants = m_pConstants;
		ID3D11Buffer* pQuad = m_pQuad;

		pContext->IASetInputLayout(pInputLayout);
		pContext->OMSetRenderTargets(numTargets, rtvs, nullptr);
		pContext->RSSetViewports(1, &viewport);
		pContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
		pContext->VSSetShader(pVertexShader, nullptr, 0);
		pContext->PSSetShader(pShader, nullptr, 0);
		pContext->PSSetShaderResources(0, kMaxInputs, srvs);
		if (samplers.size()) {
			pContext->PSSetSamplers(0, (UINT)samplers.size(), samplers.begin());
		}
		pContext->PSSetConstantBuffers(0, 1, &pConstants);
		pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		pContext->IASetVertexBuffers(0, 1, &pQuad, &stride, &offset);
		pContext->Draw(4, 0);

		pContext->PSSetShaderResources(0, kMaxInputs, noViews);
		ID3D11RenderTargetView* noTargets[kMaxTargets] = {};
		pContext->OMSetRenderTargets(kMaxTargets, noTargets, nullptr);
	}

private:
	CComPtr<ID3D11Buffer> m_pConstants;
	CComPtr<ID3D11Buffer> m_pQuad;
	UINT m_floats = 0;
};
