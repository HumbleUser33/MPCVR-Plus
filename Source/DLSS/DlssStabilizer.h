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
#include <string>
#include "DX11Helper.h"
#include "DlssOpticalFlow.h"

// CDlssStabilizer
//
// Steadies DLSS 5 NR over time without touching the network. Only its change to
// the picture, output - input, is filtered over time and then added to the current
// frame, so the video itself is never delayed -- the EFFECT variant of
// Shaders/d3d11/ps_dlss_stabilize.hlsl. The history is clamped to the current
// neighbourhood, so a wrong motion estimate cannot drag old content far.
//
// Motion comes from NVIDIA Optical Flow, on the picture reduced by a whole factor:
// the history follows the picture, so moving areas are steadied too. Where Optical
// Flow cannot run, nothing is steadied and GetStatusLine() says why. The shader
// detector that used to stand in, and steadied only what stood still, is gone.
//
// Measured in tools/dlssnr_probe --tstab on real pictures with known motion: plain
// DLSS shows 1.14x to 1.33x the low-frequency flicker of its input, a moving object
// up to 2.18x its own; with Optical Flow 1.01x to 1.05x and 1.13x, with the whole
// effect kept (RTX 4060).

class CDlssStabilizer
{
public:
	// How Optical Flow runs. The defaults are what the stabilizer was measured
	// with; DLSS Super Resolution needs finer vectors (--tsrq).
	struct FlowSettings {
		UINT flowFactor = 0;                                  // working pixels per flow pixel, 1 to 4; 0: about 540 lines
		UINT gridSize = 4;                                    // flow pixels per vector
		NV_OF_PERF_LEVEL perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
		bool bidirectional = true;                            // backward flow and cost feed the confidence map
		bool cost = true;
		bool temporalHints = true;                            // the engine starts from the last picture's flow
		UINT flowBlur = 0;                                    // working pixels the flow frame's box reaches beyond its own
		// Motion only, for DLSS Super Resolution: each block drawn to the picture's
		// global motion where it lies within the noise of it (PASS 3 of
		// ps_dlss_stabilize.hlsl, after cs_dlss_global_motion.hlsl).
		bool  snap     = false;
		float snapLow  = 0.5f;                                // working pixels from the global motion where a block starts to keep its vector
		float snapHigh = 1.0f;                                // and where it keeps it whole
		int   snapGate = 0;                                   // 0: by distance; 1: by confidence; 2: only far and trusted blocks
		float snapDeadZone = 0.0f;                            // a global motion shorter than this, working pixels, is none
		int   globalRefine = 0;                               // with snap: Gauss-Newton steps refining the global motion on the
		                                                      // flow frames (cs_dlss_global_refine.hlsl); 0: none
		float globalNear   = 1.0f;                            // and the flow pixels a block's own vector may lie from it to take part
		float globalRobust = 0.0f;                            // and groups past this times the median residual left out; 0: none
		// What keeps its own vector: the mean of the trusted blocks around it that move
		// alike, then a blend with the same content one picture earlier (PASS 4).
		int   vectorRadius   = 0;                             // blocks on each side; 0: none
		float vectorSigma    = 1.0f;                          // working pixels: how alike two vectors must be
		float vectorTemporal = 1.0f;                          // weight of this picture's vector; 1: the last one is not used
		int   vectorShift = 0;                                // mean shift steps toward the motion the window shares; 0: none
		float vectorSupportLow  = 0.1f;                       // with vectorShift: the share of the window that must agree to begin keeping
		float vectorSupportHigh = 0.3f;                       // and to keep whole
		int   vectorRefine  = 0;                              // halving search steps on the frames around a kept vector; 0: none
		float vectorReach   = 4.0f;                           // flow pixels on each side of the block centre the search compares
		float vectorTexture = 0.02f;                          // the least luma deviation of that window for a search
		float vectorEvidence = 2.0f;                          // with vectorShift: trusted blocks' worth that must share a motion
		bool operator==(const FlowSettings&) const = default;
	};

	// What DLSS Super Resolution runs with, measured in tools/dlssnr_probe
	// --tsrstill. Raw, the vectors made a still background shimmer half as much again
	// as exact ones, left the grain along the frame's edges, and made a moving subject
	// shimmer six times as much. Here each block climbs to the motion the
	// trusted blocks around it share, keeps it if enough of them do and it lies more
	// than 0.75 to 1.5 pixels from the global motion, has it searched again on the
	// frames themselves, and blends with where the same content was one picture
	// earlier; everything else takes the global motion. That is the median of the
	// trusted blocks, refined on the flow frames with what moves on its own left out,
	// and taken as it is however small: a film weaves, and a wall that moves half a
	// pixel and is told it did not shimmered more than the source itself (--tsrclip).
	static FlowSettings ForDlssSR();

	// What DLSS 5 NR's stabilizer runs with: Optical Flow on the working picture
	// divided by divider (0: about 540 lines), its flow frame unblurred as the
	// stabilizer was measured with, and the vectors the network is given cleaned the
	// way DLSS SR's are. With them the network keeps 14 to 27 % more of its effect and
	// shimmers less than the source itself, a moving object included, leaving behind
	// one what exact vectors leave; the raw vectors steadied less, most on a film that
	// weaves (--tstab, --tsrclipnr).
	// The stabilizer itself still warps its history with PASS 1's raw motion, as it
	// was measured with.
	static FlowSettings ForDlssNR(UINT divider);

	// Everything for a working size and these settings; nothing if they already
	// match. Optical Flow that cannot start leaves the object made, steadying
	// nothing, and GetStatusLine() says why. The renderer's input layout, vertex
	// shader and samplers are used for every pass and must outlive this object's
	// resources.
	//
	// bMotionOnly builds the Optical Flow vectors and nothing else, for DLSS Super
	// Resolution: no history, no result. Stabilize then does nothing, and
	// GetMotionVectors() is null while Optical Flow is down.
	HRESULT Create(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, UINT width, UINT height,
		ID3D11InputLayout* pInputLayout, ID3D11VertexShader* pVertexShader,
		ID3D11SamplerState* pSamplerPoint, ID3D11SamplerState* pSamplerLinear,
		bool bMotionOnly = false, const FlowSettings& flow = FlowSettings());
	void Release();

	bool Matches(UINT width, UINT height, bool bMotionOnly = false, const FlowSettings& flow = FlowSettings()) const;
	bool IsCreated() const { return m_width != 0; }

	// The next picture starts over: a seek, a new session.
	void Reset();

	// A new picture: its motion against the previous one. Call before DLSS, so the
	// vectors can reach the network as well.
	void PrepareMotion(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pInput);

	// The vectors DLSS is given, DLSSNR.MVec or DLSS SR's: RG16F, working-size pixels,
	// current -> previous, shared with the DLSS device. With FlowSettings::snap, the
	// blocks drawn to the global motion (PASS 4 and PASS 3); without, PASS 1's raw
	// motion. Null while Optical Flow is not running. Zero where no flow exists yet.
	ID3D11Texture2D* GetMotionVectors() const;

	// With FlowSettings::snap, the picture's global motion as the last new picture
	// measured it: RGBA32F 1x1, x and y and the median distance of the blocks to it in
	// flow pixels, w the number of blocks. Null otherwise.
	ID3D11Texture2D* GetGlobalMotion() const { return m_pGlobalMotion; }
	UINT GetFlowFactor() const { return m_flowFactor; }   // working pixels per flow pixel
	// With FlowSettings::snap, PASS 4's blocks for the last new picture: RGBA32F, one
	// pixel per vector block, xy the block's own vector in flow pixels, z how much of
	// it it keeps. Null otherwise.
	ID3D11Texture2D* GetBlockField() const { return m_BlockField[m_iBlockField].pTexture; }
	const CDlssOpticalFlow& GetFlow() const { return m_Flow; }   // the engine's own output, for the harness

	// After DLSS: the steadied picture into GetResult(). strength 0..1, 1 being the
	// measured setting. Only a new picture moves the history on. A redraw
	// (bNewPicture false) is steadied against the same history as its picture, so
	// it shows what changed meanwhile -- another setting, a zoom after upscaling --
	// without taking a second step.
	void Stabilize(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pInput,
		ID3D11ShaderResourceView* pNetwork, float strength, bool bNewPicture);

	Tex2D_t* GetResult() { return &m_TexResult; }

	bool HasFlow() const { return m_bFlow; }   // Optical Flow is running: something is steadied
	const std::wstring& GetStatusLine() const { return m_status; }

private:
	struct Target {
		CComPtr<ID3D11Texture2D>          pTexture;
		CComPtr<ID3D11ShaderResourceView> pShaderResource;
		CComPtr<ID3D11RenderTargetView>   pRenderTarget;
	};

	static HRESULT CreateTarget(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format, Target& target);
	HRESULT CreateResources(ID3D11Device* pDevice, UINT width, UINT height, bool bMotionOnly, const FlowSettings& flow);
	void MeasureGlobalMotion(ID3D11DeviceContext* pContext);
	void Draw(ID3D11DeviceContext* pContext, ID3D11PixelShader* pShader,
		std::initializer_list<ID3D11RenderTargetView*> targets, UINT width, UINT height,
		std::initializer_list<ID3D11ShaderResourceView*> inputs, const float* constants, size_t count);

	CComPtr<ID3D11Device>      m_pDevice;
	CComPtr<ID3D11PixelShader> m_pPSFlowFrame;
	CComPtr<ID3D11PixelShader> m_pPSFlowMotion;
	CComPtr<ID3D11PixelShader> m_pPSStabilize;
	CComPtr<ID3D11PixelShader> m_pPSSnapMotion;
	CComPtr<ID3D11PixelShader> m_pPSBlockMotion;
	CComPtr<ID3D11ComputeShader>       m_pCSGlobalMotion;
	CComPtr<ID3D11Texture2D>           m_pGlobalMotion;       // RGBA32F 1x1, see GetGlobalMotion
	CComPtr<ID3D11ShaderResourceView>  m_pGlobalMotionView;
	CComPtr<ID3D11UnorderedAccessView> m_pGlobalMotionTarget;
	CComPtr<ID3D11ComputeShader>       m_pCSRefineStep;       // FlowSettings::globalRefine: its two passes
	CComPtr<ID3D11ComputeShader>       m_pCSRefineSolve;
	CComPtr<ID3D11Texture2D>           m_pRefinePartial[2];   // RGBA32F, a pixel per 16 x 16 group of the flow frame
	CComPtr<ID3D11ShaderResourceView>  m_pRefinePartialView[2];
	CComPtr<ID3D11UnorderedAccessView> m_pRefinePartialTarget[2];
	CComPtr<ID3D11Buffer>      m_pConstants;
	CComPtr<ID3D11Buffer>      m_pQuad;

	Tex2D_t m_TexMotion;                       // RG16F, shared with the DLSS device
	CComPtr<ID3D11RenderTargetView> m_pMotionTarget;
	Tex2D_t m_TexVectors;                      // RG16F, shared: with snap, outside motion-only, the cleaned vectors
	CComPtr<ID3D11RenderTargetView> m_pVectorsTarget;   // for the network, m_TexMotion staying PASS 1's
	Target  m_Confidence;                      // R8
	Target  m_BlockField[2];                   // RGBA32F, a pixel per vector block: PASS 4, this picture and the last
	Target  m_History[2];                      // RGBA16F: the steadied effect
	Target  m_Input[2];                        // RGBA16F: network inputs, for the reprojection check
	Tex2D_t m_TexResult;                       // RGBA16F: what the renderer shows
	CComPtr<ID3D11RenderTargetView> m_pResultTarget;

	CDlssOpticalFlow m_Flow;

	// The renderer's objects.
	ID3D11InputLayout*  m_pInputLayout   = nullptr;
	ID3D11VertexShader* m_pVertexShader  = nullptr;
	ID3D11SamplerState* m_pSamplerPoint  = nullptr;
	ID3D11SamplerState* m_pSamplerLinear = nullptr;

	UINT   m_width  = 0;
	UINT   m_height = 0;
	UINT   m_failedWidth  = 0;   // the last size Create could not make
	UINT   m_failedHeight = 0;
	UINT   m_flowFactor = 1;
	bool   m_bFlow        = false;
	bool   m_bMotionOnly  = false;
	FlowSettings m_FlowSettings;

	int  m_iHistory      = 0;       // m_History slot the last new picture wrote
	int  m_iInput        = 0;       // m_Input slot holding the last new picture's input
	int  m_iBlockField   = 0;       // m_BlockField slot the last new picture wrote
	bool m_bBlockHistory = false;   // and it is the picture just before this one
	bool m_bHistoryValid = false;
	bool m_bHaveMotion   = false;
	bool m_bHaveResult   = false;   // a new picture was steadied since the reset
	bool m_bLastReset    = true;    // and it started the history over

	std::wstring m_status;
};
