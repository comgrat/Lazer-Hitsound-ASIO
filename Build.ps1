#requires -Version 5.1
param([switch]$SkipTests)
$ErrorActionPreference='Stop'
$repo=$PSScriptRoot
if(!$env:VSCMD_ARG_TGT_ARCH -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64'){throw 'Run this script from an x64 Visual Studio Developer PowerShell/Command Prompt.'}
if(!(Get-Command cl.exe -ErrorAction SilentlyContinue)){throw 'MSVC cl.exe is not available in this shell.'}
if(!(Get-Command dotnet.exe -ErrorAction SilentlyContinue)){throw '.NET 10 SDK is required.'}
Push-Location -LiteralPath $repo
try {
    & cmd.exe /d /c Compile.bat
    if($LASTEXITCODE){throw 'Native build failed.'}
    & dotnet.exe build (Join-Path $repo 'managed\Asio.ManagedBridge.csproj') -c Release
    if($LASTEXITCODE){throw 'Managed build failed.'}
    & cl.exe /nologo /utf-8 /std:c++17 /EHsc /O2 /MT (Join-Path $repo 'tools\device\AsioDeviceCheck.cpp') "/Forelease\AsioDeviceCheck.obj" "/Ferelease\AsioDeviceCheck.exe" /link /LIBPATH:lib fmod_vc.lib
    if($LASTEXITCODE){throw 'Device checker build failed.'}
    if(!$SkipTests){
        & (Join-Path $repo 'tools\qa\Test-Core.ps1')
        if($LASTEXITCODE){throw 'Core regression failed.'}
    }
    Write-Output 'Build completed. Export a clean portable folder with distribution/Build-Share.ps1.'
} finally { Pop-Location }
