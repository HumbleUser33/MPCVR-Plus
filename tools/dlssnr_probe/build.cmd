@ECHO OFF
REM Builds the DLSS 5 NR probe and the caller shim it needs.
REM The shim source is shared with the NgxShim project -- there is only one copy.

SETLOCAL
PUSHD %~dp0

FOR /F "usebackq tokens=*" %%i IN (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) DO SET VSPATH=%%i
IF "%VSPATH%"=="" (
  ECHO Visual Studio not found.
  EXIT /B 1
)
CALL "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo || EXIT /B 1

ECHO Building the caller shim (nvngx.dll)...
cl /nologo /EHsc /std:c++20 /O2 /MT /LD "..\..\NgxShim\ngxshim.cpp" /Fe:nvngx.dll ^
   /link /DEF:"..\..\NgxShim\ngxshim.def" || EXIT /B 1

ECHO Building the probe...
SET MH=..\..\external\minhook\src
cl /nologo /EHsc /std:c++20 /O2 /MT probe.cpp ^
   "%MH%\hook.c" "%MH%\buffer.c" "%MH%\trampoline.c" "%MH%\hde\hde64.c" ^
   /Fe:dlssnr_probe.exe || EXIT /B 1

ECHO Building the harness (compiles the filter's own DLSS code)...
SET SRC=..\..\Source
REM The stabilizer's shaders, compiled and embedded the way the filter does it
REM (Shaders\compile_shaders.cmd, Source\res\MpcVideoRenderer.rc2).
SET SH=..\..\Shaders\d3d11
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

REM ONNX Runtime with DirectML, when it has been fetched into external\onnxruntime.
REM Without it the suite builds and runs, minus the neural rows.
SET ORT=..\..\external\onnxruntime\onnxruntime
SET DML=..\..\external\onnxruntime\directml
SET ORTFLAGS=
SET ORTLIB=
IF EXIST "%ORT%\build\native\include\onnxruntime_cxx_api.h" (
  ECHO   with ONNX Runtime and DirectML
  SET ORTFLAGS=/DHAVE_ONNXRUNTIME /I%ORT%\build\native\include
  SET ORTLIB=%ORT%\runtimes\win-x64\native\onnxruntime.lib
  COPY /Y "%ORT%\runtimes\win-x64\native\onnxruntime.dll" . >NUL
  COPY /Y "%DML%\bin\x64-win\DirectML.dll" . >NUL
)

cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE /I"%SRC%" %ORTFLAGS% harness.cpp %ORTLIB% ^
   "%SRC%\DLSS\DlssNR.cpp" "%SRC%\DLSS\DlssMotionMask.cpp" "%SRC%\DLSS\DlssOpticalFlow.cpp" "%SRC%\DLSS\DlssStabilizer.cpp" ^
   "%SRC%\DLSS\DlssSR.cpp" "%SRC%\Upscale\MpvShader.cpp" ^
   "%SRC%\DX11Helper.cpp" "%SRC%\Utils\Util.cpp" ^
   "%MH%\hook.c" "%MH%\buffer.c" "%MH%\trampoline.c" "%MH%\hde\hde64.c" ^
   detector_shaders.res /Fe:dlssnr_harness.exe || EXIT /B 1

ECHO Building the video processor rebuild test (the paused green frame)...
cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE /I"%SRC%" vp_rebuild_test.cpp ^
   "%SRC%\D3D11VP.cpp" "%SRC%\DX11Helper.cpp" ^
   /Fe:vp_rebuild_test.exe /link strmiids.lib || EXIT /B 1

ECHO Building the video processor probe (what it does with 4:2:0 and with 4:4:4)...
cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE /I"%SRC%" vp444_probe.cpp ^
   "%SRC%\D3D11VP.cpp" "%SRC%\DX11Helper.cpp" ^
   /Fe:vp444_probe.exe /link strmiids.lib || EXIT /B 1

ECHO Building the 4:4:4 chroma pass test (the shaders the filter generates, compiled)...
cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE /I"%SRC%" shader444_test.cpp shader444_guids.cpp ^
   "%SRC%\Shaders.cpp" "%SRC%\Helper.cpp" "%SRC%\csputils.cpp" ^
   "%SRC%\Utils\Util.cpp" "%SRC%\Utils\CPUInfo.cpp" "%SRC%\DX11Helper.cpp" ^
   /Fe:shader444_test.exe /link strmiids.lib windowscodecs.lib mfuuid.lib || EXIT /B 1

ECHO Building the playback test (render ahead in a DirectShow graph, needs the x64 filter built)...
cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 /DNDEBUG ^
   /DUNICODE /D_UNICODE /I"%SRC%" /I"..\..\external\BaseClasses" playback_test.cpp ^
   /Fe:playback_test.exe /link "..\..\_bin\lib\Release_x64\BaseClasses.lib" dbghelp.lib d3d11.lib ^
   /MANIFEST:EMBED /MANIFESTINPUT:playback_test.manifest || EXIT /B 1

ECHO Building the player tap (the reported freeze, against the installed player)...
cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE player_tap.cpp ^
   /Fe:player_tap.exe /link dbghelp.lib d3d11.lib dxgi.lib || EXIT /B 1

ECHO Building the freeze dump (what a frozen player is waiting for)...
cl /nologo /EHsc /std:c++20 /O2 /MT /DNOMINMAX /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
   /DUNICODE /D_UNICODE freeze_dump.cpp ^
   /Fe:freeze_dump.exe /link dbghelp.lib || EXIT /B 1

DEL /Q *.obj *.exp *.cso *.res 2>NUL

ECHO.
ECHO Run dlssnr_harness.exe, vp_rebuild_test.exe, shader444_test.exe and playback_test.exe
ECHO before putting a build in the player.
ECHO.
ECHO Done. Usage:
ECHO   dlssnr_probe.exe                  report only, nothing altered
ECHO   dlssnr_probe.exe --spoof-auto     report the architecture the DLL asks for
ECHO   dlssnr_probe.exe --spoof 1B0      report a specific architecture id
ECHO.
ECHO   other flags: --dll ^<path^>  --w 1920  --h 1080  --no-shim
POPD
