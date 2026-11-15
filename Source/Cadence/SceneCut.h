// CSceneCut: whether a picture opens a new shot, on any card, for the cadence slips.
//
// A cadence slip -- the refresh more that a film at 23.976, 29.97 or 59.94 pictures a
// second owes a screen at 60 Hz every few seconds -- is not seen where the picture changes
// anyway: the last picture of a shot held a refresh longer is the cut coming a refresh
// later (CPresentSchedule, SetCadenceSlips). This says where the cuts are.
//
// It is the detector DLSS Super Resolution drops its history on (cs_dlss_scene_cut.hlsl,
// whose thresholds were chosen on 48 minutes of six films beside ffmpeg's scene score),
// run here without NVIDIA Optical Flow: on a frame of its own made from the picture as it
// is drawn (cs_scene_cut_luma.hlsl), and with no global motion to move the last frame by,
// which the detector only needs to tell a pan from a cut in the residual -- its ratio to
// the pictures before and the change of the histogram tell them apart anyway.
//
// The answer is the GPU's: Measure records the passes after the picture is drawn, and
// Read has it once the GPU is done with the picture, which the render thread knows when it
// comes to present it. Not done by then is no cut: the slip waits for the next one.

#pragma once

#include <atlbase.h>
#include <d3d11.h>
#include <cstdint>

class CSceneCut
{
public:
	~CSceneCut() { Release(); }
	bool Init(ID3D11Device* pDevice);
	void Release();
	bool IsReady() const { return m_pCSCut != nullptr; }

	// The next picture follows nothing: a seek, a new film, a setting changed.
	void Reset() { m_bHaveLast = false; m_bStale = true; }

	// Measures pPicture (the video area, w x h) against the picture given before.
	// encoding 0 SDR, 2 HDR10; whiteNits: the SDR white an HDR picture is seen under.
	// id names the picture for Read.
	bool Measure(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture,
		UINT w, UINT h, UINT encoding, float whiteNits, uint64_t id);

	// 1 when picture id opens a shot, 0 when not, -1 when the GPU has not said yet (or the
	// picture was not measured). state, when given, gets the detector's 16 values.
	int Read(ID3D11DeviceContext* pContext, uint64_t id, float* state = nullptr);

	// The detector's own thresholds (cs_dlss_scene_cut.hlsl), the stabilizer's.
	static constexpr float kHistogram   = 0.20f;
	static constexpr float kRatio       = 3.0f;
	static constexpr float kResidual    = 0.001f;
	static constexpr float kCorrelation = 0.25f;
	static constexpr float kLightHist   = 0.25f;
	static constexpr float kLightRatio  = 30.0f;

private:
	struct Tex {
		CComPtr<ID3D11Texture2D> pTex;
		CComPtr<ID3D11ShaderResourceView> pSrv;
		CComPtr<ID3D11UnorderedAccessView> pUav;
		HRESULT Create(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format);
		void Release() { pUav.Release(); pSrv.Release(); pTex.Release(); }
	};
	bool Ensure(ID3D11Device* pDevice, UINT w, UINT h);
	void ReleaseTextures();

	static constexpr int kSlots = 3;
	struct Slot {
		CComPtr<ID3D11Buffer> pStaging;
		uint64_t id = 0;
		bool bPending = false;
	};

	CComPtr<ID3D11ComputeShader> m_pCSLuma, m_pCSSums, m_pCSCut;
	CComPtr<ID3D11Buffer> m_pLumaConstants, m_pCutConstants;
	CComPtr<ID3D11Buffer> m_pState;
	CComPtr<ID3D11UnorderedAccessView> m_pStateUav;
	CComPtr<ID3D11Texture2D> m_pGlobal;               // 1 x 1, zero: no global motion
	CComPtr<ID3D11ShaderResourceView> m_pGlobalSrv;
	Tex m_frame[2];                                   // this picture's frame and the one before's (R8)
	Tex m_sums[2];                                    // PASS 1's per group (RGBA32F)
	Slot m_slots[kSlots];
	UINT m_w = 0, m_h = 0;                            // the pictures'
	UINT m_fw = 0, m_fh = 0, m_factor = 1;            // the frames', and picture pixels per frame pixel
	int m_current = 0;
	int m_nextSlot = 0;
	bool m_bHaveLast = false;
	bool m_bStale = true;                             // the state is cleared before the next use
};
