#requires -Version 5.1
function Invoke-BoundedProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [string[]]$ArgumentList=@(),
        [Parameter(Mandatory=$true)][string]$LogPrefix,
        [int]$TimeoutSeconds=120,
        [string]$WorkingDirectory=(Get-Location).Path
    )
    if($TimeoutSeconds -lt 1){throw 'A positive process deadline is required.'}
    # All callers use simple Windows paths/flags, without embedded quotes or a
    # trailing slash. Reject unsupported quoting instead of invoking a shell.
    foreach($argument in $ArgumentList){if($argument.Contains('"') -or $argument.EndsWith('\')){throw 'Unsupported process argument quoting.'}}
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$FilePath
    $start.Arguments=(@($ArgumentList|ForEach-Object{'"'+$_+'"'}) -join ' ')
    $start.WorkingDirectory=$WorkingDirectory
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding=[Text.UTF8Encoding]::new($false)
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    try {
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $timedOut=!$process.WaitForExit($TimeoutSeconds*1000)
        if($timedOut){$process.Kill();$process.WaitForExit()}
        $code=$process.ExitCode
        $out=$stdout.GetAwaiter().GetResult();$err=$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($LogPrefix+'.stdout.log',$out,[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($LogPrefix+'.stderr.log',$err,[Text.UTF8Encoding]::new($false))
        [Console]::Out.Write($out);[Console]::Error.Write($err)
        if($timedOut){throw ('Timed out after {0}s: {1}; logs={2}' -f $TimeoutSeconds,$FilePath,$LogPrefix)}
        if($code -ne 0){throw ('Process exited {0}: {1}; logs={2}' -f $code,$FilePath,$LogPrefix)}
    } finally {$process.Dispose()}
}
