// SPDX-License-Identifier: MIT
// From Echo-Storm/ls-addon-manager, revision 8d3eac351d42480326290b0f8965127a630b44f2
// (engine/own_synth.*), Copyright (c) 2026 Echo-Storm and andreiday; the licence is
// EchoStorm-LICENSE.txt beside this file.
//
// CEchoSynth11: EchoStorm's frame synthesis, OwnSynth, on the Direct3D 11 device -- the
// shaders CEchoSynth runs on a Direct3D 12 queue for frame generation
// (Shaders/d3d11/cs_echo_synth.hlsl), driven here in the same order and with the same
// constants, one picture at a time. It makes the picture a cadence slip adds
// (Source/DX11VideoProcessorCadence.cpp): the film at the moment of a refresh that falls
// between two of its pictures.
//
// The marks EchoStorm accumulates from pair to pair, to keep a channel's logo where it is,
// are left out: a slip makes one picture every few seconds, and a count of marks wants
// every pair. Its grain matching, which needs no history, is kept for SDR pictures.
//
// The shaders bind fourteen UAVs, which takes feature level 11_1; Init says no below it.

#pragma once

#include <atlbase.h>
#include <d3d11.h>
#include <cstdint>
#include <string>

class CEchoSynth11
{
public:
	~CEchoSynth11() { Release(); }
	bool Init(ID3D11Device* pDevice);
	void Release();
	bool IsReady() const { return m_pCS[0] != nullptr; }

	// Buffers for frames of this size; nothing is done when they already are.
	bool Ensure(ID3D11Device* pDevice, uint32_t w, uint32_t h);

	// The picture at time t in (0, 1) between pBefore (0) and pNow (1), into pOut.
	// pBefore, pNow: any format a shader reads as colour, w x h; pMotion: RG16F pixel
	// vectors from pNow to pBefore (CEchoFlow::Motion); pOut: a w x h texture that takes
	// typed stores. encoding, whiteNits: 0 SDR, 2 HDR10, and the SDR white an HDR frame is
	// compared under; soften and grain as OwnSynth::Record.
	bool Make(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pBefore, ID3D11ShaderResourceView* pNow,
		ID3D11ShaderResourceView* pMotion, ID3D11UnorderedAccessView* pOut, float t,
		uint32_t encoding, float whiteNits, float soften, float grain);

	const std::wstring& Status() const { return m_status; }

private:
	enum Entry { Clear, Err0, Box5, ErrA, Splat, Hyp1, Hyp2, Holes, Synth, MarkPrep, MarkLarge, MarkUpdate, GrainStat, EntryCount };
	static constexpr int kBuffers = 13;
	static constexpr uint32_t kScale = 2;   // EchoStorm's analysis cells: 2 x 2 pixels

	void ReleaseBuffers();

	CComPtr<ID3D11ComputeShader> m_pCS[EntryCount];
	CComPtr<ID3D11Buffer> m_pConstants;
	CComPtr<ID3D11SamplerState> m_pLinear;
	CComPtr<ID3D11Buffer> m_pBuf[kBuffers];
	CComPtr<ID3D11UnorderedAccessView> m_pBufUav[kBuffers];
	uint32_t m_seed = 0;
	uint32_t m_w = 0, m_h = 0, m_aw = 0, m_ah = 0;
	std::wstring m_status;
};
