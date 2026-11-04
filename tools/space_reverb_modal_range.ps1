param([Parameter(Mandatory=$true)][string]$Prefix,[Parameter(Mandatory=$true)][ValidateSet(1024,2048)][int]$Count)
$ErrorActionPreference='Stop'
if($Prefix -notmatch '^[a-z0-9-]+$'){throw 'Invalid evidence prefix'}
$base=(Resolve-Path 'build/space-reverb-product-tank-20261017').Path
$evidence=Join-Path $base $Prefix
$controllerSnapshot=Join-Path $base "$Prefix-controller.ps1";$readerSnapshot=Join-Path $base "$Prefix-reader.py"
if((Test-Path -LiteralPath $evidence) -or (Test-Path -LiteralPath $controllerSnapshot) -or (Test-Path -LiteralPath $readerSnapshot)){throw 'Evidence exists'}
Copy-Item -LiteralPath $PSCommandPath -Destination $controllerSnapshot
Copy-Item -LiteralPath 'tools/space_reverb_modal_range_readback.py' -Destination $readerSnapshot
$controllerHash=(Get-FileHash -LiteralPath $controllerSnapshot).Hash;$readerHash=(Get-FileHash -LiteralPath $readerSnapshot).Hash
$sidecar=Join-Path $evidence 'modes.bin';$code=1
try {
    & ./tools/space_reverb_native_check.ps1 -Prefix $Prefix -Target RackSynthSpaceReverbDirectModalRangeProbe -NativeArguments @('--range',"$Count",('"{0}"' -f $sidecar))
    $code=$LASTEXITCODE
} finally {
    if(Test-Path -LiteralPath $sidecar){
        [ordered]@{sha256=(Get-FileHash -LiteralPath $sidecar).Hash;bytes=(Get-Item -LiteralPath $sidecar).Length;controller_snapshot=$controllerSnapshot;controller_sha256=$controllerHash;reader_snapshot=$readerSnapshot;reader_sha256=$readerHash;scope='Pending independent full range readback; no CPU acceptance'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'sidecar.json') -Encoding utf8
    }
}
exit $code
