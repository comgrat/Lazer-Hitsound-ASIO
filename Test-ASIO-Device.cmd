@echo off
chcp 65001 >nul
cd /d "%~dp0"
AsioDeviceCheck.exe
set "DEVICE_TEST_EXIT=%errorlevel%"
echo.
pause
exit /b %DEVICE_TEST_EXIT%
