[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Program,
    [Parameter(Mandatory)][ValidateSet('3.12', '3.14')][string]$Version,
    [Parameter(Mandatory)][string]$NativeRunner
)
$ErrorActionPreference = 'Stop'
# Keep selectors as an array: splatting a scalar string can discard the Python arguments.
$executable = (Get-Command $Program -CommandType Application -ErrorAction Stop).Source
$arguments = @('--run-seconds', '10', '--', $executable)
if ($Program -eq 'py') { $arguments += "-$Version" }
$arguments += @('-c', "import sys; assert '.'.join(map(str, sys.version_info[:2])) == '$Version'; print(sys.executable)")
$reported = & $NativeRunner @arguments
if ($LASTEXITCODE -ne 0) { throw "Python $Version identity failed (exit $LASTEXITCODE)." }
if (@($reported).Count -ne 1 -or -not [IO.Path]::IsPathFullyQualified([string]$reported) -or
    -not (Test-Path -LiteralPath ([string]$reported) -PathType Leaf)) {
    throw 'Python did not report one executable path; refusing an unverified interpreter.'
}
[IO.Path]::GetFullPath([string]$reported)
