/*
* (C) 2019-2024 see Authors.txt
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

#include <d3dcommon.h>

struct PS_COLOR_TRANSFORM {
	DirectX::XMFLOAT4 cm_r;
	DirectX::XMFLOAT4 cm_g;
	DirectX::XMFLOAT4 cm_b;
	DirectX::XMFLOAT4 cm_c;
};

struct PS_DOVI_POLY_CURVE {
	DirectX::XMFLOAT4 pivots_data[7];
	DirectX::XMFLOAT4 coeffs_data[8];
};

#define PS_RESHAPE_POLY 1
#define PS_RESHAPE_MMR  2

struct PS_DOVI_CURVE {
	DirectX::XMFLOAT4 pivots_data[7];
	DirectX::XMFLOAT4 coeffs_data[8];
	DirectX::XMFLOAT4 mmr_data[8 * 6];
	struct {
		uint32_t methods;
		uint32_t mmr_single;
		uint32_t min_order;
		uint32_t max_order;
	} params;
};

static_assert(sizeof(PS_DOVI_CURVE) % 16 == 0);

enum :int {
	SHADER_CONVERT_NONE = 0,
	SHADER_CONVERT_TO_SDR,
	SHADER_CONVERT_TO_PQ,
};

HRESULT CompileShader(const std::string& srcCode, const D3D_SHADER_MACRO* pDefines, LPCSTR pTarget, ID3DBlob** ppCode);

HRESULT GetShaderConvertColor(
	const bool bDX11,
	const UINT width,
	const long texW, long texH,
	const RECT rect,
	const FmtConvParams_t& fmtParams,
	const DXVA2_ExtendedFormat exFmt,
	const MediaSideDataDOVIMetadata* const pDoviMetadata,
	const int chromaScaling,
	const int convertType,
	const bool blendDeinterlace,
	ID3DBlob** ppCode);

// A compute shader that rebuilds the chroma the same way and writes the picture as
// packed 4:4:4 -- AYUV for an 8-bit source, Y410 above -- so that the D3D11 video
// processor, which reads it next, has no chroma of its own to rebuild. The picture
// stays in Y'CbCr, brought to the scale the processor reads studio range on, and the
// conversion to RGB is still its own.
HRESULT GetShaderConvertTo444(
	const UINT width,
	const long texW, const long texH,
	const FmtConvParams_t& fmtParams,
	const DXVA2_ExtendedFormat exFmt,
	const int chromaScaling,
	const DXGI_FORMAT packedFormat,
	ID3DBlob** ppCode);

// The way back: the R'G'B' picture a DLSS pass leaves, written as the Y'CbCr the D3D11
// video processor takes, so that the processor can enlarge it after the pass. Nothing
// is converted but the matrix -- the transfer function and the primaries stay as they
// are, and the processor is told the picture is in the space it already is in.
//
// Three shaders. P010, which the driver's Super Resolution acts on, is written a plane
// at a time: RGBTOVIDEO_Luma into the R16 view of the texture, RGBTOVIDEO_Chroma into
// its R16G16 view at half the size, the chroma sited as MPEG-2 sites it. Y410, for a
// picture whose chroma the shaders were asked to keep whole, is one compute shader
// through an R32_UINT view, as GetShaderConvertTo444 writes it.
//
// width and height are the picture's; scale10 is how many codes of the 10-bit scale a
// level of the 8-bit one is written as (4, or 1023/255 where a driver reads the studio
// range from the full scale).
enum : int {
	RGBTOVIDEO_Luma = 0,
	RGBTOVIDEO_Chroma,
	RGBTOVIDEO_Packed,
};
HRESULT GetShaderRgbToVideo(
	const int part,
	const bool bBT2020,
	const UINT width, const UINT height,
	const double scale10,
	ID3DBlob** ppCode);
