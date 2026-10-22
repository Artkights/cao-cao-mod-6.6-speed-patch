@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-dual.ps1" -Tests
exit /b %errorlevel%
