@echo off
rem One-click launcher for D2R on Windows ARM64. Double-click to play.
rem   --rebuild        rebuild dmhost.exe with ninja first
rem   --res 1280x720   override the render resolution (default 1600x900)
rem Game files are expected in ..\Diablo next to this checkout (ExeFS + RomFS),
rem or point DM_ROMFS at the RomFS folder.
setlocal
cd /d "%~dp0"
set "ROOT=%CD%"
set "BIN=%ROOT%\build\win\obj\dmhost.exe"
set "REBUILD="
set "RES="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--rebuild" goto arg_rebuild
if /i "%~1"=="--res" goto arg_res
echo Unknown argument: %~1
goto usage
:arg_rebuild
set "REBUILD=1"
shift
goto parse
:arg_res
set "RES=%~2"
shift
shift
goto parse

:parsed
if defined REBUILD (
    echo Building dmhost.exe ...
    set "NINJA="
    for /f "delims=" %%i in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%i"
    if not defined NINJA (
        echo ninja not found - install llvm-mingw + ninja, see docs\WINDOWS_ARM64.zh-CN.md
        goto fail
    )
    pushd "%ROOT%"
    "%NINJA%" -C "%ROOT%\build\win\obj"
    if errorlevel 1 (
        popd
        echo Build failed.
        goto fail
    )
    popd
)

if not exist "%BIN%" (
    echo %BIN% is missing - run this script with --rebuild once.
    goto fail
)

if not defined DM_ROMFS set "DM_ROMFS=%ROOT%\..\Diablo\RomFS"
if not exist "%DM_ROMFS%\..\ExeFS\main" (
    echo Game files not found under "%DM_ROMFS%".
    echo Put the Switch ExeFS main/sdk/subsdk0 + RomFS in %ROOT%\..\Diablo, or set DM_ROMFS.
    goto fail
)
if not exist "%ROOT%\build\win\dmdata\main.patched.bin" (
    echo Missing build\win\dmdata - see step 1 in docs\WINDOWS_ARM64.zh-CN.md
    goto fail
)
if not exist "%ROOT%\build\win\shaders" (
    echo Missing build\win\shaders - see step 2 in docs\WINDOWS_ARM64.zh-CN.md
    goto fail
)

if not exist "%ROOT%\build\win\save" mkdir "%ROOT%\build\win\save"
if not exist "%ROOT%\build\win\temp" mkdir "%ROOT%\build\win\temp"
set "DM_SAVE=%ROOT%\build\win\save"
set "DM_TEMP=%ROOT%\build\win\temp"
set "DM_WINDOW=1"
if not defined DM_TACT_PREFETCH set "DM_TACT_PREFETCH=0"
rem UI overlay (bottom HUD v4, panel changes, keyboard key caps). Generate it once with
rem dm\win\tools\gen_overlay_win.ps1; without it the game runs with the original UI.
if not defined DM_ROMFS_OVERLAY if exist "%ROOT%\build\win\overlay" set "DM_ROMFS_OVERLAY=%ROOT%\build\win\overlay"
rem Touch overlay: the virtual stick, the twelve round buttons, the skill-set switch, town portal and map are drawn by the host
rem (dm\win\touch_win.cpp) and appear once you are in a run; it also keeps dm_padmode from locking the handheld layout.
rem Set DM_WIN_TOUCH=0 before running this script to play without it.
if not defined DM_WIN_TOUCH set "DM_WIN_TOUCH=1"
if defined RES set "DM_GAME_RES=%RES%"

rem one instance only: the save dir has a transaction log, two processes would fight over it.
rem Close the old instance politely first (WM_CLOSE) so it asks the game to save and waits for the commit,
rem and only force-kill it if it is still there after ~8 seconds.
"%SystemRoot%\System32\taskkill.exe" /im dmhost.exe >nul 2>nul
set "OLDN=0"
:waitold
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq dmhost.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "dmhost.exe" >nul || goto oldgone
set /a OLDN+=1
if %OLDN% geq 8 goto oldkill
"%SystemRoot%\System32\ping.exe" -n 2 127.0.0.1 >nul
goto waitold
:oldkill
echo The previous instance did not save and quit in 8s - killing it.
"%SystemRoot%\System32\taskkill.exe" /im dmhost.exe /f >nul 2>nul
:oldgone

cd /d "%ROOT%\build\win"
echo Launching %BIN%
rem dmhost.exe is a GUI-subsystem program: start it detached so this console window closes at once
rem instead of sitting there behind the game. Diagnostics go to build\win\dmhost.log (DM_LOG_FILE overrides it).
start "" "%BIN%" dmdata main sdk subsdk0
echo Started - log: %CD%\dmhost.log
goto end

:usage
echo Usage: start-win-arm64.bat [--rebuild] [--res WxH]

:fail
pause

:end
endlocal
