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
    # .NET Framework's redirected StreamWriter takes Console.InputEncoding
    # during Start and can emit its BOM immediately, before any explicit Write.
    $oldInputEncoding=[Console]::InputEncoding
    try {[Console]::InputEncoding=$utf8;$null=$process.Start()}
    finally {[Console]::InputEncoding=$oldInputEncoding}
    $testProcessId=$process.Id
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    # Send explicit UTF-8 bytes, independent of the restored host encoding.
    $inputStream=$process.StandardInput.BaseStream
    if($InputText){$bytes=$utf8.GetBytes($InputText);$inputStream.Write($bytes,0,$bytes.Length)}
    $inputStream.Close()
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
$entry=Join-Path $caseRoot 'LazerASIO.exe'
$codes=@('zh-CN','zh-TW','en','fr','es')
try {
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot 'language.ini'))) 'clean package has no language preference'
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot 'gui.ini'))) 'clean package has no window preferences'
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot '.asio-device-configured'))) 'clean package has no device marker'
    Check (!(Test-Path -LiteralPath (Join-Path $caseRoot 'logs'))) 'clean package has no logs'
    $initial=Invoke-TestProcess $entry @('--language') $decoy "4`r`n" 'first-language-menu'
    Check ($initial.exit -eq 0 -and $initial.stdout.Contains('Français') -and $initial.stdout.Contains('Español') -and $initial.stdout.Contains('繁體中文')) 'first language menu includes all languages and selects French'
    foreach($code in $codes) {
        $catalog=Read-Catalog (Join-Path $caseRoot ("locales\$code.lang"))
        $select=Invoke-TestProcess $entry @('--language',$code) $decoy '' ($code+'-select')
        Check ($select.exit -eq 0 -and $select.stdout.Contains($catalog['language.name'])) ($code+' saved language with native name')
        $preference=[IO.File]::ReadAllText((Join-Path $caseRoot 'language.ini'),$utf8)
        Check ($preference.Contains('Language = '+$code)) ($code+' saved code in separate language.ini')
        $current=Invoke-TestProcess $entry @('--current-language') $decoy '' ($code+'-current')
        Check ($current.exit -eq 0 -and $current.stdout.Trim() -eq $code) ($code+' preference is read back from language.ini')
        $help=Invoke-TestProcess $entry @('--help') $decoy '' ($code+'-help')
        Check ($help.exit -eq 0 -and $help.stdout.Contains($catalog['gui.usage'])) ($code+' localized command-line usage')
        $show=Invoke-TestProcess $entry @('--show') $decoy '' ($code+'-config')
        Check ($show.exit -eq 0 -and $show.stdout.Contains('Buffer Size = 16') -and $show.stdout.Contains('Output Volume = 50') -and $show.stdout.Contains('Contract Mode = 1') -and !$show.stdout.Contains('999')) ($code+' config including Adaptive remains language/cwd independent')
        $list=Invoke-TestProcess $entry @('--list') $decoy '' ($code+'-list')
        Check ($list.exit -eq 0 -and $list.stdout.Contains($catalog['config.devices'])) ($code+' localized device enumeration without opening device')
        $backend=Invoke-TestProcess (Join-Path $caseRoot 'asio_backend.exe') @() $decoy '' ($code+'-backend-guard')
        Check ($backend.exit -eq 2 -and $backend.stderr.Contains($catalog['backend.internal'])) ($code+' localized backend guard before device open')
        $unconfigured=Invoke-TestProcess $entry @('--watch','--process','ASIO-I18N-NoSuchGame-81973','--max-wait','1','--instance','i18n') $decoy '' ($code+'-watch-unconfigured')
        Check ($unconfigured.exit -eq 1 -and $unconfigured.stderr.Contains($catalog['gui.first_use_cli'])) ($code+' watcher refuses to attach before device setup')
        # Isolated wait: unique watcher name, nonexistent target and a 1 s deadline.
        [IO.File]::WriteAllText((Join-Path $caseRoot '.asio-device-configured'),'test-only',[Text.Encoding]::ASCII)
        try {
            $watch=Invoke-TestProcess $entry @('--watch','--process','ASIO-I18N-NoSuchGame-81973','--max-wait','1','--instance','i18n') $decoy '' ($code+'-watch')
            Check ($watch.exit -eq 1 -and $watch.stdout.Contains('ASIO-I18N-NoSuchGame-81973.exe') -and $watch.stdout.Contains($catalog['watcher.behavior']) -and $watch.stderr.Contains($catalog['watcher.timeout'])) ($code+' watcher reaches localized wait and expected timeout')
        } finally {
            $marker=Join-Path $caseRoot '.asio-device-configured';if(!(Get-Item -LiteralPath $marker).FullName.StartsWith($run+'\')){throw 'Unsafe test marker removal'};Remove-Item -LiteralPath $marker
        }
        Check ((Get-FileHash -LiteralPath $audioConfig).Hash -eq $audioHash) ($code+' language changes do not alter audio config')
    }
    $oldLanguage=[IO.File]::ReadAllText((Join-Path $caseRoot 'language.ini'),$utf8)
    $invalid=Invoke-TestProcess $entry @('--language','invalid-code') $decoy '' 'invalid-language'
    Check ($invalid.exit -eq 2 -and [IO.File]::ReadAllText((Join-Path $caseRoot 'language.ini'),$utf8) -eq $oldLanguage) 'invalid language rejected without replacing preference'
    [IO.File]::WriteAllText((Join-Path $caseRoot 'language.ini'),"[Interface]`r`nLanguage = invalid`r`n",[Text.Encoding]::ASCII)
    $fallback=Invoke-TestProcess $entry @('--current-language') $decoy '' 'invalid-preference-fallback'
    Check ($fallback.exit -eq 0 -and $fallback.stdout.Trim() -eq 'en') 'invalid preference falls back to English'
    $eof=Invoke-TestProcess $entry @('--language') $decoy '' 'language-menu-eof'
    Check ($eof.exit -eq 2) 'language menu EOF cancels without hanging'
    $unknown=Invoke-TestProcess $entry @('--no-such-flag') $decoy '' 'unknown-flag'
    Check ($unknown.exit -eq 2 -and $unknown.stdout.Contains('LazerASIO.exe')) 'unknown flag prints usage instead of opening the window'
    # Catalog absence, an exclusive read lock and invalid UTF-8 must keep
    # readable English rather than abort startup.
    $broken=Join-Path $run 'catalog-failure-fixture';$null=New-Item -ItemType Directory -Path $broken,(Join-Path $broken 'locales')
    Copy-Item -LiteralPath $entry -Destination (Join-Path $broken 'LazerASIO.exe')
    Copy-Item -LiteralPath (Join-Path $caseRoot 'fmod.dll') -Destination (Join-Path $broken 'fmod.dll')
    [IO.File]::WriteAllText((Join-Path $broken 'language.ini'),"[Interface]`r`nLanguage = fr`r`n",[Text.Encoding]::ASCII)
    $english=(Read-Catalog (Join-Path $caseRoot 'locales\en.lang'))['gui.usage']
    $missing=Invoke-TestProcess (Join-Path $broken 'LazerASIO.exe') @('--help') $decoy '' 'catalog-missing'
    Check ($missing.exit -eq 0 -and $missing.stdout.Contains($english)) 'missing catalogs use built-in English'
    $lockedFile=Join-Path $broken 'locales\fr.lang'
    [IO.File]::WriteAllText($lockedFile,'gui.usage=WRONG',$utf8)
    $stream=[IO.File]::Open($lockedFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    try{$locked=Invoke-TestProcess (Join-Path $broken 'LazerASIO.exe') @('--help') $decoy '' 'catalog-locked';Check ($locked.exit -eq 0 -and $locked.stdout.Contains($english)) 'exclusive catalog lock does not abort startup'}finally{$stream.Dispose()}
    [IO.File]::WriteAllBytes($lockedFile,[byte[]]@(255,254,0,1))
    $invalidUtf8=Invoke-TestProcess (Join-Path $broken 'LazerASIO.exe') @('--help') $decoy '' 'catalog-invalid-utf8'
    Check ($invalidUtf8.exit -eq 0 -and $invalidUtf8.stdout.Contains($english)) 'invalid UTF8 catalog falls back to English'
    foreach($entryFile in $sourceManifest){Check ((Get-FileHash -LiteralPath $entryFile.path).Hash -eq $entryFile.hash) ('source untouched: '+[IO.Path]::GetFileName($entryFile.path))}
    $result=[ordered]@{pass=$true;checks=$checks.Count;failures=0;languages=$codes;hardware_device_opened=$false;game_started=$false;real_audio_listening=$false;window_opened=$false;watcher_isolation='Temporary copy only: unique watcher name, nonexistent target, 1s deadline';readiness_markers='tests/WatcherProbe.cpp (all five catalogs)';cases=$checks;run=$run}
    [IO.File]::WriteAllText((Join-Path $run 'RESULT.json'),($result|ConvertTo-Json -Depth 8),$utf8)
    Write-Output ('PASS multilingual checks='+$checks.Count+'; result='+$run+'\RESULT.json')
} catch {
    $result=[ordered]@{pass=$false;checks=$checks.Count;failure=$_.Exception.Message;cases=$checks;run=$run}
    [IO.File]::WriteAllText((Join-Path $run 'RESULT.json'),($result|ConvertTo-Json -Depth 8),$utf8)
    throw
}
