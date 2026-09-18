@echo off
setlocal

rem Package the existing ESP-IDF build. The package contains only the merged
rem core firmware image; the production Python tool writes the SN afterward.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_esp32_merged_bin.ps1" ^
  -SkipBuild %*

set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" pause
exit /b %EXIT_CODE%
