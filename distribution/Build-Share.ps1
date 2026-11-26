param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$SourceCommit=$env:ASIO_SOURCE_COMMIT,
    [string]$PackageVersion='1.3.0',
    [string]$BinarySourceCommit=''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
# Resolve provenance before creating or changing the export directory. GitHub
# source ZIPs have no .git; callers must supply the archive's real commit ID.
$commit=([string]$SourceCommit).Trim()
if(!$commit){
    $git=Get-Command git.exe -ErrorAction SilentlyContinue
    if($git -and (Test-Path -LiteralPath (Join-Path $repo '.git'))){
        $detected=& $git.Source -C $repo rev-parse --verify HEAD 2>$null
        if($LASTEXITCODE -eq 0){$commit=([string]$detected).Trim()}
    }
}
if($commit -notmatch '^[0-9a-fA-F]{40}$'){
    throw 'A full source commit is required. Use Git, -SourceCommit <40 hex>, or ASIO_SOURCE_COMMIT for a source archive.'
}
$commit=$commit.ToLowerInvariant()
if(!$BinarySourceCommit){$BinarySourceCommit=$commit}
if($BinarySourceCommit -notmatch '^[0-9a-fA-F]{40}$'){throw 'BinarySourceCommit must be a full 40-character commit.'}
$BinarySourceCommit=$BinarySourceCommit.ToLowerInvariant()
if($PackageVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$'){throw 'PackageVersion must be a semantic version or prerelease.'}
$binaries=@('LazerASIO.exe','asio_backend.exe','osu!asio_sound.dll','fmod.dll')
$inputs=@('AUTHORS.md','THIRD_PARTY_NOTICES.md','LICENSE','licenses\MinHook.txt','licenses\Harmony.txt','licenses\FMOD.txt','docs\USAGE.md')
$inputs+=$binaries|ForEach-Object{'release\'+$_}
$inputs+=@('Asio.ManagedBridge.dll','Asio.ManagedBridge.deps.json','0Harmony.dll')|ForEach-Object{'managed\bin\Release\net10.0\'+$_}
$inputs+=@('zh-CN','zh-TW','en','fr','es')|ForEach-Object{'locales\'+$_+'.lang'}
foreach($relative in $inputs){if(!(Test-Path -LiteralPath (Join-Path $repo $relative) -PathType Leaf)){throw ('Missing package input before export: '+$relative)}}
foreach ($name in @('client','logs','.asio-device-configured','language.ini','language.ini.tmp','gui.ini','gui.ini.tmp','asio_hook.log','playback-trace.enabled')) {
    if (Test-Path -LiteralPath (Join-Path $output $name)) { throw '目标已有运行资料，请选择新的空分享目录。' }
}
# Entry points replaced by LazerASIO.exe in 1.3.0 are removed from an older package folder.
$obsoleteFiles = @('config.exe','osu!asio_sound.exe','osu!asio_sound (select osu exename).exe','AsioDeviceCheck.exe','Start-ASIO.cmd','Test-ASIO-Device.cmd','Change-Language.cmd',
    'managed\Start-Offline.ps1','managed\Prepare-OfflineClient.ps1','managed\Start-Portable.ps1','managed\ClientFiles.ps1','managed\client-manifest.json','managed\Start-Standalone.ps1','managed\Localization.ps1')
$allowedFiles = @('LazerASIO.exe','config.ini','asio_backend.exe','osu!asio_sound.dll','fmod.dll','README.md','AUTHORS.md','THIRD_PARTY_NOTICES.md','LICENSE','licenses\MinHook.txt','licenses\Harmony.txt','licenses\FMOD.txt','MANIFEST.json','SHA256SUMS.txt',
    'locales\zh-CN.lang','locales\zh-TW.lang','locales\en.lang','locales\fr.lang','locales\es.lang',
    'managed\Asio.ManagedBridge.dll','managed\Asio.ManagedBridge.deps.json','managed\0Harmony.dll')+$obsoleteFiles
if (Test-Path -LiteralPath $output) {
    foreach ($item in Get-ChildItem -LiteralPath $output -Recurse -Force) {
        $relative = $item.FullName.Substring($output.Length+1)
        if (($item.PSIsContainer -and $relative -notin @('managed','locales','licenses')) -or
            (!$item.PSIsContainer -and $relative -notin $allowedFiles)) {
            throw "目标目录包含其他资料，保留原目录：$relative"
        }
    }
}
New-Item -ItemType Directory -Path $output,(Join-Path $output 'managed'),(Join-Path $output 'locales'),(Join-Path $output 'licenses') -Force | Out-Null
foreach($name in @('AUTHORS.md','THIRD_PARTY_NOTICES.md','LICENSE')){Copy-Item -LiteralPath (Join-Path $repo $name) -Destination (Join-Path $output $name) -Force}
foreach($name in @('MinHook.txt','Harmony.txt','FMOD.txt')){Copy-Item -LiteralPath (Join-Path "$repo\licenses" $name) -Destination (Join-Path "$output\licenses" $name) -Force}
foreach($code in @('zh-CN','zh-TW','en','fr','es')){Copy-Item -LiteralPath (Join-Path "$repo\locales" ($code+'.lang')) -Destination (Join-Path "$output\locales" ($code+'.lang')) -Force}
foreach ($name in $binaries) {
    Copy-Item -LiteralPath (Join-Path "$repo\release" $name) -Destination (Join-Path $output $name) -Force
}
foreach ($name in @('Asio.ManagedBridge.dll','Asio.ManagedBridge.deps.json','0Harmony.dll')) {
    Copy-Item -LiteralPath (Join-Path "$repo\managed\bin\Release\net10.0" $name) -Destination (Join-Path "$output\managed" $name) -Force
}
foreach ($name in $obsoleteFiles) {
    $obsolete=Join-Path $output $name
    if (Test-Path -LiteralPath $obsolete) {
        $resolved=[IO.Path]::GetFullPath($obsolete)
        if($resolved -ne (Join-Path $output $name)){throw '旧入口路径不在预期导出位置，停止清理。'}
        Remove-Item -LiteralPath $resolved
    }
}
Copy-Item -LiteralPath (Join-Path $repo 'docs\USAGE.md') -Destination "$output\README.md" -Force
[IO.File]::WriteAllText("$output\config.ini", "[Config]`r`nBuffer Size = 16`r`nASIO Driver ID = 0`r`nSample Rate = 48000`r`nOutput Volume = 50`r`nDebug Log = 0`r`nContract Mode = 1`r`n", [Text.Encoding]::ASCII)
$entries = @(Get-ChildItem -LiteralPath $output -Recurse -File | Where-Object { $_.Name -notin @('MANIFEST.json','SHA256SUMS.txt') } |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($output.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
[ordered]@{version=$PackageVersion;contract_default='Adaptive';languages=@('zh-CN','zh-TW','en','fr','es');source_commit=$commit.Trim();binary_source_commit=$BinarySourceCommit;client_included=$false;starts_game=$false;copies_client=$false;runtime_required='net10.0';compatibility='核对已审核音频实现契约；已检查Game1026/1110和Framework1019/1030';entry='LazerASIO.exe';backend='asio_backend.exe';files=$entries} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$output\MANIFEST.json" -Encoding UTF8
$sums = @($entries | ForEach-Object { $_.sha256+'  '+$_.path })
$sums += (Get-FileHash -LiteralPath "$output\MANIFEST.json" -Algorithm SHA256).Hash.ToLowerInvariant()+'  MANIFEST.json'
$sums | Set-Content -LiteralPath "$output\SHA256SUMS.txt" -Encoding ASCII
$zip = $output+'.zip'
Compress-Archive -LiteralPath @(Get-ChildItem -LiteralPath $output | ForEach-Object { $_.FullName }) -DestinationPath $zip -Force
Write-Output "已导出：$output"
Write-Output ('ZIP：'+$zip+'；MB='+[math]::Round((Get-Item -LiteralPath $zip).Length/1MB,2))
Write-Output ('SHA256='+(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
