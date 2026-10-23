@echo off
rem Copy the built plugin + config into the game directory (dev/test helper).
rem   deploy.bat            -> install HZDRawMouse.asi + HZDRawMouse.ini
rem   deploy.bat uninstall  -> remove them (Ultimate ASI Loader / winmm.dll is left alone)
rem Note: UAL must be named winmm.dll for this game - version.dll is pre-loaded from System32 by the
rem Windows compatibility shim (AcGenral.dll) that Microsoft applies to HorizonZeroDawn.exe.
setlocal
if not defined HZD_DIR set "HZD_DIR=D:\Program Files\Games\Steam\steamapps\common\Horizon Zero Dawn"
if not exist "%HZD_DIR%\HorizonZeroDawn.exe" (echo Game not found in "%HZD_DIR%" - set HZD_DIR & exit /b 1)
tasklist /FI "IMAGENAME eq HorizonZeroDawn.exe" | find /I "HorizonZeroDawn.exe" >nul && (echo Game is running - close it first & exit /b 1)

if /I "%~1"=="uninstall" (
    del /Q "%HZD_DIR%\HZDRawMouse.asi" "%HZD_DIR%\HZDRawMouse.ini" 2>nul
    echo Removed HZDRawMouse from "%HZD_DIR%"
    exit /b 0
)

if not exist "%~dp0build\HZDRawMouse.asi" (echo Build first: build.bat & exit /b 1)
copy /Y "%~dp0build\HZDRawMouse.asi" "%HZD_DIR%\" >nul || exit /b 1
copy /Y "%~dp0config\HZDRawMouse.dev.ini" "%HZD_DIR%\HZDRawMouse.ini" >nul || exit /b 1
if not exist "%HZD_DIR%\winmm.dll" echo WARNING: Ultimate ASI Loader (winmm.dll) is not installed in the game dir
if exist "%HZD_DIR%\version.dll" echo WARNING: version.dll in the game dir is ignored by this game (compat shim) - use winmm.dll
echo Deployed HZDRawMouse.asi + HZDRawMouse.ini to "%HZD_DIR%"
