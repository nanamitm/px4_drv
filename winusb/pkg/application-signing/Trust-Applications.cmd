@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Trust-Applications.ps1" -Action Trust
set "px4_app_exit=%ERRORLEVEL%"
pause
exit /b %px4_app_exit%
