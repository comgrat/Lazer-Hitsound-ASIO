@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
"%~dp0osu!asio_sound.exe"
if errorlevel 1 (
    echo.
    pause
    exit /b 1
)
