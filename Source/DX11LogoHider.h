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

#include <string>
#include "DX11Helper.h"
#include "Logo/LogoWorker.h"
#include "Logo/LogoParams.h"

// Hiding a channel's logo, on the GPU: the first step of the artifact removal, on the
// decoded planes of a two-plane source (NV12, P010, P016 and their kin). It writes planes
// of its own, the whole picture copied and the logo's box worked; the conversion reads
// them in place of the decoder's (CDX11VideoProcessor::SrcPlane).
//
// Its analysis (Source/Logo) runs on a worker thread from pictures read back as it asks
// for them, one every Logo::kSampleSeconds of the film: copied on the GPU into textures of
// their own, then mapped a few pictures later without waiting, so that the render thread
// never stands still for it.

class CDX11LogoHider
{
public:
	~CDX11LogoHider() { Release(true); }

	HRESULT InitShaders(ID3D11Device* pDevice);
	void Release(const bool bShaders);

	// The picture's planes: luma W x H, chroma divided by divW x divH. What was found is
	// kept while they stay the same; a new size starts again from the library.
	void SetFormat(int W, int H, int divW, int divH, const std::wstring& film);
	void SetMargin(int margin);   // luma pixels round the logo worked as well
	void Find();     // a new analysis, the logo hidden meanwhile kept until it gives one
	void Forget();   // the library emptied and the logo dropped
	void Stop();     // the worker stopped and nothing hidden

	// The settings page's list: the logos kept for this film's size, by what they are, one
	// a line, the most recently used first. The list given is kept, so that the index the
	// page sends back means the logo it showed.
	std::wstring List();
	// 0: the logo is found or recognised by itself; i: the i-th of the last list, chosen.
	int Choice() const;
	void Choose(int i);

	// A new picture (newPicture) or the same one again, its planes as SRVs: pictures read
	// back for the analysis as it asks, a fit taken when one is ready, the logo taken out
	// into the hider's own planes. Returns whether it wrote them.
	bool Run(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pLuma, ID3D11ShaderResourceView* pChroma,
		REFERENCE_TIME rt, bool newPicture);
	ID3D11ShaderResourceView* Plane(const int i) const { return i ? m_Out[1].pShaderResource.p : m_Out[0].pShaderResource.p; }
	const Tex2D_t& PlaneTex(const int i) const { return m_Out[i ? 1 : 0]; }

	bool HasLogo() const { return m_bMaps; }
	bool Working() const;   // an analysis or a recognition under way
	std::wstring Status();

private:
	HRESULT Targets();
	void Sample(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pLuma, ID3D11ShaderResourceView* pChroma, REFERENCE_TIME rt);
	void Collect(ID3D11DeviceContext* pContext);
	HRESULT Upload(ID3D11DeviceContext* pContext, const Logo::Maps& maps);
	void Dispatch(ID3D11DeviceContext* pContext, ID3D11ComputeShader* pShader, ID3D11Buffer* pConstants, const void* data, UINT size,
		ID3D11ShaderResourceView* const* srvs, UINT nSrv, ID3D11UnorderedAccessView* const* uavs, UINT nUav, UINT gx, UINT gy);
	void ReadStrength(ID3D11DeviceContext* pContext);

	CComPtr<ID3D11Device> m_pDevice;
	CComPtr<ID3D11ComputeShader> m_pCSLuma, m_pCSCrop[2], m_pCSCopy[2], m_pCSFill[2], m_pCSRemove[2];
	CComPtr<ID3D11Buffer> m_pPackConstants, m_pRemoveConstants;
	CComPtr<ID3D11Buffer> m_pState, m_pStateStage;
	CComPtr<ID3D11UnorderedAccessView> m_pStateUav;
	bool m_bStatePending = false;
	int m_nStateWait = 0;
	float m_fShown = 1;   // the strength last read back: 0 off screen

	int m_W = 0, m_H = 0, m_divW = 2, m_divH = 2;
	std::wstring m_film;
	std::vector<Logo::Listed> m_Listed;   // as the page was last given it
	std::wstring m_Chosen;                // the file of the logo chosen on the page, if one is

	// What the logo is taken out into.
	Tex2D_t m_Out[2];
	CComPtr<ID3D11UnorderedAccessView> m_pOutUav[2];

	// The logo.
	Logo::Maps m_Maps;
	bool m_bMaps = false;       // a logo to hide
	bool m_bUploaded = false;   // and its maps on the GPU
	Logo::RemoveOptions m_Opt;
	Tex2D_t m_Map[2], m_Mirror[2];
	CComPtr<ID3D11Buffer> m_pPyramid[2];
	CComPtr<ID3D11UnorderedAccessView> m_pPyramidUav[2];

	// The analysis, and the pictures read back for it.
	Logo::CWorker m_Worker;
	static constexpr REFERENCE_TIME kNoTime = INT64_MIN;
	REFERENCE_TIME m_rtSampled = kNoTime;
	int m_nSinceSample = 0;
	struct Readback {
		Tex2D_t pack[2];                     // what the shader copied out (UAV)
		CComPtr<ID3D11UnorderedAccessView> pUav[2];
		CComPtr<ID3D11Texture2D> pStage[2];  // read back from these
		bool bLuma = false;                  // the luma at the working size, or a box
		Logo::Rect box;
		UINT w = 0, h = 0;
		bool bPending = false;
		uint64_t order = 0;
	};
	Readback m_Read[3];
	uint64_t m_nOrder = 0;
};
