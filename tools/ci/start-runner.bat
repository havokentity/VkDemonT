@echo off
REM SPDX-License-Identifier: MIT
REM Copyright (c) 2026 Rajesh D'Monte
REM
REM Double-clickable wrapper for start-runner.ps1, so kicking off CI does not
REM require opening a shell first. Deliberately NOT a service: this machine is
REM a workstation and is not on 24/7, and a boot-start service would happily
REM fire a GPU golden run while you are mid-game.
REM
REM Prefers PowerShell 7 (pwsh) and falls back to Windows PowerShell, since the
REM script is compatible with both.
setlocal
set "SCRIPT=%~dp0start-runner.ps1"

where pwsh >nul 2>&1
if %ERRORLEVEL%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
)

REM Pause only when launched by double-click (no console attached beforehand),
REM so the window does not vanish before an error can be read. When run from an
REM existing shell this is skipped and the exit code passes straight through.
if "%~1"=="" pause
endlocal
