@echo off
setlocal EnableExtensions
title RiptOPL SMB Server
cd /d "%~dp0"

REM ============================================================================
REM  RiptOPL SMBv1 server -- double-click launcher for Windows.
REM
REM  EASIEST: drag your PS2 games folder onto this .bat file.
REM  OR: set GAMES below to your games folder, then just double-click.
REM  OR: leave it -- the launcher will ask you for the folder.
REM
REM  The share is WRITABLE by default (OPL needs it for per-game settings + VMC saves).
REM  Advanced (edit the launch line near the bottom if you want these):
REM    --read-only    serve read-only (no saves / no VMC writes)
REM    --take-445     use the standard port 445 (admin; pauses Windows file sharing)
REM    add a 2nd share:  --share apps="E:\PS2Apps"
REM ============================================================================
set "GAMES=D:\PS2Games"
set "PORT=1111"
REM ============================================================================

REM A folder dragged onto this .bat overrides the GAMES setting above.
if not "%~1"=="" set "GAMES=%~1"

echo.
echo   ============================================================
echo     RiptOPL SMB Server
echo   ============================================================
echo.

REM --- locate Python 3 ---
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY ( where python >nul 2>nul && set "PY=python" )
if not defined PY (
  echo   Python 3 is required, but it was not found on this PC.
  echo.
  echo   1^) Install it from:  https://www.python.org/downloads/
  echo   2^) IMPORTANT: during setup, tick "Add python.exe to PATH".
  echo   3^) Then run this launcher again.
  echo.
  pause
  exit /b 1
)

REM --- make sure the games folder exists; ask if not ---
:askfolder
if exist "%GAMES%\" goto run
echo   Games folder not found:  "%GAMES%"
echo   ^(This is the folder that holds your CD\ DVD\ ... OPL game folders.^)
echo.
set /p "GAMES=  Type or paste the full path to your PS2 games folder: "
echo.
goto askfolder

:run
REM --- one-time: allow the port through Windows Firewall (you'll see a single admin prompt) ---
echo   Checking Windows Firewall (a one-time "Yes" admin prompt may appear)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "if (-not (Get-NetFirewallRule -DisplayName 'RiptOPL SMB Server' -EA SilentlyContinue)) { try { Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-Command','New-NetFirewallRule -DisplayName ''RiptOPL SMB Server'' -Direction Inbound -Protocol TCP -LocalPort %PORT% -Action Allow -Profile Private,Domain' -Wait } catch { Write-Host '  (skipped -- if OPL cannot connect, allow TCP %PORT% inbound manually)' } }"
echo.
echo   Games folder : %GAMES%
echo   Port         : %PORT%
echo.
echo   In OPL -^> Network settings:
echo     * PC IP Address : your PC's LAN IP ^(usually 192.168.x.x; see below -- NOT a VPN IP^)
echo     * SMB Port      : %PORT%
echo     * Share         : games
echo     * User / Pass   : leave blank ^(guest^)
echo.
echo   Leave this window OPEN while you play. Close it to stop the server.
echo   ------------------------------------------------------------
echo.

%PY% "%~dp0smbserver_opl.py" --share games="%GAMES%" --port %PORT%

echo.
echo   ------------------------------------------------------------
echo   The SMB server has stopped. You can close this window.
pause >nul
endlocal
