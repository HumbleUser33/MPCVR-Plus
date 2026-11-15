// SPDX-License-Identifier: MIT
// From Echo-Storm/ls-addon-manager, revision 8d3eac351d42480326290b0f8965127a630b44f2
// (engine/flow_estimator.*, engine/flow_fix.*), Copyright (c) 2026 Echo-Storm and andreiday;
// the licence is EchoStorm-LICENSE.txt beside this file.
//
// CEchoFlow: EchoStorm's motion estimate and its FlowFix, on the Direct3D 11 device, for the
// frame generators where NVIDIA Optical Flow is not there (any card that is not an RTX), or
// beside it. The passes are EchoStorm's (Shaders/d3d11/cs_echo_flow.hlsl and
// cs_echo_flowfix.hlsl); what drives them is written again here for Direct3D 11, in the
// order and with the constants EchoStorm's FlowEstimator::Record and FlowFix::Record use.
// MPC-NeuralRenderer, whose frame generation drives EchoStorm the same way, analyses a
// frame more than 2560 pixels wide at half its size, and so does this.
//
// The vectors come out at the frames' size, in their pixels, from a pixel of the frame just
// given to where it was in the one before: what CFrameGenMotion hands every generator.

#pragma once

#include <atlbase.h>
#include <d3d11.h>
#include <initializer_list>
#include <string>

class CEchoFlow
{
public:
	// The shaders, from the filter's resources; false, and Status() says why, without them.
	bool Init(ID3D11Device* pDevice);
	void Release();
	bool IsReady() const { return m_pCS[0] != nullptr; }

	// Textures for frames of width x height; nothing is done when they already are.
	bool Ensure(ID3D11Device* pDevice, UINT width, UINT height);
	// The next frame has no frame before it: its vectors are zero.
	void Forget() { m_bPrevious = false; }

	// The motion of pFrame (width x height, any format a shader reads as colour) since the
	// frame given before, pBefore -- the same frame the call before was given, still as it
	// was; null when there is none. encoding: 0 SDR, 2 HDR10 (PQ, Rec.2020); whiteNits:
	// the SDR white an HDR frame is matched under.
	void Measure(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pFrame, ID3D11ShaderResourceView* pBefore,
		UINT encoding, float whiteNits);

	// RG16F at the frames' size, pixels, current -> previous; zero after a Forget.
	ID3D11Texture2D* Motion() const { return m_out.pTex; }
	ID3D11ShaderResourceView* MotionView() const { return m_out.pSrv; }
	// The estimate before FlowFix, at the analysis size: for the harness's port check.
	ID3D11Texture2D* Estimate() const { return m_motion.pTex; }
	const std::wstring& Status() const { return m_status; }
	UINT AnalysisScale() const { return m_scale; }

private:
	static constexpr int kMaxLevels = 7;
	static constexpr int kGlobalRadius = 20;   // the whole picture's shift, tried within +-this at the smallest size
	enum { Luma, Down, Global, Search, Median, Pixel, FixLuma, FixTiles, FixDecide, FixApply, ShaderCount };

	struct Tex {
		CComPtr<ID3D11Texture2D> pTex;
		CComPtr<ID3D11ShaderResourceView> pSrv;
		CComPtr<ID3D11UnorderedAccessView> pUav;
		HRESULT Create(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format);
		void Release() { pUav.Release(); pSrv.Release(); pTex.Release(); }
	};

	// One pass: these views bound (the rest none), the constants, and unbound after.
	void Run(ID3D11DeviceContext* pContext, int shader, std::initializer_list<ID3D11ShaderResourceView*> srvs,
		std::initializer_list<ID3D11UnorderedAccessView*> uavs, const void* constants, UINT size, UINT groupsX, UINT groupsY);
	void ReleaseTextures();

	CComPtr<ID3D11ComputeShader> m_pCS[ShaderCount];
	CComPtr<ID3D11Buffer> m_pConstants;
	CComPtr<ID3D11SamplerState> m_pLinear;

	UINT m_width = 0, m_height = 0;     // the frames'
	UINT m_scale = 1;                   // they are analysed at 1/m_scale of that
	int m_levels = 0;
	UINT m_lw[kMaxLevels] = {}, m_lh[kMaxLevels] = {}, m_gw[kMaxLevels] = {}, m_gh[kMaxLevels] = {};
	Tex m_luma[2][kMaxLevels];          // two pyramids: this frame's and the one before's (R16F)
	Tex m_grid[kMaxLevels];             // a vector per 4x4 block of each size from 1 up (RG16F, that size's pixels)
	Tex m_filtered;                     // the half-size grid after the median
	Tex m_global;                       // the cost of each shift of the whole picture (R32F)
	Tex m_motion, m_distrust;           // at the analysis size: the vectors and where they cannot be trusted
	// FlowFix: half-size brightness of both frames, each tile's dominant movement, each cell's choice
	UINT m_cs = 2, m_cw = 0, m_ch = 0, m_tile = 32, m_tx = 0, m_ty = 0;
	Tex m_fixLumaA, m_fixLumaC, m_modes, m_cells;
	Tex m_out;                          // the vectors at the frames' size
	int m_current = 0;
	bool m_bPrevious = false;
	std::wstring m_status;
};
