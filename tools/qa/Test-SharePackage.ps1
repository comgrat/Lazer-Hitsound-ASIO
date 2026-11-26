# Independent QA for the portable ASIOHook share package.
#
# Safety rules:
#   - Only LazerASIO.exe --show and --list are executed (config.exe in
#     layouts before 1.3.0). The window itself is never opened.
#   - --show must be read-only and must not enumerate or open a device.
#   - --list enumerates ASIO drivers but must never call FMOD_System_Init.
#   - No game, no account, no beatmap data, no interactive configuration.
#   - The package is copied to a path containing spaces under
#     tests/Distribution/temporary relocation and executed from a different cwd.
#
# The test is intentionally independent from the master scripts. It parses the
# PowerShell files with the language parser and reads only package files.
#
# Usage:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/qa/Test-SharePackage.ps1 -PackageDirectory .\artifacts\LazerASIO
#
# Exit codes: 0 = all required checks passed, 1 = one or more checks failed,
#             2 = setup error (missing package directory or unsafe relocation).
#
# This script is ASCII-only so Windows PowerShell 5.1 can read it without a BOM.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [string]$DumpbinPath = '',

    [string]$RelocationRoot = ''
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$script:PassCount = 0
$script:FailCount = 0
$script:WarnCount = 0
$script:Checks = New-Object System.Collections.ArrayList

function Add-Check {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Detail = ''
    )
    $status = if ($Ok) { 'PASS' } else { 'FAIL' }
    if ($Ok) { $script:PassCount++ } else { $script:FailCount++ }
    $cleanDetail = ($Detail -replace '\r?\n', ' | ').Trim()
    $null = $script:Checks.Add([pscustomobject]@{
        name = $Name
        status = $status
        detail = $cleanDetail
    })
    if ($cleanDetail) {
        Write-Output ("{0} {1} :: {2}" -f $status, $Name, $cleanDetail)
    }
    else {
        Write-Output ("{0} {1}" -f $status, $Name)
    }
}

function Add-Warn {
    param(
        [string]$Name,
        [string]$Detail = ''
    )
    $script:WarnCount++
    $cleanDetail = ($Detail -replace '\r?\n', ' | ').Trim()
    $null = $script:Checks.Add([pscustomobject]@{
        name = $Name
        status = 'WARN'
        detail = $cleanDetail
    })
    if ($cleanDetail) {
        Write-Output ("WARN {0} :: {1}" -f $Name, $cleanDetail)
    }
    else {
        Write-Output ("WARN {0}" -f $Name)
    }
}

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-Sha256IfExists {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Get-Sha256 -Path $Path) }
    return ''
}

function Get-RelativePath {
    param(
        [string]$Root,
        [string]$FullPath
    )
    $rootWithSlash = $Root.TrimEnd('\') + '\'
    if ($FullPath.StartsWith($rootWithSlash, [StringComparison]::OrdinalIgnoreCase)) {
        return $FullPath.Substring($rootWithSlash.Length)
    }
    return $FullPath
}

function Get-TreeManifest {
    param([string]$Root)
    $manifest = @{}
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return $manifest }
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -Recurse -Force -File -ErrorAction Stop)) {
        $relative = Get-RelativePath -Root $Root -FullPath $file.FullName
        $manifest[$relative] = (Get-Sha256 -Path $file.FullName)
    }
    return $manifest
}

function Compare-TreeManifest {
    param(
        [hashtable]$Before,
        [hashtable]$After
    )
    $changes = New-Object System.Collections.ArrayList
    foreach ($key in @($Before.Keys | Sort-Object)) {
        if (-not $After.ContainsKey($key)) {
            $null = $changes.Add("deleted:$key")
        }
        elseif ($After[$key] -ne $Before[$key]) {
            $null = $changes.Add("modified:$key")
        }
    }
    foreach ($key in @($After.Keys | Sort-Object)) {
        if (-not $Before.ContainsKey($key)) {
            $null = $changes.Add("created:$key")
        }
    }
    return @($changes)
}

function Find-PackageFile {
    param(
        [string]$Root,
        [string[]]$Candidates
    )
    foreach ($relative in $Candidates) {
        $candidate = Join-Path $Root $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    return $null
}

function Read-StrictUtf8 {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $offset = 0
    $hasBom = $false
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $offset = 3
        $hasBom = $true
    }
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $text = $encoding.GetString($bytes, $offset, $bytes.Length - $offset)
    return [pscustomobject]@{
        Text = $text
        HasUtf8Bom = $hasBom
        HasUtf16Bom = ($bytes.Length -ge 2 -and (($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF)))
        Bytes = $bytes
    }
}

function Get-CommandNames {
    param([string]$Path)
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors -and $errors.Count -gt 0) { return @() }
    $commands = $ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.CommandAst]
    }, $true)
    return @($commands | ForEach-Object { $_.GetCommandName() } | Where-Object { $_ })
}

function Get-FunctionNames {
    param([string]$Path)
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors -and $errors.Count -gt 0) { return @() }
    $functions = $ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
    }, $true)
    return @($functions | ForEach-Object { $_.Name })
}

function Invoke-ConfigProcess {
    param(
        [string]$Executable,
        [string]$Arguments,
        [string]$WorkingDirectory,
        [string]$StdoutPath,
        [string]$StderrPath,
        [int]$TimeoutMilliseconds = 20000
    )
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.Arguments = $Arguments
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $started = $process.Start()
    if (-not $started) { throw "could not start $Executable" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
        $timedOut = $true
        try { $process.Kill() } catch { }
        $null = $process.WaitForExit(5000)
    }
    $watch.Stop()
    $stdout = ''
    $stderr = ''
    try { $stdout = $stdoutTask.Result } catch { $stdout = '' }
    try { $stderr = $stderrTask.Result } catch { $stderr = '' }
    $exitCode = $null
    try { $exitCode = $process.ExitCode } catch { $exitCode = $null }
    try { $process.Dispose() } catch { }

    $utf8NoBom = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($StdoutPath, $stdout, $utf8NoBom)
    [IO.File]::WriteAllText($StderrPath, $stderr, $utf8NoBom)

    return [pscustomobject]@{
        ExitCode = $exitCode
        TimedOut = $timedOut
        DurationMilliseconds = $watch.ElapsedMilliseconds
        Stdout = $stdout
        Stderr = $stderr
        StdoutPath = $StdoutPath
        StderrPath = $StderrPath
    }
}

function Test-ConfigValueLines {
    param(
        [string]$Text,
        [hashtable]$Values
    )
    $missing = New-Object System.Collections.ArrayList
    foreach ($key in @($Values.Keys | Sort-Object)) {
        $pattern = '(?im)^\s*' + [regex]::Escape($key) + '\s*=\s*' + [regex]::Escape([string]$Values[$key]) + '\s*$'
        if ($Text -notmatch $pattern) {
            $null = $missing.Add(("{0}={1}" -f $key, $Values[$key]))
        }
    }
    return @($missing)
}

function Get-DeveloperPathHits {
    param([string]$Text)
    $pattern = '(?i)(?:^|[^A-Za-z0-9])[DF]:[\\/]{1,2}(?:osu-asio-fix|repair)(?:[\\/]|$)'
    return @([regex]::Matches($Text, $pattern) | ForEach-Object { $_.Value })
}

function Get-BinaryDeveloperPathHits {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $hits = New-Object System.Collections.ArrayList
    foreach ($encoding in @([Text.Encoding]::ASCII, [Text.Encoding]::Unicode)) {
        $text = $encoding.GetString($bytes)
        foreach ($match in [regex]::Matches($text, '(?i)[DF]:[\\/]{1,2}(?:osu-asio-fix|repair)[^"\x00\r\n]{0,120}')) {
            $null = $hits.Add($match.Value)
        }
    }
    return @($hits | Select-Object -Unique)
}

# ---------------------------------------------------------------------------
# Setup and safe relocation
# ---------------------------------------------------------------------------
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath $PackageDirectory -PathType Container)) {
    Write-Output ("SETUP_ERROR package directory not found: {0}" -f $PackageDirectory)
    exit 2
}
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\')
$allowedRelocationPrefix = Join-Path $repoRoot 'tests\Distribution'
$defaultRelocationBase = Join-Path $allowedRelocationPrefix 'temporary relocation'
$relocationBase = if ([string]::IsNullOrWhiteSpace($RelocationRoot)) {
    $defaultRelocationBase
}
else {
    [IO.Path]::GetFullPath($RelocationRoot).TrimEnd('\')
}
if (-not ($relocationBase.Equals($allowedRelocationPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $relocationBase.StartsWith($allowedRelocationPrefix + '\', [StringComparison]::OrdinalIgnoreCase))) {
    Write-Output ("SETUP_ERROR relocation must stay under {0}: {1}" -f $allowedRelocationPrefix, $relocationBase)
    exit 2
}
$relocation = Join-Path $relocationBase 'ASIOHook Share'
if (-not $relocation.StartsWith($allowedRelocationPrefix + '\', [StringComparison]::OrdinalIgnoreCase)) {
    Write-Output ("SETUP_ERROR unsafe relocation path: {0}" -f $relocation)
    exit 2
}
if ((Split-Path -Leaf $relocation) -ne 'ASIOHook Share') {
    Write-Output ("SETUP_ERROR unexpected relocation leaf: {0}" -f $relocation)
    exit 2
}

Write-Output '=== ASIOHook share package independent QA ==='
Write-Output ("package={0}" -f $package)
Write-Output ("working_directory={0}" -f $repoRoot)
Write-Output ("relocation={0}" -f $relocation)
Write-Output ("powershell={0}" -f $PSVersionTable.PSVersion)
Write-Output ("time_utc={0}" -f [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))

$dumpbin = ''
if ($DumpbinPath) {
    if (Test-Path -LiteralPath $DumpbinPath -PathType Leaf) { $dumpbin = (Resolve-Path -LiteralPath $DumpbinPath).Path }
}
if (-not $dumpbin) {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) { $dumpbin = $command.Source }
}
Add-Check -Name 'dumpbin available for dependency check' -Ok ([bool]$dumpbin) -Detail $(if ($dumpbin) { $dumpbin } else { 'dumpbin.exe not found; pass -DumpbinPath' })

New-Item -ItemType Directory -Force -Path $relocationBase | Out-Null
if (Test-Path -LiteralPath $relocation) {
    $resolved=(Resolve-Path -LiteralPath $relocation).Path
    if($resolved -ne $relocation -or !$resolved.StartsWith($allowedRelocationPrefix+'\',[StringComparison]::OrdinalIgnoreCase) -or (Get-Item -LiteralPath $resolved).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Unsafe test-relocation cleanup target.'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $relocation | Out-Null
foreach ($item in @(Get-ChildItem -LiteralPath $package -Force -ErrorAction Stop)) {
    Copy-Item -LiteralPath $item.FullName -Destination $relocation -Recurse -Force
}

$allFiles = @(Get-ChildItem -LiteralPath $package -Recurse -Force -File -ErrorAction Stop)
$allDirectories = @(Get-ChildItem -LiteralPath $package -Recurse -Force -Directory -ErrorAction Stop)

# ---------------------------------------------------------------------------
# Required files
# ---------------------------------------------------------------------------
$required = [ordered]@{}
$guiLayout = Test-Path -LiteralPath (Join-Path $package 'LazerASIO.exe')
if ($guiLayout) {
    $required['window entry'] = Find-PackageFile -Root $package -Candidates @('LazerASIO.exe')
} else {
    $required['config.exe'] = Find-PackageFile -Root $package -Candidates @('config.exe')
    $required['player executable'] = Find-PackageFile -Root $package -Candidates @('osu!asio_sound.exe')
}
$required['config.ini'] = Find-PackageFile -Root $package -Candidates @('config.ini')
$newEntryLayout = Test-Path -LiteralPath (Join-Path $package 'asio_backend.exe')
$standaloneLayout = Test-Path -LiteralPath (Join-Path $package 'managed\Start-Standalone.ps1')
if ($newEntryLayout) { $required['audio backend'] = Find-PackageFile -Root $package -Candidates @('asio_backend.exe') }
$required['hook DLL'] = Find-PackageFile -Root $package -Candidates @('osu!asio_sound.dll')
$required['fmod.dll'] = Find-PackageFile -Root $package -Candidates @('fmod.dll')
if (!$guiLayout) { $required['device checker'] = Find-PackageFile -Root $package -Candidates @('AsioDeviceCheck.exe') }
$multilingualLayout=Test-Path -LiteralPath (Join-Path $package 'locales\en.lang')
if($multilingualLayout){
    if (!$guiLayout) {
        $required['language selector']=Find-PackageFile -Root $package -Candidates @('Change-Language.cmd')
        $required['watcher localization']=Find-PackageFile -Root $package -Candidates @('managed\Localization.ps1')
    }
    foreach($code in @('zh-CN','zh-TW','en','fr','es')){$required[('language '+$code)]=Find-PackageFile -Root $package -Candidates @('locales\'+$code+'.lang')}
}
if (!$guiLayout -and -not $required['device checker']) {
    $deviceCheck = Get-ChildItem -LiteralPath $package -Recurse -File -Filter '*DeviceCheck*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($deviceCheck) { $required['device checker'] = $deviceCheck.FullName }
}
$required['managed bridge'] = Find-PackageFile -Root $package -Candidates @('managed\Asio.ManagedBridge.dll')
$required['managed Harmony'] = Find-PackageFile -Root $package -Candidates @('managed\0Harmony.dll')
if ($guiLayout) {
    # The watcher runs inside LazerASIO.exe; no managed launcher script ships.
} elseif ($standaloneLayout) {
    $required['managed standalone watcher'] = Find-PackageFile -Root $package -Candidates @('managed\Start-Standalone.ps1')
} elseif ($newEntryLayout) {
    $required['managed program inventory'] = Find-PackageFile -Root $package -Candidates @('managed\ClientFiles.ps1')
} else {
    $required['managed client manifest'] = Find-PackageFile -Root $package -Candidates @('managed\client-manifest.json')
}
$required['managed deps JSON'] = Find-PackageFile -Root $package -Candidates @('managed\Asio.ManagedBridge.deps.json', 'managed\osu!.deps.json')
if (-not $required['managed deps JSON']) {
    $managedDirectory = Join-Path $package 'managed'
    if (Test-Path -LiteralPath $managedDirectory) {
        $depsFile = Get-ChildItem -LiteralPath $managedDirectory -File -Filter '*.deps.json' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($depsFile) { $required['managed deps JSON'] = $depsFile.FullName }
    }
}
if (!$standaloneLayout -and !$guiLayout) {
    $required['managed Start-Offline'] = Find-PackageFile -Root $package -Candidates @('managed\Start-Offline.ps1')
    $required['managed Prepare-OfflineClient'] = Find-PackageFile -Root $package -Candidates @('managed\Prepare-OfflineClient.ps1')
    $required['managed Start-Portable'] = Find-PackageFile -Root $package -Candidates @('managed\Start-Portable.ps1')
}
if (!$guiLayout) { $required['Start-ASIO.cmd'] = Find-PackageFile -Root $package -Candidates @('distribution\Start-ASIO.cmd', 'Start-ASIO.cmd') }
$required['Chinese README'] = Find-PackageFile -Root $package -Candidates @('README.md')
if (-not $required['Chinese README']) {
    $rootReadme = Get-ChildItem -LiteralPath $package -File -Filter 'README*.md' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($rootReadme) { $required['Chinese README'] = $rootReadme.FullName }
}
if (-not $required['Chinese README']) {
    $distributionReadme = Get-ChildItem -LiteralPath (Join-Path $package 'distribution') -File -Filter 'README*.md' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($distributionReadme) { $required['Chinese README'] = $distributionReadme.FullName }
}

foreach ($label in @($required.Keys)) {
    $path = $required[$label]
    Add-Check -Name ("required file: {0}" -f $label) -Ok ([bool]$path) -Detail $(if ($path) { Get-RelativePath -Root $package -FullPath $path } else { 'missing' })
}
$missingRequired = @($required.Keys | Where-Object { -not $required[$_] })
if ($guiLayout) {
    $replaced = @('config.exe','osu!asio_sound.exe','osu!asio_sound (select osu exename).exe','AsioDeviceCheck.exe','Start-ASIO.cmd','Test-ASIO-Device.cmd','Change-Language.cmd','managed\Start-Standalone.ps1','managed\Localization.ps1') |
        Where-Object { Test-Path -LiteralPath (Join-Path $package $_) }
    Add-Check -Name 'entry points replaced by LazerASIO.exe are absent' -Ok (@($replaced).Count -eq 0) -Detail ($(if (@($replaced).Count) { $replaced -join ', ' } else { 'single window entry' }))
}
Add-Check -Name 'required file set complete' -Ok ($missingRequired.Count -eq 0) -Detail ($(if ($missingRequired.Count) { 'missing: ' + ($missingRequired -join ', ') } else { 'all required files present' }))

$requiredFiles = @($required.Values | Where-Object { $_ } | ForEach-Object { Get-Item -LiteralPath $_ })
foreach ($file in $requiredFiles) {
    Add-Check -Name ("non-empty: {0}" -f (Get-RelativePath -Root $package -FullPath $file.FullName)) -Ok ($file.Length -gt 0) -Detail ("{0} bytes" -f $file.Length)
}

$manifestPath = $required['managed client manifest']
if ($manifestPath) {
    try {
        $manifestText = (Read-StrictUtf8 -Path $manifestPath).Text
        $manifest = $manifestText | ConvertFrom-Json
        $manifestFiles = @($manifest.files)
        Add-Check -Name 'client manifest is valid JSON with file list' -Ok ($manifestFiles.Count -gt 0) -Detail ("{0} files" -f $manifestFiles.Count)
        Add-Check -Name 'client manifest names supported client 2026.1026.0-tachyon' -Ok ([string]$manifest.client -match '2026\.1026\.0-tachyon') -Detail ([string]$manifest.client)
        Add-Check -Name 'client manifest names supported framework 2026.1019.0' -Ok ([string]$manifest.framework -match '2026\.1019\.0') -Detail ([string]$manifest.framework)
        $manifestGame = @($manifestFiles | Where-Object { $_.path -eq 'osu.Game.dll' })
        $manifestFramework = @($manifestFiles | Where-Object { $_.path -eq 'osu.Framework.dll' })
        Add-Check -Name 'client manifest includes osu.Game.dll and osu.Framework.dll hashes' -Ok ($manifestGame.Count -eq 1 -and $manifestFramework.Count -eq 1) -Detail 'both assemblies present'
        $badManifestPaths = New-Object System.Collections.ArrayList
        foreach ($entry in $manifestFiles) {
            $entryPath = [string]$entry.path
            $entryHash = [string]$entry.sha256
            if ([string]::IsNullOrWhiteSpace($entryPath)) { $null = $badManifestPaths.Add('empty path'); continue }
            if ([IO.Path]::IsPathRooted($entryPath) -or $entryPath -match '(^|[\\/])\.\.([\\/]|$)') { $null = $badManifestPaths.Add("unsafe:$entryPath") }
            if ($entryHash -notmatch '^[0-9a-fA-F]{64}$') { $null = $badManifestPaths.Add("bad-hash:$entryPath") }
            if ($entryPath -match '(?i)(^|[\\/])(logs|songs|skins|replays|exports|cache|files)([\\/]|$)' -or
                $entryPath -match '(?i)\.(db|ini|cfg|log|realm|token|osz|osu)$') {
                $null = $badManifestPaths.Add("personal-data:$entryPath")
            }
        }
        Add-Check -Name 'client manifest contains only relative program files' -Ok ($badManifestPaths.Count -eq 0) -Detail ($(if ($badManifestPaths.Count) { (@($badManifestPaths | Select-Object -First 8) -join ', ') } else { 'all relative and hash-addressed' }))
    }
    catch {
        Add-Check -Name 'client manifest is valid JSON with file list' -Ok $false -Detail $_.Exception.Message
    }
}

# ---------------------------------------------------------------------------
# Configuration file: five original fields plus optional numeric Contract Mode
# ---------------------------------------------------------------------------
$configPath = $required['config.ini']
$configValues = @{}
$configOrder = @()
$configRaw = ''
if ($configPath) {
    $configBytes = [IO.File]::ReadAllBytes($configPath)
    $configHasBom = $configBytes.Length -ge 3 -and $configBytes[0] -eq 0xEF -and $configBytes[1] -eq 0xBB -and $configBytes[2] -eq 0xBF
    $configRaw = [Text.Encoding]::ASCII.GetString($configBytes)
    Add-Check -Name 'config.ini ASCII/no BOM' -Ok ((-not $configHasBom) -and ($configRaw -notmatch '[^\x00-\x7F]')) -Detail $(if ($configHasBom) { 'UTF-8 BOM present' } elseif ($configRaw -match '[^\x00-\x7F]') { 'non-ASCII bytes present' } else { 'ASCII' })
    $configLines = @($configRaw -split "`r?`n")
    $entries = New-Object System.Collections.ArrayList
    foreach ($line in $configLines) {
        if ($line -match '^\s*([^=]+?)\s*=\s*(.*?)\s*$') {
            $null = $entries.Add([pscustomobject]@{ Key = $Matches[1]; Value = $Matches[2] })
        }
    }
    $expectedKeys = @('Buffer Size', 'ASIO Driver ID', 'Sample Rate', 'Output Volume', 'Debug Log')
    if(@($entries|Where-Object {$_.Key -eq 'Contract Mode'}).Count){$expectedKeys+='Contract Mode'}
    Add-Check -Name 'config.ini has the supported audio fields' -Ok ($entries.Count -eq $expectedKeys.Count) -Detail ("found {0} key/value lines" -f $entries.Count)
    $actualOrder = @($entries | ForEach-Object { $_.Key })
    Add-Check -Name 'config.ini preserves supported field order' -Ok (($actualOrder -join '|') -eq ($expectedKeys -join '|')) -Detail (($actualOrder -join '|'))
    foreach ($expectedKey in $expectedKeys) {
        $entry = $entries | Where-Object { $_.Key -ieq $expectedKey } | Select-Object -First 1
        if (-not $entry) {
            Add-Check -Name ("config field present: {0}" -f $expectedKey) -Ok $false -Detail 'missing'
            continue
        }
        $configValues[$expectedKey] = $entry.Value
        $configOrder += $expectedKey
        $number = 0
        Add-Check -Name ("config value numeric: {0}" -f $expectedKey) -Ok ([int]::TryParse($entry.Value, [ref]$number)) -Detail $entry.Value
        Add-Check -Name ("config value not a device name: {0}" -f $expectedKey) -Ok ($entry.Value -notmatch '[A-Za-z]') -Detail $entry.Value
    }
    $unexpectedKeys = @($entries | ForEach-Object { $_.Key } | Where-Object { $expectedKeys -notcontains $_ })
    Add-Check -Name 'config.ini has no extra fields' -Ok ($unexpectedKeys.Count -eq 0) -Detail ($(if ($unexpectedKeys.Count) { $unexpectedKeys -join ', ' } else { 'none' }))

    if ($configValues.ContainsKey('Buffer Size') -and [int]$configValues['Buffer Size'] -lt 1) { Add-Check -Name 'config buffer range' -Ok $false -Detail $configValues['Buffer Size'] }
    if ($configValues.ContainsKey('ASIO Driver ID') -and [int]$configValues['ASIO Driver ID'] -lt 0) { Add-Check -Name 'config driver range' -Ok $false -Detail $configValues['ASIO Driver ID'] }
    if ($configValues.ContainsKey('Sample Rate') -and [int]$configValues['Sample Rate'] -lt 8000) { Add-Check -Name 'config sample rate range' -Ok $false -Detail $configValues['Sample Rate'] }
    if ($configValues.ContainsKey('Output Volume')) {
        $volume = [int]$configValues['Output Volume']
        Add-Check -Name 'config volume range 0..100' -Ok ($volume -ge 0 -and $volume -le 100) -Detail $volume
    }
    if ($configValues.ContainsKey('Debug Log')) {
        Add-Check -Name 'config debug flag 0/1' -Ok (@(0, 1) -contains [int]$configValues['Debug Log']) -Detail $configValues['Debug Log']
    }
    if ($configValues.ContainsKey('Contract Mode')) {
        Add-Check -Name 'config contract mode 0/1' -Ok (@(0, 1) -contains [int]$configValues['Contract Mode']) -Detail $configValues['Contract Mode']
        Add-Check -Name 'clean package defaults to Adaptive' -Ok ([int]$configValues['Contract Mode'] -eq 1) -Detail $configValues['Contract Mode']
    }
    if ($configValues.ContainsKey('Debug Log') -and [int]$configValues['Debug Log'] -ne 0) {
        Add-Warn -Name 'config default Debug Log is 0' -Detail ("Debug Log={0}" -f $configValues['Debug Log'])
    }
    if ($configValues.ContainsKey('ASIO Driver ID') -and [int]$configValues['ASIO Driver ID'] -ne 0) {
        Add-Warn -Name 'config default ASIO Driver ID is 0' -Detail ("ASIO Driver ID={0}" -f $configValues['ASIO Driver ID'])
    }
}

# ---------------------------------------------------------------------------
# README: Chinese, matching client version, no client bundled, portable launch
# ---------------------------------------------------------------------------
$readmePath = $required['Chinese README']
if ($readmePath) {
    try {
        $readme = Read-StrictUtf8 -Path $readmePath
        $readmeText = $readme.Text
        Add-Check -Name 'README valid UTF-8' -Ok $true -Detail $(if ($readme.HasUtf8Bom) { 'UTF-8 BOM' } else { 'UTF-8 no BOM' })
        Add-Check -Name 'README contains Chinese text' -Ok ($readmeText -match '[\u4E00-\u9FFF]') -Detail 'CJK present'
        Add-Check -Name 'README names supported client 2026.1026.0-tachyon' -Ok ($readmeText -match '2026\.1026\.0-tachyon') -Detail 'version string'
        Add-Check -Name 'README mentions client isolation directory' -Ok ($readmeText -match '(?i)client') -Detail 'client'
        Add-Check -Name 'README mentions config' -Ok ($readmeText -match '(?i)config\.(exe|ini)') -Detail 'config.exe/config.ini'
        if ($guiLayout) { Add-Check -Name 'README mentions launcher' -Ok ($readmeText.Contains('LazerASIO.exe')) -Detail 'LazerASIO.exe' }
        else { Add-Check -Name 'README mentions launcher' -Ok ($readmeText -match '(?i)Start-(ASIO|Portable)') -Detail 'Start-ASIO/Start-Portable' }
        Add-Check -Name 'README says client is not included' -Ok ($readmeText -match '(?:\u4E0D\u5305\u542B|\u4E0D\u542B|\u4E0D\u63D0\u4F9B)[^\r\n]{0,24}(?:\u5BA2\u6237\u7AEF|\u6E38\u620F)') -Detail 'no-client statement'
        Add-Check -Name 'README mentions offline/account safety' -Ok ($readmeText -match '(?:\u79BB\u7EBF|\u8D26\u53F7)') -Detail 'offline/account'
    }
    catch {
        Add-Check -Name 'README valid UTF-8' -Ok $false -Detail $_.Exception.Message
    }
}

# ---------------------------------------------------------------------------
# No bundled client / user data
# ---------------------------------------------------------------------------
$badDirectoryNames = @('client', 'logs', 'replays', 'songs', 'skins', 'exports', 'cache', 'files')
$badDirectories = @($allDirectories | Where-Object { $badDirectoryNames -contains $_.Name.ToLowerInvariant() })
Add-Check -Name 'no client or user-data directories' -Ok ($badDirectories.Count -eq 0) -Detail ($(if ($badDirectories.Count) { ($badDirectories | ForEach-Object { Get-RelativePath -Root $package -FullPath $_.FullName }) -join ', ' } else { 'none' }))

$badFileNames = @('osu!.exe', 'osu!.dll', 'osu!.runtimeconfig.json', 'osu.Game.dll', 'osu.Framework.dll', 'osu.Desktop.dll', 'storage.ini', 'framework.ini', '.asio-offline-copy', '.asio-device-configured', 'config.ini.tmp', 'language.ini', 'language.ini.tmp', 'playback-trace.enabled')
$badExtensions = @('.log', '.db', '.realm', '.realm-wal', '.token', '.osz', '.osu', '.cache', '.cfg')
$badFiles = New-Object System.Collections.ArrayList
foreach ($file in $allFiles) {
    $nameLower = $file.Name.ToLowerInvariant()
    $relative = Get-RelativePath -Root $package -FullPath $file.FullName
    if ($badFileNames -contains $nameLower) { $null = $badFiles.Add($relative); continue }
    if ($badExtensions -contains $file.Extension.ToLowerInvariant()) { $null = $badFiles.Add($relative); continue }
    if ($nameLower -match '\.db-') { $null = $badFiles.Add($relative); continue }
    if ($nameLower -eq 'osu!.deps.json' -and $relative -notmatch '^managed\\') { $null = $badFiles.Add($relative); continue }
    if ($file.Extension -ieq '.ini' -and $nameLower -ne 'config.ini') { $null = $badFiles.Add($relative); continue }
    if ($file.Extension -ieq '.pdb') { $null = $badFiles.Add($relative); continue }
}
Add-Check -Name 'no user logs/databases/account/client artifacts' -Ok ($badFiles.Count -eq 0) -Detail ($(if ($badFiles.Count) { (@($badFiles) -join ', ') } else { 'none' }))

$totalBytes = ($allFiles | Measure-Object -Property Length -Sum).Sum
Add-Check -Name 'package does not contain a full client (size under 100 MB)' -Ok ($totalBytes -lt 100MB) -Detail ("{0:N1} MB" -f ($totalBytes / 1MB))

# ---------------------------------------------------------------------------
# Text encoding, PowerShell AST syntax, CMD static syntax
# ---------------------------------------------------------------------------
$textExtensions = @('.ps1', '.cmd', '.bat', '.ini', '.md', '.json', '.txt', '.xml', '.config', '.lang')
$textFiles = @($allFiles | Where-Object { $textExtensions -contains $_.Extension.ToLowerInvariant() })
$textByPath = @{}
$encodingFailures = New-Object System.Collections.ArrayList

foreach ($file in $textFiles) {
    $relative = Get-RelativePath -Root $package -FullPath $file.FullName
    $extension = $file.Extension.ToLowerInvariant()
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    $hasUtf16 = $bytes.Length -ge 2 -and (($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF))
    $hasUtf8Bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    try {
        $decoded = Read-StrictUtf8 -Path $file.FullName
        $text = $decoded.Text
    }
    catch {
        $null = $encodingFailures.Add(("{0}: invalid UTF-8 ({1})" -f $relative, $_.Exception.Message))
        continue
    }
    if ($text.IndexOf([char]0) -ge 0) {
        $null = $encodingFailures.Add(("{0}: NUL byte in text file" -f $relative))
        continue
    }
    $textByPath[$file.FullName] = $text

    if ($extension -eq '.ps1') {
        if ($hasUtf16) { $null = $encodingFailures.Add(("{0}: UTF-16 PowerShell is not accepted" -f $relative)) }
        if (-not $hasUtf8Bom -and $text -match '[^\x00-\x7F]') {
            $null = $encodingFailures.Add(("{0}: non-ASCII PowerShell without UTF-8 BOM (Windows PowerShell 5.1 would misread it)" -f $relative))
        }
    }
    elseif ($extension -eq '.cmd' -or $extension -eq '.bat') {
        if ($hasUtf8Bom -or $hasUtf16) { $null = $encodingFailures.Add(("{0}: cmd.exe must not receive a BOM" -f $relative)) }
        if ($text -match '[^\x00-\x7F]' -and $text -notmatch '(?im)^\s*chcp\s+65001\b') {
            $null = $encodingFailures.Add(("{0}: non-ASCII CMD without 'chcp 65001'" -f $relative))
        }
        if (($text -replace "`r`n", '') -match "`n") {
            $null = $encodingFailures.Add(("{0}: bare LF line ending in CMD" -f $relative))
        }
    }
    elseif ($extension -eq '.ini') {
        if ($hasUtf8Bom -or $hasUtf16 -or $text -match '[^\x00-\x7F]') {
            $null = $encodingFailures.Add(("{0}: INI should be ASCII/no BOM" -f $relative))
        }
    }
}
Add-Check -Name 'text encoding checks' -Ok ($encodingFailures.Count -eq 0) -Detail ($(if ($encodingFailures.Count) { $encodingFailures -join '; ' } else { 'UTF-8/ASCII policy satisfied' }))

$psFiles = @($allFiles | Where-Object { $_.Extension -ieq '.ps1' })
$psSyntaxFailures = New-Object System.Collections.ArrayList
foreach ($file in $psFiles) {
    $tokens = $null
    $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    if ($errors -and $errors.Count -gt 0) {
        $messages = @($errors | ForEach-Object { $_.Message })
        $null = $psSyntaxFailures.Add(("{0}: {1}" -f (Get-RelativePath -Root $package -FullPath $file.FullName), ($messages -join '; ')))
    }
}
Add-Check -Name 'PowerShell AST syntax (all .ps1)' -Ok ($psSyntaxFailures.Count -eq 0) -Detail ($(if ($psSyntaxFailures.Count) { $psSyntaxFailures -join '; ' } else { ("{0} files parsed" -f $psFiles.Count) }))

$invokeExpressionFiles = @()
foreach ($file in $psFiles) {
    $commands = @(Get-CommandNames -Path $file.FullName)
    if (@($commands | Where-Object { $_ -ieq 'Invoke-Expression' }).Count -gt 0) {
        $invokeExpressionFiles += (Get-RelativePath -Root $package -FullPath $file.FullName)
    }
}
Add-Check -Name 'PowerShell AST has no Invoke-Expression' -Ok ($invokeExpressionFiles.Count -eq 0) -Detail ($(if ($invokeExpressionFiles.Count) { $invokeExpressionFiles -join ', ' } else { 'none' }))

$startCmdPath = $required['Start-ASIO.cmd']
if ($startCmdPath -and $textByPath.ContainsKey($startCmdPath)) {
    $cmdText = $textByPath[$startCmdPath]
    Add-Check -Name 'Start-ASIO.cmd starts with @echo off' -Ok ($cmdText -match '^\s*@echo off') -Detail 'first line'
    Add-Check -Name 'Start-ASIO.cmd anchors to its own directory' -Ok ($cmdText -match '%~dp0') -Detail '%~dp0'
    if ($newEntryLayout) {
        Add-Check -Name 'Start-ASIO.cmd uses the public EXE' -Ok ($cmdText.Contains('osu!asio_sound.exe')) -Detail 'public launcher'
    } else {
        Add-Check -Name 'Start-ASIO.cmd invokes managed Start-Portable.ps1' -Ok ($cmdText -match '(?i)Start-Portable\.ps1') -Detail 'Start-Portable.ps1'
        Add-Check -Name 'Start-ASIO.cmd uses PowerShell' -Ok ($cmdText -match '(?i)(powershell|pwsh)(\.exe)?') -Detail 'powershell'
    }
    Add-Check -Name 'Start-ASIO.cmd has no developer D:/F: path' -Ok (@(Get-DeveloperPathHits -Text $cmdText).Count -eq 0) -Detail 'no D:\osu-asio-fix or F:\repair'
}

# ---------------------------------------------------------------------------
# Launcher and Prepare static contracts
# ---------------------------------------------------------------------------
$portablePath = $required['managed Start-Portable']
if ($portablePath -and $textByPath.ContainsKey($portablePath)) {
    $portableText = $textByPath[$portablePath]
    Add-Check -Name 'Start-Portable uses $PSScriptRoot' -Ok ($portableText -match '\$PSScriptRoot') -Detail 'portable root'
    Add-Check -Name 'Start-Portable references Prepare-OfflineClient.ps1' -Ok ($portableText -match '(?i)Prepare-OfflineClient\.ps1') -Detail 'Prepare-OfflineClient.ps1'
    Add-Check -Name 'Start-Portable references Start-Offline.ps1' -Ok ($portableText -match '(?i)Start-Offline\.ps1') -Detail 'Start-Offline.ps1'
    Add-Check -Name 'Start-Portable creates/uses package-root client directory' -Ok ($portableText -match '(?i)client') -Detail 'client'
    if ($newEntryLayout) {
        Add-Check -Name 'Start-Portable uses program inventory and source update key' -Ok ($portableText.Contains('Get-AsioClientPrograms') -and $portableText.Contains('source_key')) -Detail 'program inventory is not a fixed-version gate'
        Add-Check -Name 'Start-Portable supports updating isolated programs' -Ok ($portableText.Contains('-UpdateExisting')) -Detail 'preserve existing isolated data'
    } else {
        Add-Check -Name 'Start-Portable uses client-manifest.json' -Ok ($portableText -match '(?i)client-manifest\.json') -Detail 'manifest'
        $portableFunctions = @(Get-FunctionNames -Path $portablePath)
        Add-Check -Name 'Start-Portable AST defines build assertion function' -Ok ($portableFunctions -contains 'Assert-PortableBuild') -Detail ($(if ($portableFunctions.Count) { $portableFunctions -join ', ' } else { 'none' }))
    }
    Add-Check -Name 'Start-Portable has no developer D:/F: path' -Ok (@(Get-DeveloperPathHits -Text $portableText).Count -eq 0) -Detail 'no D:\osu-asio-fix or F:\repair'
}

$preparePath = $required['managed Prepare-OfflineClient']
if ($preparePath -and $textByPath.ContainsKey($preparePath)) {
    $prepareText = $textByPath[$preparePath]
    if ($newEntryLayout) {
        $inventoryText=(Read-StrictUtf8 -Path $required['managed program inventory']).Text
        Add-Check -Name 'program inventory limits the runtime family' -Ok ($inventoryText.Contains('net10.0')) -Detail 'runtime needs review when changed'
        Add-Check -Name 'program inventory is a positive program-only selection' -Ok ($inventoryText.Contains("'.dll'") -and $inventoryText.Contains('*.resources.dll') -and $inventoryText.Contains('osu!.deps.json')) -Detail 'no arbitrary account/config/database copy'
        Add-Check -Name 'Prepare accepts only marked existing copies' -Ok ($prepareText.Contains('$UpdateExisting') -and $prepareText.Contains('.asio-offline-copy')) -Detail 'unknown existing destinations rejected'
        Add-Check -Name 'Prepare blocks overlapping source/destination and links' -Ok ($prepareText.Contains('$sourcePath.StartsWith') -and $prepareText.Contains('ReparsePoint')) -Detail 'no redirected writes'
        Add-Check -Name 'bridge embeds an audio implementation contract' -Ok ([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($required['managed bridge'])).Contains('Asio.AudioContract.json')) -Detail 'actual positive/negative contract behavior tested separately'
    } else {
        Add-Check -Name 'Prepare checks 2026.1026.0-tachyon' -Ok ($prepareText -match '2026\\?\.1026\\?\.0-tachyon') -Detail 'supported client version'
        Add-Check -Name 'Prepare checks framework 2026.1019.0' -Ok ($prepareText -match '2026\\?\.1019\\?\.0') -Detail 'framework version'
        Add-Check -Name 'Prepare uses robocopy' -Ok ($prepareText -match '(?i)robocopy') -Detail 'robocopy'
        Add-Check -Name 'Prepare excludes private client data' -Ok (($prepareText -match '\*\.ini') -and ($prepareText -match '\*\.db') -and ($prepareText -match '\*\.realm') -and ($prepareText -match '\*\.token')) -Detail '*.ini/*.db/*.realm/*.token'
        Add-Check -Name 'Prepare requires destination to be absent' -Ok (($prepareText -match '(?i)Test-Path[^\r\n]*\$destination') -and ($prepareText -match '(?i)throw')) -Detail 'Test-Path + throw'
    }
    Add-Check -Name 'Prepare writes isolation markers in the copy' -Ok (($prepareText -match '\.asio-offline-copy') -and ($prepareText -match 'framework\.ini') -and ($prepareText -match 'storage\.ini')) -Detail 'markers'
    Add-Check -Name 'Prepare does not launch a process' -Ok ($prepareText -notmatch '(?i)Start-Process') -Detail 'no Start-Process'
    Add-Check -Name 'Prepare does not write to selected source client' -Ok ($prepareText -notmatch '(?is)(Set-Content|Out-File|WriteAllText|WriteAllBytes)[^\r\n]*\$source') -Detail 'no source writes'
    Add-Check -Name 'Prepare supports hash-checked application manifest' -Ok (($prepareText -match '(?i)ApplicationManifest') -and ($prepareText -match '(?i)Get-FileHash') -and ($prepareText -match '(?i)(copyPlan|\$plan)')) -Detail 'manifest hash plan'
    $depsIndex = if ($newEntryLayout) { $prepareText.IndexOf('Get-AsioClientPrograms') } else { $prepareText.IndexOf('osu!.deps.json') }
    $newItemIndex = $prepareText.IndexOf('New-Item')
    Add-Check -Name 'Prepare validates version before creating destination' -Ok ($depsIndex -ge 0 -and $newItemIndex -ge 0 -and $depsIndex -lt $newItemIndex) -Detail ("deps={0} new-item={1}" -f $depsIndex, $newItemIndex)
    $hashIndex = $prepareText.IndexOf('Get-FileHash')
    Add-Check -Name 'Prepare verifies copied file hashes before creating destination' -Ok ($hashIndex -ge 0 -and $newItemIndex -ge 0 -and $hashIndex -lt $newItemIndex) -Detail ("hash={0} new-item={1}" -f $hashIndex, $newItemIndex)
    $copyIndex = $prepareText.IndexOf($(if($newEntryLayout){'Copy-Item'}else{'robocopy'}))
    $runtimeIndex = $prepareText.IndexOf('runtimeconfig')
    Add-Check -Name 'Prepare changes runtimeconfig only in the copy' -Ok ($copyIndex -ge 0 -and $runtimeIndex -ge 0 -and $copyIndex -lt $runtimeIndex) -Detail ("robocopy={0} runtimeconfig={1}" -f $copyIndex, $runtimeIndex)
}

$offlinePath = $required['managed Start-Offline']
if ($offlinePath -and $textByPath.ContainsKey($offlinePath)) {
    $offlineText = $textByPath[$offlinePath]
    Add-Check -Name 'Start-Offline uses $PSScriptRoot' -Ok ($offlineText -match '\$PSScriptRoot') -Detail 'portable root'
    Add-Check -Name 'Start-Offline enforces isolated copy markers' -Ok (($offlineText -match '\.asio-offline-copy') -and ($offlineText -match 'framework\.ini') -and ($offlineText -match 'storage\.ini')) -Detail 'markers'
}

# ---------------------------------------------------------------------------
# No hard-coded developer paths in text files
# ---------------------------------------------------------------------------
$developerPathHits = New-Object System.Collections.ArrayList
if ($standaloneLayout) {
    $watcherPath=$required['managed standalone watcher']
    $watcherText=$textByPath[$watcherPath]
    $watcherCommands=@(Get-CommandNames -Path $watcherPath)
    $tokens=$null; $errors=$null
    $tree=[Management.Automation.Language.Parser]::ParseFile($watcherPath,[ref]$tokens,[ref]$errors)
    $launches=@($tree.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Start-Process'},$true))
    Add-Check -Name 'standalone watcher detects processes' -Ok ($watcherCommands -contains 'Get-Process') -Detail 'process enumeration'
    Add-Check -Name 'standalone watcher attaches by explicit PID' -Ok ($watcherText.Contains('--live') -and $watcherText.Contains('--attach-pid')) -Detail 'runtime attachment'
    Add-Check -Name 'standalone watcher only launches internal audio backend' -Ok ($launches.Count -eq 1 -and $launches[0].Extent.Text.Contains("'asio_backend.exe'")) -Detail 'single backend launch command'
    Add-Check -Name 'standalone watcher has no copy or client preparation commands' -Ok (@($watcherCommands | Where-Object {$_ -in @('Copy-Item','Move-Item','Remove-Item','robocopy')}).Count -eq 0) -Detail 'no client copy'
    Add-Check -Name 'standalone watcher does not configure StartupHook launch' -Ok (!$watcherText.Contains('DOTNET_STARTUP_HOOKS') -and !$watcherText.Contains('runtimeconfig')) -Detail 'live host API'
    Add-Check -Name 'standalone watcher continues after game exit' -Ok ($watcherText.Contains('$seen') -and $watcherText.Contains('while (') -and $watcherText.Contains('$backend.Dispose()')) -Detail 'watch loop and owned backend cleanup'
    Add-Check -Name 'standalone package excludes client-copy helper scripts' -Ok (!(Test-Path -LiteralPath (Join-Path $package 'managed\Start-Portable.ps1')) -and !(Test-Path -LiteralPath (Join-Path $package 'managed\Prepare-OfflineClient.ps1'))) -Detail 'independent package'
    $bundle=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $package 'MANIFEST.json') | ConvertFrom-Json
    Add-Check -Name 'package release version is declared' -Ok ($bundle.version -match '^\d+\.\d+\.\d+') -Detail $bundle.version
    if($bundle.binary_source_commit){Add-Check -Name 'manifest records prior tested binary source' -Ok ($bundle.binary_source_commit -match '^[0-9a-f]{40}$') -Detail $bundle.binary_source_commit}
    Add-Check -Name 'standalone manifest declares no game launch or copy' -Ok ($bundle.starts_game -eq $false -and $bundle.copies_client -eq $false -and $bundle.client_included -eq $false) -Detail 'starts_game=false copies_client=false client_included=false'
    $badHashes=@(foreach($entry in $bundle.files) {
        $target=[IO.Path]::GetFullPath((Join-Path $package $entry.path))
        if(!$target.StartsWith($package.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $target -PathType Leaf) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry.sha256) { $entry.path }
    })
    Add-Check -Name 'standalone manifest hashes match all delivered files' -Ok ($badHashes.Count -eq 0 -and $bundle.files.Count -eq (@($allFiles | Where-Object {$_.Name -notin @('MANIFEST.json','SHA256SUMS.txt')})).Count) -Detail ('mismatches='+($badHashes -join ','))
}
foreach ($file in $textFiles) {
    if (-not $textByPath.ContainsKey($file.FullName)) { continue }
    $text = $textByPath[$file.FullName]
    $hits = @(Get-DeveloperPathHits -Text $text)
    if ($hits.Count -gt 0) {
        $relative = Get-RelativePath -Root $package -FullPath $file.FullName
        $null = $developerPathHits.Add(("{0}: {1}" -f $relative, ($hits -join ', ')))
    }
}
Add-Check -Name 'no D:\osu-asio-fix or F:\repair in package text files' -Ok ($developerPathHits.Count -eq 0) -Detail ($(if ($developerPathHits.Count) { $developerPathHits -join '; ' } else { 'none' }))

$binaryPathHits = New-Object System.Collections.ArrayList
foreach ($file in @($allFiles | Where-Object { $_.Extension -ieq '.exe' -or $_.Extension -ieq '.dll' })) {
    $hits = @(Get-BinaryDeveloperPathHits -Path $file.FullName)
    if ($hits.Count -gt 0) {
        $null = $binaryPathHits.Add(("{0}: {1}" -f (Get-RelativePath -Root $package -FullPath $file.FullName), ($hits -join ', ')))
    }
}
if ($binaryPathHits.Count -gt 0) {
    Add-Warn -Name 'binary strings contain developer paths (diagnostic only)' -Detail ($binaryPathHits -join '; ')
}
else {
    Add-Check -Name 'binary strings contain no developer D:/F: paths' -Ok $true -Detail 'none'
}

# ---------------------------------------------------------------------------
# Native PE dependencies: no external VC runtime
# ---------------------------------------------------------------------------
$nativeFiles = New-Object System.Collections.ArrayList
foreach ($label in @('window entry', 'config.exe', 'player executable', 'audio backend', 'hook DLL', 'fmod.dll', 'device checker')) {
    $path = $required[$label]
    if ($path -and -not $nativeFiles.Contains($path)) { $null = $nativeFiles.Add($path) }
}
if ($dumpbin) {
    foreach ($native in $nativeFiles) {
        $relative = Get-RelativePath -Root $package -FullPath $native
        $dumpOutput = @(& $dumpbin /nologo /dependents $native 2>&1 | ForEach-Object { $_.ToString() })
        $dumpExit = $LASTEXITCODE
        $dumpLog = Join-Path $relocationBase ('dumpbin-' + ($relative -replace '[\\/:*?"<>| ]', '_') + '.log')
        [IO.File]::WriteAllLines($dumpLog, $dumpOutput, [Text.UTF8Encoding]::new($false))
        Add-Check -Name ("dumpbin /dependents: {0}" -f $relative) -Ok ($dumpExit -eq 0) -Detail ("exit={0} log={1}" -f $dumpExit, $dumpLog)
        if ($dumpExit -ne 0) { continue }
        $dependencies = @()
        $inDependencies = $false
        foreach ($line in $dumpOutput) {
            if ($line -match 'Image has the following dependencies:') { $inDependencies = $true; continue }
            if ($inDependencies) {
                if ($line -match '^\s+(\S+\.dll)\s*$') { $dependencies += $Matches[1] }
                elseif ($line -match '^\s*Summary') { break }
            }
        }
        $runtimeDependencies = @($dependencies | Where-Object { $_ -match '(?i)^(VCRUNTIME|MSVCP|MSVCR|ucrtbase|api-ms-win-crt-)' })
        if ($relative -ieq 'fmod.dll') {
            if ($runtimeDependencies.Count -gt 0) {
                Add-Warn -Name 'fmod.dll external VC runtime' -Detail ($runtimeDependencies -join ', ')
            }
            else {
                Add-Check -Name 'fmod.dll has no external VC runtime' -Ok $true -Detail 'none'
            }
        }
        else {
            Add-Check -Name ("no external VC runtime: {0}" -f $relative) -Ok ($runtimeDependencies.Count -eq 0) -Detail ($(if ($runtimeDependencies.Count) { $runtimeDependencies -join ', ' } else { 'static /MT dependency set' }))
        }

        $headers = @(& $dumpbin /nologo /headers $native 2>&1 | ForEach-Object { $_.ToString() })
        $machine = 'unknown'
        foreach ($line in $headers) {
            if ($line -match 'machine \(([^)]+)\)') { $machine = $Matches[1]; break }
        }
        Add-Check -Name ("PE machine x64: {0}" -f $relative) -Ok ($machine -ieq 'x64') -Detail $machine
    }
}
else {
    Add-Check -Name 'PE dependency checks executed' -Ok $false -Detail 'dumpbin unavailable'
}

# ---------------------------------------------------------------------------
# Relocated-copy behavioral test
# ---------------------------------------------------------------------------
$missingInRelocation = @()
foreach ($label in @($required.Keys)) {
    $sourcePath = $required[$label]
    if (-not $sourcePath) {
        $missingInRelocation += ("missing-source:" + $label)
        continue
    }
    $relative = Get-RelativePath -Root $package -FullPath $sourcePath
    if (-not (Test-Path -LiteralPath (Join-Path $relocation $relative) -PathType Leaf)) {
        $missingInRelocation += $relative
    }
}
Add-Check -Name 'package copied to path containing spaces' -Ok ($relocation -match ' ') -Detail $relocation
Add-Check -Name 'all required files present after relocation' -Ok ($missingInRelocation.Count -eq 0) -Detail ($(if ($missingInRelocation.Count) { $missingInRelocation -join ', ' } else { 'all present' }))

$relocatedConfig = Join-Path $relocation 'config.ini'
$relocatedConfigExe = Join-Path $relocation $(if ($guiLayout) { 'LazerASIO.exe' } else { 'config.exe' })
$sourceManifest = Get-TreeManifest -Root $package
$relocatedManifestBefore = Get-TreeManifest -Root $relocation
$copyDiff = @(Compare-TreeManifest -Before $sourceManifest -After $relocatedManifestBefore)
Add-Check -Name 'relocated package hashes match source package' -Ok ($copyDiff.Count -eq 0) -Detail ($(if ($copyDiff.Count) { $copyDiff -join ', ' } else { 'all copied file hashes identical' }))
$packageConfigHashBefore = if ($configPath) { Get-Sha256IfExists -Path $configPath } else { '' }
$relocatedConfigHashBefore = Get-Sha256IfExists -Path $relocatedConfig

$decoyCwd = Join-Path $relocationBase 'different cwd'
$emptyCwd = Join-Path $relocationBase 'different cwd 2'
foreach ($directory in @($decoyCwd, $emptyCwd)) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$decoyConfigPath = Join-Path $decoyCwd 'config.ini'
$decoyTemplate = [ordered]@{
    'Buffer Size' = 731
    'ASIO Driver ID' = 9
    'Sample Rate' = 44101
    'Output Volume' = 37
    'Debug Log' = 1
}
$decoyMap = @{}
foreach ($key in @($decoyTemplate.Keys)) {
    $value = $decoyTemplate[$key]
    if ($configValues.ContainsKey($key)) {
        $original = [string]$configValues[$key]
        if ($key -eq 'Debug Log') {
            $value = if ($original -eq '1') { 0 } else { 1 }
        }
        else {
            while ([string]$value -eq $original) { $value++ }
        }
    }
    $decoyMap[$key] = [string]$value
}
$decoyLines = @('[Config]') + @($decoyMap.Keys | ForEach-Object { '{0} = {1}' -f $_, $decoyMap[$_] })
$decoyText = ($decoyLines -join "`r`n") + "`r`n"
[IO.File]::WriteAllText($decoyConfigPath, $decoyText, [Text.UTF8Encoding]::new($false))
$decoyHashBefore = Get-Sha256 -Path $decoyConfigPath
$decoyManifestBefore = Get-TreeManifest -Root $decoyCwd
$emptyManifestBefore = Get-TreeManifest -Root $emptyCwd

$configExeBytes = if (Test-Path -LiteralPath $relocatedConfigExe) { [IO.File]::ReadAllBytes($relocatedConfigExe) } else { @() }
$configExeAscii = [Text.Encoding]::ASCII.GetString($configExeBytes)
$configExeUtf8 = [Text.Encoding]::UTF8.GetString($configExeBytes)
$hasShowFlag = $configExeAscii.Contains('--show')
$hasListFlag = $configExeAscii.Contains('--list')
Add-Check -Name 'LazerASIO.exe advertises --show' -Ok $hasShowFlag -Detail 'binary flag string'
Add-Check -Name 'LazerASIO.exe advertises --list' -Ok $hasListFlag -Detail 'binary flag string'
Add-Check -Name 'LazerASIO.exe carries stereo L/R output instruction' -Ok ($configExeUtf8 -match '\u7ACB\u4F53\u58F0L/R' -or ($multilingualLayout -and ($configExeUtf8.Contains('Output is stereo L/R') -or ($guiLayout -and $configExeUtf8.Contains('(stereo L/R)'))))) -Detail 'stereo L/R marker'

$relocatedPlayer = Join-Path $relocation $(if($newEntryLayout){'asio_backend.exe'}else{'osu!asio_sound.exe'})
if (Test-Path -LiteralPath $relocatedPlayer) {
    $playerUtf8 = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($relocatedPlayer))
    Add-Check -Name 'player exe carries stereo 2-channel marker' -Ok ($playerUtf8.Contains('stereo L/R, 2 software channels') -or ($multilingualLayout -and $playerUtf8.Contains('stereo L/R; software buffer='))) -Detail '2 software channels marker'
}
else {
    Add-Check -Name 'player exe carries stereo 2-channel marker' -Ok $false -Detail 'player exe missing'
}

$showResult = $null
$listResult = $null
$secondShowResult = $null
if ($hasShowFlag -and $hasListFlag -and (Test-Path -LiteralPath $relocatedConfigExe)) {
    $showStdout = Join-Path $relocationBase 'config-show.stdout.log'
    $showStderr = Join-Path $relocationBase 'config-show.stderr.log'
    $showResult = Invoke-ConfigProcess -Executable $relocatedConfigExe -Arguments '--show' -WorkingDirectory $decoyCwd -StdoutPath $showStdout -StderrPath $showStderr
    Add-Check -Name 'LazerASIO.exe --show exits 0' -Ok ($null -ne $showResult -and $showResult.ExitCode -eq 0 -and -not $showResult.TimedOut) -Detail ("exit={0} timeout={1} duration_ms={2}" -f $showResult.ExitCode, $showResult.TimedOut, $showResult.DurationMilliseconds)
    if ($null -ne $showResult) {
        Add-Check -Name 'LazerASIO.exe --show did not time out' -Ok (-not $showResult.TimedOut) -Detail ("{0} ms" -f $showResult.DurationMilliseconds)
        Add-Check -Name 'LazerASIO.exe --show prints supported config fields' -Ok ($showResult.Stdout -match '(?m)^\[Config\]') -Detail '[Config]'
        if ($configValues.Count -eq $expectedKeys.Count) {
            $showValueMissing = @(Test-ConfigValueLines -Text $showResult.Stdout -Values $configValues)
            Add-Check -Name 'LazerASIO.exe --show reads exe-side config values' -Ok ($showValueMissing.Count -eq 0) -Detail ($(if ($showValueMissing.Count) { 'missing: ' + ($showValueMissing -join ', ') } else { 'all supported values match package config.ini' }))
        }
        else {
            Add-Check -Name 'LazerASIO.exe --show reads exe-side config values' -Ok $false -Detail 'package config.ini missing or incomplete; value check not possible'
        }
        $showExtraLines = @($showResult.Stdout -split "`r?`n" | Where-Object {
                $_ -and $_ -notmatch '^\[Config\]$' -and $_ -notmatch '^(Buffer Size|ASIO Driver ID|Sample Rate|Output Volume|Debug Log|Contract Mode)\s*='
            })
        Add-Check -Name 'LazerASIO.exe --show prints only the supported config fields' -Ok ($showExtraLines.Count -eq 0) -Detail ($(if ($showExtraLines.Count) { ($showExtraLines | Select-Object -First 4) -join ' | ' } else { 'no device name or extra output' }))
        $decoyHits = New-Object System.Collections.ArrayList
        foreach ($key in @($decoyMap.Keys)) {
            if ($configValues.ContainsKey($key) -and [string]$configValues[$key] -eq [string]$decoyMap[$key]) { continue }
            $pattern = '(?im)^\s*' + [regex]::Escape($key) + '\s*=\s*' + [regex]::Escape($decoyMap[$key]) + '\s*$'
            if ($showResult.Stdout -match $pattern) { $null = $decoyHits.Add(("{0}={1}" -f $key, $decoyMap[$key])) }
        }
        Add-Check -Name 'LazerASIO.exe --show does not read cwd decoy config' -Ok ($decoyHits.Count -eq 0) -Detail ($(if ($decoyHits.Count) { 'decoy values found: ' + ($decoyHits -join ', ') } else { 'package values used' }))
        $showForbidden = @('System_Init', 'FMOD_System_Init', 'FMOD System Initialize', '(?:\u6B63\u5728\u68C0\u67E5\u6240\u9009\u8BBE\u5907|\u914D\u7F6E\u5DF2\u4FDD\u5B58)')
        $showForbiddenHits = @($showForbidden | Where-Object { $showResult.Stdout -match $_ -or $showResult.Stderr -match $_ })
        Add-Check -Name 'LazerASIO.exe --show does not enumerate or open a device' -Ok ($showForbiddenHits.Count -eq 0 -and $showResult.Stdout -notmatch '(?i)DeviceID\s+\d+') -Detail ($(if ($showForbiddenHits.Count) { $showForbiddenHits -join ', ' } else { 'no init/enumeration markers' }))
        Add-Check -Name 'LazerASIO.exe --show does not write cwd' -Ok ((Get-Sha256 -Path $decoyConfigPath) -eq $decoyHashBefore -and @(Compare-TreeManifest -Before $decoyManifestBefore -After (Get-TreeManifest -Root $decoyCwd)).Count -eq 0) -Detail 'decoy cwd unchanged'
        Add-Check -Name 'LazerASIO.exe --show leaves package config.ini unchanged' -Ok ($relocatedConfigHashBefore -ne '' -and (Get-Sha256IfExists -Path $relocatedConfig) -eq $relocatedConfigHashBefore) -Detail 'relocated config.ini hash unchanged'

        $secondShowStdout = Join-Path $relocationBase 'config-show-empty-cwd.stdout.log'
        $secondShowStderr = Join-Path $relocationBase 'config-show-empty-cwd.stderr.log'
        $secondShowResult = Invoke-ConfigProcess -Executable $relocatedConfigExe -Arguments '--show' -WorkingDirectory $emptyCwd -StdoutPath $secondShowStdout -StderrPath $secondShowStderr
        Add-Check -Name 'LazerASIO.exe --show exits 0 from a second cwd' -Ok ($null -ne $secondShowResult -and $secondShowResult.ExitCode -eq 0 -and -not $secondShowResult.TimedOut) -Detail ("exit={0}" -f $secondShowResult.ExitCode)
        Add-Check -Name 'LazerASIO.exe --show output is cwd-independent' -Ok ($null -ne $secondShowResult -and $secondShowResult.Stdout -eq $showResult.Stdout) -Detail 'same output from decoy cwd and empty cwd'
        Add-Check -Name 'LazerASIO.exe --show does not write config.ini in empty cwd' -Ok (-not (Test-Path -LiteralPath (Join-Path $emptyCwd 'config.ini')) -and @(Compare-TreeManifest -Before $emptyManifestBefore -After (Get-TreeManifest -Root $emptyCwd)).Count -eq 0) -Detail 'empty cwd unchanged'

        $listStdout = Join-Path $relocationBase 'config-list.stdout.log'
        $listStderr = Join-Path $relocationBase 'config-list.stderr.log'
        $listResult = Invoke-ConfigProcess -Executable $relocatedConfigExe -Arguments '--list' -WorkingDirectory $emptyCwd -StdoutPath $listStdout -StderrPath $listStderr
        Add-Check -Name 'LazerASIO.exe --list exits 0' -Ok ($null -ne $listResult -and $listResult.ExitCode -eq 0 -and -not $listResult.TimedOut) -Detail ("exit={0} timeout={1} duration_ms={2}" -f $listResult.ExitCode, $listResult.TimedOut, $listResult.DurationMilliseconds)
        if ($null -ne $listResult) {
            $emptyMessages=@(Get-ChildItem -LiteralPath (Join-Path $relocation 'locales') -Filter '*.lang' -File | ForEach-Object {
                foreach($line in [IO.File]::ReadAllLines($_.FullName,[Text.Encoding]::UTF8)) {
                    if($line.StartsWith('config.no_devices=')){$line.Substring('config.no_devices='.Length)}
                }
            })
            $hasDevices=$listResult.Stdout -match '(?i)DeviceID\s+\d+'
            $reportsEmpty=@($emptyMessages|Where-Object {$listResult.Stdout.Contains($_)}).Count -gt 0
            Add-Check -Name 'LazerASIO.exe --list reports devices or successful empty enumeration' -Ok ($hasDevices -or $reportsEmpty) -Detail $(if($hasDevices){'DeviceID line present'}else{'no-device message present='+$reportsEmpty})
            $listForbidden = @('System_Init', 'FMOD_System_Init', 'FMOD System Initialize', '(?:\u6B63\u5728\u68C0\u67E5\u6240\u9009\u8BBE\u5907|\u914D\u7F6E\u5DF2\u4FDD\u5B58)')
            $listForbiddenHits = @($listForbidden | Where-Object { $listResult.Stdout -match $_ -or $listResult.Stderr -match $_ })
            Add-Check -Name 'LazerASIO.exe --list never initialises/opens device' -Ok ($listForbiddenHits.Count -eq 0) -Detail ($(if ($listForbiddenHits.Count) { $listForbiddenHits -join ', ' } else { 'no System_Init/device-check markers' }))
            Add-Check -Name 'LazerASIO.exe --list does not write config or flag' -Ok ($relocatedConfigHashBefore -ne '' -and (Get-Sha256IfExists -Path $relocatedConfig) -eq $relocatedConfigHashBefore -and -not (Test-Path -LiteralPath (Join-Path $emptyCwd 'config.ini')) -and -not (Test-Path -LiteralPath (Join-Path $emptyCwd '.asio-device-configured')) -and @(Compare-TreeManifest -Before $emptyManifestBefore -After (Get-TreeManifest -Root $emptyCwd)).Count -eq 0) -Detail 'empty cwd unchanged; config.ini unchanged'
        }
    }
}
else {
    Add-Check -Name 'LazerASIO.exe flag gate before execution' -Ok $false -Detail 'refusing to run an executable without --show/--list; interactive config could open a sound device'
}

$relocatedManifestAfter = Get-TreeManifest -Root $relocation
$relocationChanges = @(Compare-TreeManifest -Before $relocatedManifestBefore -After $relocatedManifestAfter)
Add-Check -Name 'config runs leave relocated package unchanged' -Ok ($relocationChanges.Count -eq 0) -Detail ($(if ($relocationChanges.Count) { $relocationChanges -join ', ' } else { 'no file created/modified/deleted' }))
if ($configPath) {
    Add-Check -Name 'config runs leave original package unchanged' -Ok ($packageConfigHashBefore -ne '' -and (Get-Sha256IfExists -Path $configPath) -eq $packageConfigHashBefore) -Detail 'original config.ini hash unchanged'
}

# ---------------------------------------------------------------------------
# Summary and JSON result
# ---------------------------------------------------------------------------
$summary = [ordered]@{
    task_id = 'L_SHARE_PACKAGE_QA'
    package_directory = $package
    relocation_directory = $relocation
    checks_total = $script:Checks.Count
    checks_passed = $script:PassCount
    checks_failed = $script:FailCount
    checks_warned = $script:WarnCount
    config_show_exit_code = if ($showResult) { $showResult.ExitCode } else { $null }
    config_list_exit_code = if ($listResult) { $listResult.ExitCode } else { $null }
    config_show_log = if ($showResult) { $showResult.StdoutPath } else { $null }
    config_show_error_log = if ($showResult) { $showResult.StderrPath } else { $null }
    config_list_log = if ($listResult) { $listResult.StdoutPath } else { $null }
    config_list_error_log = if ($listResult) { $listResult.StderrPath } else { $null }
    generated_at_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
}
$result = [ordered]@{
    summary = $summary
    checks = @($script:Checks)
}
$resultPath = Join-Path $relocationBase 'qa-result.json'
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding utf8
Write-Output ("RESULT_JSON={0}" -f $resultPath)
Write-Output ("SUMMARY pass={0} fail={1} warn={2}" -f $script:PassCount, $script:FailCount, $script:WarnCount)
if ($script:FailCount -gt 0) {
    exit 1
}
exit 0
