@echo off
rem Refresh the build timestamp in version\version.h. Version numbers stay as edited.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0update_version.ps1" -Root "%~dp0.."
exit /b %ERRORLEVEL%
