#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'Invoke-BoundedProcess.ps1')
$output=Join-Path $repo 'tests\bin'
$null=New-Item -ItemType Directory -Path $output -Force
$probe=Join-Path $output 'backend-path.exe'
$source=Join-Path $repo 'tests\BackendPathProbe.cpp'
& cl.exe /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /Brepro $source ("/Fo$output\backend-path.obj") ("/Fe$probe") /link (Join-Path $repo 'lib\fmod_vc.lib') User32.lib Advapi32.lib
if($LASTEXITCODE){throw 'Cannot build backend path fixture.'}
$unicode=([string][char]0x65E5)+[char]0x672C+[char]0x8A9E+' '+[char]0xD83C+[char]0xDFB5
$run=Join-Path $output ('public-'+[guid]::NewGuid().ToString('N'))
$caseRoot=Join-Path $run ('renamed '+$unicode+' with spaces')
$decoy=Join-Path $run 'unrelated cwd'
$null=New-Item -ItemType Directory -Path $caseRoot,$decoy,(Join-Path $caseRoot 'locales')
Copy-Item -LiteralPath $probe -Destination (Join-Path $caseRoot 'backend-path.exe')
foreach($name in @('config.exe','fmod.dll')){Copy-Item -LiteralPath (Join-Path "$repo\release" $name) -Destination $caseRoot}
Copy-Item -Path "$repo\locales\*.lang" -Destination (Join-Path $caseRoot 'locales')
$config=Join-Path $caseRoot 'config.ini'
$valid="[Config]`r`nBuffer Size = 16`r`nASIO Driver ID = 0`r`nSample Rate = 48000`r`nOutput Volume = 37`r`nDebug Log = 0`r`nContract Mode = 0`r`n"
[IO.File]::WriteAllText($config,$valid,[Text.Encoding]::ASCII)
[IO.File]::WriteAllText((Join-Path $decoy 'config.ini'),$valid.Replace('37','1'),[Text.Encoding]::ASCII)
Invoke-BoundedProcess -FilePath (Join-Path $caseRoot 'backend-path.exe') -WorkingDirectory $decoy -LogPrefix (Join-Path $run 'backend-unicode')
$showLog=Join-Path $run 'config-unicode'
Invoke-BoundedProcess -FilePath (Join-Path $caseRoot 'config.exe') -ArgumentList @('--show') -WorkingDirectory $decoy -LogPrefix $showLog
if(![IO.File]::ReadAllText($showLog+'.stdout.log').Contains('Output Volume = 37')){throw 'Config read decoy cwd.'}
$cases=@(
 @{name='encoding';text="["+[char]0+"Config]";key='settings.encoding';arg=''},
 @{name='duplicate';text=$valid+"Buffer Size = 16`n";key='settings.duplicate';arg='Buffer Size'},
 @{name='integer';text=$valid.Replace('Output Volume = 37','Output Volume = bad');key='settings.integer';arg='Output Volume'},
 @{name='missing';text=$valid.Replace("Buffer Size = 16`r`n",'');key='settings.missing';arg='Buffer Size'},
 @{name='range';text=$valid.Replace('Output Volume = 37','Output Volume = 101');key='settings.range';arg=''},
 @{name='unreadable';text=('x'*65537);key='settings.unreadable';arg=''}
)
$count=2
foreach($code in @('zh-CN','zh-TW','en','fr','es')){
 $catalog=@{};foreach($line in [IO.File]::ReadAllLines((Join-Path "$repo\locales" ($code+'.lang')),[Text.Encoding]::UTF8)){if($line -match '^([^#=]+)=(.*)$'){$catalog[$Matches[1]]=$Matches[2]}}
 [IO.File]::WriteAllText((Join-Path $caseRoot 'language.ini'),"[Interface]`nLanguage = $code`n",[Text.Encoding]::ASCII)
 foreach($case in $cases){
  [IO.File]::WriteAllText($config,$case.text,[Text.UTF8Encoding]::new($false))
  $before=(Get-FileHash -LiteralPath $config).Hash
  $log=Join-Path $run ($code+'-'+$case.name)
  Invoke-BoundedProcess -FilePath (Join-Path $caseRoot 'config.exe') -ArgumentList @('--show') -WorkingDirectory $decoy -LogPrefix $log
  $actual=[IO.File]::ReadAllText($log+'.stderr.log',[Text.Encoding]::UTF8)
  $reason=$catalog[$case.key].Replace('{0}',$case.arg)
  if(!$actual.Contains($reason)){throw ('Missing localized cause: '+$code+' '+$case.name)}
  if((Get-FileHash -LiteralPath $config).Hash -ne $before){throw 'Read-only display modified invalid config.'}
  $count++
 }
}
Write-Output ('PASS public boundary cases='+$count+'; Unicode beyond ACP, six invalid inputs in five languages; no game/device startup; logs='+$run)
