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
#include <cmath>
#include <cstring>
#include <tuple>
#include "MpvShader.h"

namespace {

// The constants every translated pass reads (MpvGlobals in mpv_shaders.py): output
// size, input and target sizes, misc, then per bind its size and shift, then the
// parameters -- four floats each.
constexpr size_t kMaxBinds = 8;
constexpr size_t kMaxParams = 8;
constexpr size_t kConstants = 4 * (3 + kMaxBinds + kMaxBinds + kMaxParams);

std::vector<std::string> Words(const char* text)
{
	std::vector<std::string> words;
	if (text) {
		std::string word;
		for (const char* p = text; ; p++) {
			if (*p == ' ' || *p == '\0') {
				if (!word.empty()) {
					words.push_back(word);
					word.clear();
				}
				if (*p == '\0') {
					break;
				}
			} else {
				word += *p;
			}
		}
	}
	return words;
}

// libplacebo's size and condition expressions: reverse polish, floats only.
bool EvalRpn(const std::vector<std::string>& tokens, const std::function<bool(const std::string&, double&)>& lookup, double& result)
{
	double stack[16];
	size_t depth = 0;
	for (const std::string& tok : tokens) {
		if (tok == "!") {
			if (depth < 1) {
				return false;
			}
			stack[depth - 1] = (stack[depth - 1] == 0.0) ? 1.0 : 0.0;
			continue;
		}
		if (tok.size() == 1 && strchr("+-*/%<>=", tok[0])) {
			if (depth < 2) {
				return false;
			}
			const double b = stack[--depth];
			double& a = stack[depth - 1];
			switch (tok[0]) {
			case '+': a = a + b; break;
			case '-': a = a - b; break;
			case '*': a = a * b; break;
			case '/': a = a / b; break;
			case '%': a = std::fmod(a, b); break;
			case '>': a = (a > b) ? 1.0 : 0.0; break;
			case '<': a = (a < b) ? 1.0 : 0.0; break;
			case '=': a = (std::abs(a - b) <= 1e-6 * std::max(std::abs(a), std::abs(b))) ? 1.0 : 0.0; break;
			}
			continue;
		}
		if (depth == std::size(stack)) {
			return false;
		}
		char* end = nullptr;
		const double value = strtod(tok.c_str(), &end);
		if (end != tok.c_str() && *end == '\0') {
			stack[depth++] = value;
			continue;
		}
		if (!lookup(tok, stack[depth])) {
			return false;
		}
		depth++;
	}
	if (depth != 1) {
		return false;
	}
	result = stack[0];
	return true;
}

} // namespace

// CMpvShader::Texture

HRESULT CMpvShader::Texture::CheckCreate(ID3D11Device* pDevice, UINT w, UINT h)
{
	if (pTexture && width == w && height == h) {
		return S_OK;
	}
	Release();
	if (!pDevice || !w || !h) {
		return E_INVALIDARG;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // what libplacebo gives the passes
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	// Unordered access as well: which of the two a pass wants is not known here, and
	// the flag costs nothing on a texture no pass writes that way.
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &pTexture);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateShaderResourceView(pTexture, nullptr, &pShaderResource);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(pTexture, nullptr, &pRenderTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateUnorderedAccessView(pTexture, nullptr, &pUnorderedAccess);
	}
	if (FAILED(hr)) {
		Release();
		return hr;
	}
	width = w;
	height = h;
	return S_OK;
}

void CMpvShader::Texture::Release()
{
	pUnorderedAccess.Release();
	pRenderTarget.Release();
	pShaderResource.Release();
	pTexture.Release();
	width = 0;
	height = 0;
}

// CMpvShader

HRESULT CMpvShader::Load(ID3D11Device* pDevice, const MpvShaderInfo& info, UINT vertexShaderResid, const DataSource& source)
{
	Release();
	CheckPointer(pDevice, E_POINTER);
	if (pDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
		return E_NOTIMPL; // shader model 5
	}

	auto fail = [this](HRESULT hr) {
		Release();
		return FAILED(hr) ? hr : E_FAIL;
	};

	const BYTE* data = nullptr;
	size_t size = 0;
	if (!source(vertexShaderResid, data, size)) {
		return fail(E_FAIL);
	}
	HRESULT hr = pDevice->CreateVertexShader(data, size, nullptr, &m_pVertexShader);
	if (FAILED(hr)) {
		return fail(hr);
	}

	m_tables.resize(info.textureCount);
	for (UINT i = 0; i < info.textureCount; i++) {
		const MpvTextureInfo& t = info.textures[i];
		if (!source(t.resid, data, size) || size != 8 * (size_t)t.width * t.height) {
			return fail(E_FAIL);
		}
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = t.width;
		desc.Height = t.height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA init = { data, 8 * t.width, 0 };
		Texture& texture = m_tables[i].texture;
		hr = pDevice->CreateTexture2D(&desc, &init, &texture.pTexture);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreateShaderResourceView(texture.pTexture, nullptr, &texture.pShaderResource);
		}
		if (FAILED(hr)) {
			return fail(hr);
		}
		texture.width = t.width;
		texture.height = t.height;
		m_tables[i].linear = t.linear;
	}

	auto slotOf = [this](const std::string& name) {
		const auto it = std::find(m_saveNames.begin(), m_saveNames.end(), name);
		if (it != m_saveNames.end()) {
			return (int)(it - m_saveNames.begin());
		}
		m_saveNames.push_back(name);
		return (int)m_saveNames.size() - 1;
	};

	m_passes.resize(info.passCount);
	for (UINT i = 0; i < info.passCount; i++) {
		const MpvPassInfo& p = info.passes[i];
		Pass& pass = m_passes[i];
		pass.pInfo = &p;
		pass.hooks = Words(p.hooks);
		for (const std::string& name : Words(p.binds)) {
			Bind bind;
			int table = -1;
			for (UINT t = 0; t < info.textureCount; t++) {
				if (name == info.textures[t].name) {
					table = (int)t;
				}
			}
			if (name == "HOOKED" || std::find(pass.hooks.begin(), pass.hooks.end(), name) != pass.hooks.end()) {
				bind.kind = Bind::Plane;
			} else if (table >= 0) {
				bind.kind = Bind::Table;
				bind.index = table;
			} else {
				bind.kind = Bind::Saved;
				bind.index = slotOf(name);
			}
			pass.binds.push_back(bind);
		}
		if (pass.binds.size() > kMaxBinds) {
			return fail(E_FAIL);
		}
		pass.saveSlot = p.save ? slotOf(p.save) : -1;
		pass.when = Words(p.when);
		pass.width = Words(p.width);
		pass.height = Words(p.height);
		if (!source(p.resid, data, size)) {
			return fail(E_FAIL);
		}
		hr = p.blockW && p.blockH
			? pDevice->CreateComputeShader(data, size, nullptr, &pass.pCompute)
			: pDevice->CreatePixelShader(data, size, nullptr, &pass.pShader);
		if (FAILED(hr)) {
			return fail(hr);
		}
	}
	m_saved.assign(m_saveNames.size(), nullptr);

	const D3D11_BUFFER_DESC cbDesc = { (UINT)(kConstants * sizeof(float)), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	hr = pDevice->CreateBuffer(&cbDesc, nullptr, &m_pConstants);
	if (FAILED(hr)) {
		return fail(hr);
	}

	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	hr = pDevice->CreateSamplerState(&sampDesc, &m_pSamplerLinear);
	if (SUCCEEDED(hr)) {
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		hr = pDevice->CreateSamplerState(&sampDesc, &m_pSamplerPoint);
	}
	if (FAILED(hr)) {
		return fail(hr);
	}

	m_pDevice = pDevice;
	m_pInfo = &info;
	return S_OK;
}

CMpvShader::Texture* CMpvShader::Acquire(UINT w, UINT h)
{
	for (auto& pooled : m_pool) {
		if (!pooled->bInUse && pooled->texture.width == w && pooled->texture.height == h) {
			pooled->bInUse = true;
			return &pooled->texture;
		}
	}
	auto pooled = std::make_unique<Pooled>();
	if (FAILED(pooled->texture.CheckCreate(m_pDevice, w, h))) {
		return nullptr;
	}
	pooled->bInUse = true;
	m_pool.push_back(std::move(pooled));
	return &m_pool.back()->texture;
}

void CMpvShader::ReleaseToPool(const Texture* pTexture)
{
	for (auto& pooled : m_pool) {
		if (&pooled->texture == pTexture) {
			pooled->bInUse = false;
			return;
		}
	}
}

void CMpvShader::Release()
{
	m_pInfo = nullptr;
	m_pool.clear();
	m_passOutput.clear();
	std::fill(std::begin(m_poolFor), std::end(m_poolFor), 0u);
	m_passes.clear();
	m_tables.clear();
	m_saveNames.clear();
	m_saved.clear();
	m_pVertexShader.Release();
	m_pConstants.Release();
	m_pSamplerLinear.Release();
	m_pSamplerPoint.Release();
	m_pDevice.Release();
}

bool CMpvShader::MakePlan(UINT inW, UINT inH, UINT outW, UINT outH, Plan& plan) const
{
	const size_t count = m_passes.size();
	plan.run.assign(count, false);
	plan.w.assign(count, 0);
	plan.h.assign(count, 0);
	plan.lastReader.assign(count, -1);
	plan.lastPlanePass = -1;
	if (!inW || !inH || !outW || !outH) {
		return false;
	}

	UINT planeW = inW;
	UINT planeH = inH;
	std::vector<std::pair<UINT, UINT>> saved(m_saveNames.size());
	// Which pass last wrote what, to know when a texture is free again.
	std::vector<int> writerOfSlot(m_saveNames.size(), -1);
	int writerOfPlane = -1;

	// A shader's //!WHEN clause asks whether it is worth running at the size the
	// window happens to want: FSRCNNX and ArtCNN want 1.3x before they will start,
	// RAVU wants the picture to be growing at all. That is mpv's economy, and it is
	// not this renderer's: here the method the user picked in the list is the method
	// that runs, at any scale, including while the picture is being reduced -- which
	// is what "use the Upscaling method to reduce the frame to 50%" says it does.
	// Below those thresholds the network used to stand aside and Catmull-Rom took the
	// whole picture, with the list still naming the network.
	//
	// So the question is put at the network's own scale -- twice the plane, which is
	// what these shaders produce -- rather than at the window's. The two conditions
	// the shaders carry are both "is it worth it" and never "can it", so nothing
	// structural is bypassed, and OUTPUT is left alone everywhere else: some passes
	// size themselves from it, and those must still land on the real target.
	bool bAskingWhen = false;
	for (size_t i = 0; i < count; i++) {
		const Pass& pass = m_passes[i];
		auto lookup = [&](const std::string& token, double& value) {
			const size_t dot = token.find('.');
			if (dot == std::string::npos) {
				return false;
			}
			const std::string name = token.substr(0, dot);
			const std::string field = token.substr(dot + 1);
			const bool wide = (field == "w" || field == "width");
			if (!wide && field != "h" && field != "height") {
				return false;
			}
			UINT w = 0, h = 0;
			if (name == "OUTPUT") {
				w = bAskingWhen ? std::max(outW, inW * 2) : outW;
				h = bAskingWhen ? std::max(outH, inH * 2) : outH;
			} else if (name == "HOOKED" || std::find(pass.hooks.begin(), pass.hooks.end(), name) != pass.hooks.end()) {
				w = planeW;
				h = planeH;
			} else if (name == "NATIVE" || name == "NATIVE_CROPPED" || name == "MAIN" || name == "MAINPRESUB") {
				w = inW;
				h = inH;
			} else {
				const auto it = std::find(m_saveNames.begin(), m_saveNames.end(), name);
				if (it == m_saveNames.end() || !saved[it - m_saveNames.begin()].first) {
					return false;
				}
				std::tie(w, h) = saved[it - m_saveNames.begin()];
			}
			value = wide ? w : h;
			return true;
		};

		double when = 1.0;
		if (!pass.when.empty()) {
			bAskingWhen = true;
			const bool bRead = EvalRpn(pass.when, lookup, when);
			bAskingWhen = false;
			if (!bRead) {
				return false;
			}
		}
		if (when == 0.0) {
			continue;
		}
		double w = planeW;
		double h = planeH;
		if ((!pass.width.empty() && !EvalRpn(pass.width, lookup, w))
				|| (!pass.height.empty() && !EvalRpn(pass.height, lookup, h))) {
			return false;
		}
		const long lw = std::lround(w);
		const long lh = std::lround(h);
		if (lw < 1 || lh < 1 || lw > 16384 || lh > 16384) {
			return false;
		}
		for (const Bind& bind : pass.binds) {
			if (bind.kind == Bind::Saved && !saved[bind.index].first) {
				return false; // reads what no pass has saved yet
			}
		}

		plan.run[i] = true;
		plan.w[i] = (UINT)lw;
		plan.h[i] = (UINT)lh;
		for (const Bind& bind : pass.binds) {
			const int writer = (bind.kind == Bind::Saved) ? writerOfSlot[bind.index]
				: (bind.kind == Bind::Plane) ? writerOfPlane : -1;
			if (writer >= 0) {
				plan.lastReader[writer] = (int)i;
			}
		}
		if (pass.saveSlot >= 0) {
			saved[pass.saveSlot] = { (UINT)lw, (UINT)lh };
			writerOfSlot[pass.saveSlot] = (int)i;
		} else {
			planeW = (UINT)lw;
			planeH = (UINT)lh;
			writerOfPlane = (int)i;
			plan.lastPlanePass = (int)i;
		}
	}

	plan.planeW = planeW;
	plan.planeH = planeH;
	return plan.lastPlanePass >= 0;
}

bool CMpvShader::Applies(UINT inW, UINT inH, UINT outW, UINT outH, UINT* pW, UINT* pH) const
{
	if (!m_pInfo) {
		return false;
	}
	Plan plan;
	if (!MakePlan(inW, inH, outW, outH, plan)) {
		return false;
	}
	if (pW) {
		*pW = plan.planeW;
	}
	if (pH) {
		*pH = plan.planeH;
	}
	return true;
}

HRESULT CMpvShader::Process(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pInput, UINT inW, UINT inH,
	UINT outW, UINT outH, float shiftX, float shiftY, Texture& out)
{
	if (!m_pInfo) {
		return E_UNEXPECTED;
	}
	CheckPointer(pContext, E_POINTER);
	CheckPointer(pInput, E_POINTER);
	if (!MakePlan(inW, inH, outW, outH, m_plan)) {
		return S_FALSE;
	}
	HRESULT hr = out.CheckCreate(m_pDevice, m_plan.planeW, m_plan.planeH);
	if (FAILED(hr)) {
		return hr;
	}

	// The pool holds textures of the sizes the last picture needed; another size means
	// another picture, and none of them is of any use.
	const UINT sizes[4] = { inW, inH, outW, outH };
	if (!std::equal(std::begin(sizes), std::end(sizes), std::begin(m_poolFor))) {
		m_pool.clear();
		std::copy(std::begin(sizes), std::end(sizes), std::begin(m_poolFor));
	}
	for (auto& pooled : m_pool) {
		pooled->bInUse = false;
	}
	m_passOutput.assign(m_passes.size(), nullptr);

	// The caller has just drawn into the plane these passes read. While its render
	// target view is still bound, Direct3D drops the shader resource view of the same
	// texture, and a compute pass -- which sets no render target of its own -- would
	// read nothing but zeros.
	pContext->OMSetRenderTargets(0, nullptr, nullptr);

	pContext->IASetInputLayout(nullptr);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pContext->VSSetShader(m_pVertexShader, nullptr, 0);
	pContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	pContext->PSSetConstantBuffers(0, 1, &m_pConstants.p);
	pContext->CSSetConstantBuffers(0, 1, &m_pConstants.p);

	std::fill(m_saved.begin(), m_saved.end(), nullptr);
	ID3D11ShaderResourceView* pPlane = pInput;
	UINT planeW = inW;
	UINT planeH = inH;
	bool bOriginalPlane = true; // the shift describes the input only

	for (size_t i = 0; i < m_passes.size(); i++) {
		if (!m_plan.run[i]) {
			continue;
		}
		Pass& pass = m_passes[i];
		const UINT w = m_plan.w[i];
		const UINT h = m_plan.h[i];
		const bool bFinal = ((int)i == m_plan.lastPlanePass);
		Texture* pTarget = bFinal ? &out : Acquire(w, h);
		if (!pTarget) {
			return E_FAIL;
		}
		Texture& target = *pTarget;

		float constants[kConstants] = {};
		constants[0] = (float)w;
		constants[1] = (float)h;
		constants[2] = 1.0f / w;
		constants[3] = 1.0f / h;
		constants[4] = (float)inW;
		constants[5] = (float)inH;
		constants[6] = (float)outW;
		constants[7] = (float)outH;
		constants[10] = 0.5f; // random: fixed, no shader here uses it

		ID3D11ShaderResourceView* views[1 + kMaxBinds] = {};
		ID3D11SamplerState* samplers[1 + kMaxBinds] = {};
		for (size_t b = 0; b < pass.binds.size(); b++) {
			const Bind& bind = pass.binds[b];
			UINT tw = 0, th = 0;
			bool bLinear = true;
			switch (bind.kind) {
			case Bind::Plane:
				views[1 + b] = pPlane;
				tw = planeW;
				th = planeH;
				if (bOriginalPlane) {
					constants[4 * (3 + kMaxBinds + b)] = shiftX;
					constants[4 * (3 + kMaxBinds + b) + 1] = shiftY;
				}
				break;
			case Bind::Saved:
				if (!m_saved[bind.index]) {
					return E_FAIL;
				}
				views[1 + b] = m_saved[bind.index]->pShaderResource;
				tw = m_saved[bind.index]->width;
				th = m_saved[bind.index]->height;
				break;
			case Bind::Table:
				views[1 + b] = m_tables[bind.index].texture.pShaderResource;
				tw = m_tables[bind.index].texture.width;
				th = m_tables[bind.index].texture.height;
				bLinear = m_tables[bind.index].linear;
				break;
			}
			samplers[1 + b] = bLinear ? m_pSamplerLinear.p : m_pSamplerPoint.p;
			constants[4 * (3 + b)] = (float)tw;
			constants[4 * (3 + b) + 1] = (float)th;
			constants[4 * (3 + b) + 2] = 1.0f / tw;
			constants[4 * (3 + b) + 3] = 1.0f / th;
		}

		D3D11_MAPPED_SUBRESOURCE mr;
		hr = pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (FAILED(hr)) {
			return hr;
		}
		memcpy(mr.pData, constants, sizeof(constants));
		pContext->Unmap(m_pConstants, 0);

		const UINT slots = (UINT)(1 + pass.binds.size());
		ID3D11ShaderResourceView* noViews[1 + kMaxBinds] = {};
		if (pass.pCompute) {
			// One workgroup per block of output pixels, the last one partly outside:
			// the pass drops what falls past the edge itself.
			pContext->CSSetShader(pass.pCompute, nullptr, 0);
			pContext->CSSetShaderResources(0, slots, views);
			pContext->CSSetSamplers(0, slots, samplers);
			pContext->CSSetUnorderedAccessViews(0, 1, &target.pUnorderedAccess.p, nullptr);
			pContext->Dispatch((w + pass.pInfo->blockW - 1) / pass.pInfo->blockW,
				(h + pass.pInfo->blockH - 1) / pass.pInfo->blockH, 1);

			// A later pass reads this output: nothing may still be writing it.
			ID3D11UnorderedAccessView* noAccess[1] = {};
			pContext->CSSetUnorderedAccessViews(0, 1, noAccess, nullptr);
			pContext->CSSetShaderResources(0, slots, noViews);
		} else {
			const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
			pContext->OMSetRenderTargets(1, &target.pRenderTarget.p, nullptr);
			pContext->RSSetViewports(1, &viewport);
			pContext->PSSetShader(pass.pShader, nullptr, 0);
			pContext->PSSetShaderResources(0, slots, views);
			pContext->PSSetSamplers(0, slots, samplers);
			pContext->Draw(3, 0);

			// A later pass reads this output: nothing may hold it as a target then.
			pContext->PSSetShaderResources(0, slots, noViews);
			pContext->OMSetRenderTargets(0, nullptr, nullptr);
		}

		// What this pass was the last to read is free again.
		for (size_t j = 0; j < i; j++) {
			if (m_plan.lastReader[j] == (int)i && m_passOutput[j]) {
				ReleaseToPool(m_passOutput[j]);
				m_passOutput[j] = nullptr;
			}
		}
		m_passOutput[i] = bFinal ? nullptr : &target;

		if (pass.saveSlot >= 0) {
			m_saved[pass.saveSlot] = &target;
		} else {
			pPlane = target.pShaderResource;
			planeW = w;
			planeH = h;
			bOriginalPlane = false;
		}
	}

	return S_OK;
}
