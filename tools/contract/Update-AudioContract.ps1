#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ClientDirectory,
    [string]$OutputFile='',
    [string]$CatalogFile='',
    [switch]$AppendReviewed,
    [string]$ReviewId=''
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if(!(Get-Command dotnet.exe -ErrorAction SilentlyContinue)){throw '.NET 10 SDK is required for the maintenance tool.'}
$project=Join-Path $PSScriptRoot 'ContractTool\ContractTool.csproj'
if($AppendReviewed){
    if(!$ReviewId){throw '-AppendReviewed requires a meaningful -ReviewId after reviewing the candidate and diff.'}
    if(!$CatalogFile){$CatalogFile=Join-Path $repo 'managed\audio-contract.json'}
    $arguments=@('append-reviewed',[IO.Path]::GetFullPath($ClientDirectory),[IO.Path]::GetFullPath($CatalogFile),$ReviewId,'--reviewed')
} else {
    if(!$OutputFile){$OutputFile=Join-Path $repo ('artifacts\contracts\candidate-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.json')}
    $arguments=@('inspect',[IO.Path]::GetFullPath($ClientDirectory),[IO.Path]::GetFullPath($OutputFile))
    if($CatalogFile){$arguments+=[IO.Path]::GetFullPath($CatalogFile)}
}
& dotnet.exe run --project $project -c Release -- @arguments
$code=$LASTEXITCODE
if($code -eq 3){Write-Warning 'Candidate extracted, but structural/semantic checks failed. Review the diff; promotion was not performed.'}
exit $code
