@echo off
REM  Icarus - build the firmware WITH Bluetooth LE MIDI on your own PC.
REM  Checks the requirements, installs what is missing (asks first), gets the latest sources, builds.
REM  See Build-Icarus-BLE.ps1 for the switches (-CheckOnly, -NoUpdate, -Yes, -Pi32Root <dir>).
setlocal EnableExtensions
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Icarus-BLE.ps1" %*
set RC=%ERRORLEVEL%
echo.
pause
exit /b %RC%
