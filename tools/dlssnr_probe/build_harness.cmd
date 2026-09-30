@ECHO OFF
REM Just the harness, for when a suite is being written and build.cmd's ten
REM minutes are ten minutes too many. It produces the same dlssnr_harness.exe.

SETLOCAL
PUSHD %~dp0

FOR /F "usebackq tokens=*" %%i IN (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) DO SET VSPATH=%%i
IF "%VSPATH%"=="" (
  ECHO Visual Studio not found.
  EXIT /B 1
)
CALL "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo || EXIT /B 1

SET SRC=..\..\Source
SET MH=..\..\external\minhook\src
SET SH=..\..\Shaders\d3d11

REM The stabilizer's shaders and their resource, as build.cmd makes them: a
REM build here is the first thing run after a clean, so they may not exist yet.
IF NOT EXIST detector_shaders.res (
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_motion_luma.cso "%SH%\ps_dlss_motion.hlsl" /DPASS=1 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_motion_diff.cso "%SH%\ps_dlss_motion.hlsl" /DPASS=2 /DBASELINES=3 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_motion_age.cso "%SH%\ps_dlss_motion.hlsl" /DPASS=3 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_motion_mask.cso "%SH%\ps_dlss_motion.hlsl" /DPASS=4 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_flowframe.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=0 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_flowmotion.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=1 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_stabilize.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=2 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_snapmotion.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=3 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_blockmotion.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=4 >NUL || EXIT /B 1
  fxc /nologo /O2 /T cs_5_0 /Fo cs_dlss_global_motion.cso "%SH%\cs_dlss_global_motion.hlsl" >NUL || EXIT /B 1
  rc /nologo /fo detector_shaders.res detector_shaders.rc || EXIT /B 1
)

cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE /I"%SRC%" harness.cpp ^
   "%SRC%\DLSS\DlssNR.cpp" "%SRC%\DLSS\DlssMotionMask.cpp" "%SRC%\DLSS\DlssOpticalFlow.cpp" "%SRC%\DLSS\DlssStabilizer.cpp" ^
   "%SRC%\DLSS\DlssSR.cpp" "%SRC%\Upscale\MpvShader.cpp" ^
   "%SRC%\DX11Helper.cpp" "%SRC%\Utils\Util.cpp" ^
   "%MH%\hook.c" "%MH%\buffer.c" "%MH%\trampoline.c" "%MH%\hde\hde64.c" ^
   detector_shaders.res /Fe:dlssnr_harness.exe || EXIT /B 1

ECHO Done.
POPD
