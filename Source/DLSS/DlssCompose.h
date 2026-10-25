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

#include <initializer_list>
#include "DX11Helper.h"
#include "DlssPass.h"

// CDlssCompose
//
// DLSS 5 NR at a working size smaller than the picture. The network costs in
// proportion to the pixels it is given -- 62 ms at 3840x2160 on an RTX 4060, longer
// than a frame of film lasts -- so the picture is reduced, the network runs on that,
// and what it changed is carried back onto the picture at its own size
// (Shaders/d3d11/ps_dlss_compose.hlsl). The picture keeps its own fine detail and
// grain; the network's change reaches it down to the detail the working size holds.
//
// The ways of carrying the change back are measured against the network run at the
// full size in tools/dlssnr_probe --tnrscale, which runs this class as the filter does.
// The filter runs Difference: 53.7 dB from the full-size result at 75 % and 48.2 at 50 %
// on a 4K film frame, where the picture left alone is 42.6 and the output enlarged
// 46.6 and 42.4, with the grain lost. Joint came within 0.13 dB of it either way for the
// same cost; Guided took the grain away. The filter embeds Difference's passes only.

class CDlssCompose
{
public:
	enum class Method {
		Plain,        // the network's output enlarged: what reducing alone would give, for comparison
		Difference,   // out - in enlarged by Catmull-Rom, added to the picture
		Joint,        // out - in by joint bilateral upsampling, guided by the picture
		Guided,       // the guided filter: out = a * in + b fitted at the working size, applied to the picture
	};

	struct Tuning {
		float sigma   = 0.05f;    // Joint: how far apart two colours may be and still count
		float epsilon = 0.001f;   // Guided: the regularisation
		int   radius  = 2;        // Guided: the box, working pixels on each side
	};

	// The working size for a picture at percent of its size: even, at least 64 pixels
	// each way, and never more than the picture.
	static void WorkingSize(UINT fullW, UINT fullH, int percent, UINT& workW, UINT& workH);

	// Everything for these sizes and this method; nothing if they already match. The
	// renderer's input layout and vertex shader are used for every pass and must outlive
	// this object's resources.
	HRESULT Create(ID3D11Device* pDevice, UINT fullW, UINT fullH, UINT workW, UINT workH, Method method,
		ID3D11InputLayout* pInputLayout, ID3D11VertexShader* pVertexShader);
	void Release();

	bool Matches(UINT fullW, UINT fullH, UINT workW, UINT workH, Method method) const;
	bool IsCreated() const { return m_fullW != 0; }
	UINT WorkWidth() const { return m_workW; }
	UINT WorkHeight() const { return m_workH; }

	// The picture, the whole of a fullW x fullH texture, reduced into the network's input.
	void Reduce(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture, ID3D11RenderTargetView* pNetworkInput);

	// The network's change, from its input to its output at the working size, carried onto
	// the picture: into GetResult().
	void Compose(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture,
		ID3D11ShaderResourceView* pNetworkInput, ID3D11ShaderResourceView* pNetworkOutput,
		const Tuning& tuning = Tuning());

	Tex2D_t* GetResult() { return &m_TexResult; }

private:
	using Target = CPassTarget;

	void Draw(ID3D11DeviceContext* pContext, ID3D11PixelShader* pShader,
		std::initializer_list<ID3D11RenderTargetView*> targets, UINT width, UINT height,
		std::initializer_list<ID3D11ShaderResourceView*> inputs, const float* constants, size_t count);

	CComPtr<ID3D11PixelShader> m_pPS[9];   // PASS 0 to 8 of ps_dlss_compose.hlsl, those the method needs
	CQuadPass                  m_Pass;   // the constants and the quad every pass draws with

	Target  m_ReduceX;                     // RGBA16F, reduced across: workW x fullH
	Target  m_Sums[4];                     // RGBA32F at the working size, Guided only
	Target  m_AB[2];                       // RGBA32F at the working size, Guided only
	Tex2D_t m_TexResult;                   // RGBA16F, the picture with the change: fullW x fullH
	CComPtr<ID3D11RenderTargetView> m_pResultTarget;

	ID3D11InputLayout*  m_pInputLayout  = nullptr;
	ID3D11VertexShader* m_pVertexShader = nullptr;

	UINT   m_fullW = 0, m_fullH = 0;
	UINT   m_workW = 0, m_workH = 0;
	Method m_Method = Method::Difference;
};
