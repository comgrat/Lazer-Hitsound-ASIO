param(
    [string]$ProcessName = 'osu!',
    [switch]$SelectProcess,
    [switch]$Once,
    [int]$MaximumWaitSeconds = 0,
    [int]$TestTargetPid = 0
)
$ErrorActionPreference = 'Stop'
$bundleRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Localization.ps1')
Initialize-AsioLanguage $bundleRoot
function Read-AsioAttachStatus([hashtable]$State) {
    $State.Pending += $State.Reader.ReadToEnd()
    while (($newline=$State.Pending.IndexOf("`n")) -ge 0) {
        $line=$State.Pending.Substring(0,$newline)
        $State.Pending=$State.Pending.Substring($newline+1)
        if ($line -match '^\[ASIO_READY\]') { Write-Output $line.TrimEnd("`r"); $State.Ready=$true }
        elseif ($line -match '^\[(ASIO config|Verified Contract|Compat/Adaptive)\]') { Write-Output $line.TrimEnd("`r") }
    }
}
if ($SelectProcess) { $ProcessName = Read-Host (Get-AsioText 'watcher.process_prompt'); if (!$ProcessName) { $ProcessName='osu!' } }
$ProcessName = [IO.Path]::GetFileNameWithoutExtension($ProcessName)
if (!$ProcessName -or $ProcessName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) { throw (Get-AsioText 'watcher.invalid_process') }
$logs = Join-Path $bundleRoot 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$mutex = [Threading.Mutex]::new($false, 'Local\OsuAsioStandaloneWatcher')
$locked = $false
$backend = $null
$outputReader = $null
$previousOptIn = $env:ASIO_STANDALONE_ENABLE
$previousPlaybackTrace = $env:ASIO_PLAYBACK_TRACE
try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked=$true }
    if (!$locked) { throw (Get-AsioText 'watcher.already_running') }
    Write-Output (Get-AsioText 'watcher.waiting' @($ProcessName))
    Write-Output (Get-AsioText 'watcher.behavior')
    Write-Output (Get-AsioText 'watcher.config' @((Join-Path $bundleRoot 'config.ini')))
    if ((Test-Path -LiteralPath (Join-Path $bundleRoot 'playback-trace.enabled'))) { $env:ASIO_PLAYBACK_TRACE='1'; Write-Output (Get-AsioText 'watcher.trace') }
    if (!(Test-Path -LiteralPath (Join-Path $bundleRoot '.asio-device-configured'))) {
        Write-Output (Get-AsioText 'watcher.first_use')
        & (Join-Path $bundleRoot 'config.exe')
        if ($LASTEXITCODE -ne 0) { throw (Get-AsioText 'watcher.config_failed') }
        Initialize-AsioLanguage $bundleRoot
    }
    $seen = [Collections.Generic.HashSet[string]]::new()
    $deadline = if ($MaximumWaitSeconds -gt 0) { [DateTime]::UtcNow.AddSeconds($MaximumWaitSeconds) } else { [DateTime]::MaxValue }
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = $null
        foreach ($game in @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Sort-Object StartTime)) {
            try {
                if ($TestTargetPid -gt 0 -and $game.Id -ne $TestTargetPid) { continue }
                $path = $game.Path
                $identity = "$($game.Id):$($game.StartTime.ToUniversalTime().Ticks)"
                if ($seen.Contains($identity) -or !$path) { continue }
                $directory = Split-Path $path -Parent
                $valid = $true
                foreach ($file in @('osu!.dll','osu.Game.dll','osu.Framework.dll','coreclr.dll','hostfxr.dll')) {
                    if (!(Test-Path -LiteralPath (Join-Path $directory $file) -PathType Leaf)) { $valid=$false; break }
                }
                if (!$valid) { $null=$seen.Add($identity); continue }
                foreach ($assemblyName in @('osu.Game','osu.Framework')) {
                    if ([Reflection.AssemblyName]::GetAssemblyName((Join-Path $directory "$assemblyName.dll")).Name -ne $assemblyName) { $valid=$false; break }
                }
                if (!$valid) { $null=$seen.Add($identity); continue }
                $candidate=$game
                $null=$seen.Add($identity)
                break
            } catch { continue }
        }
        if (!$candidate) { Start-Sleep -Milliseconds 250; continue }
        $gameId=$candidate.Id
        Write-Output (Get-AsioText 'watcher.found' @($gameId,$candidate.Path))
        $stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $stdout=Join-Path $logs "attach-$gameId-$stamp.stdout.log"
        $stderr=Join-Path $logs "attach-$gameId-$stamp.stderr.log"
        $env:ASIO_STANDALONE_ENABLE='1'
        $backend=Start-Process -FilePath (Join-Path $bundleRoot 'asio_backend.exe') -ArgumentList @('--live','--attach-pid',"$gameId") -WorkingDirectory $bundleRoot -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null=$backend.Handle
        $outputStream=[IO.FileStream]::new($stdout,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $outputReader=[IO.StreamReader]::new($outputStream,[Text.UTF8Encoding]::new($false))
        $status=@{Reader=$outputReader;Pending='';Ready=$false}
        while (!$backend.HasExited) {
            Read-AsioAttachStatus $status
            Start-Sleep -Milliseconds 250
            $backend.Refresh()
        }
        $backend.WaitForExit()
        $result=$backend.ExitCode
        Read-AsioAttachStatus $status
        $announced=[bool]$status.Ready
        $outputReader.Dispose(); $outputReader=$null
        if ($result -ne 0 -or !$announced) {
            Write-Warning (Get-AsioText 'watcher.attach_failed' @($result,$stderr))
            if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Encoding UTF8 | Select-Object -Last 8 | Write-Output }
        } else { Write-Output (Get-AsioText 'watcher.released') }
        $backend.Dispose(); $backend=$null
        if ($Once) { if ($result -ne 0 -or !$announced) { exit 1 }; exit 0 }
        Write-Output (Get-AsioText 'watcher.continue')
    }
    if ($MaximumWaitSeconds -gt 0) { throw (Get-AsioText 'watcher.timeout') }
} finally {
    # Never terminate the user's game. Only release this invocation's backend.
    if ($backend -and !$backend.HasExited) { try { $backend.Kill(); $backend.WaitForExit() } catch {} }
    if ($backend) { $backend.Dispose() }
    if ($outputReader) { $outputReader.Dispose() }
    $env:ASIO_STANDALONE_ENABLE=$previousOptIn
    $env:ASIO_PLAYBACK_TRACE=$previousPlaybackTrace
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
