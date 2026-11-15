#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageDirectory,[string]$ReportDirectory='')
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackageDirectory).Path
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$reportRoot=if($ReportDirectory){[IO.Path]::GetFullPath($ReportDirectory)}else{Join-Path $repo 'tests\Distribution\artifacts\multilingual'}
New-Item -ItemType Directory -Path $reportRoot -Force | Out-Null
$run=Join-Path $reportRoot ('run-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null=New-Item -ItemType Directory -Path $run
$utf8=[Text.UTF8Encoding]::new($false)
$checks=[Collections.Generic.List[object]]::new()
function Check([bool]$Condition,[string]$Name) {
    $checks.Add([ordered]@{name=$Name;passed=$Condition})
    if(!$Condition){throw ('FAIL: '+$Name)}
}
function Invoke-TestProcess([string]$Exe,[string[]]$Arguments,[string]$Cwd,[string]$InputText='',[string]$Tag='process') {
    $p=[Diagnostics.ProcessStartInfo]::new()
    $p.FileName=$Exe
    foreach($argument in $Arguments){if($argument.Contains('"')){throw 'Unsupported embedded double quote in argument'}}
    $p.Arguments=(@($Arguments|ForEach-Object{'"'+$_+'"'})-join ' ')
    $p.WorkingDirectory=$Cwd;$p.UseShellExecute=$false;$p.CreateNoWindow=$true
    $p.RedirectStandardOutput=$true;$p.RedirectStandardError=$true;$p.RedirectStandardInput=$true
    $p.StandardOutputEncoding=$utf8;$p.StandardErrorEncoding=$utf8
    $process=[Diagnostics.Process]::new();$process.StartInfo=$p
    $null=$process.Start();$testProcessId=$process.Id
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if($InputText){$process.StandardInput.Write($InputText)};$process.StandardInput.Close()
    $finished=$process.WaitForExit(25000)
    if(!$finished){$process.Kill();$process.WaitForExit();throw ('Timed out own test process: '+$Tag)}
    $process.WaitForExit()
    $result=[ordered]@{exit=$process.ExitCode;pid=$testProcessId;stdout=$stdout.GetAwaiter().GetResult();stderr=$stderr.GetAwaiter().GetResult()}
    $process.Dispose()
    [IO.File]::WriteAllText((Join-Path $run ($Tag+'.stdout.log')),$result.stdout,$utf8)
    [IO.File]::WriteAllText((Join-Path $run ($Tag+'.stderr.log')),$result.stderr,$utf8)
    return $result
}
function Read-Catalog([string]$File) {
    $table=@{}
    foreach($line in [IO.File]::ReadAllLines($File,$utf8)) {if(!$line -or $line.StartsWith('#')){continue};$eq=$line.IndexOf('=');if($eq -gt 0){$table[$line.Substring(0,$eq)]=$line.Substring($eq+1)}}
    return $table
}
$sourceManifest=@(Get-ChildItem -LiteralPath $package -File -Recurse -Force|ForEach-Object{[ordered]@{path=$_.FullName;hash=(Get-FileHash -LiteralPath $_.FullName).Hash}})
$caseRoot=Join-Path $run 'renamed root 中文 with spaces'
Copy-Item -LiteralPath $package -Destination $caseRoot -Recurse
$decoy=Join-Path $run 'unrelated working directory';$null=New-Item -ItemType Directory -Path $decoy
[IO.File]::WriteAllText((Join-Path $decoy 'config.ini'),"[Config]`r`nBuffer Size = 256`r`nASIO Driver ID = 999`r`nSample Rate = 44100`r`nOutput Volume = 1`r`nDebug Log = 1`r`n",[Text.Encoding]::ASCII)
$audioConfig=Join-Path $caseRoot 'config.ini';$audioHash=(Get-FileHash -LiteralPath $audioConfig).Hash
$codes=@('zh-CN','zh-TW','en','fr','es')
try {
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot 'language.ini'))) 'clean package has no language preference'
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot '.asio-device-configured'))) 'clean package has no device marker'
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot 'logs'))) 'clean package has no logs'
    $initial=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--language') $decoy "4`r`n" 'first-language-menu'
    Check ($initial.exit -eq 0 -and $initial.stdout.Contains('Français') -and $initial.stdout.Contains('Español') -and $initial.stdout.Contains('繁體中文')) 'first language menu includes all languages and selects French'
    foreach($code in $codes) {
        $catalog=Read-Catalog (Join-Path $caseRoot ("locales\$code.lang"))
        $select=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--language',$code) $decoy '' ($code+'-select')
        Check ($select.exit -eq 0 -and $select.stdout.Contains($catalog['language.name'])) ($code+' saved language with native name')
        $preference=[IO.File]::ReadAllText((Join-Path $caseRoot 'language.ini'),$utf8)
        Check ($preference.Contains('Language = '+$code)) ($code+' saved code in separate language.ini')
        $current=Invoke-TestProcess (Join-Path $caseRoot 'osu!asio_sound.exe') @('--current-language') $decoy '' ($code+'-current')
        Check ($current.exit -eq 0 -and $current.stdout.Trim() -eq $code) ($code+' shared preference across executables')
        $help=Invoke-TestProcess (Join-Path $caseRoot 'osu!asio_sound.exe') @('--help') $decoy '' ($code+'-help')
        Check ($help.exit -eq 0 -and $help.stdout.Contains($catalog['launcher.usage'])) ($code+' localized launcher text')
        $show=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--show') $decoy '' ($code+'-config')
        Check ($show.exit -eq 0 -and $show.stdout.Contains('Buffer Size = 16') -and $show.stdout.Contains('Output Volume = 50') -and !$show.stdout.Contains('999')) ($code+' five-field config remains language/cwd independent')
        $list=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--list') $decoy '' ($code+'-list')
        Check ($list.exit -eq 0 -and $list.stdout.Contains($catalog['config.devices'])) ($code+' localized device enumeration without opening device')
        $backend=Invoke-TestProcess (Join-Path $caseRoot 'asio_backend.exe') @() $decoy '' ($code+'-backend-guard')
        Check ($backend.exit -eq 2 -and $backend.stderr.Contains($catalog['backend.internal'])) ($code+' localized backend guard before device open')
        $check=Invoke-TestProcess (Join-Path $caseRoot 'AsioDeviceCheck.exe') @('--list') $decoy '' ($code+'-device-list')
        Check ($check.exit -eq 0 -and $check.stdout.Contains($catalog['device.list'])) ($code+' optional device tool shares language')
        . (Join-Path $caseRoot 'managed\Localization.ps1')
        Initialize-AsioLanguage $caseRoot
        $rendered=Get-AsioText 'watcher.found' @(123,'F:\example {0}\osu!.exe')
        Check ($rendered.Contains('123') -and $rendered.Contains('F:\example {0}\osu!.exe') -and $script:AsioLanguage -eq $code) ($code+' watcher placeholders preserve braces in values')
        Check ((Get-AsioText 'watcher.behavior') -eq $catalog['watcher.behavior']) ($code+' watcher uses selected language')
        # Isolate only singleton and target for the real public-EXE wait test.
        $watcher=Join-Path $caseRoot 'managed\Start-Standalone.ps1'
        $original=[IO.File]::ReadAllText($watcher,$utf8)
        $isolated=$original.Replace("[string]`$ProcessName = 'osu!'","[string]`$ProcessName = 'ASIO-I18N-NoSuchGame-81973'").Replace('[int]$MaximumWaitSeconds = 0','[int]$MaximumWaitSeconds = 1').Replace('Local\OsuAsioStandaloneWatcher','Local\AsioI18nTest_20261115')
        [IO.File]::WriteAllText($watcher,$isolated,[Text.UTF8Encoding]::new($true))
        [IO.File]::WriteAllText((Join-Path $caseRoot '.asio-device-configured'),'test-only',[Text.Encoding]::ASCII)
        try {
            $entry=Invoke-TestProcess (Join-Path $caseRoot 'osu!asio_sound.exe') @() $decoy "`r`n" ($code+'-public-entry')
            Check ($entry.exit -eq 1 -and $entry.stdout.Contains('ASIO-I18N-NoSuchGame-81973.exe') -and $entry.stdout.Contains($catalog['watcher.behavior']) -and $entry.stderr.Contains($catalog['watcher.timeout']) -and !$entry.stderr.Contains('ParserError')) ($code+' public EXE reaches localized wait and expected timeout')
        } finally {
            [IO.File]::WriteAllText($watcher,$original,[Text.UTF8Encoding]::new($true))
            $marker=Join-Path $caseRoot '.asio-device-configured';if(!(Get-Item -LiteralPath $marker).FullName.StartsWith($run+'\')){throw 'Unsafe test marker removal'};Remove-Item -LiteralPath $marker
        }
        Check ((Get-FileHash -LiteralPath $audioConfig).Hash -eq $audioHash) ($code+' language changes do not alter audio config')
    }
    $oldLanguage=[IO.File]::ReadAllText((Join-Path $caseRoot 'language.ini'),$utf8)
    $invalid=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--language','invalid-code') $decoy '' 'invalid-language'
    Check ($invalid.exit -eq 2 -and [IO.File]::ReadAllText((Join-Path $caseRoot 'language.ini'),$utf8) -eq $oldLanguage) 'invalid language rejected without replacing preference'
    [IO.File]::WriteAllText((Join-Path $caseRoot 'language.ini'),"[Interface]`r`nLanguage = invalid`r`n",[Text.Encoding]::ASCII)
    $fallback=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--current-language') $decoy '' 'invalid-preference-fallback'
    Check ($fallback.exit -eq 0 -and $fallback.stdout.Trim() -eq 'en') 'invalid preference falls back to English'
    # Verify readiness recognition directly, using arbitrary display text. The
    # marker must work in every language and must not infer success from words.
    $watcherTreeTokens=$null;$watcherTreeErrors=$null
    $tree=[Management.Automation.Language.Parser]::ParseFile((Join-Path $caseRoot 'managed\Start-Standalone.ps1'),[ref]$watcherTreeTokens,[ref]$watcherTreeErrors)
    $readStatus=$tree.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Read-AsioAttachStatus'},$true)[0]
    . ([scriptblock]::Create($readStatus.Extent.Text))
    foreach($code in $codes){
        $catalog=Read-Catalog (Join-Path $caseRoot ("locales\$code.lang"))
        $reader=[IO.StringReader]::new(('[ASIO_READY] '+$catalog['backend.ready']+"`n"))
        $status=@{Reader=$reader;Pending='';Ready=$false}
        $null=Read-AsioAttachStatus $status
        Check ([bool]$status.Ready) ($code+' fixed readiness marker ignores translated wording')
        $reader.Dispose()
    }
    $reader=[IO.StringReader]::new("looks ready but no machine marker`n")
    $status=@{Reader=$reader;Pending='';Ready=$false};$null=Read-AsioAttachStatus $status
    Check (!$status.Ready) 'display text alone never marks attachment ready';$reader.Dispose()
    $eof=Invoke-TestProcess (Join-Path $caseRoot 'config.exe') @('--language') $decoy '' 'language-menu-eof'
    Check ($eof.exit -eq 2) 'language menu EOF cancels without hanging'
    # Review regression: catalog absence, exclusive read lock and invalid UTF-8
    # must retain readable English and must not abort watcher initialization.
    $broken=Join-Path $run 'catalog-failure-fixture';$null=New-Item -ItemType Directory -Path $broken,(Join-Path $broken 'locales')
    Initialize-AsioLanguage $broken
    Check ((Get-AsioText 'watcher.behavior').StartsWith('The tool does not start')) 'missing catalogs use built-in English'
    $lockedFile=Join-Path $broken 'locales\en.lang'
    [IO.File]::WriteAllText($lockedFile,'watcher.behavior=WRONG',$utf8)
    $stream=[IO.File]::Open($lockedFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    try{Initialize-AsioLanguage $broken;Check ((Get-AsioText 'watcher.behavior').StartsWith('The tool does not start')) 'exclusive catalog lock does not abort startup'}finally{$stream.Dispose()}
    [IO.File]::WriteAllBytes($lockedFile,[byte[]]@(255,254,0,1))
    Initialize-AsioLanguage $broken
    Check ((Get-AsioText 'watcher.behavior').StartsWith('The tool does not start')) 'invalid UTF8 catalog falls back to English'
    foreach($entry in $sourceManifest){Check ((Get-FileHash -LiteralPath $entry.path).Hash -eq $entry.hash) ('source untouched: '+[IO.Path]::GetFileName($entry.path))}
    $result=[ordered]@{pass=$true;checks=$checks.Count;failures=0;languages=$codes;hardware_device_opened=$false;game_started=$false;real_audio_listening=$false;launcher_isolation='Temporary copy only: unique mutex, nonexistent target, 1s deadline';cases=$checks;run=$run}
    [IO.File]::WriteAllText((Join-Path $run 'RESULT.json'),($result|ConvertTo-Json -Depth 8),$utf8)
    Write-Output ('PASS multilingual checks='+$checks.Count+'; result='+$run+'\RESULT.json')
} catch {
    $result=[ordered]@{pass=$false;checks=$checks.Count;failure=$_.Exception.Message;cases=$checks;run=$run}
    [IO.File]::WriteAllText((Join-Path $run 'RESULT.json'),($result|ConvertTo-Json -Depth 8),$utf8)
    throw
}
