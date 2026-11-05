@echo off
echo.
echo.
title Restore MPCVR-Plus default settings...
REM MPCVR-Plus's own key, left there empty: it copies the original renderer's settings
REM only when it has no key at all. The original's key is not touched.
reg delete "HKEY_CURRENT_USER\Software\MPC-BE Filters\MPC Video Renderer Plus" /f >NUL 2>&1
reg add "HKEY_CURRENT_USER\Software\MPC-BE Filters\MPC Video Renderer Plus" /f >NUL
echo    settings were reset to default
echo.
pause >NUL
