#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$run=Join-Path $repo ('tests\bin\export-'+[guid]::NewGuid().ToString('N'))
$archive=Join-Path $run 'source archive'
$null=New-Item -ItemType Directory -Path (Join-Path $archive 'distribution')
Copy-Item -LiteralPath "$repo\distribution\Build-Share.ps1" -Destination (Join-Path $archive 'distribution\Build-Share.ps1')
$exporter=Join-Path $archive 'distribution\Build-Share.ps1'
$target=Join-Path $run 'missing-commit'
$previous=$env:ASIO_SOURCE_COMMIT
try {
    $env:ASIO_SOURCE_COMMIT=''
    $rejected=$false
    try{& $exporter -OutputDirectory $target}catch{$rejected=$_.Exception.Message.Contains('source commit')}
    if(!$rejected -or (Test-Path -LiteralPath $target)){throw 'Missing commit left a partial export.'}
    $rejected=$false
    try{& $exporter -OutputDirectory (Join-Path $run 'missing-input') -SourceCommit ('a'*40)}catch{$rejected=$_.Exception.Message.Contains('Missing package input')}
    if(!$rejected -or (Test-Path -LiteralPath (Join-Path $run 'missing-input'))){throw 'Missing input left a partial export.'}
    $inputs=@('AUTHORS.md','THIRD_PARTY_NOTICES.md','LICENSE','docs\USAGE.md','licenses\MinHook.txt','licenses\Harmony.txt','licenses\FMOD.txt')
    $inputs+=@('LazerASIO.exe','asio_backend.exe','osu!asio_sound.dll','fmod.dll')|ForEach-Object{'release\'+$_}
    $inputs+=@('Asio.ManagedBridge.dll','Asio.ManagedBridge.deps.json','0Harmony.dll')|ForEach-Object{'managed\bin\Release\net10.0\'+$_}
    $inputs+=@('zh-CN','zh-TW','en','fr','es')|ForEach-Object{'locales\'+$_+'.lang'}
    foreach($relative in $inputs){$destination=Join-Path $archive $relative;$null=New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force;Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination}
    $env:ASIO_SOURCE_COMMIT='b'*40
    $target=Join-Path $run 'archive-package'
    & $exporter -OutputDirectory $target
    $manifest=Get-Content -LiteralPath (Join-Path $target 'MANIFEST.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($manifest.source_commit -ne ('b'*40)){throw 'Archive commit override was not recorded.'}
    if((Get-FileHash -LiteralPath (Join-Path $target 'README.md')).Hash -ne (Get-FileHash -LiteralPath "$repo\docs\USAGE.md").Hash){throw 'Package README is not canonical usage guide.'}
    foreach($line in [IO.File]::ReadAllLines((Join-Path $target 'SHA256SUMS.txt'))){if($line -notmatch '^([0-9a-f]{64})  (.+)$'){throw 'Malformed checksum'};$hash=$Matches[1];$file=Join-Path $target $Matches[2];if((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hash){throw 'Export hash mismatch'}}
    Write-Output ('PASS export preflight, source archive environment fallback, canonical guide and checksums; fixtures='+$run)
} finally {$env:ASIO_SOURCE_COMMIT=$previous}
