@ECHO OFF
REM Everything this fork adds, measured in one go, into one report.
REM
REM Step 8 of MERGING-UPSTREAM.md: run it after a merge, then
REM
REM   python compare_report.py baseline\run_all_report.txt run_all_report.txt
REM
REM "0 failures" is necessary and not sufficient -- a merge can pass every check and
REM still cost half a decibel somewhere, so the numbers are what you compare.
REM
REM   run_all.cmd [report.txt]
REM
REM Build the filter (build_mpcvr.cmd Build Release x64 and Win32) and the tools
REM (build.cmd) first: this only measures.

SETLOCAL ENABLEDELAYEDEXPANSION
PUSHD "%~dp0"

SET "REPORT=%~1"
IF "%REPORT%"=="" SET "REPORT=%~dp0run_all_report.txt"
SET "REF=upscale_refs\4K1.png"

IF NOT EXIST "dlssnr_harness.exe" (
  ECHO dlssnr_harness.exe is missing. Run build.cmd first.
  POPD & EXIT /B 1
)

ECHO MPCVR-DLSS5, the whole battery> "%REPORT%"
ECHO Started %DATE% %TIME%>> "%REPORT%"
FOR /F "tokens=*" %%v IN ('git -C "%~dp0..\.." log -1 --format^=%%h%%d') DO ECHO Commit %%v>> "%REPORT%"
ECHO.>> "%REPORT%"

REM One quoted command per step: nothing is shifted or re-split, so a step cannot go
REM missing over a quoting subtlety.
CALL :Run "the filter's own code, 300 frames"  "dlssnr_harness.exe --frames 300"
CALL :Run "the DLSS passes against the harness" "dlssnr_harness.exe --tport --tstabport --tsrport --nomodels"
CALL :Run "the prescalers against the harness" "dlssnr_harness.exe --tmpvport --nonr --nomodels"
CALL :Run "the pipeline"                       "dlssnr_harness.exe --tpipeline --nonr --nomodels"
CALL :Run "upscalers on film"                  "dlssnr_harness.exe --tupscale --nonr --nomodels"
CALL :Run "chroma upsamplers on film"          "dlssnr_harness.exe --tchroma --nonr"
CALL :Run "sharpeners on film"                 "dlssnr_harness.exe --tsharpen --nonr --nomodels"
CALL :Run "video processor rebuild"            "vp_rebuild_test.exe"
CALL :Run "the generated 4:4:4 chroma pass"    "shader444_test.exe"
CALL :Run "playback, 10 s"                     "playback_test.exe --seconds 10"
CALL :Run "the upscalers through the filter"   "playback_test.exe --scalers --seconds 4"
CALL :Run "chroma through the filter, 8-bit"   "playback_test.exe --chroma %REF% --seconds 3"
CALL :Run "chroma through the filter, 10-bit"  "playback_test.exe --chroma10 %REF% --seconds 3"
CALL :Run "settings changed while playing"     "playback_test.exe --toggle --seconds 3"
CALL :Run "the processor's extras, both ways"  "playback_test.exe --switch --toggle --seconds 3"
CALL :Run "pictures from a decoder's device"   "playback_test.exe --gpu --toggle --seconds 3"
CALL :Run "the Settings page and its lists"    "playback_test.exe --mainpage 2"
CALL :Run "the DLSS page"                      "playback_test.exe --dlsspage 2"

ECHO.>> "%REPORT%"
ECHO Finished %DATE% %TIME%>> "%REPORT%"
ECHO.
ECHO Report: %REPORT%
FINDSTR /C:"[STEP]" /C:"failure(s)" /C:"check(s) failed" "%REPORT%"
ECHO.
ECHO Now: python compare_report.py baseline\run_all_report.txt "%REPORT%"
POPD
ENDLOCAL
EXIT /B 0

:Run
SET "WHAT=%~1"
SET "LINE=%~2"
ECHO   %WHAT%
ECHO ========================================================================>> "%REPORT%"
ECHO [STEP] %WHAT%>> "%REPORT%"
ECHO [CMD ] %LINE%>> "%REPORT%"
ECHO ========================================================================>> "%REPORT%"
%LINE% >> "%REPORT%" 2>&1
IF ERRORLEVEL 1 ECHO [STEP] %WHAT% -- exit code !ERRORLEVEL!>> "%REPORT%"
ECHO.>> "%REPORT%"
EXIT /B 0
