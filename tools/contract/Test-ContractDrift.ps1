#requires -Version 5.1
[CmdletBinding(DefaultParameterSetName='Local')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Local')][string]$ClientDirectory,
    [Parameter(Mandatory=$true,ParameterSetName='Download')][switch]$DownloadRelease,
    [Parameter(ParameterSetName='Download')][string]$ReleaseTag='latest',
    [Parameter(ParameterSetName='Download')][string]$ExpectedSha256='',
    [string]$OutputDirectory=''
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo ('artifacts\drift-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))}
$output=[IO.Path]::GetFullPath($OutputDirectory)
# Evidence is append-only: no previous report or source client is overwritten.
if(Test-Path -LiteralPath $output){throw 'Choose a new output directory.'}
if($ClientDirectory){
    $ClientDirectory=(Resolve-Path -LiteralPath $ClientDirectory).Path.TrimEnd('\')
    if($output.StartsWith($ClientDirectory+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Evidence must be outside the read-only client.'}
}
New-Item -ItemType Directory -Path $output | Out-Null
$catalog=Join-Path $repo 'managed\audio-contract.json'
$catalogHash=(Get-FileHash -LiteralPath $catalog).Hash
$source=[ordered]@{kind='local';client=$ClientDirectory}
$code=1
try {
    if($DownloadRelease){
        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
        if($ReleaseTag -ne 'latest' -and $ReleaseTag -notmatch '^[0-9A-Za-z._-]+$'){throw 'Invalid release tag.'}
        $api=if($ReleaseTag -eq 'latest'){'https://api.github.com/repos/ppy/osu/releases/latest'}else{'https://api.github.com/repos/ppy/osu/releases/tags/'+$ReleaseTag}
        $headers=@{'User-Agent'='LazerASIO-contract-drift';Accept='application/vnd.github+json'}
        if($env:GITHUB_TOKEN){$headers.Authorization='Bearer '+$env:GITHUB_TOKEN}
        $release=Invoke-RestMethod -Uri $api -Headers $headers
        $tag=[string]$release.tag_name
        if($tag -notmatch '^[0-9A-Za-z._-]+$'){throw 'Unexpected release tag.'}
        # Resolve latest once, then pin the exact tag/asset/digest for this run.
        $asset=@($release.assets|Where-Object{$_.name -eq "osulazer-$tag-full.nupkg"})
        if($asset.Count -ne 1){throw 'Expected exactly one Windows full nupkg.'}
        $url=[string]$asset[0].browser_download_url
        if($url -ne "https://github.com/ppy/osu/releases/download/$tag/osulazer-$tag-full.nupkg"){throw 'Unexpected release asset URL.'}
        $digest=[string]$asset[0].digest
        if($ExpectedSha256){
            if($ExpectedSha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'ExpectedSha256 must have 64 hex digits.'}
            $expected=$ExpectedSha256.ToLowerInvariant()
            if($digest -and $digest -ne ('sha256:'+$expected)){throw 'Pinned digest disagrees with GitHub asset metadata.'}
        }else{
            if($digest -notmatch '^sha256:([a-fA-F0-9]{64})$'){throw 'No SHA-256 available; supply ExpectedSha256 explicitly.'}
            $expected=$Matches[1].ToLowerInvariant()
        }
        $source=[ordered]@{kind='official-release';tag=$tag;api=$api;release_id=$release.id;asset_id=$asset[0].id;url=$url;expected_sha256=$expected}
        $source|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'source.json') -Encoding UTF8
        $archive=Join-Path $output 'release.nupkg'
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
        $actual=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
        if($actual -ne $expected){throw 'Downloaded package hash mismatch.'}
        $source.actual_sha256=$actual
        # Read ZIP entries directly; no installer or client executable is run.
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip=[IO.Compression.ZipFile]::OpenRead($archive)
        try{
            $games=@($zip.Entries|Where-Object{$_.FullName -match '(^|/)osu\.Game\.dll$'})
            if($games.Count -ne 1){throw 'Ambiguous game assembly in release package.'}
            $prefix=$games[0].FullName.Substring(0,$games[0].FullName.Length-'osu.Game.dll'.Length)
            $ClientDirectory=Join-Path $output 'metadata-client'
            New-Item -ItemType Directory -Path $ClientDirectory|Out-Null
            foreach($entry in $zip.Entries){
                if(!$entry.FullName.StartsWith($prefix,[StringComparison]::Ordinal)){continue}
                $name=$entry.FullName.Substring($prefix.Length)
                if($name -notmatch '^[^/\\]+\.dll$'){continue}
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $ClientDirectory $name),$false)
            }
        }finally{$zip.Dispose()}
    }
    foreach($name in @('osu!.dll','osu.Framework.dll','osu.Game.dll')){
        if(!(Test-Path -LiteralPath (Join-Path $ClientDirectory $name) -PathType Leaf)){throw ('Missing client assembly: '+$name)}
    }
    $hashes=@{};foreach($file in Get-ChildItem -LiteralPath $ClientDirectory -Filter '*.dll' -File){$hashes[$file.Name]=(Get-FileHash -LiteralPath $file.FullName).Hash}
    $source.client=$ClientDirectory
    $source|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'source.json') -Encoding UTF8
    . (Join-Path $repo 'tools\qa\Invoke-BoundedProcess.ps1')
    $dotnet=(Get-Command dotnet.exe -ErrorAction Stop).Source
    Invoke-BoundedProcess -FilePath $dotnet -ArgumentList @('build',(Join-Path $PSScriptRoot 'ContractTool\ContractTool.csproj'),'-c','Release','--nologo') -WorkingDirectory $repo -TimeoutSeconds 180 -LogPrefix (Join-Path $output 'tool-build') | Out-Null
    $tool=Join-Path $PSScriptRoot 'ContractTool\bin\Release\net10.0\ContractTool.dll'
    $candidate=Join-Path $output 'candidate.json'
    try{
        Invoke-BoundedProcess -FilePath $dotnet -ArgumentList @($tool,'inspect',$ClientDirectory,$candidate) -WorkingDirectory $repo -TimeoutSeconds 180 -LogPrefix (Join-Path $output 'inspect') | Out-Null
    }catch{if(!(Test-Path -LiteralPath $candidate)){throw}}
    foreach($name in $hashes.Keys){if((Get-FileHash -LiteralPath (Join-Path $ClientDirectory $name)).Hash -ne $hashes[$name]){throw ('Client modified: '+$name)}}
    $result=Get-Content -Raw -Encoding UTF8 -LiteralPath $candidate|ConvertFrom-Json
    $status=[string]$result.validation.status
    $code=if($status -eq 'ExactMatch'){0}elseif($status -eq 'AdaptiveMatch'){2}else{3}
    $report=@('Audio contract drift check',('Status: '+$status),('Framework: '+$result.framework.version),('Game: '+$result.game.version),('Reviewed entry: '+$result.validation.reviewedId),('Tier1 errors: '+$result.validation.structuralErrors.Count),('Tier2 errors: '+$result.validation.semanticErrors.Count),'No promotion performed. Client and runtime catalog unchanged.')
    $report|Set-Content -LiteralPath (Join-Path $output 'REPORT.txt') -Encoding UTF8
    $report|ForEach-Object{Write-Output $_}
}catch{
    $_.Exception.ToString()|Set-Content -LiteralPath (Join-Path $output 'ERROR.txt') -Encoding UTF8
    Write-Error -ErrorAction Continue $_
    $code=1
}finally{
    if((Get-FileHash -LiteralPath $catalog).Hash -ne $catalogHash){throw 'Runtime catalog changed.'}
}
Write-Output ('Evidence: '+$output)
exit $code
