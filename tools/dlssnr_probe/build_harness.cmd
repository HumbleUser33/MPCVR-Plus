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
REM The compiler build.cmd takes, for the reason it gives.
SET VCVER=
SET "V143=%VSPATH%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt"
IF EXIST "%V143%" FOR /F "usebackq tokens=*" %%v IN ("%V143%") DO (
  IF EXIST "%VSPATH%\VC\Tools\MSVC\%%v\atlmfc\include\afx.h" SET VCVER=-vcvars_ver=%%v
)
CALL "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo %VCVER% || EXIT /B 1
ECHO MSVC %VCToolsVersion%

SET SRC=..\..\Source
SET INC=..\..\Include
SET MH=..\..\external\minhook\src
SET SH=..\..\Shaders\d3d11

REM The stabilizer's and the composition's shaders and their resource, as build.cmd
REM makes them: a build here is the first thing run after a clean, so they may not
REM exist yet.
IF NOT EXIST detector_shaders.res (
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_flowframe.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=0 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_flowmotion.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=1 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_stabilize.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=2 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_snapmotion.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=3 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_stab_blockmotion.cso "%SH%\ps_dlss_stabilize.hlsl" /DPASS=4 >NUL || EXIT /B 1
  fxc /nologo /O2 /T cs_5_0 /Fo cs_dlss_global_motion.cso "%SH%\cs_dlss_global_motion.hlsl" >NUL || EXIT /B 1
  fxc /nologo /O2 /T cs_5_0 /Fo cs_dlss_global_step.cso "%SH%\cs_dlss_global_refine.hlsl" /DPASS=1 >NUL || EXIT /B 1
  fxc /nologo /O2 /T cs_5_0 /Fo cs_dlss_global_solve.cso "%SH%\cs_dlss_global_refine.hlsl" /DPASS=2 >NUL || EXIT /B 1
  fxc /nologo /O2 /T cs_5_0 /Fo cs_dlss_scene_cut.cso "%SH%\cs_dlss_scene_cut.hlsl" /DPASS=2 >NUL || EXIT /B 1
  fxc /nologo /O2 /T cs_5_0 /Fo cs_dlss_scene_sums.cso "%SH%\cs_dlss_scene_cut.hlsl" /DPASS=1 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_diff.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=0 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_joint.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=1 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_sums.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=2 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_ab.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=3 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_abx.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=4 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_aby.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=5 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_guided.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=6 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_reducex.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=7 >NUL || EXIT /B 1
  fxc /nologo /O2 /T ps_4_0 /Fo ps_dlss_compose_reducey.cso "%SH%\ps_dlss_compose.hlsl" /DPASS=8 >NUL || EXIT /B 1
  rc /nologo /fo detector_shaders.res detector_shaders.rc || EXIT /B 1
)

REM ONNX Runtime with DirectML, as build.cmd: the neural rows when it is there.
SET ORT=..\..\external\onnxruntime\onnxruntime
SET ORTFLAGS=
SET ORTLIB=
IF EXIST "%ORT%\build\native\include\onnxruntime_cxx_api.h" (
  SET ORTFLAGS=/DHAVE_ONNXRUNTIME /I%ORT%\build\native\include
  SET ORTLIB=%ORT%\runtimes\win-x64\native\onnxruntime.lib
)

cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE /I"%INC%" /I"%SRC%" %ORTFLAGS% harness.cpp %ORTLIB% ^
   "%SRC%\DLSS\DlssNR.cpp" "%SRC%\DLSS\DlssOpticalFlow.cpp" "%SRC%\DLSS\DlssStabilizer.cpp" ^
   "%SRC%\DLSS\DlssSR.cpp" "%SRC%\DLSS\DlssCompose.cpp" "%SRC%\Upscale\MpvShader.cpp" ^
   "%SRC%\DX11Helper.cpp" "%SRC%\Utils\Util.cpp" "%SRC%\Logo\LogoAnalysis.cpp" ^
   "%MH%\hook.c" "%MH%\buffer.c" "%MH%\trampoline.c" "%MH%\hde\hde64.c" ^
   detector_shaders.res /Fe:dlssnr_harness.exe || EXIT /B 1

ECHO Done.
POPD
