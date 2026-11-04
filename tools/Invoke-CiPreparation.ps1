[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$NativeRunner,
    [Parameter(Mandatory)][string]$Script,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Python312,
    [Parameter(Mandatory)][string]$Python314,
    [Parameter(Mandatory)][string]$Pluginval,
    [Parameter(Mandatory)][string]$OutputRoot,
    [switch]$RequireMachineResources
)
$ErrorActionPreference = 'Stop'
$nonce = [Guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$arguments = @('--', (Get-Process -Id $PID).Path, '-NoProfile', '-NonInteractive', '-File', $Script,
    '-PrepareNonce', $nonce, '-Python312', $Python312, '-Python314', $Python314,
    '-Pluginval', $Pluginval, '-OutputRoot', $OutputRoot)
if ($RequireMachineResources) {
    $arguments = @('--require-machine-resources') + $arguments
    $arguments += '-RequireMachineResources'
}
# Keep native progress on the console; the caller receives only the validated state object.
& $NativeRunner @arguments | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "CI preparation failed (exit $LASTEXITCODE)." }
try {
    $state = Get-Content -LiteralPath (Join-Path $OutputRoot 'prepare.json') -Raw | ConvertFrom-Json
    if ($state.nonce -cne $nonce -or
        [IO.Path]::GetFullPath([string]$state.source) -ine [IO.Path]::GetFullPath($Source)) { throw 'Wrong invocation' }
    foreach ($program in @($state.python312, $state.python314)) {
        if (-not [IO.Path]::IsPathFullyQualified([string]$program) -or
            -not (Test-Path -LiteralPath $program -PathType Leaf)) { throw 'Missing interpreter' }
    }
} catch { throw 'CI preparation receipt is missing or invalid.' }
$state
