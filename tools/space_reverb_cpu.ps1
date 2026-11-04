param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$SourceBundle,
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [ValidateSet('sse2','automatic')][string]$Backend='sse2',
    [switch]$Resume
)
$ErrorActionPreference='Stop'
$workspace=(Get-Location).Path
$binaryPath=(Resolve-Path -LiteralPath $Binary).Path
$sourcePath=(Resolve-Path -LiteralPath $SourceBundle).Path
$evidencePath=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$binaryHash=(Get-FileHash -LiteralPath $binaryPath).Hash
$scriptHash=(Get-FileHash -LiteralPath $PSCommandPath).Hash
$manifestPath=Join-Path $evidencePath 'manifest.json'
if(Test-Path -LiteralPath $evidencePath){
    if(Test-Path -LiteralPath (Join-Path $evidencePath 'admission-stop1.json')){throw 'Retired evidence cannot resume'}
    if(!$Resume){throw 'Existing evidence requires explicit Resume'}
    $manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if($manifest.binary_sha256 -ne $binaryHash -or $manifest.controller_sha256 -ne $scriptHash -or $manifest.backend_policy -ne $Backend){throw 'Resume identity mismatch'}
}else{
    if($Resume){throw 'Cannot resume missing evidence'}
    New-Item -ItemType Directory -Path $evidencePath | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $evidencePath 'source') -Recurse
    Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $evidencePath 'RackSynthSpaceReverbCpuFrozen.exe')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidencePath 'controller.ps1')
    $sources=@(Get-ChildItem -LiteralPath (Join-Path $evidencePath 'source') -Recurse -File | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($evidencePath.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
    [ordered]@{binary_sha256=$binaryHash;controller_sha256=$scriptHash;backend_policy=$Backend;created_utc=[DateTime]::UtcNow.ToString('o');rows=24;warm_seconds=10;measure_seconds=20;timing_format='native little-endian IEEE754 binary64 seconds';source=$sources;acceptance='PENDING_REVIEW'} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    $manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
}
$frozenBinary=Join-Path $evidencePath 'RackSynthSpaceReverbCpuFrozen.exe'
if((Get-FileHash -LiteralPath $frozenBinary).Hash -ne $binaryHash){throw 'Frozen executable changed'}
foreach($entry in $manifest.source){if((Get-FileHash -LiteralPath (Join-Path $evidencePath $entry.path)).Hash -ne $entry.sha256){throw "Frozen source changed: $($entry.path)"}}
function Wait-NativeIdle {
    $deadline=[DateTime]::UtcNow.AddMinutes(5)
    while(@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -like 'RackSynth*' -or $_.ProcessName -in @('Runner.Worker','cl','link','pluginval') -or ($_.ProcessName -ne 'Runner.Listener' -and $_.Path -match '[\\/]ai-rack-synth[\\/]')}).Count){if([DateTime]::UtcNow -ge $deadline){return $false};Start-Sleep -Milliseconds 500}
    return $true
}
$anyFailure=$false
for($index=0;$index -lt 24;$index++){
    $key='row-{0:D2}' -f $index
    $completePath=Join-Path $evidencePath "$key.complete.json"
    if(Test-Path -LiteralPath $completePath){$prior=Get-Content -Raw -LiteralPath $completePath | ConvertFrom-Json;if($prior.exit_code -ne 0 -or !$prior.valid_complete_native_record){$anyFailure=$true};continue}
    $firstAttempt=1
    foreach($file in @(Get-ChildItem -LiteralPath $evidencePath -Filter "$key-attempt-*.json" -File)){$number=[int]($file.BaseName -replace '^.*-attempt-','');$firstAttempt=[Math]::Max($firstAttempt,$number+1)}
    $completed=$false
    for($interruption=0;$interruption -lt 3;$interruption++){
        if(!(Wait-NativeIdle)){Write-Output "CPU_DEFERRED row=$index reason=peer_busy";exit 3}
        $attempt=$firstAttempt+$interruption
        $prefix=Join-Path $evidencePath "$key-attempt-$attempt"
        foreach($extension in @('.stdout','.stderr','.json','.timings.bin')){if(Test-Path -LiteralPath "$prefix$extension"){throw 'Attempt path exists'}}
        $env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
        $started=[DateTime]::UtcNow
        $mode=if($Backend -eq 'automatic'){'--row-automatic'}else{'--row'}
        $arguments=@($mode,"$index",('"{0}"' -f "$prefix.timings.bin"))
        $process=Start-Process -FilePath $frozenBinary -ArgumentList $arguments -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -Wait -PassThru
        $nativeExit=$process.ExitCode
        $timingHash=$null;$timingBytes=0
        if(Test-Path -LiteralPath "$prefix.timings.bin"){$timingHash=(Get-FileHash -LiteralPath "$prefix.timings.bin").Hash;$timingBytes=(Get-Item -LiteralPath "$prefix.timings.bin").Length}
        [ordered]@{row=$index;attempt=$attempt;exit_code=$nativeExit;started_utc=$started.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash -LiteralPath "$prefix.stderr").Hash;timings_sha256=$timingHash;timings_bytes=$timingBytes} |
            ConvertTo-Json | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
        if($nativeExit -eq 3){Write-Output "CPU_INTERRUPTED row=$index attempt=$attempt";continue}
        $lines=@(Get-Content -LiteralPath "$prefix.stdout")
        $result=@($lines | Where-Object {$_ -match "^CPU_RESULT row=$index "})
        $footer=@($lines | Where-Object {$_ -eq "CPU_FOOTER row=$index complete=1 acceptance=0"})
        $fields=[ordered]@{}
        if($result.Count -eq 1){foreach($pair in [regex]::Matches($result[0],'(\S+)=(\S+)')){$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}}
        $valid=$nativeExit -in @(0,1) -and $result.Count -eq 1 -and $footer.Count -eq 1 -and $fields['domain'] -eq 'poleContour' -and $fields['interrupted'] -eq '0'
        if($valid){$valid=$timingBytes -eq 8*[long]$fields['samples'] -and (($nativeExit -eq 0) -eq ($fields['pass'] -eq '1'))}
        if($valid){$valid=$fields['backend'] -in $(if($Backend -eq 'automatic'){@('sse2','avx2')}else{@('sse2')})}
        [ordered]@{row=$index;attempt=$attempt;exit_code=$nativeExit;valid_complete_native_record=$valid;stdout="$key-attempt-$attempt.stdout";timings="$key-attempt-$attempt.timings.bin";fields=$fields;acceptance='PENDING_REVIEW'} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $completePath -Encoding utf8
        $completed=$true;if($nativeExit -ne 0 -or !$valid){$anyFailure=$true}
        Write-Output "CPU_ROW row=$index exit=$nativeExit valid=$valid p99=$($fields['p99_seconds']) misses=$($fields['misses'])"
        break
    }
    if(!$completed){Write-Output "CPU_DEFERRED row=$index reason=three_interruptions";exit 3}
}
$records=@(Get-ChildItem -LiteralPath $evidencePath -Filter 'row-*.complete.json' -File | Sort-Object Name | ForEach-Object {Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json})
$summaryPath=Join-Path $evidencePath 'summary.json'
if(!(Test-Path -LiteralPath $summaryPath)){
    [ordered]@{rows=$records.Count;expected_rows=24;failed_rows=@($records | Where-Object {$_.exit_code -ne 0}).Count;invalid_records=@($records | Where-Object {!$_.valid_complete_native_record}).Count;failed=$anyFailure;binary_sha256=$binaryHash;acceptance='PENDING_INDEPENDENT_TIMING_READBACK'} |
        ConvertTo-Json | Set-Content -LiteralPath $summaryPath -Encoding utf8
}
Write-Output "CPU_COMPLETE rows=$($records.Count) failed=$anyFailure acceptance=0"
if($anyFailure){exit 1}
exit 0
