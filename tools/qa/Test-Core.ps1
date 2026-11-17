#requires -Version 5.1
[CmdletBinding()]
param([int]$TestTimeoutSeconds=120)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'Invoke-BoundedProcess.ps1')
if(!(Get-Command cl.exe -ErrorAction SilentlyContinue)){throw 'Use an x64 Visual Studio developer shell.'}
$output=Join-Path $repo 'tests\bin'
New-Item -ItemType Directory -Path $output -Force|Out-Null
$fixtureDirectory=Join-Path $output ('T_OUTPUT_REGRESSION_RUN_'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureDirectory|Out-Null
$cases=@(
 @{name='routing';source='test_routing.cpp';fmod=$false;hook=$false;arguments=@()},
 @{name='audio-lease';source='AudioLeaseMock.cpp';fmod=$false;hook=$false;arguments=@()},
 @{name='startup-lease';source='StartupLeaseProbe.cpp';fmod=$true;hook=$false;arguments=@()},
 @{name='diagnostic-log';source='DiagnosticLogProbe.cpp';fmod=$false;hook=$false;arguments=@()},
 @{name='native-hook';source='NativeHookMock.cpp';fmod=$false;hook=$true;arguments=@()},
 @{name='native-restart';source='NativeRestartProbe.cpp';fmod=$false;hook=$true;arguments=@()},
 @{name='recovery-integration';source='RecoveryIntegration.cpp';fmod=$true;hook=$true;arguments=@()},
 @{name='mixer-abi';source='MixerAbiRegression.cpp';fmod=$false;hook=$true;arguments=@()},
 @{name='player-output';source='PlayerOutputProbe.cpp';fmod=$true;hook=$false;arguments=@()},
 @{name='output-regression';source='OutputRegression.cpp';fmod=$true;hook=$false;arguments=@('--artifacts',$fixtureDirectory)},
 @{name='digital-output';source='DigitalOutputProbe.cpp';fmod=$true;hook=$false;arguments=@()},
 @{name='config-enumeration';source='ConfigEnumerationProbe.cpp';fmod=$false;hook=$false;arguments=@()},
 @{name='wake-latency-old';source='WakeLatencyProbe.cpp';fmod=$false;hook=$false;arguments=@('--old')},
 @{name='wake-latency-new';source='WakeLatencyProbe.cpp';fmod=$false;hook=$false;arguments=@('--new')}
)
$failed=@()
Copy-Item -LiteralPath (Join-Path $repo 'fmod.dll') -Destination (Join-Path $output 'fmod.dll') -Force
foreach($case in $cases){
    $exe=Join-Path $output ($case.name+'.exe')
    $object=Join-Path $output ($case.name+'.obj')
    $arguments=@('/nologo','/utf-8','/std:c++17','/EHsc','/O2','/MT',('/I'+$repo),('/I'+(Join-Path $repo 'tests')),(Join-Path "$repo\tests" $case.source),('/Fo'+$object),('/Fe'+$exe),'/link','/LTCG','User32.lib','Advapi32.lib','winmm.lib')
    if($case.fmod){$arguments+=@((Join-Path $repo 'lib\fmod_vc.lib'))}
    if($case.hook){$arguments+=(Join-Path $repo 'lib\libMinHook.x64.lib')}
    if($case.name -eq 'recovery-integration') {
        $fixtureObject=Join-Path $output 'recovery-hook-fixture.obj'
        & cl.exe /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /c (Join-Path $repo 'tests\RecoveryHookFixture.cpp') ('/Fo'+$fixtureObject)
        if($LASTEXITCODE){$failed+=$case.name;continue}
        $arguments+=$fixtureObject
    }
    & cl.exe @arguments
    if($LASTEXITCODE){$failed+=$case.name;continue}
    try {
        Invoke-BoundedProcess -FilePath $exe -ArgumentList @($case.arguments) -WorkingDirectory $output -TimeoutSeconds $TestTimeoutSeconds -LogPrefix (Join-Path $output $case.name)
    } catch {Write-Warning $_.Exception.Message;$failed+=$case.name}
}
if($failed.Count){throw ('Failed regressions: '+($failed -join ', '))}
Invoke-BoundedProcess -FilePath (Get-Command dotnet.exe).Source -ArgumentList @('run','--project',(Join-Path $repo 'tests\AudioContractProbe\AudioContractProbe.csproj'),'-c','Release','--','--tiered-self-test') -WorkingDirectory $repo -TimeoutSeconds $TestTimeoutSeconds -LogPrefix (Join-Path $output 'tiered-contract')
Invoke-BoundedProcess -FilePath (Get-Command dotnet.exe).Source -ArgumentList @('run','--project',(Join-Path $repo 'tests\ManagedGraphProbe\ManagedGraphProbe.csproj'),'-c','Release') -WorkingDirectory $repo -TimeoutSeconds $TestTimeoutSeconds -LogPrefix (Join-Path $output 'managed-graph')
Invoke-BoundedProcess -FilePath (Get-Command dotnet.exe).Source -ArgumentList @('run','--project',(Join-Path $repo 'tests\ManagedLogProbe\ManagedLogProbe.csproj'),'-c','Release') -WorkingDirectory $repo -TimeoutSeconds $TestTimeoutSeconds -LogPrefix (Join-Path $output 'managed-log')
Write-Output ('PASS '+$cases.Count+' native/core regression programs and managed contract/graph/log probes; hardware output and game startup were not used.')
