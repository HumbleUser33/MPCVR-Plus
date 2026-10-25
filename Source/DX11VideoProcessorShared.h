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

// What the files holding this fork's parts of CDX11VideoProcessor (DX11VideoProcessorDlss.cpp,
// ...Mpv.cpp, ...Present.cpp and ...Stats.cpp) share with DX11VideoProcessor.cpp, which they
// were taken out of. Upstream's two drawing helpers are defined there, and lost their
// static for this alone.

HRESULT FillVertexBuffer(ID3D11DeviceContext* pDeviceContext, ID3D11Buffer* pVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip);

void TextureBlt11(
	ID3D11DeviceContext* pDeviceContext,
	ID3D11RenderTargetView* pRenderTargetView, D3D11_VIEWPORT& viewport,
	ID3D11InputLayout* pInputLayout,
	ID3D11VertexShader* pVertexShader,
	ID3D11PixelShader* pPixelShader,
	ID3D11ShaderResourceView* pShaderResourceViews,
	ID3D11SamplerState* pSampler,
	ID3D11Buffer* pConstantBuffer,
	ID3D11Buffer* pVertexBuffer);

// The mpv prescaler an upscaling setting stands for, or nullptr (DX11VideoProcessorMpv.cpp).
const MpvShaderInfo* MpvLumaShader(const int upscaling);
