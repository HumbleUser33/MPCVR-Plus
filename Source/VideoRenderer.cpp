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

#include "stdafx.h"
#include <atomic>
#include <optional>
#include <evr.h> // for MR_VIDEO_ACCELERATION_SERVICE, because the <mfapi.h> does not contain it
#include <Mferror.h>
#include "Helper.h"
#include "PropPage.h"
#include "SettingsPage/SettingsPage.h"
#include "VideoRendererInputPin.h"
#include "../Include/Version.h"
#include "VideoRenderer.h"
#include "SubPic/XySubPicProvider.h"
#include "SubPic/XySubPicQueueImpl.h"

#define WM_SWITCH_FULLSCREEN (WM_APP + 0x1000)

#define OPT_REGKEY_VIDEORENDERER           L"Software\\MPC-BE Filters\\MPC Video Renderer"
#define OPT_UseD3D11                       L"UseD3D11"
#define OPT_ShowStatistics                 L"ShowStatistics"
#define OPT_ResizeStatistics               L"ResizeStatistics"
#define OPT_TextureFormat                  L"TextureFormat"
#define OPT_VPEnableNV12                   L"VPEnableNV12"
#define OPT_VPEnableP01x                   L"VPEnableP01x"
#define OPT_VPEnableYUY2                   L"VPEnableYUY2"
#define OPT_VPEnableOther                  L"VPEnableOther"
#define OPT_VPDeinterlacing                L"VPDeinterlacing"
#define OPT_DoubleFrateDeint               L"DoubleFramerateDeinterlace"
#define OPT_VPScaling                      L"VPScaling"
#define OPT_VPSuperResolution              L"VPSuperResolution"
#define OPT_VPRTXVideoHDR                  L"VPRTXVideoHDR"
#define OPT_VPReplaceChroma                L"VPReplaceChroma"
#define OPT_Sharpen                        L"Sharpen"
#define OPT_SharpenLevel                   L"SharpenLevel"
#define OPT_ChromaUpsampling               L"ChromaUpsampling"
#define OPT_Upscaling                      L"Upscaling"
#define OPT_Downscaling                    L"Downscaling"
#define OPT_InterpolateAt50pct             L"InterpolateAt50pct"
#define OPT_Dither                         L"Dither"
#define OPT_DeintBlend                     L"DeinterlaceBlend"
#define OPT_SwapEffect                     L"SwapEffect"
#define OPT_ExclusiveFullscreen            L"ExclusiveFullscreen"
#define OPT_VBlankBeforePresent            L"VBlankBeforePresent"
#define OPT_AdjustPresentTime              L"AdjustPresentationTime"
#define OPT_ReinitByDisplay                L"ReinitWhenChangingDisplay"
#define OPT_HdrPreferDoVi                  L"HdrPreferDoVi"
#define OPT_HdrPassthrough                 L"HdrPassthrough"
#define OPT_HdrLocaLToneMapping            L"HdrLocalToneMapping"
#define OPT_HdrLocaLToneMappingType        L"HdrLocalToneMappingType"
#define OPT_HdrDisplayNits                 L"HdrDisplayNits"
#define OPT_HdrToggleDisplay               L"HdrToggleDisplay"
#define OPT_HdrOsdBrightness               L"HdrOsdBrightness"
#define OPT_ConvertToSdr                   L"ConvertToSdr"
#define OPT_UseD3DFullscreen               L"UseD3DFullscreen"
#define OPT_DisplayNits                    L"DisplayNits"
#define OPT_DlssNR                         L"DlssNeuralRendering"
#define OPT_DlssNRStyle                    L"DlssNRStyle"
#define OPT_DlssNRPreset                   L"DlssNRPreset"
#define OPT_DlssNRIntensity                L"DlssNRIntensity"
#define OPT_DlssNRLocalTone                L"DlssNRLocalToneStrength"
#define OPT_DlssNRLocalStructure           L"DlssNRLocalStructureStrength"
#define OPT_DlssNRSkinStructure            L"DlssNRSkinStructureStrength"
#define OPT_DlssNRAutoMask                 L"DlssNRUseAutoMask"
#define OPT_DlssNRAfterUpscale             L"DlssNRApplyAfterUpscaling"
#define OPT_DlssNRToggleKey                L"DlssNRToggleKey"
#define OPT_DlssNRDllPath                  L"DlssNRDllPath"
#define OPT_DlssNRStabilizer               L"DlssNRStabilizerStrength"
#define OPT_DlssNRMotionVectors            L"DlssNRMotionVectorsToNetwork"
#define OPT_DlssNRWorkingScale             L"DlssNRWorkingScale"
#define OPT_DlssNRFlowDivider              L"DlssNRFlowDivider"
#define OPT_DlssSR                         L"DlssSRUpscaling"
#define OPT_DlssSRPreset                   L"DlssSRPreset"
#define OPT_DlssSRDllPath                  L"DlssSRDllPath"
#define OPT_DlssSRDlaa                     L"DlssSRDlaa"
#define OPT_UpscalingSmall                 L"UpscalingSmall"
#define OPT_UpscalingSmallFilter           L"UpscalingSmallFilter"
#define OPT_DlssRenderAhead                L"DlssRenderAhead"
#define OPT_VideoDelay                     L"VideoDelay"

static std::atomic_int g_nInstance = 0;

// The keyboard hook is a plain callback, so it needs a way back to the filter.
// One renderer per process in practice; if a second one appears the last to
// install wins, which is no worse than two filters fighting over one key.
static CMpcVideoRenderer* g_pToggleKeyOwner = nullptr;

// The settings to and from a registry key, by name. The filter's own key and each
// preset's go through these two and nothing else, so a setting added later is in
// the presets without anybody having to remember them.
static void ReadSettings(CRegKey& key, Settings_t& sets)
{
	DWORD dw;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_UseD3D11, dw)) {
		sets.bUseD3D11 = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ShowStatistics, dw)) {
		sets.bShowStats = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ResizeStatistics, dw)) {
		sets.iResizeStats = discard<int>(dw, 0, 0, 1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_TextureFormat, dw)) {
		switch (dw) {
		case TEXFMT_AUTOINT:
		case TEXFMT_8INT:
		case TEXFMT_10INT:
		case TEXFMT_16FLOAT:
			sets.iTexFormat = dw;
			break;
		default:
			sets.iTexFormat = TEXFMT_AUTOINT;
		}
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableNV12, dw)) {
		sets.VPFmts.bNV12 = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableP01x, dw)) {
		sets.VPFmts.bP01x = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableYUY2, dw)) {
		sets.VPFmts.bYUY2 = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableOther, dw)) {
		sets.VPFmts.bOther = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPDeinterlacing, dw)) {
		sets.iVPDeinterlacing = discard<int>(dw, DEINT_Enable, DEINT_Disable, DEINT_HackFutureFrames);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DoubleFrateDeint, dw)) {
		sets.bDeintDouble = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPScaling, dw)) {
		sets.bVPScaling = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPSuperResolution, dw)) {
		sets.iVPSuperRes = discard<int>(dw, SUPERRES_Disable, 0, SUPERRES_COUNT-1);
	}
#ifdef _WIN64
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPRTXVideoHDR, dw)) {
		sets.bVPRTXVideoHDR = !!dw;
	}
#endif
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPReplaceChroma, dw)) {
		sets.bVPReplaceChroma = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Sharpen, dw)) {
		sets.iSharpen = discard<int>(dw, SHARPEN_Disabled, 0, SHARPEN_COUNT - 1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SharpenLevel, dw)) {
		sets.iSharpenLevel = discard<int>(dw, SHARPEN_LEVEL_DEF, SHARPEN_LEVEL_MIN, SHARPEN_LEVEL_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ChromaUpsampling, dw)) {
		sets.iChromaScaling = discard<int>(dw, CHROMA_CatmullRom, 0, CHROMA_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Upscaling, dw)) {
		sets.iUpscaling = discard<int>(dw, UPSCALE_Jinc2, 0, UPSCALE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Downscaling, dw)) {
		sets.iDownscaling = discard<int>(dw, DOWNSCALE_Hamming, 0, DOWNSCALE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_InterpolateAt50pct, dw)) {
		sets.bInterpolateAt50pct = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Dither, dw)) {
		sets.bUseDither = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DeintBlend, dw)) {
		sets.bDeintBlend = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SwapEffect, dw)) {
		sets.iSwapEffect = discard<int>(dw, SWAPEFFECT_Flip, SWAPEFFECT_Discard, SWAPEFFECT_Flip);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ExclusiveFullscreen, dw)) {
		sets.bExclusiveFS = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VBlankBeforePresent, dw)) {
		sets.bVBlankBeforePresent = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_AdjustPresentTime, dw)) {
		sets.bAdjustPresentTime = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ReinitByDisplay, dw)) {
		sets.bReinitByDisplay = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrPreferDoVi, dw)) {
		sets.bHdrPreferDoVi = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrPassthrough, dw)) {
		sets.bHdrPassthrough = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrLocaLToneMapping, dw)) {
		sets.bHdrLocalToneMapping = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrLocaLToneMappingType, dw)) {
		sets.iHdrLocalToneMappingType = discard<int>(dw, 1, 1, 5);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrDisplayNits, dw)) {
		sets.iHdrDisplayMaxNits = discard<int>(dw, HDR_NITS_DEF, HDR_NITS_MIN, HDR_NITS_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrToggleDisplay, dw)) {
		sets.iHdrToggleDisplay = discard<int>(dw, HDRTD_On, HDRTD_Disabled, HDRTD_OnOff);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrOsdBrightness, dw)) {
		sets.iHdrOsdBrightness = discard<int>(dw, 0, 0, 2);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ConvertToSdr, dw)) {
		sets.bConvertToSdr = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DisplayNits, dw)) {
		sets.iSDRDisplayNits = discard<int>(dw, SDR_NITS_DEF, SDR_NITS_MIN, SDR_NITS_MAX);
	}
#ifdef _WIN64
	// The DLSS 5 NR snippet is x64 only.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNR, dw)) {
		sets.bDlssNR = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRStyle, dw)) {
		sets.iDlssNRStyle = discard<int>((int)dw, DLSSNR_STYLE_Default, 0, DLSSNR_STYLE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRPreset, dw)) {
		sets.iDlssNRPreset = discard<int>((int)dw, 0, 0, DLSSNR_PRESET_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRIntensity, dw)) {
		sets.iDlssNRIntensity = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_STR_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRLocalTone, dw)) {
		sets.iDlssNRLocalTone = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_STR_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRLocalStructure, dw)) {
		sets.iDlssNRLocalStructure = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_STR_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRSkinStructure, dw)) {
		// Negative values round-trip through DWORD as two's complement.
		sets.iDlssNRSkinStructure = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_SKIN_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRAutoMask, dw)) {
		sets.bDlssNRAutoMask = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRAfterUpscale, dw)) {
		sets.bDlssNRAfterUpscale = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRStabilizer, dw)) {
		sets.iDlssNRStabilizer = discard<int>((int)dw, DLSSNR_STAB_DEF, DLSSNR_STAB_MIN, DLSSNR_STAB_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRWorkingScale, dw)) {
		sets.iDlssNRWorkingScale = discard<int>((int)dw, DLSSNR_SCALE_DEF, DLSSNR_SCALE_MIN, DLSSNR_SCALE_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRFlowDivider, dw)) {
		sets.iDlssNRFlowDivider = discard<int>((int)dw, DLSSNR_FLOW_DEF, DLSSNR_FLOW_MIN, DLSSNR_FLOW_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRMotionVectors, dw)) {
		sets.bDlssNRMotionVectors = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRToggleKey, dw)) {
		sets.iDlssNRToggleKey = discard<int>((int)dw, VK_F12, 0, 0xFF);
	}
	{
		ULONG nChars = std::size(sets.szDlssNRDllPath);
		if (ERROR_SUCCESS != key.QueryStringValue(OPT_DlssNRDllPath, sets.szDlssNRDllPath, &nChars)) {
			sets.szDlssNRDllPath[0] = L'\0';
		}
		sets.szDlssNRDllPath[std::size(sets.szDlssNRDllPath) - 1] = L'\0';
	}
	// DLSS Super Resolution, x64 as well: the NGX runtime has no 32-bit build.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssSR, dw)) {
		sets.bDlssSR = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssSRDlaa, dw)) {
		sets.bDlssSRDlaa = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssSRPreset, dw)) {
		sets.iDlssSRPreset = ((int)dw >= DLSSSR_PRESET_J && (int)dw <= DLSSSR_PRESET_M) ? (int)dw : DLSSSR_PRESET_DEF;
	}
	{
		ULONG nChars = std::size(sets.szDlssSRDllPath);
		if (ERROR_SUCCESS != key.QueryStringValue(OPT_DlssSRDllPath, sets.szDlssSRDllPath, &nChars)) {
			sets.szDlssSRDllPath[0] = L'\0';
		}
		sets.szDlssSRDllPath[std::size(sets.szDlssSRDllPath) - 1] = L'\0';
	}
#endif
	// Render ahead is set on the main page and covers the mpv prescalers as well,
	// so it is not x64 only like the rest of DLSS.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssRenderAhead, dw)) {
		sets.bDlssRenderAhead = !!dw;
	}
	// Not x64 only either: these are plain resize shaders.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_UpscalingSmall, dw)) {
		sets.bUpscalingSmall = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_UpscalingSmallFilter, dw)) {
		sets.iUpscalingSmall = discard<int>(dw, UPSCALE_Jinc2, 0, UPSCALE_COUNT-1);
	}
	// Signed, in a registry that only keeps unsigned values: it goes in as it is and
	// comes back through the same cast.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VideoDelay, dw)) {
		sets.iVideoDelay = discard<int>((int)dw, 0, VIDEO_DELAY_MIN, VIDEO_DELAY_MAX);
	}
}

static void WriteSettings(CRegKey& key, const Settings_t& sets)
{
	key.SetDWORDValue(OPT_UseD3D11,            sets.bUseD3D11);
	key.SetDWORDValue(OPT_ShowStatistics,      sets.bShowStats);
	key.SetDWORDValue(OPT_ResizeStatistics,    sets.iResizeStats);
	key.SetDWORDValue(OPT_TextureFormat,       sets.iTexFormat);
	key.SetDWORDValue(OPT_VPEnableNV12,        sets.VPFmts.bNV12);
	key.SetDWORDValue(OPT_VPEnableP01x,        sets.VPFmts.bP01x);
	key.SetDWORDValue(OPT_VPEnableYUY2,        sets.VPFmts.bYUY2);
	key.SetDWORDValue(OPT_VPEnableOther,       sets.VPFmts.bOther);
	key.SetDWORDValue(OPT_VPDeinterlacing,     sets.iVPDeinterlacing);
	key.SetDWORDValue(OPT_DoubleFrateDeint,    sets.bDeintDouble);
	key.SetDWORDValue(OPT_VPScaling,           sets.bVPScaling);
	key.SetDWORDValue(OPT_VPSuperResolution,   sets.iVPSuperRes);
#ifdef _WIN64
	key.SetDWORDValue(OPT_VPRTXVideoHDR,       sets.bVPRTXVideoHDR);
#endif
	key.SetDWORDValue(OPT_VPReplaceChroma,     sets.bVPReplaceChroma);
	key.SetDWORDValue(OPT_Sharpen,             sets.iSharpen);
	key.SetDWORDValue(OPT_SharpenLevel,        sets.iSharpenLevel);
	key.SetDWORDValue(OPT_ChromaUpsampling,    sets.iChromaScaling);
	key.SetDWORDValue(OPT_Upscaling,           sets.iUpscaling);
	key.SetDWORDValue(OPT_Downscaling,         sets.iDownscaling);
	key.SetDWORDValue(OPT_InterpolateAt50pct,  sets.bInterpolateAt50pct);
	key.SetDWORDValue(OPT_Dither,              sets.bUseDither);
	key.SetDWORDValue(OPT_DeintBlend,          sets.bDeintBlend);
	key.SetDWORDValue(OPT_SwapEffect,          sets.iSwapEffect);
	key.SetDWORDValue(OPT_ExclusiveFullscreen, sets.bExclusiveFS);
	key.SetDWORDValue(OPT_VBlankBeforePresent, sets.bVBlankBeforePresent);
	key.SetDWORDValue(OPT_AdjustPresentTime,   sets.bAdjustPresentTime);
	key.SetDWORDValue(OPT_ReinitByDisplay,     sets.bReinitByDisplay);
	key.SetDWORDValue(OPT_HdrPreferDoVi,       sets.bHdrPreferDoVi);
	key.SetDWORDValue(OPT_HdrPassthrough,      sets.bHdrPassthrough);
	key.SetDWORDValue(OPT_HdrLocaLToneMapping, sets.bHdrLocalToneMapping);
	key.SetDWORDValue(OPT_HdrLocaLToneMappingType, sets.iHdrLocalToneMappingType);
	key.SetDWORDValue(OPT_HdrDisplayNits,      sets.iHdrDisplayMaxNits);
	key.SetDWORDValue(OPT_HdrToggleDisplay,    sets.iHdrToggleDisplay);
	key.SetDWORDValue(OPT_HdrOsdBrightness,    sets.iHdrOsdBrightness);
	key.SetDWORDValue(OPT_ConvertToSdr,        sets.bConvertToSdr);
	key.SetDWORDValue(OPT_DisplayNits,         sets.iSDRDisplayNits);
#ifdef _WIN64
	key.SetDWORDValue(OPT_DlssNR,              sets.bDlssNR);
	key.SetDWORDValue(OPT_DlssNRStyle,         sets.iDlssNRStyle);
	key.SetDWORDValue(OPT_DlssNRPreset,        sets.iDlssNRPreset);
	key.SetDWORDValue(OPT_DlssNRIntensity,     sets.iDlssNRIntensity);
	key.SetDWORDValue(OPT_DlssNRLocalTone,     sets.iDlssNRLocalTone);
	key.SetDWORDValue(OPT_DlssNRLocalStructure, sets.iDlssNRLocalStructure);
	key.SetDWORDValue(OPT_DlssNRSkinStructure, sets.iDlssNRSkinStructure);
	key.SetDWORDValue(OPT_DlssNRAutoMask,      sets.bDlssNRAutoMask);
	key.SetDWORDValue(OPT_DlssNRAfterUpscale,  sets.bDlssNRAfterUpscale);
	key.SetDWORDValue(OPT_DlssNRStabilizer,    sets.iDlssNRStabilizer);
	key.SetDWORDValue(OPT_DlssNRMotionVectors, sets.bDlssNRMotionVectors);
	key.SetDWORDValue(OPT_DlssNRWorkingScale,  sets.iDlssNRWorkingScale);
	key.SetDWORDValue(OPT_DlssNRFlowDivider,   sets.iDlssNRFlowDivider);
	key.SetDWORDValue(OPT_DlssNRToggleKey,     sets.iDlssNRToggleKey);
	key.SetStringValue(OPT_DlssNRDllPath,      sets.szDlssNRDllPath);
	key.SetDWORDValue(OPT_DlssSR,              sets.bDlssSR);
	key.SetDWORDValue(OPT_DlssSRDlaa,          sets.bDlssSRDlaa);
	key.SetDWORDValue(OPT_DlssSRPreset,        sets.iDlssSRPreset);
	key.SetStringValue(OPT_DlssSRDllPath,      sets.szDlssSRDllPath);
#endif
	key.SetDWORDValue(OPT_DlssRenderAhead,     sets.bDlssRenderAhead);
	key.SetDWORDValue(OPT_UpscalingSmall,      sets.bUpscalingSmall);
	key.SetDWORDValue(OPT_UpscalingSmallFilter, sets.iUpscalingSmall);
	key.SetDWORDValue(OPT_VideoDelay,          (DWORD)sets.iVideoDelay);
}

// Presets: eight keys beside the settings, each holding a whole Settings_t under
// the same names, the name the user gave it and the key that recalls it.
static std::wstring PresetKeyPath(int slot)
{
	return std::format(L"{}\\Presets\\{}", OPT_REGKEY_VIDEORENDERER, slot + 1);
}

// What belongs to the installation and not to a taste stays out of a preset:
// where the two DLLs are, the DLSS toggle key, and how late the room's sound is.
static void KeepInstallation(Settings_t& sets, const Settings_t& current)
{
	sets.iDlssNRToggleKey = current.iDlssNRToggleKey;
	wcscpy_s(sets.szDlssNRDllPath, current.szDlssNRDllPath);
	wcscpy_s(sets.szDlssSRDllPath, current.szDlssSRDllPath);
	sets.iVideoDelay = current.iVideoDelay;
}


static const wchar_t g_szClassName[] = L"VRWindow";

LPCWSTR g_pszOldParentWndProc = L"OldParentWndProc";
LPCWSTR g_pszThis = L"This";

static void RemoveParentWndProc(HWND hWnd)
{
	DLog(L"RemoveParentWndProc()");
	auto pfnOldProc = GetPropW(hWnd, g_pszOldParentWndProc);
	if (pfnOldProc) {
		auto lpPreviousProc = SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
		if (lpPreviousProc != 0) {
			RemovePropW(hWnd, g_pszOldParentWndProc);
			RemovePropW(hWnd, g_pszThis);
		} else {
			ASSERT(false);
		}
	}
}

static LRESULT CALLBACK ParentWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	auto pfnOldProc = reinterpret_cast<WNDPROC>(GetPropW(hWnd, g_pszOldParentWndProc));
	auto pThis = static_cast<CMpcVideoRenderer*>(GetPropW(hWnd, g_pszThis));

	switch (Msg) {
		case WM_DESTROY:
			if (pfnOldProc) {
				auto lpPreviousProc = SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
				if (lpPreviousProc != 0) {
					RemovePropW(hWnd, g_pszOldParentWndProc);
					RemovePropW(hWnd, g_pszThis);
				} else {
					ASSERT(false);
				}
			}
			break;
		case WM_DISPLAYCHANGE:
			DLog(L"ParentWndProc() - WM_DISPLAYCHANGE");
			pThis->OnDisplayModeChange(true);
			break;
		case WM_MOVE:
			if (pThis->m_bExclusiveScreen) {
				// I don't know why, but without this, the filter freezes when switching from fullscreen to window in DX9 mode.
				if (pfnOldProc) {
					SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
					SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ParentWndProc));
				}
			} else {
				pThis->OnWindowMove();
			}
			break;
		case WM_NCACTIVATE:
			if (!wParam && pThis->m_bExclusiveScreen && !pThis->m_bIsD3DFullscreen) {
				return 0;
			}
			break;
		case WM_RBUTTONUP:
			if (pThis->m_bExclusiveScreen) {
				// block context menu in exclusive fullscreen
				return 0;
			}
			break;
/*
		case WM_SYSCOMMAND:
			if (pThis->m_bExclusiveScreen && wParam == SC_MINIMIZE) {
				// block minimize in exclusive fullscreen
				return 0;
			}
			break;
*/
	}

	if (!pfnOldProc || reinterpret_cast<LONG_PTR>(pfnOldProc) == reinterpret_cast<LONG_PTR>(ParentWndProc)) {
		ASSERT(false);
		return 0;
	}

	return CallWindowProcW(pfnOldProc, hWnd, Msg, wParam, lParam);
}

//
// CMpcVideoRenderer
//

CMpcVideoRenderer::CMpcVideoRenderer(LPUNKNOWN pUnk, HRESULT* phr)
	: CBaseVideoRenderer2(__uuidof(this), L"MPC Video Renderer", pUnk, phr)
{
	DLog(L"CMpcVideoRenderer::CMpcVideoRenderer()");

	auto nPrevInstance = g_nInstance++; // always increment g_nInstance in the constructor
	if (nPrevInstance > 0) {
		*phr = E_ABORT;
		DLog(L"Previous copy of CMpcVideoRenderer found! Initialization aborted.");
		return;
	}

	DLog(L"Windows {}", GetWindowsVersion());
	DLog(GetNameAndVersion());

	ASSERT(S_OK == *phr);
	m_pInputPin = new CVideoRendererInputPin(this, phr, L"In", this);
	ASSERT(S_OK == *phr);

	// read settings

	CRegKey key;
	if (ERROR_SUCCESS == key.Open(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER, KEY_READ)) {
		ReadSettings(key, m_Sets);
	}
	ReadPresetKeys();

	if (!IsWindows10OrGreater()) {
		m_Sets.bHdrPassthrough = false;
		m_Sets.iHdrToggleDisplay = HDRTD_Disabled;
	}

	HRESULT hr = S_FALSE;

	if (m_Sets.bUseD3D11 && IsWindows7SP1OrGreater()) {
		m_VideoProcessor.reset(new CDX11VideoProcessor(this, m_Sets, hr));
		if (SUCCEEDED(hr)) {
			hr = m_VideoProcessor->Init(m_hWnd, false);
		}

		if (FAILED(hr)) {
			m_VideoProcessor.reset();
		}
		DLogIf(S_OK == hr, L"Direct3D11 initialization successfully!");
	}

	if (!m_VideoProcessor) {
		m_VideoProcessor.reset(new CDX9VideoProcessor(this, m_Sets, hr));
		if (SUCCEEDED(hr)) {
			hr = m_VideoProcessor->Init(::GetForegroundWindow(), false);
		}

		DLogIf(S_OK == hr, L"Direct3D9 initialization successfully!");
	}

	*phr = hr;

	return;
}

// ---------------------------------------------------------------------------
// DLSS toggle key
//
// A thread-local WH_KEYBOARD hook on the thread that owns the video window, so
// it sees only that thread's input -- no system-wide hook. It swallows the one
// key it is bound to and passes everything else through untouched, which
// matters because the default (Home) is also a seek shortcut in most players.
// ---------------------------------------------------------------------------

LRESULT CALLBACK CMpcVideoRenderer::ToggleKeyProc(int code, WPARAM wParam, LPARAM lParam)
{
	CMpcVideoRenderer* pThis = g_pToggleKeyOwner;

	if (code == HC_ACTION && pThis && wParam) {
		// What is held with the key is only asked once a preset wants that key, and a
		// caret means somebody is typing: a Shift+1 meant for a file name is not ours.
		int held = -1;
		for (int slot = 0; slot < PRESET_COUNT; slot++) {
			if (!pThis->m_presetKeys[slot] || (int)wParam != (pThis->m_presetKeys[slot] & 0xFF)) {
				continue;
			}
			if (held < 0) {
				GUITHREADINFO gui = { sizeof(gui) };
				if (GetGUIThreadInfo(GetCurrentThreadId(), &gui) && gui.hwndCaret) {
					break;
				}
				held = (GetKeyState(VK_SHIFT) < 0 ? PRESETKEY_Shift : 0)
				     | (GetKeyState(VK_CONTROL) < 0 ? PRESETKEY_Ctrl : 0)
				     | (GetKeyState(VK_MENU) < 0 ? PRESETKEY_Alt : 0);
			}
			if (((int)wParam | held) == pThis->m_presetKeys[slot]) {
				const bool bKeyUp = (lParam & 0x80000000) != 0;
				const bool bRepeat = (lParam & 0x40000000) != 0;
				if (!bKeyUp && !bRepeat) {
					pThis->RecallPreset(slot);
				}
				return 1; // consumed, like the toggle key
			}
		}
	}

	if (code == HC_ACTION && pThis && pThis->m_Sets.iDlssNRToggleKey
			&& (int)wParam == pThis->m_Sets.iDlssNRToggleKey) {
		const bool bKeyUp = (lParam & 0x80000000) != 0;
		const bool bRepeat = (lParam & 0x40000000) != 0;

		if (!bKeyUp && !bRepeat) {
			pThis->Flt_SetBool("dlssNR", !pThis->m_Sets.bDlssNR);
		}
		return 1; // consumed, so the player does not also act on it
	}

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void CMpcVideoRenderer::ReadPresetKeys()
{
	for (int slot = 0; slot < PRESET_COUNT; slot++) {
		m_presetKeys[slot] = 0;
		GetPresetInfo(slot, nullptr, 0, &m_presetKeys[slot]);
	}
}

// A preset recalled by its key: applied as the page's Apply would, and kept as the
// settings in force so the next film starts with it.
void CMpcVideoRenderer::RecallPreset(int slot)
{
	Settings_t sets;
	if (S_OK != LoadPreset(slot, sets)) {
		return;
	}
	KeepInstallation(sets, m_Sets);
	sets.bShowStats = m_Sets.bShowStats;   // recalled with the statistics as they are, up or not

	// The name goes up before the settings go in. A film that is paused is drawn again
	// by the settings themselves and by nothing after: with the name posted afterwards
	// that picture went out without it, and the next recall showed the name of this
	// one -- or none, or the wrong one, as the redraws fell.
	wchar_t name[64] = {};
	GetPresetInfo(slot, name, (int)std::size(name), nullptr);
	m_VideoProcessor->ShowNotice(name[0] ? std::format(L"Preset {}: {}", slot + 1, name)
	                                     : std::format(L"Preset {}", slot + 1));
	SetSettings(sets);
	SaveSettings();

	// Paused, nothing draws the name unless asked, and nothing takes it down either.
	if (m_filterState != State_Running) {
		Redraw();
	}
	if (m_hNoticeTimer) {
		DeleteTimerQueueTimer(nullptr, m_hNoticeTimer, INVALID_HANDLE_VALUE);
		m_hNoticeTimer = nullptr;
	}
	CreateTimerQueueTimer(&m_hNoticeTimer, nullptr, NoticeTimerProc, this, 2600, 0, WT_EXECUTEONLYONCE);
}

void CALLBACK CMpcVideoRenderer::NoticeTimerProc(void* pParam, BOOLEAN)
{
	auto pThis = static_cast<CMpcVideoRenderer*>(pParam);
	if (pThis->m_filterState != State_Running) {
		pThis->Redraw();
	}
}

STDMETHODIMP CMpcVideoRenderer::SavePreset(int slot, const Settings_t& sets, LPCWSTR name, int vk)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	CRegKey key;
	if (ERROR_SUCCESS != key.Create(HKEY_CURRENT_USER, PresetKeyPath(slot).c_str())) {
		return E_FAIL;
	}
	WriteSettings(key, sets);
	key.SetDWORDValue(L"Saved", 1);
	key.Close();
	return SetPresetInfo(slot, name, vk);
}

STDMETHODIMP CMpcVideoRenderer::LoadPreset(int slot, Settings_t& sets)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	CRegKey key;
	DWORD saved = 0;
	if (ERROR_SUCCESS != key.Open(HKEY_CURRENT_USER, PresetKeyPath(slot).c_str(), KEY_READ)
			|| ERROR_SUCCESS != key.QueryDWORDValue(L"Saved", saved) || !saved) {
		return S_FALSE;   // nothing was ever saved here
	}
	sets.SetDefault();
	ReadSettings(key, sets);
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetPresetInfo(int slot, LPWSTR name, int cchName, int* pVk)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	if (name && cchName > 0) {
		name[0] = 0;
	}
	if (pVk) {
		*pVk = 0;
	}
	CRegKey key;
	if (ERROR_SUCCESS != key.Open(HKEY_CURRENT_USER, PresetKeyPath(slot).c_str(), KEY_READ)) {
		return S_FALSE;
	}
	if (name && cchName > 0) {
		ULONG chars = (ULONG)cchName;
		if (ERROR_SUCCESS != key.QueryStringValue(L"Name", name, &chars)) {
			name[0] = 0;
		}
	}
	DWORD dw = 0;
	if (pVk && ERROR_SUCCESS == key.QueryDWORDValue(L"Key", dw)) {
		*pVk = (int)(dw & PRESETKEY_Mask);
	}
	DWORD saved = 0;
	return (ERROR_SUCCESS == key.QueryDWORDValue(L"Saved", saved) && saved) ? S_OK : S_FALSE;
}

STDMETHODIMP CMpcVideoRenderer::SetPresetInfo(int slot, LPCWSTR name, int vk)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	CRegKey key;
	if (ERROR_SUCCESS != key.Create(HKEY_CURRENT_USER, PresetKeyPath(slot).c_str())) {
		return E_FAIL;
	}
	key.SetStringValue(L"Name", name ? name : L"");
	key.SetDWORDValue(L"Key", (DWORD)(vk & PRESETKEY_Mask));
	key.Close();
	ReadPresetKeys();
	InstallToggleKeyHook();
	return S_OK;
}

void CMpcVideoRenderer::InstallToggleKeyHook()
{
	bool bAnyKey = m_Sets.iDlssNRToggleKey != 0;
	for (const int vk : m_presetKeys) {
		bAnyKey = bAnyKey || vk != 0;
	}
	if (!bAnyKey) {
		RemoveToggleKeyHook();
		return;
	}

	// Keys arrive on the player's UI thread, which owns its main window -- not
	// necessarily the thread that called Init(). Hooking our own thread saw no
	// keystrokes at all, which is why the shortcut did nothing.
	HWND hTarget = m_hWndParentMain ? m_hWndParentMain
	             : (m_hWndParent ? m_hWndParent : m_hWndWindow);
	if (!hTarget) {
		return;
	}
	const DWORD dwThread = GetWindowThreadProcessId(hTarget, nullptr);
	if (!dwThread) {
		return;
	}
	if (m_hKeyboardHook && dwThread == m_dwHookedThread) {
		return;
	}

	RemoveToggleKeyHook();
	g_pToggleKeyOwner = this;
	m_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD, &CMpcVideoRenderer::ToggleKeyProc,
	                                    nullptr, dwThread);
	m_dwHookedThread = m_hKeyboardHook ? dwThread : 0;
	DLogIf(!m_hKeyboardHook, L"CMpcVideoRenderer::InstallToggleKeyHook() : failed on thread {}, error {}",
		dwThread, GetLastError());
}

void CMpcVideoRenderer::RemoveToggleKeyHook()
{
	if (m_hKeyboardHook) {
		UnhookWindowsHookEx(m_hKeyboardHook);
		m_hKeyboardHook = nullptr;
	}
	m_dwHookedThread = 0;
	if (g_pToggleKeyOwner == this) {
		g_pToggleKeyOwner = nullptr;
	}
}

CMpcVideoRenderer::~CMpcVideoRenderer()
{
	DLog(L"CMpcVideoRenderer::~CMpcVideoRenderer()");

	RemoveToggleKeyHook();
	if (m_hNoticeTimer) {
		// Waits for a callback that is running: it must not outlive what it draws with.
		DeleteTimerQueueTimer(nullptr, m_hNoticeTimer, INVALID_HANDLE_VALUE);
		m_hNoticeTimer = nullptr;
	}

	UnregisterClassW(g_szClassName, g_hInst);

	if (m_hWndParentMain) {
		RemoveParentWndProc(m_hWndParentMain);
	}

	if (m_bExclusiveScreen && !m_bIsD3DFullscreen && m_hWndParentMain) {
		PostMessageW(m_hWndParentMain, WM_SWITCH_FULLSCREEN, 0, 0);
	}

	m_VideoProcessor.reset();

	if (m_hWndWindow) {
		::SendMessageW(m_hWndWindow, WM_CLOSE, 0, 0);
	}

	g_nInstance--; // always decrement g_nInstance in the destructor
}

void CMpcVideoRenderer::NewSegment(REFERENCE_TIME startTime)
{
	DLog(L"CMpcVideoRenderer::NewSegment()");

	m_rtStartTime = startTime;
}

HRESULT CMpcVideoRenderer::BeginFlush()
{
	DLog(L"CMpcVideoRenderer::BeginFlush()");

	m_bFlushing = true;
	return __super::BeginFlush();
}

HRESULT CMpcVideoRenderer::EndFlush()
{
	DLog(L"CMpcVideoRenderer::EndFlush()");

	m_VideoProcessor->Flush();

	HRESULT hr = __super::EndFlush();

	m_bFlushing = false;

	return hr;
}

long CMpcVideoRenderer::CalcImageSize(CMediaType& mt, bool redefine_mt)
{
	BITMAPINFOHEADER* pBIH = GetBIHfromVIHs(&mt);
	if (!pBIH) {
		ASSERT(FALSE); // excessive checking
		return 0;
	}

	if (redefine_mt) {
		CAutoLock cRendererLock(&m_RendererLock);

		CSize Size(pBIH->biWidth, pBIH->biHeight);

		BOOL ret = m_VideoProcessor->GetAlignmentSize(mt, Size);

		if (ret && (Size.cx != pBIH->biWidth || Size.cy != pBIH->biHeight)) {
			BYTE* pbFormat = mt.ReallocFormatBuffer(VR_EXRADATA_POS + sizeof(VR_Extradata));
			if (pbFormat) {
				// update pointer after realoc
				pBIH = GetBIHfromVIHs(&mt);
				// copy data to VR_Extradata
				VR_Extradata* vrextra = reinterpret_cast<VR_Extradata*>(pbFormat + VR_EXRADATA_POS);
				vrextra->QueryWidth  = Size.cx;
				vrextra->QueryHeight = Size.cy;
				vrextra->FrameWidth  = pBIH->biWidth;
				vrextra->FrameHeight = pBIH->biHeight;
				vrextra->Compression = pBIH->biCompression;
			}

			// new media type must have non-empty rcSource
			RECT& rcSource = ((VIDEOINFOHEADER*)mt.pbFormat)->rcSource;
			if (IsRectEmpty(&rcSource)) {
				rcSource = { 0, 0, pBIH->biWidth, abs(pBIH->biHeight) };
			}
			RECT& rcTarget = ((VIDEOINFOHEADER*)mt.pbFormat)->rcTarget;
			if (IsRectEmpty(&rcTarget)) {
				// CoreAVC Video Decoder does not work correctly with empty rcTarget
				rcTarget = rcSource;
			}

			DLog(L"CMpcVideoRenderer::CalcImageSize() buffer size changed from {}x{} to {}x{}", pBIH->biWidth, pBIH->biHeight, Size.cx, Size.cy);
			// overwrite buffer size
			pBIH->biWidth  = Size.cx;
			pBIH->biHeight = Size.cy;
			pBIH->biSizeImage = DIBSIZE(*pBIH);
		}
	}

	return pBIH->biSizeImage ? pBIH->biSizeImage : DIBSIZE(*pBIH);
}

// CBaseRenderer

HRESULT CMpcVideoRenderer::CheckMediaType(const CMediaType* pmt)
{
	CheckPointer(pmt, E_POINTER);
	CheckPointer(pmt->pbFormat, E_POINTER);

	if (pmt->majortype == MEDIATYPE_Video && (pmt->formattype == FORMAT_VideoInfo2 || pmt->formattype == FORMAT_VideoInfo)) {
		for (const auto& sudPinType : sudPinTypesIn) {
			if (pmt->subtype == *sudPinType.clsMinorType) {
				CAutoLock cRendererLock(&m_RendererLock);

				if (!m_VideoProcessor->VerifyMediaType(pmt)) {
					return VFW_E_UNSUPPORTED_VIDEO;
				}

				return S_OK;
			}
		}
	}

	return E_FAIL;
}

HRESULT CMpcVideoRenderer::SetMediaType(const CMediaType *pmt)
{
	DLog(L"CMpcVideoRenderer::SetMediaType()\n{}", MediaType2Str(pmt));

	CheckPointer(pmt, E_POINTER);
	CheckPointer(pmt->pbFormat, E_POINTER);

	CAutoLock cVideoLock(&m_InterfaceLock);
	CAutoLock cRendererLock(&m_RendererLock);

	CSize framesize, aspect;
	m_VideoProcessor->GetVideoSize(framesize.cx, framesize.cy);
	m_VideoProcessor->GetAspectRatio(aspect.cx, aspect.cy);

	CMediaType mt(*pmt);

	m_bSetNewMediaTypeToInputPin = false;

	auto inputPin = static_cast<CVideoRendererInputPin*>(m_pInputPin);
	inputPin->ClearNewMediaType();
	if (!inputPin->FrameInVideoMem()) {
		CMediaType mtNew(*pmt);
		long ret = CalcImageSize(mtNew, true);

		if (mtNew != mt) {
			if (S_OK == m_pInputPin->GetConnected()->QueryAccept(&mtNew)) {
				DLog(L"CMpcVideoRenderer::SetMediaType() : upstream filter accepted new media type. QueryAccept return S_OK");
				inputPin->SetNewMediaType(mtNew);
				m_bSetNewMediaTypeToInputPin = true;
			}
		}
	}

	if (mt != m_inputMT && !m_VideoProcessor->InitMediaType(&mt)) {
		return VFW_E_UNSUPPORTED_VIDEO;
	}

	if (!m_videoRect.IsRectNull()) {
		m_VideoProcessor->SetVideoRect(m_videoRect);
	}

	CSize framesizeNew, aspectNew;
	m_VideoProcessor->GetVideoSize(framesizeNew.cx, framesizeNew.cy);
	m_VideoProcessor->GetAspectRatio(aspectNew.cx, aspectNew.cy);

	if (framesize.cx && aspect.cx && m_pSink) {
		if (aspectNew != aspect || framesizeNew != framesize
				|| aspectNew != m_videoAspectRatio || framesizeNew != m_videoSize) {
			m_pSink->Notify(EC_VIDEO_SIZE_CHANGED, MAKELPARAM(framesizeNew.cx, framesizeNew.cy), 0);
		}
	}

	m_videoSize = framesizeNew;
	m_videoAspectRatio = aspectNew;

	return S_OK;
}

HRESULT CMpcVideoRenderer::DoRenderSample(IMediaSample* pSample)
{
	CheckPointer(pSample, E_POINTER);

	if (m_bSetNewMediaTypeToInputPin) {
		auto inputPin = static_cast<CVideoRendererInputPin*>(m_pInputPin);
		inputPin->ClearNewMediaType();
		m_bSetNewMediaTypeToInputPin = false;
	}

	HRESULT hr = m_VideoProcessor->ProcessSample(pSample);
	m_trRenderHeld = (int)std::min<REFERENCE_TIME>(m_VideoProcessor->TakeHeldTime(), INT_MAX);

	if (SUCCEEDED(hr)) {
		m_bValidBuffer = true;
	}

	if (m_Stepping && !(--m_Stepping)) {
		this->NotifyEvent(EC_STEP_COMPLETE, 0, 0);
	}

	return hr;
}

// A note on the thread itself, for whoever looks at a frozen player from outside.
//
// Windows keeps a description per thread and hands it out across processes, so a tool
// that walks the stacks of a hung player can read it with no cooperation and no file
// to leave behind. It is the only way to tell two waits apart that look identical in
// a stack. Resolved at the first call because Windows 7 does not have it.
static void SayWhatThisThreadWaitsFor(const std::wstring& what)
{
	using PFN_SetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static PFN_SetThreadDescription pfn = []() -> PFN_SetThreadDescription {
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		return kernel ? (PFN_SetThreadDescription)GetProcAddress(kernel, "SetThreadDescription") : nullptr;
	}();
	if (pfn) {
		pfn(GetCurrentThread(), what.c_str());
	}
}

// Who gives the graph its clock. The renderer schedules every picture against it, so
// when it stops the first thing worth knowing is whose it is -- and a reference clock
// that comes from the sound card stops when the sound does.
void CMpcVideoRenderer::NoteWhoKeepsTime()
{
	wcscpy_s(m_szClockName, L"none");
	if (!m_pClock) {
		return;
	}
	wcscpy_s(m_szClockName, L"the system");
	if (CComQIPtr<IBaseFilter> pClockFilter = m_pClock) {
		FILTER_INFO info = {};
		if (SUCCEEDED(pClockFilter->QueryFilterInfo(&info))) {
			if (info.achName[0]) {
				wcsncpy_s(m_szClockName, info.achName, _TRUNCATE);
			}
			if (info.pGraph) {
				info.pGraph->Release();
			}
		}
	}
}

// Waiting for this picture's turn, with a way out.
//
// A picture is given its turn in PrepareReceive: the clock is asked to signal an
// event when its time comes, and the streaming thread then waits for that event
// here. Pause cancels that advise and resets the event -- and it does so whether or
// not the event has already been signalled, so a turn that came at the wrong moment
// is simply lost. Play is supposed to give the picture a new turn, and usually does.
// When it does not, the wait upstream waits ten seconds, finds nothing, and waits ten
// seconds again, for ever: the graph goes on running, the sound with it, and the
// picture never moves again until the player is paused and played once more. That is
// the freeze reported on a display at 24 or 25 Hz, where the window between a
// picture being scheduled and this wait being entered is a whole refresh wide.
//
// So the wait is bounded, and what it finds at the bound decides. Two things can go
// wrong and they are counted apart, because which one fires says what happened.
//
//   no advise left   the turn was taken away after it was given, and nothing will
//                    ever wake this thread. Take the picture now rather than never.
//   advise still set but seconds have passed. A renderer is handed one picture at a
//                    time and waits at most a frame or two for it, so a turn that is
//                    still seconds away means the time it was scheduled against is
//                    not the time the clock is keeping. Take it too.
//
// A filter that really is paused is left alone: waiting there is right, and Play will
// hand out the turn.
//
// While a wait goes long the thread says what it is waiting for in its own
// description, which a dump of the frozen player reads straight out: the two cases
// are indistinguishable from the outside otherwise, and both look like a thread
// sitting in WaitForRenderTime.
HRESULT CMpcVideoRenderer::WaitForRenderTime()
{
	HANDLE objects[2] = { m_ThreadSignal, m_RenderEvent };
	const ULONGLONG waitStart = GetTickCount64();
	bool bSaidSo = false;
	// Whatever way this is left, the note goes with it: a dump that reads a note
	// from a wait that has already ended points at the wrong thing entirely.
	const auto Leave = [&bSaidSo](HRESULT hr) {
		if (bSaidSo) {
			SayWhatThisThreadWaitsFor(L"MPCVR streaming");
		}
		return hr;
	};

	// A stopped clock cannot be waited for, so while it is stopped this is where the
	// time comes from: each picture is due one frame duration after the one before
	// it, counted from when that one was taken and not from now, so that the time
	// spent drawing and presenting is part of the frame rather than added to it.
	// That is the difference between the film's own rate and a third of it.
	const ULONGLONG frameMs = std::max<ULONGLONG>(10, m_FrameStats.GetAverageFrameDuration() / 10000);
	if (m_bClockStalled && !m_tickPictureDue) {
		m_tickPictureDue = GetTickCount64() + frameMs;
	}

	for (;;) {
		DWORD quantum = 500;
		if (m_bClockStalled) {
			const ULONGLONG now = GetTickCount64();
			quantum = (DWORD)std::clamp<ULONGLONG>(m_tickPictureDue > now ? m_tickPictureDue - now : 0, 1, 500);
		}
		const DWORD result = WaitForMultipleObjects(2, objects, FALSE, quantum);
		if (result == WAIT_OBJECT_0) {
			return Leave(VFW_E_STATE_CHANGED);
		}
		if (result == WAIT_OBJECT_0 + 1) {
			SignalTimerFired();
			// A turn that arrives is a clock that runs.
			m_bClockStalled = false;
			m_tickPictureDue = 0;
			return Leave(NOERROR);
		}
		if (result != WAIT_TIMEOUT) {
			return Leave(VFW_E_STATE_CHANGED);
		}
		if (!m_bStreaming && m_State != State_Running) {
			continue;   // paused for real
		}

		if (m_bClockStalled) {
			const ULONGLONG now = GetTickCount64();
			if (now < m_tickPictureDue) {
				continue;
			}
			// The next one is due a frame later. If this one has fallen a long way
			// behind -- the machine was busy, or the stall is only starting -- the
			// count starts again from here rather than chasing a debt it cannot pay.
			m_tickPictureDue = (now - m_tickPictureDue > 4 * frameMs)
				? now + frameMs : m_tickPictureDue + frameMs;
			m_nRenderWaitsLate++;
			return Leave(NOERROR);
		}

		const ULONGLONG bound = 1000;
		const ULONGLONG waited = GetTickCount64() - waitStart;
		if (!bSaidSo) {
			// Half a second of waiting never happens while a film plays, so this
			// costs nothing in the ordinary case.
			bSaidSo = true;
			SayWhatThisThreadWaitsFor(std::format(
				L"MPCVR waiting for a picture's turn: advise {}, streaming {}, state {}",
				m_dwAdvise ? L"live" : L"gone", m_bStreaming ? 1 : 0, (int)m_State));
		}

		if (!m_dwAdvise) {
			m_nRenderWaitsBroken++;
			DLog(L"CMpcVideoRenderer::WaitForRenderTime() : the turn was taken away, taking the picture now ({} so far)", m_nRenderWaitsBroken);
			return Leave(NOERROR);
		}
		if (waited >= bound) {
			m_nRenderWaitsLate++;
			// How far away the turn was, and how far the clock moved since the last
			// time one was this far away. A renderer and the pictures it is fed keep
			// the same time or they do not: if the distance stays the same picture
			// after picture while the clock runs normally, the two are simply not
			// measuring from the same place, and no amount of waiting will bring them
			// together. These two numbers are the difference between knowing that and
			// guessing it, and they cost nothing until something has already gone
			// wrong.
			// Read out of values, never out of the pending sample: this thread does
			// not hold the renderer lock while it waits, and the sample can be let go
			// from under it by a flush. Reading it here cost an access violation in
			// the bench before this was written down.
			CRefTime rtStream;
			if (SUCCEEDED(StreamTime(rtStream))) {
				const REFERENCE_TIME rtBase = (REFERENCE_TIME)m_tStart;
				const REFERENCE_TIME rtClockNow = (REFERENCE_TIME)rtStream + rtBase;
				const REFERENCE_TIME rtSample = m_FrameStats.GeTimestamp();
				// The three are kept apart on purpose. The distance to the turn says
				// how far out of step the two are; the clock says whether time is
				// passing at all; and the base is the only thing that moves between
				// them, so a distance that holds while the clock runs is the base.
				m_msLastTurnAhead = (int)((rtSample - (REFERENCE_TIME)rtStream) / 10000);
				m_msClockSinceLastTurn = m_rtLastTurnClock ? (int)((rtClockNow - m_rtLastTurnClock) / 10000) : 0;
				m_msBaseSinceLastTurn = m_rtLastTurnBase ? (int)((rtBase - m_rtLastTurnBase) / 10000) : 0;
				// Less than a millisecond of clock between two turns that were a
				// bound apart is a clock that has stopped, whatever it says.
				if (m_rtLastTurnClock && std::abs(m_msClockSinceLastTurn) < 1) {
					m_bClockStalled = true;
				}
				m_rtLastTurnClock = rtClockNow;
				m_rtLastTurnBase = rtBase;
			}
			DLog(L"CMpcVideoRenderer::WaitForRenderTime() : {} ms waiting on a turn still {} ms away, taking the picture now ({} so far)",
				waited, m_msLastTurnAhead, m_nRenderWaitsLate);
			return Leave(NOERROR);
		}
	}
}

HRESULT CMpcVideoRenderer::Receive(IMediaSample* pSample)
{
	// override CBaseRenderer::Receive() for the implementation of the search during the pause

	if (m_bFlushing) {
		DLog(L"CMpcVideoRenderer::Receive() - flushing, skip sample");
		return S_OK;
	}

	ASSERT(pSample);

	// It may return VFW_E_SAMPLE_REJECTED code to say don't bother

	HRESULT hr = PrepareReceive(pSample);
	ASSERT(m_bInReceive == SUCCEEDED(hr));
	if (FAILED(hr)) {
		if (hr == VFW_E_SAMPLE_REJECTED) {
			return NOERROR;
		}
		return hr;
	}

	// We realize the palette in "PrepareRender()" so we have to give away the
	// filter lock here.
	if (m_State == State_Paused) {
		// no need to use InterlockedExchange
		m_bInReceive = FALSE;
		{
			// We must hold both these locks
			CAutoLock cVideoLock(&m_InterfaceLock);
			if (m_State == State_Stopped)
				return NOERROR;

			m_bInReceive = TRUE;
		}
		Ready();
	}

	if (m_State == State_Paused) {
		m_bInReceive = FALSE;

		CAutoLock cRendererLock(&m_RendererLock);
		DoRenderSample(m_pMediaSample);
	}

	// Having set an advise link with the clock we sit and wait. We may be
	// awoken by the clock firing or by a state change. The rendering call
	// will lock the critical section and check we can still render the data

	hr = WaitForRenderTime();
	if (FAILED(hr)) {
		m_bInReceive = FALSE;
		return NOERROR;
	}

	//  Set this here and poll it until we work out the locking correctly
	//  It can't be right that the streaming stuff grabs the interface
	//  lock - after all we want to be able to wait for this stuff
	//  to complete
	m_bInReceive = FALSE;

	// We must hold both these locks
	CAutoLock cVideoLock(&m_InterfaceLock);

	// since we gave away the filter wide lock, the sate of the filter could
	// have chnaged to Stopped
	if (m_State == State_Stopped)
		return NOERROR;

	CAutoLock cRendererLock(&m_RendererLock);

	// Deal with this sample

	if (m_State == State_Running) {
		Render(m_pMediaSample);
	}

	ClearPendingSample();
	SendEndOfStream();
	CancelNotification();
	return NOERROR;
}

void CMpcVideoRenderer::UpdateDisplayInfo()
{
	const HMONITOR hMonPrimary = MonitorFromPoint(CPoint(0, 0), MONITOR_DEFAULTTOPRIMARY);

	MONITORINFOEXW mi = { sizeof(mi) };
	GetMonitorInfoW(m_hMon, (MONITORINFO*)&mi);

	bool ret = GetDisplayConfig(mi.szDevice, m_DisplayConfig);
	if (m_hMon == hMonPrimary) {
		m_bPrimaryDisplay = true;
	} else {
		m_bPrimaryDisplay = false;
	}

	m_VideoProcessor->SetDisplayInfo(m_DisplayConfig, m_bPrimaryDisplay, m_bExclusiveScreen);
}

void CMpcVideoRenderer::OnDisplayModeChange(const bool bReset/* = false*/)
{
	if (m_bDisplayModeChanging) {
		return;
	}

	m_bDisplayModeChanging = true;

	if (bReset && !m_VideoProcessor->IsInit()) {
		m_VideoProcessor->Reset(true);
	}

	m_hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
	UpdateDisplayInfo();

	m_bDisplayModeChanging = false;
}

void CMpcVideoRenderer::OnWindowMove()
{
	if (GetActive()) {
		const HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
		if (hMon != m_hMon) {
			if (m_Sets.bReinitByDisplay) {
				CAutoLock cRendererLock(&m_RendererLock);

				Init(true);
			}
			else if (m_VideoProcessor->Type() == VP_DX11) {
				CAutoLock cRendererLock(&m_RendererLock);

				m_VideoProcessor->Reset(false);
			}

			m_hMon = hMon;
			UpdateDisplayInfo();
			m_VideoProcessor->UpdateStatsByDisplay();
		}
	}
}

STDMETHODIMP CMpcVideoRenderer::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
	CheckPointer(ppv, E_POINTER);

	IFQIRETURN(IKsPropertySet)
	IFQIRETURN(IMFGetService)
	IFQIRETURN(IBasicVideo)
	IFQIRETURN(IBasicVideo2)
	IFQIRETURN(IVideoWindow)
	IFQIRETURN(ISpecifyPropertyPages)
	IFQIRETURN(IVideoRenderer)
	IFQIRETURN(IExFilterConfig)
	if (riid == __uuidof(ISubRender) && m_VideoProcessor && m_VideoProcessor->Type() == 9) {
		return GetInterface((ISubRender*)this, ppv);
	}
	if (riid == __uuidof(ISubRender11) && m_VideoProcessor && m_VideoProcessor->Type() == 11) {
		return GetInterface((ISubRender11*)this, ppv);
	}
	if (riid == __uuidof(ID3DFullscreenControl) && m_bEnableFullscreenControl) {
		return GetInterface((ID3DFullscreenControl*)this, ppv);
	}
	if (riid == __uuidof(ISubRenderConsumer2)) {
		return GetInterface((ISubRenderConsumer2*)this, ppv);
	}
	if (riid == __uuidof(ISubRenderConsumer)) {
		return GetInterface((ISubRenderConsumer*)this, ppv);
	}
	if (riid == __uuidof(ISubRenderOptions)) {
		return GetInterface((ISubRenderOptions*)this, ppv);
	}
	return __super::NonDelegatingQueryInterface(riid, ppv);
}

// IMediaFilter
STDMETHODIMP CMpcVideoRenderer::Run(REFERENCE_TIME rtStart)
{
	DLog(L"CMpcVideoRenderer::Run()");

	if (m_State == State_Running) {
		return NOERROR;
	}

	CAutoLock cVideoLock(&m_InterfaceLock);
	m_filterState = State_Running;
	m_bClockStalled = false;
	NoteWhoKeepsTime();

	return CBaseVideoRenderer2::Run(rtStart);
}

STDMETHODIMP CMpcVideoRenderer::Pause()
{
	DLog(L"CMpcVideoRenderer::Pause()");

	m_filterState = State_Paused;

	return CBaseVideoRenderer2::Pause();
}

STDMETHODIMP CMpcVideoRenderer::Stop()
{
	DLog(L"CMpcVideoRenderer::Stop()");

	m_filterState = State_Stopped;
	m_bValidBuffer = false;

	{
		CAutoLock cRendererLock(&m_RendererLock);
		m_VideoProcessor->Flush();
	}

	return CBaseVideoRenderer2::Stop();
}

#if _DEBUG
std::wstring PropSetAndIdToString(REFGUID PropSet, ULONG Id)
{
#define UNPACK_VALUE(VALUE) case VALUE: str += L#VALUE; break;
	std::wstring str;
	if (PropSet == AM_KSPROPSETID_CopyProt) {
		str.assign(L"AM_KSPROPSETID_CopyProt, ");
		switch (Id) {
			UNPACK_VALUE(AM_PROPERTY_COPY_MACROVISION);
			UNPACK_VALUE(AM_PROPERTY_COPY_ANALOG_COMPONENT);
			UNPACK_VALUE(AM_PROPERTY_COPY_DIGITAL_CP);
		default:
			str += std::to_wstring(Id);
		};
	}
	else if (PropSet == AM_KSPROPSETID_FrameStep) {
		str.assign(L"AM_KSPROPSETID_FrameStep, ");
		switch (Id) {
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_STEP);
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_CANCEL);
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_CANSTEP);
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_CANSTEPMULTIPLE);
		default:
			str += std::to_wstring(Id);
		};
	}
	else {
		str.assign(GUIDtoWString(PropSet) + L", " + std::to_wstring(Id));
	}
	return str;
#undef UNPACK_VALUE
}

#endif

// IKsPropertySet
STDMETHODIMP CMpcVideoRenderer::Set(REFGUID PropSet, ULONG Id, LPVOID pInstanceData, ULONG InstanceLength, LPVOID pPropertyData, ULONG DataLength)
{
	DLog(L"IKsPropertySet::Set({}, {}, {}, {}, {})", PropSetAndIdToString(PropSet, Id), pInstanceData, InstanceLength, pPropertyData, DataLength);

	if (PropSet == AM_KSPROPSETID_CopyProt) {
		if (Id == AM_PROPERTY_COPY_MACROVISION || Id == AM_PROPERTY_COPY_DIGITAL_CP) {
			DLogIf(Id == AM_PROPERTY_COPY_MACROVISION, L"No Macrovision please");
			DLogIf(Id == AM_PROPERTY_COPY_DIGITAL_CP, L"No Digital CP please");
			return S_OK;
		}
	}
	else if (PropSet == AM_KSPROPSETID_FrameStep) {
		if (Id == AM_PROPERTY_FRAMESTEP_STEP) {
			m_Stepping = 1;
			return S_OK;
		}
		if (Id == AM_PROPERTY_FRAMESTEP_CANSTEP || Id == AM_PROPERTY_FRAMESTEP_CANSTEPMULTIPLE) {
			return S_OK;
		}
	}
	else {
		return E_PROP_SET_UNSUPPORTED;
	}

	return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP CMpcVideoRenderer::Get(REFGUID PropSet, ULONG Id, LPVOID pInstanceData, ULONG InstanceLength, LPVOID pPropertyData, ULONG DataLength, ULONG* pBytesReturned)
{
	DLog(L"IKsPropertySet::Get({}, {}, {}, {}, {}, ...)", PropSetAndIdToString(PropSet, Id), pInstanceData, InstanceLength, pPropertyData, DataLength);

	if (PropSet == AM_KSPROPSETID_CopyProt) {
		if (Id == AM_PROPERTY_COPY_ANALOG_COMPONENT) {
			if (pPropertyData && DataLength >= sizeof(ULONG) && pBytesReturned) {
				*(ULONG*)pPropertyData = FALSE;
				*pBytesReturned = sizeof(ULONG);
				return S_OK;
			}
			return E_INVALIDARG;
		}
	}
	else {
		return E_PROP_SET_UNSUPPORTED;
	}

	return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP CMpcVideoRenderer::QuerySupported(REFGUID PropSet, ULONG Id, ULONG* pTypeSupport)
{
	DLog(L"IKsPropertySet::QuerySupported({}, ...)", PropSetAndIdToString(PropSet, Id));

	if (PropSet == AM_KSPROPSETID_CopyProt) {
		if (Id == AM_PROPERTY_COPY_MACROVISION || Id == AM_PROPERTY_COPY_DIGITAL_CP) {
			*pTypeSupport = KSPROPERTY_SUPPORT_SET;
			return S_OK;
		}
		if (Id == AM_PROPERTY_COPY_ANALOG_COMPONENT) {
			*pTypeSupport = KSPROPERTY_SUPPORT_GET;
			return S_OK;
		}
	}
	else {
		return E_PROP_SET_UNSUPPORTED;
	}

	return E_PROP_ID_UNSUPPORTED;
}

// IMFGetService
STDMETHODIMP CMpcVideoRenderer::GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject)
{
	if (guidService == MR_VIDEO_ACCELERATION_SERVICE) {
		if (riid == IID_IDirect3DDeviceManager9 && m_VideoProcessor->GetDeviceManager9()) {
			return m_VideoProcessor->GetDeviceManager9()->QueryInterface(riid, ppvObject);
		}
	}
	else if (guidService == MR_VIDEO_MIXER_SERVICE) {
		if (riid == IID_IMFVideoProcessor || riid == IID_IMFVideoMixerBitmap) {
			return m_VideoProcessor->QueryInterface(riid, ppvObject);
		}
	}

	return E_NOINTERFACE;
}

// IBasicVideo
STDMETHODIMP CMpcVideoRenderer::GetSourcePosition(long *pLeft, long *pTop, long *pWidth, long *pHeight)
{
	CheckPointer(pLeft,E_POINTER);
	CheckPointer(pTop,E_POINTER);
	CheckPointer(pWidth,E_POINTER);
	CheckPointer(pHeight,E_POINTER);

	CRect rect;
	long lAspectX;
	long lAspectY;
	{
		CAutoLock cVideoLock(&m_InterfaceLock);

		m_VideoProcessor->GetSourceRect(rect);
		m_VideoProcessor->GetAspectRatio(lAspectX, lAspectY);
	}

	*pLeft = rect.left;
	*pTop = rect.top;
	*pWidth = rect.Width();
	*pHeight = rect.Height();

	if (lAspectX && lAspectY) {
		// Apply aspect ratio (as in VMR9 and madVR) so that DVD-Video menu buttons are activated correctly with the mouse.
		*pWidth = *pHeight * lAspectX / lAspectY;
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::SetDestinationPosition(long Left, long Top, long Width, long Height)
{
	const CRect videoRect(Left, Top, Left + Width, Top + Height);
	if (videoRect.IsRectNull()) {
		return S_OK;
	}

	if (videoRect != m_videoRect) {
		m_videoRect = videoRect;

		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->SetVideoRect(videoRect);
	}

	if (m_bForceRedrawing) {
		Redraw();
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetDestinationPosition(long *pLeft, long *pTop, long *pWidth, long *pHeight)
{
	CheckPointer(pLeft,E_POINTER);
	CheckPointer(pTop,E_POINTER);
	CheckPointer(pWidth,E_POINTER);
	CheckPointer(pHeight,E_POINTER);

	CRect rect;
	{
		CAutoLock cVideoLock(&m_InterfaceLock);

		m_VideoProcessor->GetVideoRect(rect);
	}

	*pLeft = rect.left;
	*pTop = rect.top;
	*pWidth = rect.Width();
	*pHeight = rect.Height();

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetVideoSize(long *pWidth, long *pHeight)
{
	CheckPointer(pWidth, E_POINTER);
	CheckPointer(pHeight, E_POINTER);

	// retrieves the native video's width and height.
	m_VideoProcessor->GetVideoSize(*pWidth, *pHeight);

	long lAspectX;
	long lAspectY;

	m_VideoProcessor->GetAspectRatio(lAspectX, lAspectY);
	if (lAspectX && lAspectY) {
		// Apply aspect ratio (as in VMR9 and madVR) so that DVD-Video menu buttons are activated correctly with the mouse.
		*pWidth = static_cast<long>(llMulDiv(*pHeight, lAspectX, lAspectY, 0));
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetCurrentImage(long *pBufferSize, long *pDIBImage)
{
	CheckPointer(pBufferSize, E_POINTER);

	CAutoLock cVideoLock(&m_InterfaceLock);
	CAutoLock cRendererLock(&m_RendererLock);
	HRESULT hr;

	CSize framesize;
	long aspectX, aspectY;
	int iRotation;

	m_VideoProcessor->GetVideoSize(framesize.cx, framesize.cy);
	m_VideoProcessor->GetAspectRatio(aspectX, aspectY);
	iRotation = m_VideoProcessor->GetRotation();

	if (aspectX > 0 && aspectY > 0) {
		if (iRotation == 90 || iRotation == 270) {
			framesize.cy = MulDiv(framesize.cx, aspectY, aspectX);
		} else {
			framesize.cx = MulDiv(framesize.cy, aspectX, aspectY);
		}
	}

	const auto w = framesize.cx;
	const auto h = framesize.cy;

	// VFW_E_NOT_PAUSED ?

	if (w <= 0 || h <= 0) {
		return E_FAIL;
	}
	long size = sizeof(BITMAPINFOHEADER) + CalcDibRowPitch(w, 32) * h;

	if (pDIBImage == nullptr) {
		*pBufferSize = size;
		return S_OK;
	}

	if (size > *pBufferSize) {
		return E_OUTOFMEMORY;
	}

	hr = m_VideoProcessor->GetCurentImage(pDIBImage);

	return hr;
}

// IBasicVideo2
STDMETHODIMP CMpcVideoRenderer::GetPreferredAspectRatio(long *plAspectX, long *plAspectY)
{
	CheckPointer(plAspectX, E_POINTER);
	CheckPointer(plAspectY, E_POINTER);

	m_VideoProcessor->GetAspectRatio(*plAspectX, *plAspectY);

	return S_OK;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	CMpcVideoRenderer* pThis = reinterpret_cast <CMpcVideoRenderer*>(GetWindowLongPtrW(hwnd, 0));
	if (!pThis) {
		if ((uMsg != WM_NCCREATE)
				|| (nullptr == (pThis = (CMpcVideoRenderer*)((LPCREATESTRUCTW)lParam)->lpCreateParams))) {
			return DefWindowProcW(hwnd, uMsg, wParam, lParam);
		}

		SetWindowLongPtrW(hwnd, 0, reinterpret_cast<LONG_PTR>(pThis));
	}

	return pThis->OnReceiveMessage(hwnd, uMsg, wParam, lParam);
}

// Sent to the video window to have it make its own replacement, on its own thread.
static const UINT WM_VR_REMAKE = WM_APP + 0x3A1;

// bDisabled: the window being replaced was disabled, and so is this one from the
// start. MPC-HC disables whatever the renderer puts in its video area once the owner
// is set, so that the mouse falls through to its own window, which is the one that
// hides the pointer after two seconds. A replacement made enabled took the mouse for
// itself, and the pointer stayed over the film until it was closed -- upstream's fault
// too, with any setting that has the window made again, HDR10/DV first among them.
HWND CMpcVideoRenderer::CreateVideoWindow(const bool bDisabled)
{
	return CreateWindowExW(
		0,
		g_szClassName,
		nullptr,
		WS_VISIBLE | WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | (bDisabled ? WS_DISABLED : 0),
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		m_hWndParent,
		nullptr,
		g_hInst,
		this
	);
}

HRESULT CMpcVideoRenderer::Init(const bool bCreateWindow/* = false*/)
{
	CAutoLock cRendererLock(&m_RendererLock);

	HRESULT hr = S_OK;

	auto hwnd = m_hWndParent;
	while ((GetParent(hwnd)) && (GetParent(hwnd) == GetAncestor(hwnd, GA_PARENT))) {
		hwnd = GetParent(hwnd);
	}

	if (hwnd != m_hWndParentMain) {
		if (m_hWndParentMain) {
			RemoveParentWndProc(m_hWndParentMain);
		}

		m_hWndParentMain = hwnd;

		auto lpPreviousProc = SetWindowLongPtrW(m_hWndParentMain, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ParentWndProc));
		if (lpPreviousProc && lpPreviousProc != reinterpret_cast<LONG_PTR>(ParentWndProc)) {
			SetPropW(m_hWndParentMain, g_pszOldParentWndProc, reinterpret_cast<HANDLE>(lpPreviousProc));
			SetPropW(m_hWndParentMain, g_pszThis, reinterpret_cast<HANDLE>(this));
		} else {
			ASSERT(false);
		}
	}

	if (bCreateWindow) {
		const HWND hWndOld = m_hWndWindow;
		m_hWndWindow = nullptr;

		if (!m_bIsD3DFullscreen) {
			WNDCLASSEXW wc = { sizeof(wc) };
			if (!GetClassInfoExW(g_hInst, g_szClassName, &wc)) {
				wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
				wc.lpfnWndProc = WndProc;
				wc.hInstance = g_hInst;
				wc.lpszClassName = g_szClassName;
				wc.cbWndExtra = sizeof(CMpcVideoRenderer*); // pointer size
				if (!RegisterClassExW(&wc)) {
					hr = HRESULT_FROM_WIN32(GetLastError());
					DLog(L"CMpcVideoRenderer::Init() : RegisterClassExW() failed with error {}", HR2Str(hr));
					return hr;
				}
			}

			// The window is made again on the thread that made it the first time. A
			// player that opens the film on a worker thread has it there, and a
			// setting applied from the options arrives on the interface thread: made
			// here, the new window would change threads, and MPC-HC, which tracks the
			// mouse leaving its video area, is then told the pointer has left while
			// it sits on the picture -- and never hides it again. If that thread does
			// not answer, the window is made here as it always was.
			const bool bDisabled = hWndOld && !IsWindowEnabled(hWndOld);
			if (hWndOld && GetWindowThreadProcessId(hWndOld, nullptr) != GetCurrentThreadId()) {
				DWORD_PTR made = 0;
				if (SendMessageTimeoutW(hWndOld, WM_VR_REMAKE, bDisabled, 0, SMTO_ABORTIFHUNG, 2000, &made)) {
					m_hWndWindow = reinterpret_cast<HWND>(made);
				}
				DLogIf(!m_hWndWindow, L"CMpcVideoRenderer::Init() : the window's thread did not make the new one");
			}
			if (hWndOld) {
				::SendMessageW(hWndOld, WM_CLOSE, 0, 0);
			}
			if (!m_hWndWindow) {
				m_hWndWindow = CreateVideoWindow(bDisabled);
			}

			if (!m_hWndWindow) {
				hr = HRESULT_FROM_WIN32(GetLastError());
				DLog(L"CMpcVideoRenderer::Init() : CreateWindowExW() failed with error {}", HR2Str(hr));
				return E_FAIL;
			}

			if (!m_windowRect.IsRectNull()) {
				SetWindowPos(m_hWndWindow, nullptr, m_windowRect.left, m_windowRect.top, m_windowRect.Width(), m_windowRect.Height(), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
			}
		} else if (hWndOld) {
			::SendMessageW(hWndOld, WM_CLOSE, 0, 0);
		}
	}

	m_hWnd = m_bExclusiveScreen && m_VideoProcessor->Type() == VP_DX9 ? m_hWndParentMain : m_hWndWindow;
	if (m_bIsD3DFullscreen) {
		m_hWnd = m_hWndParent;
	}

	InstallToggleKeyHook();

	bool bChangeDevice = false;
	hr = m_VideoProcessor->Init(m_hWnd, false, &bChangeDevice);

	if (bChangeDevice) {
		DoAfterChangingDevice();
	}

	return hr;
}

// IVideoWindow
STDMETHODIMP CMpcVideoRenderer::put_Owner(OAHWND Owner)
{
	if (Owner && m_hWndParent != (HWND)Owner) {
		m_hWndParent = (HWND)Owner;
		return Init(true);
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::get_Owner(OAHWND *Owner)
{
	CheckPointer(Owner, E_POINTER);
	*Owner = (OAHWND)m_hWndParent;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::put_MessageDrain(OAHWND Drain)
{
	if (m_pInputPin == nullptr || m_pInputPin->IsConnected() == FALSE) {
		return VFW_E_NOT_CONNECTED;
	}
	m_hWndDrain = (HWND)Drain;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::get_MessageDrain(OAHWND* Drain)
{
	CheckPointer(Drain, E_POINTER);
	if (m_pInputPin == nullptr || m_pInputPin->IsConnected() == FALSE) {
		return VFW_E_NOT_CONNECTED;
	}
	*Drain = (OAHWND)m_hWndDrain;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::SetWindowPosition(long Left, long Top, long Width, long Height)
{
	const CRect windowRect(Left, Top, Left + Width, Top + Height);
	if (windowRect == m_windowRect) {
		return S_OK;
	}

	m_windowRect = windowRect;

	CAutoLock cRendererLock(&m_RendererLock);

	std::optional<bool> bFullScreenOpt;

	if (!m_bIsD3DFullscreen) {
		auto hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { mi.cbSize = sizeof(mi) };
		::GetMonitorInfoW(hMon, &mi);
		const CRect rcMonitor(mi.rcMonitor);
		auto fullScreen = (m_windowRect.Width() == rcMonitor.Width() && m_windowRect.Height() == rcMonitor.Height());
		if (m_Sets.bExclusiveFS || m_bExclusiveScreen) {
			auto SwitchExclusiveScreen = [this] (bool set) {
				DLog(L"CMpcVideoRenderer::SetWindowPosition() : Switch {} Exclusive screen", set ? L"to" : L"from");
				m_bExclusiveScreen = set;

				if (m_hWnd) {
					Init(m_VideoProcessor->Type() == VP_DX9 ? false : true);
					Redraw();

					if (m_hWndParentMain) {
						PostMessageW(m_hWndParentMain, WM_SWITCH_FULLSCREEN, static_cast<WPARAM>(set), 0);
					}
				}
			};

			if (!m_bExclusiveScreen && fullScreen) {
				SwitchExclusiveScreen(true);
			} else if (m_bExclusiveScreen && !fullScreen) {
				SwitchExclusiveScreen(false);
			}
		} else {
			if (!m_bFullScreen && fullScreen) {
				bFullScreenOpt = true;
			} else if (m_bFullScreen && !fullScreen) {
				bFullScreenOpt = false;
			}
		}
	}

	if (m_hWndWindow && !m_bExclusiveScreen) {
		SetWindowPos(m_hWndWindow, nullptr, Left, Top, Width, Height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
		if (Left < 0) {
			m_windowRect.OffsetRect(-Left, 0);
		}
		if (Top < 0) {
			m_windowRect.OffsetRect(0, -Top);
		}
	}

	m_VideoProcessor->SetWindowRect(m_windowRect);

	m_windowRect = windowRect;

	if (bFullScreenOpt.has_value()) {
		m_bFullScreen = *bFullScreenOpt;
		DLog(L"CMpcVideoRenderer::SetWindowPosition() : Switch {} FullScreen", m_bFullScreen ? L"to" : L"from");

		m_VideoProcessor->SwitchFullScreen(m_bFullScreen);
	}

	if (m_bForceRedrawing) {
		Redraw();
	}

	return S_OK;
}

// ISpecifyPropertyPages
STDMETHODIMP CMpcVideoRenderer::GetPages(CAUUID* pPages)
{
	CheckPointer(pPages, E_POINTER);

	static const GUID guidQualityPPage = { 0x565DCEF2, 0xAFC5, 0x11D2, 0x88, 0x53, 0x00, 0x00, 0xF8, 0x08, 0x83, 0xE3 };

	// Read once: the count and the list below must agree.
	const bool bActive = GetActive();

	// One page of ours, which carries the settings and DLSS between them.
	// Upstream's own page is still built and still registered; it is simply not
	// offered, so a merge from upstream never touches anything the user sees.
	pPages->cElems = 1 + (bActive ? 2 : 0);
	pPages->pElems = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID) * pPages->cElems));
	if (pPages->pElems == nullptr) {
		return E_OUTOFMEMORY;
	}

	ULONG n = 0;
	pPages->pElems[n++] = __uuidof(CVRSettingsPPage);
	if (bActive) {
		pPages->pElems[n++] = __uuidof(CVRInfoPPage);
		pPages->pElems[n++] = guidQualityPPage;
	}

	return S_OK;
}

// IVideoRenderer

STDMETHODIMP CMpcVideoRenderer::GetVideoProcessorInfo(std::wstring& str)
{
	return m_VideoProcessor->GetVPInfo(str);
}

STDMETHODIMP_(bool) CMpcVideoRenderer::GetActive()
{
	return m_pInputPin && m_pInputPin->GetConnected();
}

STDMETHODIMP_(unsigned) CMpcVideoRenderer::GetVideoProcessorUse()
{
	return m_VideoProcessor ? m_VideoProcessor->GetVideoProcessorUse() : 0;
}

STDMETHODIMP_(void) CMpcVideoRenderer::GetSettings(Settings_t& setings)
{
	setings = m_Sets;
}

STDMETHODIMP_(void) CMpcVideoRenderer::SetSettings(const Settings_t& setings)
{
	CAutoLock cRendererLock(&m_RendererLock);

	m_Sets = setings;
	m_VideoProcessor->Configure(m_Sets);

	// Picking a different key takes effect immediately; this is a no-op when
	// the key and the thread are unchanged.
	InstallToggleKeyHook();

	if (m_State == State_Paused) {
		// Re-render a pending sample even when the buffer is valid, so that the
		// change shows on it. There often is none -- the base class releases
		// each sample once drawn -- which is why this alone could not stop the
		// green frame after a rebuild of the hardware video processor; the
		// picture is now kept across that rebuild in Configure().
		if (m_pMediaSample) {
			m_bInReceive = FALSE;

			DoRenderSample(m_pMediaSample);
		}
		Redraw();
	}
}

STDMETHODIMP CMpcVideoRenderer::SaveSettings()
{
	CRegKey key;
	if (ERROR_SUCCESS == key.Create(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER)) {
		WriteSettings(key, m_Sets);
	}

	return S_OK;
}

// ISubRender (DX9)
STDMETHODIMP CMpcVideoRenderer::SetCallback(ISubRenderCallback* cb)
{
	m_pSubCallBack = cb;

	return S_OK;
}

// ISubRender11 (DX11)
STDMETHODIMP CMpcVideoRenderer::SetCallback11(ISubRender11Callback* cb)
{
	m_pSub11CallBack = cb;

	return S_OK;
}

// IExFilterConfig

STDMETHODIMP CMpcVideoRenderer::Flt_GetBool(LPCSTR field, bool* value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "statsEnable")) {
		*value = m_Sets.bShowStats;
		return S_OK;
	}

	if (!strcmp(field, "dlssNR")) {
		*value = m_Sets.bDlssNR;
		return S_OK;
	}

	if (!strcmp(field, "flip")) {
		*value = m_VideoProcessor->GetFlip();
		return S_OK;
	}

	if (!strcmp(field, "doubleRate")) {
		CAutoLock cRendererLock(&m_RendererLock);
		*value = m_VideoProcessor->GetDoubleRate();
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetInt(LPCSTR field, int* value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "renderType")) {
		if (m_inputMT.IsValid()) {
			*value = m_VideoProcessor->Type();
		} else {
			*value = 0; // not initialized
		}
		return S_OK;
	}
	if (!strcmp(field, "playbackState")) {
		*value = m_filterState;
		return S_OK;
	}
	if (!strcmp(field, "rotation")) {
		*value = m_VideoProcessor->GetRotation();
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetInt64(LPCSTR field, __int64 *value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "version")) {
		*value  = ((uint64_t)VER_MAJOR << 48)
				| ((uint64_t)VER_MINOR << 32)
				| ((uint64_t)VER_BUILD << 16)
				| ((uint64_t)REV_NUM);
		return S_OK;
	} else if (!strcmp(field, "originalVideoSize")) {
		SIZE size;
		m_VideoProcessor->GetVideoSize(size.cx, size.cy);

		static_assert(sizeof(*value) == sizeof(size));
		*value = *(__int64*)&size;

		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetString(LPCSTR field, LPWSTR* value, unsigned* chars)
{
	CheckPointer(value, E_POINTER);

	// One line describing the DLSS 5 NR session, or the DLSS Super Resolution one,
	// for the property page. "statsText" is the statistics as drawn on the picture,
	// for tools that log them.
	const bool bNR = !strcmp(field, "dlssStatus");
	const bool bSR = !strcmp(field, "dlssSRStatus");
	const bool bStats = !strcmp(field, "statsText");
	if (bNR || bSR || bStats) {
		CAutoLock cRendererLock(&m_RendererLock);
		std::wstring str;
		if (m_VideoProcessor) {
			str = bStats ? m_VideoProcessor->GetStatsText()
				: bSR ? m_VideoProcessor->GetDlssSRStatus() : m_VideoProcessor->GetDlssStatus();
		}
		const size_t len = str.size();
		LPWSTR buf = (LPWSTR)CoTaskMemAlloc((len + 1) * sizeof(WCHAR));
		if (!buf) {
			return E_OUTOFMEMORY;
		}
		memcpy(buf, str.c_str(), len * sizeof(WCHAR));
		buf[len] = L'\0';
		*value = buf;
		if (chars) {
			*chars = (unsigned)len;
		}
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetBin(LPCSTR field, LPVOID* value, unsigned* size)
{
	if (!strcmp(field, "displayedImage")) {
		if (m_State != State_Running) {
			Redraw();
		}

		CAutoLock cRendererLock(&m_RendererLock);

		HRESULT hr = m_VideoProcessor->GetDisplayedImage((BYTE**)value, size);

		return hr;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_SetBool(LPCSTR field, bool value)
{
	if (!strcmp(field, "cmd_redraw") && value) {
		Redraw();
		return S_OK;
	}

	// Toggling DLSS without opening the property page, so it can be bound to a
	// key or driven from a script. Same path as the checkbox: Configure()
	// applies it live, no session reload.
	if (!strcmp(field, "dlssNR")) {
		if (m_Sets.bDlssNR != value) {
			CAutoLock cRendererLock(&m_RendererLock);

			m_Sets.bDlssNR = value;
			m_VideoProcessor->Configure(m_Sets);
			SaveSettings();
			if (m_filterState != State_Running) {
				Redraw();
			}
		}
		return S_OK;
	}

	if (!strcmp(field, "cmd_clearPreScaleShaders") && value) {
		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->ClearPreScaleShaders();
		return S_OK;
	}

	if (!strcmp(field, "cmd_clearPostScaleShaders") && value) {
		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->ClearPostScaleShaders();
		return S_OK;
	}

	if (!strcmp(field, "statsEnable")) {
		m_Sets.bShowStats = value;
		m_VideoProcessor->SetShowStats(m_Sets.bShowStats);

		SaveSettings();
		if (m_filterState != State_Running) {
			Redraw();
		}
		return S_OK;
	}

	if (!strcmp(field, "lessRedraws")) {
		m_bForceRedrawing = !value;
		return S_OK;
	}

	if (!strcmp(field, "d3dFullscreenControl")) {
		m_bEnableFullscreenControl = value;
		return S_OK;
	}

	if (!strcmp(field, "flip")) {
		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->SetFlip(value);
		return S_OK;
	}

	if (!strcmp(field, "allowDeepColorBitmaps")) {
		m_VideoProcessor->SetAllowDeepColorBitmaps(value);
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_SetInt(LPCSTR field, int value)
{
	if (!strcmp(field, "rotation")) {
		// Allowed angles are multiples of 90.
		if (value % 90 == 0) {
			// The allowed rotation angle is reduced to 0, 90, 180, 270.
			value %= 360;
			if (value < 0) {
				value += 360;
			}

			CAutoLock cRendererLock(&m_RendererLock);

			m_VideoProcessor->SetRotation(value);
			return S_OK;
		}
	}

	if (!strcmp(field, "stereo3dTransform")) {
		if (value == STEREO3D_AsIs || value == STEREO3D_HalfOverUnder_to_Interlace) {
			CAutoLock cRendererLock(&m_RendererLock);

			m_VideoProcessor->SetStereo3dTransform(value);
			return S_OK;
		}
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_SetBin(LPCSTR field, LPVOID value, int size)
{
	if (size > 0) {
		auto ReadShaderData = [&](std::wstring& shaderName, std::string& shaderCode) {
			BYTE* p = (BYTE*)value;
			const BYTE* end = p + size;
			uint32_t chunkcode;
			int32_t chunksize;

			while (p + 8 < end) {
				memcpy(&chunkcode, p, 4);
				p += 4;
				memcpy(&chunksize, p, 4);
				p += 4;
				if (chunksize <= 0 || p + chunksize > end) {
					break;
				}

				switch (chunkcode) {
				case FCC('NAME'):
					shaderName.assign((LPCWSTR)p, chunksize / sizeof(wchar_t));
					break;
				case FCC('CODE'):
					shaderCode.assign((LPCSTR)p, chunksize);
					break;
				}
				p += chunksize;
			}
		};

		if (!strcmp(field, "cmd_addPreScaleShader")) {
			std::wstring shaderName;
			std::string shaderCode;

			ReadShaderData(shaderName, shaderCode);

			if (shaderCode.size()) {
				CAutoLock cRendererLock(&m_RendererLock);

				return m_VideoProcessor->AddPreScaleShader(shaderName, shaderCode);
			}
		}

		if (!strcmp(field, "cmd_addPostScaleShader")) {
			std::wstring shaderName;
			std::string shaderCode;

			ReadShaderData(shaderName, shaderCode);

			if (shaderCode.size()) {
				CAutoLock cRendererLock(&m_RendererLock);

				return m_VideoProcessor->AddPostScaleShader(shaderName, shaderCode);
			}
		}
	}

	return E_INVALIDARG;
}

// ID3DFullscreenControl

STDMETHODIMP CMpcVideoRenderer::SetD3DFullscreen(bool bEnabled)
{
	m_bExclusiveScreen = m_bIsD3DFullscreen = bEnabled;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetD3DFullscreen(bool* pbEnabled)
{
	CheckPointer(pbEnabled, E_POINTER);
	*pbEnabled = m_bIsD3DFullscreen;
	return S_OK;
}

// ISubRenderConsumer2

STDMETHODIMP CMpcVideoRenderer::Clear(REFERENCE_TIME clearNewerThan /* = 0 */)
{
	DLog(L"ISubRenderConsumer2::Clear");
	return m_pSubPicQueue->Invalidate(clearNewerThan);
}

// ISubRenderConsumer

STDMETHODIMP CMpcVideoRenderer::GetMerit(ULONG* plMerit)
{
	DLog(L"ISubRenderConsumer::GetMerit");
	CheckPointer(plMerit, E_POINTER);
	*plMerit = 4 << 16;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::Connect(ISubRenderProvider* subtitleRenderer)
{
	DLog(L"ISubRenderConsumer::Connect");

	if (m_pSubPicProvider) {
		return E_ABORT;
	}

	HRESULT hr = subtitleRenderer->SetBool("combineBitmaps", true);
	if (FAILED(hr)) {
		return hr;
	}

	if (CComQIPtr<ISubRenderConsumer> pSubConsumer = m_pSubPicQueue.p) {
		hr = pSubConsumer->Connect(subtitleRenderer);
	}
	else {
		ISubPicAllocator* pSubPicAllocator = m_VideoProcessor->GetSubPicAllocator();
		if (!pSubPicAllocator) {
			return E_FAIL;
		}

		CComPtr<ISubPicProvider> pSubPicProvider = (ISubPicProvider*)new CXySubPicProvider(subtitleRenderer);
		CComPtr<ISubPicQueue> pSubPicQueue       = (ISubPicQueue*)new CXySubPicQueueNoThread(pSubPicAllocator, &hr);

		if (SUCCEEDED(hr)) {
			CAutoLock cAutoLock(&m_InterfaceLock);
			pSubPicQueue->SetSubPicProvider(pSubPicProvider);
			m_pSubPicProvider = pSubPicProvider;
			m_pSubPicQueue = pSubPicQueue;

			pSubPicAllocator->SetInverseAlpha(true);
		}
	}

	return hr;
}

STDMETHODIMP CMpcVideoRenderer::Disconnect()
{
	DLog(L"ISubRenderConsumer::Disconnect");
	m_pSubPicProvider.Release();
	return m_pSubPicQueue->SetSubPicProvider(m_pSubPicProvider);
}

STDMETHODIMP CMpcVideoRenderer::DeliverFrame(REFERENCE_TIME start, REFERENCE_TIME stop, LPVOID context, ISubRenderFrame* subtitleFrame)
{
	//DLog(L"ISubRenderConsumer::DeliverFrame");
	HRESULT hr = E_FAIL;

	if (CComQIPtr<IXyCompatProvider> pXyProvider = m_pSubPicProvider.p) {
		hr = pXyProvider->DeliverFrame(start, stop, context, subtitleFrame);
	}

	return hr;
}

// ISubRenderOptions

STDMETHODIMP CMpcVideoRenderer::GetSize(LPCSTR field, SIZE* value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "originalVideoSize")) {
		*value = m_videoSize;
		return S_OK;
	}
	else if (!strcmp(field, "arAdjustedVideoSize")) {
		*value = m_videoSize;
		if (m_videoAspectRatio.cx > 0 && m_videoAspectRatio.cy > 0) {
			value->cx = MulDiv(m_videoSize.cx, m_videoAspectRatio.cx, m_videoAspectRatio.cy);
		}
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetRect(LPCSTR field, RECT* value)
{
	CheckPointer(value, E_POINTER);
	if (!strcmp(field, "videoOutputRect") || !strcmp(field, "subtitleTargetRect")) {
		if (m_videoRect.IsRectEmpty()) {
			if (m_windowRect.IsRectEmpty()) {
				*value = { 0, 0, 1280, 720 };
			}
			else {
				*value = m_windowRect;
			}
		}
		else {
			value->left = 0;
			value->top = 0;
			value->right = m_videoRect.Width();
			value->bottom = m_videoRect.Height();
		}
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetUlonglong(LPCSTR field, ULONGLONG* value)
{
	CheckPointer(value, E_POINTER);
	if (!strcmp(field, "frameRate")) {
		// TODO: check it
		*value = (REFERENCE_TIME)(10000000.0 / m_FrameStats.GetAverageFps());
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetDouble(LPCSTR field, double* value)
{
	CheckPointer(value, E_POINTER);
	if (!strcmp(field, "refreshRate")) {
		// TODO: check it
		*value = 1000.0 / m_FrameStats.GetAverageFps();
		// hmm, calculate Refresh Time in milliseconds (not Refresh Rate)
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetString(LPCSTR field, LPWSTR* value, int* chars)
{
	CheckPointer(value, E_POINTER);
	CheckPointer(chars, E_POINTER);
	std::wstring str;

	if (!strcmp(field, "name")) {
		str = L"MPC Video Renderer";
	}
	else if (!strcmp(field, "version")) {
		str = _CRT_WIDE(VERSION_STR);
	}
	else if (!strcmp(field, "yuvMatrix")) {
		auto exFmt = m_VideoProcessor->getSrcExFormat();

		switch (exFmt.VideoTransferMatrix) {
		default:
		case DXVA2_VideoTransferMatrix_BT709: str = L"TV.709"; break;
		case VIDEOTRANSFERMATRIX_FCC:
		case DXVA2_VideoTransferMatrix_BT601: str = L"TV.601"; break;
		case MFVideoTransferMatrix_BT2020_10: str = L"TV.2020"; break;
		}
	}

	if (str.length()) {
		const int len = str.length();
		const size_t sz = (len + 1) * sizeof(WCHAR);
		LPWSTR buf = (LPWSTR)LocalAlloc(LPTR, sz);

		if (!buf) {
			return E_OUTOFMEMORY;
		}

		wcscpy_s(buf, len + 1, str.data());
		*chars = len;
		*value = buf;

		return S_OK;
	}

	return E_INVALIDARG;
}

CComPtr<ISubPic> CMpcVideoRenderer::GetSubPic(REFERENCE_TIME rtStart)
{
	CComPtr<ISubPic> pSubPic;
	if (m_pSubPicQueue) {
		const auto rtNow = m_rtStartTime + rtStart;
		bool ret = m_pSubPicQueue->LookupSubPic(rtNow, m_filterState == State_Running, pSubPic);
	}
	return pSubPic;
}

HRESULT CMpcVideoRenderer::Redraw()
{
	CAutoLock cRendererLock(&m_RendererLock);
	const auto bDrawFrame = m_bValidBuffer && m_filterState != State_Stopped;

	HRESULT hr = S_OK;
	if (bDrawFrame) {
		hr = m_VideoProcessor->Render(0, INVALID_TIME);
	} else {
		hr = m_VideoProcessor->FillBlack();
	}

	return hr;
}

void CMpcVideoRenderer::DoAfterChangingDevice()
{
	if (m_pInputPin->IsConnected() == TRUE && m_pSink) {
		DLog(L"CMpcVideoRenderer::DoAfterChangingDevice()");
		m_bValidBuffer = false;
		auto pPin = (IPin*)m_pInputPin;
		m_pInputPin->AddRef();
		EXECUTE_ASSERT(S_OK == m_pSink->Notify(EC_DISPLAY_CHANGED, (LONG_PTR)pPin, 0));
		SetAbortSignal(TRUE);
		SAFE_RELEASE(m_pMediaSample);
		m_pInputPin->Release();
	}
}

LRESULT CMpcVideoRenderer::OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_VR_REMAKE) {
		return reinterpret_cast<LRESULT>(CreateVideoWindow(wParam != 0));
	}

	if (m_hWndDrain && !InSendMessage() && !m_bExclusiveScreen) {
		switch (uMsg) {
			case WM_CHAR:
			case WM_DEADCHAR:
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_LBUTTONDBLCLK:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_MBUTTONDBLCLK:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MOUSEACTIVATE:
			case WM_MOUSEMOVE:
			case WM_NCLBUTTONDBLCLK:
			case WM_NCLBUTTONDOWN:
			case WM_NCLBUTTONUP:
			case WM_NCMBUTTONDBLCLK:
			case WM_NCMBUTTONDOWN:
			case WM_NCMBUTTONUP:
			case WM_NCMOUSEMOVE:
			case WM_NCRBUTTONDBLCLK:
			case WM_NCRBUTTONDOWN:
			case WM_NCRBUTTONUP:
			case WM_RBUTTONDBLCLK:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_XBUTTONDBLCLK:
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
			case WM_SYSCHAR:
			case WM_SYSDEADCHAR:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
				PostMessageW(m_hWndDrain, uMsg, wParam, lParam);
				return 0L;
		}
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
