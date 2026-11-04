param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$SourceBundle,
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [ValidateSet('automatic')][string]$Backend='automatic',
    [switch]$Resume
)
$ErrorActionPreference='Stop'
$workspace=(Get-Location).Path
$binaryPath=(Resolve-Path -LiteralPath $Binary).Path
$sourcePath=(Resolve-Path -LiteralPath $SourceBundle).Path
if([IO.Path]::IsPathRooted($EvidenceRoot)){throw 'EvidenceRoot must be relative to the workspace'}
$evidencePath=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$binaryHash=(Get-FileHash -LiteralPath $binaryPath).Hash
$scriptHash=(Get-FileHash -LiteralPath $PSCommandPath).Hash
$manifestPath=Join-Path $evidencePath 'manifest.json'
if(Test-Path -LiteralPath $evidencePath){
    if(Test-Path -LiteralPath (Join-Path $evidencePath 'admission-stop1.json')){throw 'Retired evidence cannot resume'}
    if(@(Get-ChildItem -LiteralPath $evidencePath -Filter 'row-*.known-partial-failure.json').Count){throw 'Known partial failure cannot replay unchanged'}
    if(!$Resume){throw 'Existing evidence requires explicit Resume'}
    $manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if($manifest.binary_sha256 -ne $binaryHash -or $manifest.controller_sha256 -ne $scriptHash -or $manifest.backend_policy -ne $Backend){throw 'Resume identity mismatch'}
}else{
    if($Resume){throw 'Cannot resume missing evidence'}
    New-Item -ItemType Directory -Path $evidencePath | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $evidencePath 'source') -Recurse
    Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $evidencePath 'RackSynthSpaceReverbHandoffCpuFrozen.exe')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidencePath 'controller.ps1')
    $sources=@(Get-ChildItem -LiteralPath (Join-Path $evidencePath 'source') -Recurse -File | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($evidencePath.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
    [ordered]@{binary_sha256=$binaryHash;controller_sha256=$scriptHash;backend_policy=$Backend;created_utc=[DateTime]::UtcNow.ToString('o');rows=12;warm_seconds=10;measure_seconds=20;timing_format='24-byte little-endian binary64 block_seconds, binary64 copy_seconds, uint64 copy_upper_bytes';source=$sources;copy_p99_fraction=.1;core_mean_p99_fraction=.5;maximum_simultaneous_tanks=9;stable_idle_seconds=15;admission_protocol='periodic-proaudio6';schedule_format='24-byte little-endian int64 release_tick,start_tick,finish_tick';deadline='scheduled_release_plus_period';no_skips=$true;inventory_cadence_ms=100;inventory_freshness_ms=250;allocation_domain='render_thread';workload_order='tank_major';cycle_format='uint64 per block outside timer';acceptance='PENDING_REVIEW'} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    $manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
}
$frozenBinary=Join-Path $evidencePath 'RackSynthSpaceReverbHandoffCpuFrozen.exe'
if((Get-FileHash -LiteralPath $frozenBinary).Hash -ne $binaryHash){throw 'Frozen executable changed'}
foreach($entry in $manifest.source){if((Get-FileHash -LiteralPath (Join-Path $evidencePath $entry.path)).Hash -ne $entry.sha256){throw "Frozen source changed: $($entry.path)"}}
if($manifest.admission_protocol -ne 'periodic-proaudio6'){throw 'Wrong admission protocol'}
if($Resume){foreach($record in @(Get-ChildItem -LiteralPath $evidencePath -Filter 'row-*.complete.json' -File)){ $prior=Get-Content -Raw -LiteralPath $record.FullName | ConvertFrom-Json;if($prior.exit_code -ne 0 -or !$prior.valid_complete_native_record){throw 'Known completed failure cannot resume unchanged'}}}
function Wait-NativeIdle {
    $deadline=[DateTime]::UtcNow.AddMinutes(5);$quietSince=$null
    while([DateTime]::UtcNow -lt $deadline){
        $busy=@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -like 'RackSynth*' -or $_.ProcessName -in @('Runner.Worker','cl','link','pluginval','cmake','ctest') -or ($_.ProcessName -ne 'Runner.Listener' -and $_.Path -match '[\\/]ai-rack-synth[\\/]')}).Count -gt 0
        if($busy){$quietSince=$null}else{if($null -eq $quietSince){$quietSince=[DateTime]::UtcNow};if(([DateTime]::UtcNow-$quietSince).TotalSeconds -ge 15){return $true}}
        Start-Sleep -Milliseconds 250
    }
    return $false
}
$anyFailure=$false
for($index=0;$index -lt 12;$index++){
    $key='row-{0:D2}' -f $index
    $completePath=Join-Path $evidencePath "$key.complete.json"
    if(Test-Path -LiteralPath $completePath){$prior=Get-Content -Raw -LiteralPath $completePath | ConvertFrom-Json;if($prior.exit_code -ne 0 -or !$prior.valid_complete_native_record){$anyFailure=$true};continue}
    $firstAttempt=1
    foreach($file in @(Get-ChildItem -LiteralPath $evidencePath -Filter "$key-attempt-*.json" -File)){$number=[int]($file.BaseName -replace '^.*-attempt-','');$firstAttempt=[Math]::Max($firstAttempt,$number+1)}
    $completed=$false
    for($interruption=0;$interruption -lt 3;$interruption++){
        if(!(Wait-NativeIdle)){Write-Output "HANDOFF_CPU_DEFERRED row=$index reason=peer_busy";exit 3}
        $attempt=$firstAttempt+$interruption
        $prefix=Join-Path $evidencePath "$key-attempt-$attempt"
        foreach($extension in @('.stdout','.stderr','.json','.timings.bin','.timings.bin.cycles.bin','.timings.bin.schedule.bin')){if(Test-Path -LiteralPath "$prefix$extension"){throw 'Attempt path exists'}}
        $env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
        $started=[DateTime]::UtcNow
        $mode='--profile-periodic-cycles'
        $arguments=@($mode,"$index",('"{0}"' -f "$prefix.timings.bin"))
        $process=Start-Process -FilePath $frozenBinary -ArgumentList $arguments -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -Wait -PassThru
        $nativeExit=$process.ExitCode
        $timingHash=$null;$timingBytes=0;$cycleHash=$null;$cycleBytes=0;$scheduleHash=$null;$scheduleBytes=0
        if(Test-Path -LiteralPath "$prefix.timings.bin.schedule.bin"){$scheduleHash=(Get-FileHash -LiteralPath "$prefix.timings.bin.schedule.bin").Hash;$scheduleBytes=(Get-Item -LiteralPath "$prefix.timings.bin.schedule.bin").Length}
        if(Test-Path -LiteralPath "$prefix.timings.bin.cycles.bin"){$cycleHash=(Get-FileHash -LiteralPath "$prefix.timings.bin.cycles.bin").Hash;$cycleBytes=(Get-Item -LiteralPath "$prefix.timings.bin.cycles.bin").Length}
        if(Test-Path -LiteralPath "$prefix.timings.bin"){$timingHash=(Get-FileHash -LiteralPath "$prefix.timings.bin").Hash;$timingBytes=(Get-Item -LiteralPath "$prefix.timings.bin").Length}
        [ordered]@{row=$index;attempt=$attempt;exit_code=$nativeExit;started_utc=$started.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash -LiteralPath "$prefix.stderr").Hash;timings_sha256=$timingHash;timings_bytes=$timingBytes;cycles_sha256=$cycleHash;cycles_bytes=$cycleBytes;schedule_sha256=$scheduleHash;schedule_bytes=$scheduleBytes} |
            ConvertTo-Json | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
        if($nativeExit -eq 3){
            $partial=@(Get-Content -LiteralPath "$prefix.stdout" | Where-Object {$_ -match '^HANDOFF_DEADLINE_MISS '})
            if($partial.Count){[ordered]@{status='INTERRUPTED_KNOWN_DEADLINE_FAILURE_NO_REPLAY';row=$index;attempt=$attempt;record="$key-attempt-$attempt.json"} | ConvertTo-Json | Set-Content (Join-Path $evidencePath "$key.known-partial-failure.json");exit 1}
            Write-Output "HANDOFF_CPU_INTERRUPTED row=$index attempt=$attempt";continue
        }
        $lines=@(Get-Content -LiteralPath "$prefix.stdout")
        $result=@($lines | Where-Object {$_ -match "^HANDOFF_CPU_RESULT row=$index "})
        $footer=@($lines | Where-Object {$_ -eq "HANDOFF_CPU_FOOTER row=$index complete=1 acceptance=0"})
        $fields=[ordered]@{}
        if($result.Count -eq 1){foreach($pair in [regex]::Matches($result[0],'(\S+)=(\S+)')){$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}}
        $valid=$nativeExit -in @(0,1) -and $result.Count -eq 1 -and $footer.Count -eq 1 -and $fields['domain'] -eq 'poleContour' -and $fields['interrupted'] -eq '0'
        if($valid){$valid=$cycleBytes -eq 8*[long]$fields['samples'] -and $cycleHash -and @($lines | Where-Object {$_ -eq "HANDOFF_CYCLE_DIAGNOSTIC enabled=1 valid=1 samples=$($fields['samples']) format=uint64_native_little_endian outside_block_timer=1"}).Count -eq 1 -and $timingBytes -eq 24*[long]$fields['samples'] -and (($nativeExit -eq 0) -eq ($fields['pass'] -eq '1'))}
        if($valid){$valid=@($lines | Where-Object {$_ -eq 'HANDOFF_WORKLOAD order=tank_major scratch_bytes=151552'}).Count -eq 1}
        if($valid){$monitor=@($lines | Where-Object {$_ -match '^HANDOFF_INVENTORY_MONITOR mode=worker scans=([1-9][0-9]*) maximum_scan_seconds=([0-9.eE+-]+) cadence_ms=100 freshness_ms=250$'});$valid=$monitor.Count -eq 1 -and @($lines | Where-Object {$_ -eq 'HANDOFF_ALLOCATION_DOMAIN render_thread=1'}).Count -eq 1}
        if($valid){
            $periodic=@($lines | Where-Object {$_ -match '^HANDOFF_PERIODIC_CALLER '});$pfields=@{}
            if($periodic.Count -eq 1){foreach($pair in [regex]::Matches($periodic[0],'(\S+)=(\S+)')){$pfields[$pair.Groups[1].Value]=$pair.Groups[2].Value}}
            $expected=[long][Math]::Ceiling(20*[double]$fields['rate']/[double]$fields['block'])
            $valid=$periodic.Count -eq 1 -and $pfields['mode'] -eq 'periodic-proaudio6' -and $pfields['opened'] -eq '1' -and $pfields['closed'] -eq '1' -and $pfields['task'] -eq 'Pro_Audio' -and $pfields['timer'] -eq 'high_resolution' -and $pfields['schedule_valid'] -eq '1' -and [long]$pfields['anchor_tick'] -gt 0 -and $pfields['ticks_per_second'] -eq '1000000000' -and [long]$pfields['expected_samples'] -eq $expected -and [long]$pfields['samples'] -eq $expected -and [long]$fields['samples'] -eq $expected -and $pfields['priority_before'] -eq $pfields['priority_after'] -and $pfields['failure'] -eq '0' -and $pfields['platform_error'] -eq '0' -and $pfields['cleanup_error'] -eq '0' -and $pfields['deadline'] -eq 'scheduled_release_plus_period' -and $pfields['no_skips'] -eq '1' -and $scheduleBytes -eq 24*$expected -and $scheduleHash
        }
        if($valid){$valid=$fields['backend'] -in $(if($Backend -eq 'automatic'){@('sse2','avx2')}else{@('sse2')})}
        [ordered]@{row=$index;attempt=$attempt;exit_code=$nativeExit;valid_complete_native_record=$valid;stdout="$key-attempt-$attempt.stdout";timings="$key-attempt-$attempt.timings.bin";cycles="$key-attempt-$attempt.timings.bin.cycles.bin";schedule="$key-attempt-$attempt.timings.bin.schedule.bin";fields=$fields;acceptance='PENDING_REVIEW'} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $completePath -Encoding utf8
        $completed=$true;if($nativeExit -ne 0 -or !$valid){$anyFailure=$true}
        Write-Output "HANDOFF_CPU_ROW row=$index exit=$nativeExit valid=$valid p99=$($fields['p99_seconds']) misses=$($fields['misses'])"
        if($anyFailure){Write-Output "HANDOFF_CPU_KNOWN_COMPLETED_FAILURE_STOP row=$index";exit 1}
        break
    }
    if(!$completed){Write-Output "HANDOFF_CPU_DEFERRED row=$index reason=three_interruptions";exit 3}
}
$records=@(Get-ChildItem -LiteralPath $evidencePath -Filter 'row-*.complete.json' -File | Sort-Object Name | ForEach-Object {Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json})
$summaryPath=Join-Path $evidencePath 'summary.json'
if(!(Test-Path -LiteralPath $summaryPath)){
    [ordered]@{rows=$records.Count;expected_rows=12;failed_rows=@($records | Where-Object {$_.exit_code -ne 0}).Count;invalid_records=@($records | Where-Object {!$_.valid_complete_native_record}).Count;failed=$anyFailure;binary_sha256=$binaryHash;acceptance='PENDING_INDEPENDENT_TIMING_READBACK'} |
        ConvertTo-Json | Set-Content -LiteralPath $summaryPath -Encoding utf8
}
Write-Output "HANDOFF_CPU_COMPLETE rows=$($records.Count) failed=$anyFailure acceptance=0"
if($anyFailure){exit 1}
exit 0
