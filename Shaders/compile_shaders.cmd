@ECHO OFF
REM (C) 2018-2023 see Authors.txt
REM
REM This file is part of MPC-BE.
REM
REM MPC-BE is free software; you can redistribute it and/or modify
REM it under the terms of the GNU General Public License as published by
REM the Free Software Foundation; either version 3 of the License, or
REM (at your option) any later version.
REM
REM MPC-BE is distributed in the hope that it will be useful,
REM but WITHOUT ANY WARRANTY; without even the implied warranty of
REM MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
REM GNU General Public License for more details.
REM
REM You should have received a copy of the GNU General Public License
REM along with this program.  If not, see <http://www.gnu.org/licenses/>.

PUSHD %~dp0

IF /I %PROCESSOR_ARCHITECTURE%==AMD64 (
  SET fxcfolder=x64
) ELSE (
  SET fxcfolder=x86
)

SET fxcexe="%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.19041.0\%fxcfolder%\fxc.exe"
IF EXIST %fxcexe% GOTO fxc_OK
SET fxcexe="%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.18362.0\%fxcfolder%\fxc.exe"
IF EXIST %fxcexe% GOTO fxc_OK
SET fxcexe="%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.17763.0\%fxcfolder%\fxc.exe"
IF EXIST %fxcexe% GOTO fxc_OK
SET fxcexe="%ProgramFiles(x86)%\Windows Kits\10\bin\%fxcfolder%\fxc.exe"
IF EXIST %fxcexe% GOTO fxc_OK
SET fxcexe="%ProgramFiles(x86)%\Windows Kits\8.1\bin\%fxcfolder%\fxc.exe"
IF EXIST %fxcexe% GOTO fxc_OK

SET fxcexe="fxc.exe"
where /q %fxcexe%
IF %ERRORLEVEL% EQU 0 goto fxc_Ok

CALL :SubColorText "0C" "fxc.exe not found!" & ECHO.
EXIT /B

:fxc_OK

SET workdir=..\_bin\shaders
IF NOT EXIST "%workdir%\" MKDIR "%workdir%"

CALL :SubColorText "0A" "=== Compiling D3D9 shaders ===" & ECHO.

SET fxc_ps3=%fxcexe% /nologo /O2 /T ps_3_0

%fxc_ps3% /Fo "%workdir%\resizer_mitchell4_x.cso"        "d3d9\interpolation_spline4.hlsl" /DMETHOD=0 /DAXIS=0
%fxc_ps3% /Fo "%workdir%\resizer_mitchell4_y.cso"        "d3d9\interpolation_spline4.hlsl" /DMETHOD=0 /DAXIS=1
%fxc_ps3% /Fo "%workdir%\resizer_catmull4_x.cso"         "d3d9\interpolation_spline4.hlsl" /DMETHOD=1 /DAXIS=0
%fxc_ps3% /Fo "%workdir%\resizer_catmull4_y.cso"         "d3d9\interpolation_spline4.hlsl" /DMETHOD=1 /DAXIS=1
%fxc_ps3% /Fo "%workdir%\resizer_lanczos2_x.cso"         "d3d9\interpolation_lanczos2.hlsl" /DAXIS=0
%fxc_ps3% /Fo "%workdir%\resizer_lanczos2_y.cso"         "d3d9\interpolation_lanczos2.hlsl" /DAXIS=1
%fxc_ps3% /Fo "%workdir%\resizer_lanczos3_x.cso"         "d3d9\interpolation_lanczos3.hlsl" /DAXIS=0
%fxc_ps3% /Fo "%workdir%\resizer_lanczos3_y.cso"         "d3d9\interpolation_lanczos3.hlsl" /DAXIS=1

%fxc_ps3% /Fo "%workdir%\downscaler_box_x.cso"           "d3d9\convolution.hlsl" /DFILTER=0 /DAXIS=0
%fxc_ps3% /Fo "%workdir%\downscaler_box_y.cso"           "d3d9\convolution.hlsl" /DFILTER=0 /DAXIS=1
%fxc_ps3% /Fo "%workdir%\downscaler_bilinear_x.cso"      "d3d9\convolution.hlsl" /DFILTER=1 /DAXIS=0
%fxc_ps3% /Fo "%workdir%\downscaler_bilinear_y.cso"      "d3d9\convolution.hlsl" /DFILTER=1 /DAXIS=1
%fxc_ps3% /Fo "%workdir%\downscaler_hamming_x.cso"       "d3d9\convolution.hlsl" /DFILTER=2 /DAXIS=0
%fxc_ps3% /Fo "%workdir%\downscaler_hamming_y.cso"       "d3d9\convolution.hlsl" /DFILTER=2 /DAXIS=1
%fxc_ps3% /Fo "%workdir%\downscaler_bicubic05_x.cso"     "d3d9\convolution.hlsl" /DFILTER=3 /DAXIS=0 /DA=-0.5
%fxc_ps3% /Fo "%workdir%\downscaler_bicubic05_y.cso"     "d3d9\convolution.hlsl" /DFILTER=3 /DAXIS=1 /DA=-0.5
%fxc_ps3% /Fo "%workdir%\downscaler_bicubic15_x.cso"     "d3d9\convolution.hlsl" /DFILTER=3 /DAXIS=0 /DA=-1.5
%fxc_ps3% /Fo "%workdir%\downscaler_bicubic15_y.cso"     "d3d9\convolution.hlsl" /DFILTER=3 /DAXIS=1 /DA=-1.5
%fxc_ps3% /Fo "%workdir%\downscaler_lanczos_x.cso"       "d3d9\convolution.hlsl" /DFILTER=4 /DAXIS=0
%fxc_ps3% /Fo "%workdir%\downscaler_lanczos_y.cso"       "d3d9\convolution.hlsl" /DFILTER=4 /DAXIS=1

%fxc_ps3% /Fo "%workdir%\convert_yuy2.cso"               "d3d9\convert_color.hlsl" /DC_YUY2=3

CALL :SubColorText "0A" "=== Compiling D3D11 shaders ===" & ECHO.

SET fxc_ps4=%fxcexe% /nologo /O2 /T ps_4_0

%fxc_ps4% /Fo "%workdir%\ps_resizer_mitchell4_x.cso"     "d3d11\ps_interpolation_spline4.hlsl" /DMETHOD=0 /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_resizer_mitchell4_y.cso"     "d3d11\ps_interpolation_spline4.hlsl" /DMETHOD=0 /DAXIS=1
%fxc_ps4% /Fo "%workdir%\ps_resizer_catmull4_x.cso"      "d3d11\ps_interpolation_spline4.hlsl" /DMETHOD=1 /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_resizer_catmull4_y.cso"      "d3d11\ps_interpolation_spline4.hlsl" /DMETHOD=1 /DAXIS=1
%fxc_ps4% /Fo "%workdir%\ps_resizer_lanczos2_x.cso"      "d3d11\ps_interpolation_lanczos2.hlsl" /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_resizer_lanczos2_y.cso"      "d3d11\ps_interpolation_lanczos2.hlsl" /DAXIS=1
%fxc_ps4% /Fo "%workdir%\ps_resizer_lanczos3_x.cso"      "d3d11\ps_interpolation_lanczos3.hlsl" /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_resizer_lanczos3_y.cso"      "d3d11\ps_interpolation_lanczos3.hlsl" /DAXIS=1

%fxc_ps4% /Fo "%workdir%\ps_downscaler_box_x.cso"        "d3d11\ps_convolution.hlsl" /DFILTER=0 /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_downscaler_box_y.cso"        "d3d11\ps_convolution.hlsl" /DFILTER=0 /DAXIS=1
%fxc_ps4% /Fo "%workdir%\ps_downscaler_bilinear_x.cso"   "d3d11\ps_convolution.hlsl" /DFILTER=1 /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_downscaler_bilinear_y.cso"   "d3d11\ps_convolution.hlsl" /DFILTER=1 /DAXIS=1
%fxc_ps4% /Fo "%workdir%\ps_downscaler_hamming_x.cso"    "d3d11\ps_convolution.hlsl" /DFILTER=2 /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_downscaler_hamming_y.cso"    "d3d11\ps_convolution.hlsl" /DFILTER=2 /DAXIS=1
%fxc_ps4% /Fo "%workdir%\ps_downscaler_bicubic05_x.cso"  "d3d11\ps_convolution.hlsl" /DFILTER=3 /DAXIS=0 /DA=-0.5
%fxc_ps4% /Fo "%workdir%\ps_downscaler_bicubic05_y.cso"  "d3d11\ps_convolution.hlsl" /DFILTER=3 /DAXIS=1 /DA=-0.5
%fxc_ps4% /Fo "%workdir%\ps_downscaler_bicubic15_x.cso"  "d3d11\ps_convolution.hlsl" /DFILTER=3 /DAXIS=0 /DA=-1.5
%fxc_ps4% /Fo "%workdir%\ps_downscaler_bicubic15_y.cso"  "d3d11\ps_convolution.hlsl" /DFILTER=3 /DAXIS=1 /DA=-1.5
%fxc_ps4% /Fo "%workdir%\ps_downscaler_lanczos_x.cso"    "d3d11\ps_convolution.hlsl" /DFILTER=4 /DAXIS=0
%fxc_ps4% /Fo "%workdir%\ps_downscaler_lanczos_y.cso"    "d3d11\ps_convolution.hlsl" /DFILTER=4 /DAXIS=1

%fxc_ps4% /Fo "%workdir%\ps_convert_yuy2.cso"            "d3d11\ps_convert_color.hlsl" /DC_YUY2=3

%fxc_ps4% /Fo "%workdir%\ps_final_pass_10.cso"           "d3d11\ps_final_pass.hlsl" /DQUANTIZATION=1023

%fxc_ps4% /Fo "%workdir%\ps_convert_bitmap_to_pq1.cso"   "d3d11\ps_convert_bitmap_to_pq.hlsl" /DSDR_PEAK_LUM=50
%fxc_ps4% /Fo "%workdir%\ps_convert_bitmap_to_pq2.cso"   "d3d11\ps_convert_bitmap_to_pq.hlsl" /DSDR_PEAK_LUM=30

%fxc_ps4% /Fo "%workdir%\ps_dlss_motion_luma.cso"        "d3d11\ps_dlss_motion.hlsl" /DPASS=1
%fxc_ps4% /Fo "%workdir%\ps_dlss_motion_diff.cso"        "d3d11\ps_dlss_motion.hlsl" /DPASS=2 /DBASELINES=3
%fxc_ps4% /Fo "%workdir%\ps_dlss_motion_age.cso"         "d3d11\ps_dlss_motion.hlsl" /DPASS=3
%fxc_ps4% /Fo "%workdir%\ps_dlss_motion_mask.cso"        "d3d11\ps_dlss_motion.hlsl" /DPASS=4

%fxc_ps4% /Fo "%workdir%\ps_dlss_stab_flowframe.cso"     "d3d11\ps_dlss_stabilize.hlsl" /DPASS=0
%fxc_ps4% /Fo "%workdir%\ps_dlss_stab_flowmotion.cso"    "d3d11\ps_dlss_stabilize.hlsl" /DPASS=1
%fxc_ps4% /Fo "%workdir%\ps_dlss_stab_stabilize.cso"     "d3d11\ps_dlss_stabilize.hlsl" /DPASS=2
%fxc_ps4% /Fo "%workdir%\ps_dlss_stab_snapmotion.cso"    "d3d11\ps_dlss_stabilize.hlsl" /DPASS=3
%fxc_ps4% /Fo "%workdir%\ps_dlss_stab_blockmotion.cso"   "d3d11\ps_dlss_stabilize.hlsl" /DPASS=4

SET fxc_cs5=%fxcexe% /nologo /O2 /T cs_5_0
%fxc_cs5% /Fo "%workdir%\cs_dlss_global_motion.cso"      "d3d11\cs_dlss_global_motion.hlsl"

%fxc_ps4% /Fo "%workdir%\ps_mpv_luma.cso"                "d3d11\ps_mpv_prescale.hlsl" /DPASS=1
%fxc_ps4% /Fo "%workdir%\ps_mpv_combine.cso"             "d3d11\ps_mpv_prescale.hlsl" /DPASS=2
%fxc_ps4% /Fo "%workdir%\ps_mpv_chroma_plane.cso"        "d3d11\ps_mpv_prescale.hlsl" /DPASS=3
%fxc_ps4% /Fo "%workdir%\ps_mpv_combine_ar.cso"          "d3d11\ps_mpv_prescale.hlsl" /DPASS=4
%fxc_ps4% /Fo "%workdir%\ps_mpv_chroma_ar.cso"           "d3d11\ps_mpv_prescale.hlsl" /DPASS=5

SET fxc_ps5=%fxcexe% /nologo /O2 /T ps_5_0
REM Sharpening. PASS 2 is the unsharp mask held to what the neighbours cover;
REM PASS 5 is adaptive-sharpen, which needs shader model 5 for its 25 taps.
%fxc_ps4% /Fo "%workdir%\ps_sharpen_unsharp.cso"        "d3d11\ps_sharpen.hlsl" /DPASS=2
%fxc_ps5% /Fo "%workdir%\ps_sharpen_adaptive.cso"       "d3d11\ps_sharpen.hlsl" /DPASS=5

REM mpv prescalers: generated by mpv\mpv_shaders.py, do not edit this block
SET fxc_ps5=%fxcexe% /nologo /O2 /T ps_5_0
SET fxc_cs5=%fxcexe% /nologo /O2 /T cs_5_0

%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass00.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass00.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass01.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass01.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass02.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass02.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass03.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass03.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass04.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass04.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass05.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass05.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass06.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass06.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass07.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass07.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass08.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass08.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass09.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass09.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass10.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass10.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass11.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass11.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass12.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass12.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_8-0-4-1_pass13.cso" "mpv\FSRCNNX_x2_8-0-4-1\pass13.hlsl"

%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass00.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass00.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass01.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass01.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass02.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass02.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass03.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass03.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass04.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass04.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass05.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass05.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass06.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass06.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass07.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass07.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass08.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass08.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass09.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass09.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass10.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass10.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass11.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass11.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass12.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass12.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass13.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass13.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass14.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass14.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass15.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass15.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass16.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass16.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass17.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass17.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass18.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass18.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass19.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass19.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass20.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass20.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass21.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass21.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass22.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass22.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass23.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass23.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass24.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass24.hlsl"
%fxc_ps5% /Fo "%workdir%\mpv_FSRCNNX_x2_16-0-4-1_pass25.cso" "mpv\FSRCNNX_x2_16-0-4-1\pass25.hlsl"

%fxc_ps5% /Fo "%workdir%\mpv_ravu-zoom-ar-r3_pass00.cso" "mpv\ravu-zoom-ar-r3\pass00.hlsl"

%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass00.cso" "mpv\ArtCNN_C4F16_DS\pass00.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass01.cso" "mpv\ArtCNN_C4F16_DS\pass01.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass02.cso" "mpv\ArtCNN_C4F16_DS\pass02.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass03.cso" "mpv\ArtCNN_C4F16_DS\pass03.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass04.cso" "mpv\ArtCNN_C4F16_DS\pass04.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass05.cso" "mpv\ArtCNN_C4F16_DS\pass05.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass06.cso" "mpv\ArtCNN_C4F16_DS\pass06.hlsl"
%fxc_cs5% /Fo "%workdir%\mpv_ArtCNN_C4F16_DS_pass07.cso" "mpv\ArtCNN_C4F16_DS\pass07.hlsl"
REM end of the mpv prescalers

EXIT /B

:SubColorText
FOR /F "tokens=1,2 delims=#" %%A IN (
  '"PROMPT #$H#$E# & ECHO ON & FOR %%B IN (1) DO REM"') DO (
  SET "DEL=%%A")
<NUL SET /p ".=%DEL%" > "%~2"
FINDSTR /v /a:%1 /R ".18" "%~2" NUL
DEL "%~2" > NUL 2>&1
EXIT /B
