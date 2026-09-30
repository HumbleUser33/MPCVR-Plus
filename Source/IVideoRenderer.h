/*
 * (C) 2018-2026 see Authors.txt
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

#include <dxva2api.h>

enum :int {
	TEXFMT_AUTOINT = 0,
	TEXFMT_8INT = 8,
	TEXFMT_10INT = 10,
	TEXFMT_16FLOAT = 16,
};

enum :int {
	DEINT_Disable = 0,
	DEINT_Enable = 1,
	DEINT_HackFutureFrames = 2,
};

enum :int {
	SUPERRES_Disable = 0,
	SUPERRES_SD,
	SUPERRES_720p,
	SUPERRES_1080p,
	SUPERRES_1440p,
	SUPERRES_COUNT
};

enum :int {
	CHROMA_Nearest = 0,
	CHROMA_Bilinear,
	CHROMA_CatmullRom,
	CHROMA_RAVU,       // RAVU-zoom (mpv) on Cb and Cr: Direct3D 11 shaders, 4:2:0; Catmull-Rom elsewhere
	// Added after RAVU, so the numbers already saved keep their meaning. The order
	// the property page shows them in is the measured one, not this one.
	CHROMA_Jinc,       // ewa jinc, radius 3.2383, jinc-windowed: what madVR calls Jinc
	CHROMA_FSRCNNX8AR, // FSRCNNX 8 on Cb and Cr with anti-ringing; Catmull-Rom elsewhere
	CHROMA_COUNT
};

enum :int {
	UPSCALE_Nearest = 0,
	UPSCALE_Mitchell,
	UPSCALE_CatmullRom,
	UPSCALE_Lanczos2,
	UPSCALE_Lanczos3,
	UPSCALE_Jinc2,
	// mpv prescalers on luma, colour from Catmull-Rom: Direct3D 11 at feature level
	// 11.0; Catmull-Rom elsewhere and for what they leave to scale.
	UPSCALE_FSRCNNX8,
	UPSCALE_FSRCNNX16,
	UPSCALE_RAVUZoom,
	UPSCALE_FSRCNNX8AR,   // the same networks, held to the range the source covers
	UPSCALE_FSRCNNX16AR,
	UPSCALE_ArtCNN,       // ArtCNN C4F16 DS: compute passes, denoises as it enlarges
	UPSCALE_COUNT
};

enum :int {
	DOWNSCALE_Box = 0,
	DOWNSCALE_Bilinear,
	DOWNSCALE_Hamming,
	DOWNSCALE_Bicubic,
	DOWNSCALE_BicubicSharp,
	DOWNSCALE_Lanczos,
	DOWNSCALE_COUNT
};

enum :int {
	SWAPEFFECT_Discard = 0,
	SWAPEFFECT_Flip,
	SWAPEFFECT_COUNT
};

enum :int {
	HDRTD_Disabled = 0,
	HDRTD_On_Fullscreen,
	HDRTD_On,
	HDRTD_OnOff_Fullscreen,
	HDRTD_OnOff
};

#define SDR_NITS_DEF 125
#define SDR_NITS_MIN  25
#define SDR_NITS_MAX 400
#define SDR_NITS_STEP  5

constexpr inline auto HDR_NITS_DEF = 1000;
constexpr inline auto HDR_NITS_MIN = 100;
constexpr inline auto HDR_NITS_MAX = 10000;

enum :int {
	DLSSNR_STYLE_Default = 0,
	DLSSNR_STYLE_Natural,
	DLSSNR_STYLE_Cinematic,
	DLSSNR_STYLE_COUNT
};

constexpr inline auto DLSSNR_PRESET_COUNT = 4;
// Strength sliders: stored and edited as ints, divided by DLSSNR_STR_SCALE
// before being handed to NGX.
constexpr inline auto DLSSNR_STR_SCALE = 100;
constexpr inline auto DLSSNR_STR_MIN   = 0;
constexpr inline auto DLSSNR_STR_MAX   = 200;
constexpr inline auto DLSSNR_STR_DEF   = 100;
constexpr inline auto DLSSNR_SKIN_MIN  = -100;
// Temporal stabilizer, 0..100: how strongly the network's effect is steadied over
// time after it runs. 0 runs nothing; 100 is the measured setting.
constexpr inline auto DLSSNR_STAB_MIN  = 0;
constexpr inline auto DLSSNR_STAB_MAX  = 100;
constexpr inline auto DLSSNR_STAB_DEF  = 100;
// Where the stabilizer takes motion from.
constexpr inline auto DLSSNR_MOTION_DETECTOR    = 0;   // shader detector: still areas only
constexpr inline auto DLSSNR_MOTION_OPTICALFLOW = 1;   // NVIDIA Optical Flow: moving areas too
constexpr inline auto DLSSNR_MOTION_DEF         = DLSSNR_MOTION_OPTICALFLOW;

// DLSS Super Resolution render preset, stored as the NGX number: 0 lets DLSS
// choose for the scale, 10 J and 11 K are the first transformer models, 12 L and
// 13 M the second (DLSS 4.5).
constexpr inline auto DLSSSR_PRESET_DEF = 0;
constexpr inline auto DLSSSR_PRESET_J   = 10;
constexpr inline auto DLSSSR_PRESET_M   = 13;

struct VPEnableFormats_t {
	bool bNV12;
	bool bP01x;
	bool bYUY2;
	bool bOther;
};

// Sharpening, after the resize. Two methods, because they do not win on the same
// ground: adaptive-sharpen is the only one that does not amplify grain, and the
// unsharp mask held to what its neighbours cover is five times cheaper and rings
// less on a clean picture (tools/dlssnr_probe --tsharpen).
enum : int {
    SHARPEN_Disabled = 0,
    SHARPEN_Adaptive,
    SHARPEN_UnsharpClamp,
    SHARPEN_COUNT
};

// Five levels, and the same level means the same thing whichever method runs:
// each one's strength was read off its own curve at the edge gradient the other
// reaches there. Level 3 is where both peak against the reference.
constexpr inline auto SHARPEN_LEVEL_MIN = 1;
constexpr inline auto SHARPEN_LEVEL_MAX = 5;
constexpr inline auto SHARPEN_LEVEL_DEF = 3;

struct Settings_t {
	bool bUseD3D11;
	bool bShowStats;
	int  iResizeStats;
	int  iTexFormat;
	VPEnableFormats_t VPFmts;
	int  iVPDeinterlacing;
	bool bDeintDouble;
	bool bVPScaling;
	int iVPSuperRes;
	bool bVPRTXVideoHDR;
	int  iChromaScaling;
	int  iUpscaling;
	int  iDownscaling;
	bool bInterpolateAt50pct;
	bool bUseDither;
	bool bDeintBlend;
	int  iSwapEffect;
	bool bExclusiveFS;
	bool bVBlankBeforePresent;
	bool bAdjustPresentTime;
	bool bReinitByDisplay;
	bool bHdrPreferDoVi;
	bool bHdrPassthrough;
	int  iHdrToggleDisplay;
	int  iHdrOsdBrightness;
	bool bConvertToSdr;
	int  iSDRDisplayNits;
	bool bHdrLocalToneMapping;
	int  iHdrLocalToneMappingType;
	int iHdrDisplayMaxNits;
	// DLSS 5 Neural Rendering. Strengths are stored x100 so the whole struct
	// stays integral and round-trips through the DWORD-only registry.
	bool bDlssNR;
	int  iDlssNRStyle;
	int  iDlssNRPreset;
	int  iDlssNRIntensity;
	int  iDlssNRLocalTone;
	int  iDlssNRLocalStructure;
	int  iDlssNRSkinStructure;
	bool bDlssNRAutoMask;
	// The network blends with its own previous output, which does not follow the
	// motion of the video; left alone, that blend measures the same as no history.
	bool bDlssNRNoHistory;
	// Temporal stabilizer after the network, DLSSNR_STAB_*; 0 = off.
	int  iDlssNRStabilizer;
	// Its motion source, DLSSNR_MOTION_*.
	int  iDlssNRMotion;
	// Optical Flow vectors given to the network as DLSSNR.MVec as well.
	bool bDlssNRMotionVectors;
	// Run the pass at display resolution, after scaling, instead of at source
	// resolution before it. Much heavier at 4K.
	bool bDlssNRAfterUpscale;
	// Virtual-key code that toggles DLSS during playback, 0 = no key.
	int  iDlssNRToggleKey;
	wchar_t szDlssNRDllPath[MAX_PATH];
	// DLSS Super Resolution in place of the Upscaling method, set on the DLSS 5
	// page and independent of DLSS 5 NR: nvngx_dlss.dll through the display
	// driver's NGX runtime.
	bool bDlssSR;
	int  iDlssSRPreset;   // DLSSSR_PRESET_*
	wchar_t szDlssSRDllPath[MAX_PATH];
	// Start the DLSS passes early by what they take, and hold each finished picture
	// until its time, so DLSS does not make the video late (see CRenderAhead).
	bool bDlssRenderAhead;

	// Added after everything upstream has, and it stays there: the video processor's
	// own chroma upsampling measures about bilinear, and on a 10-bit source the driver
	// reads the studio range as 16/255..235/255 whatever the depth, which shifts the
	// colour by about one 8-bit level. With this, the shaders rebuild the chroma of a
	// progressive 4:2:0 or 4:2:2 picture with iChromaScaling and hand the processor a
	// 4:4:4 one. Interlaced video keeps the processor's own chroma: it is the only
	// deinterlacer there is, and it deinterlaces 4:2:0 only. Direct3D 11 only.
	bool bVPReplaceChroma;

	// Sharpening after the resize: SHARPEN_*, and which of the five levels.
	int iSharpen;
	int iSharpenLevel;

	Settings_t() {
		SetDefault();
	}

	void SetDefault() {
		if (IsWindows8OrGreater()) {
			bUseD3D11                   = true;
		} else {
			bUseD3D11                   = false;
		}
		bShowStats                      = false;
		iResizeStats                    = 0;
		iTexFormat                      = TEXFMT_AUTOINT;
		VPFmts.bNV12                    = true;
		VPFmts.bP01x                    = true;
		VPFmts.bYUY2                    = true;
		VPFmts.bOther                   = true;
		iVPDeinterlacing                = DEINT_Enable;
		bDeintDouble                    = true;
		bVPScaling                      = true;
		iVPSuperRes                     = SUPERRES_Disable;
		bVPRTXVideoHDR                  = false;
		bVPReplaceChroma                = false;
		iSharpen                        = SHARPEN_Disabled;
		iSharpenLevel                   = SHARPEN_LEVEL_DEF;
		iChromaScaling                  = CHROMA_CatmullRom;
		iUpscaling                      = UPSCALE_Jinc2;
		iDownscaling                    = DOWNSCALE_Hamming;
		bInterpolateAt50pct             = true;
		bUseDither                      = true;
		bDeintBlend                     = false;
		iSwapEffect                     = SWAPEFFECT_Flip;
		bExclusiveFS                    = false;
		bVBlankBeforePresent            = false;
		bAdjustPresentTime              = true;
		bReinitByDisplay                = false;
		bHdrPreferDoVi                  = false;
		if (IsWindows10OrGreater()) {
			bHdrPassthrough             = true;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		} else {
			bHdrPassthrough             = false;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		}
		iHdrToggleDisplay               = HDRTD_Disabled;
		bConvertToSdr                   = true;
		iHdrOsdBrightness               = 0;
		iSDRDisplayNits                 = SDR_NITS_DEF;
		bDlssNR                         = false;
		iDlssNRStyle                    = DLSSNR_STYLE_Default;
		iDlssNRPreset                   = 0;
		// Tuned by ear on real video rather than left at neutral: the local
		// terms are the ones that amplify frame-to-frame variation, so they sit
		// well below 1.00 while overall intensity sits above it.
		iDlssNRIntensity                = 150;   // 1.50
		iDlssNRLocalTone                = 30;    // 0.30
		iDlssNRLocalStructure           = 50;    // 0.50
		iDlssNRSkinStructure            = 90;    // 0.90
		bDlssNRAutoMask                 = true;
		bDlssNRNoHistory                = false;
		bDlssNRAfterUpscale             = false;
		iDlssNRStabilizer               = DLSSNR_STAB_DEF;
		iDlssNRMotion                   = DLSSNR_MOTION_DEF;
		bDlssNRMotionVectors            = false;
		iDlssNRToggleKey                = VK_F12;
		szDlssNRDllPath[0]              = L'\0';
		bDlssSR                         = false;
		iDlssSRPreset                   = DLSSSR_PRESET_DEF;
		szDlssSRDllPath[0]              = L'\0';
		bDlssRenderAhead                = true;
	}
};

// Every field of the DLSS 5 page, DLSS Super Resolution included, for the
// property pages that must not overwrite each other: the main page takes these
// from the renderer before applying.
inline void CopyDlssSettings(Settings_t& dst, const Settings_t& src)
{
	dst.bDlssNR               = src.bDlssNR;
	dst.iDlssNRStyle          = src.iDlssNRStyle;
	dst.iDlssNRPreset         = src.iDlssNRPreset;
	dst.iDlssNRIntensity      = src.iDlssNRIntensity;
	dst.iDlssNRLocalTone      = src.iDlssNRLocalTone;
	dst.iDlssNRLocalStructure = src.iDlssNRLocalStructure;
	dst.iDlssNRSkinStructure  = src.iDlssNRSkinStructure;
	dst.bDlssNRAutoMask       = src.bDlssNRAutoMask;
	dst.bDlssNRNoHistory      = src.bDlssNRNoHistory;
	dst.iDlssNRStabilizer     = src.iDlssNRStabilizer;
	dst.iDlssNRMotion         = src.iDlssNRMotion;
	dst.bDlssNRMotionVectors  = src.bDlssNRMotionVectors;
	dst.bDlssNRAfterUpscale   = src.bDlssNRAfterUpscale;
	dst.iDlssNRToggleKey      = src.iDlssNRToggleKey;
	wcscpy_s(dst.szDlssNRDllPath, src.szDlssNRDllPath);
	dst.bDlssSR               = src.bDlssSR;
	dst.iDlssSRPreset         = src.iDlssSRPreset;
	wcscpy_s(dst.szDlssSRDllPath, src.szDlssSRDllPath);
	// bDlssRenderAhead is not here: it belongs to the main page, which sets it for
	// the prescalers as much as for DLSS.
}

// What the hardware video processor is doing with the picture being played, which
// decides whether the shader lists have anything to do: it converts the formats it
// was given, chroma upsampling included, and resizes when it was told to and no DLSS
// pass has taken the resizing back from it.
enum :unsigned {
	VPUSE_Converting = 1,
	VPUSE_Resizing   = 2,
};

interface __declspec(uuid("1AB00F10-5F55-42AC-B53F-38649F11BE3E"))
IVideoRenderer : public IUnknown {
	STDMETHOD(GetVideoProcessorInfo) (std::wstring& str) PURE;
	STDMETHOD_(bool, GetActive()) PURE;

	STDMETHOD_(void, GetSettings(Settings_t& setings)) PURE;
	STDMETHOD_(void, SetSettings(const Settings_t& setings)) PURE;

	STDMETHOD(SaveSettings()) PURE;

	// Everything of ours goes after everything of upstream's, and stays there. The
	// interface keeps upstream's identifier, so a caller built against upstream's
	// header finds each method by its place in the table: one of ours put in the
	// middle moves all of upstream's that follow it, and the caller then asks for
	// the settings and saves them instead. Measured, when this was in the middle:
	// setting the options on a stock build did nothing at all and wrote its defaults
	// to the registry.
	STDMETHOD_(unsigned, GetVideoProcessorUse()) PURE;
};
