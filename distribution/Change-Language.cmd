@echo off
chcp 65001 >nul
setlocal
"%~dp0config.exe" --language
if errorlevel 1 pause
