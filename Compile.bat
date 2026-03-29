@echo off
setlocal

if "%VSCMD_ARG_TGT_ARCH%"=="" (
    echo Please run this script from an x64 Visual Studio Developer Command Prompt.
    exit /b 1
)

if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Current MSVC target is "%VSCMD_ARG_TGT_ARCH%". Re-open an x64 Developer Command Prompt.
    exit /b 1
)

set "FMOD_LIB_DIR=lib"
set "FMOD_LIB=%FMOD_LIB_DIR%\fmod_vc.lib"
set "MINHOOK_LIB=lib\libMinHook.x64.lib"
set "OUTPUT_DIR=release"

if not exist "%FMOD_LIB%" (
    echo Missing %FMOD_LIB%
    exit /b 1
)

if not exist "%MINHOOK_LIB%" (
    echo Missing %MINHOOK_LIB%
    exit /b 1
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

cl /nologo /std:c++17 /EHsc /O2 /MD config.cpp /link /OUT:"%OUTPUT_DIR%\config.exe" /LIBPATH:"%FMOD_LIB_DIR%" fmod_vc.lib Advapi32.lib User32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /O2 /MD osu!asio_sound.cpp /link /OUT:"%OUTPUT_DIR%\osu!asio_sound.exe" /LIBPATH:"%FMOD_LIB_DIR%" fmod_vc.lib Advapi32.lib User32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /O2 /MD /D__EXESELECT "osu!asio_sound (select osu exename).cpp" /link /OUT:"%OUTPUT_DIR%\osu!asio_sound (select osu exename).exe" /LIBPATH:"%FMOD_LIB_DIR%" fmod_vc.lib Advapi32.lib User32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /O2 /MD /LD dllmain.cpp /link /OUT:"%OUTPUT_DIR%\osu!asio_sound.dll" "%MINHOOK_LIB%" User32.lib
if errorlevel 1 exit /b 1

echo Build completed. Outputs are in "%OUTPUT_DIR%".
