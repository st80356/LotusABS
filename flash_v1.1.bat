@echo off
setlocal EnableExtensions EnableDelayedExpansion
title LotusABS firmware v1.1 flasher
cd /d "%~dp0"

rem ------------------------------------------------------------------
rem  LotusABS firmware v1.1 flasher - WeMos D1 mini (ESP8266, 4MB, FS 1MB)
rem  Writes v1.1\FIRMWARE.bin @ 0x000000 and v1.1\LITTLEFS.bin @ 0x300000
rem ------------------------------------------------------------------
set "FWDIR=%~dp0v1.1"
set "FW=%FWDIR%\FIRMWARE.bin"
set "FS=%FWDIR%\LITTLEFS.bin"
set "FS_ADDR=0x300000"
set "BAUD=460800"
set "SLOW_BAUD=115200"

echo.
echo  ============================================
echo   LotusABS firmware v1.1 flasher (WeMos D1 mini)
echo  ============================================
echo.
echo  NOTE: Unplug the D1 mini from the LotusABS board before flashing.
echo        The K-line transceiver shares the TX/RX pins with USB.
echo  WARNING: use at your own risk. The tool shares the diagnostic line with other
echo        units, including the airbag system. Read DISCLAIMER.txt before using it on a car.
echo  Firmware v1.1 (latest): Debug Log and Wi-Fi Firmware Update pages.
echo.

rem ---------- check firmware files ----------
if not exist "%FW%" (
    echo  [ERROR] %FW% not found. Keep the v1.1 folder next to this script.
    goto :fail
)
set "DO_FS=1"
if not exist "%FS%" (
    echo  [WARN] %FS% not found - web UI files will NOT be flashed.
    set "DO_FS=0"
)

rem ---------- find esptool ----------
set "ESPTOOL="
if exist "%~dp0esptool.exe" set "ESPTOOL="%~dp0esptool.exe""
if not defined ESPTOOL (
    where esptool.exe >nul 2>&1 && set "ESPTOOL=esptool.exe"
)
if not defined ESPTOOL (
    where esptool >nul 2>&1 && set "ESPTOOL=esptool"
)
if not defined ESPTOOL (
    for %%P in (py python) do (
        if not defined ESPTOOL (
            %%P -m esptool version >nul 2>&1 && set "ESPTOOL=%%P -m esptool"
        )
    )
)
if not defined ESPTOOL (
    echo  esptool was not found.
    set "PYEXE="
    for %%P in (py python) do (
        if not defined PYEXE (
            %%P --version >nul 2>&1 && set "PYEXE=%%P"
        )
    )
    if not defined PYEXE (
        echo  Install Python from https://www.python.org/downloads/ ^(tick "Add to PATH"^),
        echo  or put esptool.exe from https://github.com/espressif/esptool/releases
        echo  in this folder, then run this script again.
        goto :fail
    )
    set /p "INST=  Install esptool now with pip? [Y/n]: "
    if /i "!INST!"=="n" goto :fail
    !PYEXE! -m pip install --upgrade esptool
    if errorlevel 1 (
        echo  [ERROR] pip install failed.
        goto :fail
    )
    set "ESPTOOL=!PYEXE! -m esptool"
)
echo  Using: %ESPTOOL%
echo.

rem ---------- pick COM port ----------
:scan
set "PORTFILE=%TEMP%\lotusabs_ports_%RANDOM%.txt"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\((COM\d+)\)' } | ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $matches[1] + '|' + $_.Name } } | Sort-Object { [int]($_ -replace '^COM(\d+)\|.*','$1') }" > "%PORTFILE%" 2>nul

set /a N=0
for /f "usebackq tokens=1,* delims=|" %%A in ("%PORTFILE%") do (
    set /a N+=1
    set "PORT!N!=%%A"
    echo   [!N!] %%B
)
del "%PORTFILE%" >nul 2>&1

if %N%==0 (
    echo  No COM ports found. Plug in the D1 mini with a USB data cable.
    echo  If it still does not show up, install the CH340 driver.
)
echo.
echo   [R] Rescan    [M] Type port manually    [Q] Quit
echo.
set "SEL="
set /p "SEL=  Select port: "
if not defined SEL goto :scan
if /i "%SEL%"=="q" goto :end
if /i "%SEL%"=="r" ( echo. & goto :scan )
if /i "%SEL%"=="m" (
    set "COMPORT="
    set /p "COMPORT=  Enter port (e.g. COM5): "
    if not defined COMPORT goto :scan
    goto :haveport
)
set "COMPORT="
for /l %%I in (1,1,%N%) do (
    if "%SEL%"=="%%I" set "COMPORT=!PORT%%I!"
)
if not defined COMPORT (
    echo  Invalid choice.
    echo.
    goto :scan
)

:haveport
echo.
echo  Port: %COMPORT%
echo.
rem The firmware keeps nothing on the module (Wi-Fi is always the LOTUS-ABS access point), so the
rem whole flash is always erased first: clean start, no leftovers from another version.
echo  Erasing flash (always: nothing on the module needs keeping)...
%ESPTOOL% --chip esp8266 --port %COMPORT% --baud %BAUD% erase_flash
if errorlevel 1 (
    echo  Retrying erase at %SLOW_BAUD% baud...
    %ESPTOOL% --chip esp8266 --port %COMPORT% --baud %SLOW_BAUD% erase_flash
    if errorlevel 1 goto :flashfail
)
echo.

set "IMAGES=0x000000 "%FW%""
if "%DO_FS%"=="1" set "IMAGES=!IMAGES! %FS_ADDR% "%FS%""

echo  Flashing at %BAUD% baud...
%ESPTOOL% --chip esp8266 --port %COMPORT% --baud %BAUD% write_flash -fm dio -fs 4MB %IMAGES%
if not errorlevel 1 goto :ok

echo.
echo  Retrying at %SLOW_BAUD% baud...
%ESPTOOL% --chip esp8266 --port %COMPORT% --baud %SLOW_BAUD% write_flash -fm dio -fs 4MB %IMAGES%
if not errorlevel 1 goto :ok

:flashfail
echo.
echo  [ERROR] Flashing failed. Check:
echo    - the port is not open in a serial monitor or another program
echo    - the USB cable carries data (not charge-only)
echo    - the D1 mini is unplugged from the LotusABS board
echo    - if auto-reset fails: bridge D3 to GND, press RESET, flash, then remove the bridge
goto :fail

:ok
echo.
echo  ============================================
echo   Done: firmware v1.1 flashed. The D1 mini has been reset.
echo  ============================================
goto :end

:fail
echo.
pause
exit /b 1

:end
echo.
pause
exit /b 0
