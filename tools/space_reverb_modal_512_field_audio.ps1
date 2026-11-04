param([Parameter(Mandatory=$true)][string]$EvidenceRoot,[ValidateSet(512)][int]$Count=512,[switch]$Resume,[datetime]$StopBeforeUtc=[datetime]::MaxValue)
$ErrorActionPreference='Stop'
$workspace=(Get-Location).Path;$evidence=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$binary=(Resolve-Path 'build/space-reverb-feasibility/Release/RackSynthSpaceReverbDirectModalFieldAudioProbe.exe').Path
$workloads=@([pscustomobject]@{mode='calibrate';ordinal=49;row=112})
foreach($i in 0..332){if($i -ne 49){$workloads+=[pscustomobject]@{mode='row';ordinal=$i;row=([int][Math]::Floor($i/9)*21+([int][Math]::Floor(($i%9)/3)+1)*3+$i%3)}}}
$corners=@(0,20,42,62,168,188,1365,1372,1385,1412,1414,1415,1418,1421,1424,1427,1428,1448,1449,1452,1455,1461,1462,1469)
foreach($i in 0..23){$workloads+=[pscustomobject]@{mode='corner';ordinal=$i;row=$corners[$i]}}
if(!(Test-Path -LiteralPath $evidence)){
    if($Resume){throw 'Missing evidence'};New-Item -ItemType Directory -Path $evidence | Out-Null
    $source=Join-Path $evidence 'source';New-Item -ItemType Directory -Path $source | Out-Null
    $files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-ChildItem probes/space-reverb -File)+@(Get-Item src/dsp/space/CMakeLists.txt,CMakeLists.txt,docs/superpowers/plans/2026-10-18-space-reverb-modal-field-audio.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-512.md)
    $entries=@(foreach($f in $files){$relative=$f.FullName.Substring($workspace.Length+1);$target=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null;Copy-Item -LiteralPath $f.FullName -Destination $target;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $target).Hash}})
    Copy-Item -LiteralPath $binary -Destination (Join-Path $evidence 'AudioFrozen.exe')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1')
    Copy-Item -LiteralPath tools/space_reverb_modal_512_field_audio_readback.py -Destination (Join-Path $evidence 'reader.py')
    [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');scope='CI-exclusive functional raw audio; no CPU acceptance';count=$Count;binary_sha256=(Get-FileHash -LiteralPath $binary).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;reader_sha256=(Get-FileHash -LiteralPath (Join-Path $evidence 'reader.py')).Hash;workloads=$workloads;sources=$entries} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $evidence 'manifest.json') -Encoding utf8
}elseif(!$Resume){throw 'Existing evidence requires Resume'}
$manifest=Get-Content -Raw -LiteralPath (Join-Path $evidence 'manifest.json') | ConvertFrom-Json;$frozen=Join-Path $evidence 'AudioFrozen.exe'
if($manifest.count -ne $Count -or (Get-FileHash -LiteralPath $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $manifest.controller_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'reader.py')).Hash -ne $manifest.reader_sha256){throw 'Frozen identity mismatch'}
foreach($f in $manifest.sources){if((Get-FileHash -LiteralPath (Join-Path (Join-Path $evidence 'source') $f.path)).Hash -ne $f.sha256){throw 'Frozen source changed'}}
Write-Output "MODAL_FIELD_AUDIO_CONTROLLER pid=$PID count=$Count workloads=357"
foreach($work in $workloads){
    $mode=$work.mode;$ordinal=$work.ordinal;$row=$work.row;$key='{0}-{1:D3}' -f $mode,$ordinal;$complete=Join-Path $evidence "$key.complete.json"
    if(Test-Path -LiteralPath $complete){$old=Get-Content -Raw -LiteralPath $complete | ConvertFrom-Json;if($old.row -ne $row -or $old.ordinal -ne $ordinal -or $old.mode -ne $mode -or $old.exit -ne 0 -or !$old.valid){throw 'Saved failure or workload mismatch; no unchanged replay'};continue}
    if([DateTime]::UtcNow -ge $StopBeforeUtc){Write-Output 'MODAL512_AUDIO_DEFERRED_BETWEEN_COMPLETED_WORKLOADS';exit 3}
    $deadline=[DateTime]::UtcNow.AddMinutes(30)
    while(@(Get-CimInstance Win32_Process -Filter "Name='Runner.Worker.exe'").Count){if([DateTime]::UtcNow -ge $deadline){exit 3};Start-Sleep -Milliseconds 500}
    $attempt=1;while(Test-Path -LiteralPath (Join-Path $evidence "$key-attempt-$attempt.stdout")){$prior=Join-Path $evidence "$key-attempt-$attempt.stdout";if(Select-String -LiteralPath $prior -Pattern ' pass=0(?: |$)| failed=1(?: |$)' -Quiet){throw 'Known partial failure; no unchanged replay'};$attempt++}
    $prefix=Join-Path $evidence "$key-attempt-$attempt";$began=[DateTime]::UtcNow;$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
    $proc=Start-Process -FilePath $frozen -ArgumentList @("--$mode","$Count","$ordinal") -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -PassThru
    $handle=$proc.Handle;$ciInterrupted=$false
    try {while(!$proc.HasExited){if(@(Get-CimInstance Win32_Process -Filter "Name='Runner.Worker.exe'").Count){$proc.Kill();$ciInterrupted=$true;break};Start-Sleep -Milliseconds 200;$proc.Refresh()}}
    catch {$observationError=$_;try{$proc.Kill();$proc.WaitForExit()}catch{Write-Output 'MODAL_FIELD_AUDIO_OWNED_CLEANUP_ERROR'};throw $observationError}
    $proc.WaitForExit();$proc.Refresh();$nativeExit=$proc.ExitCode;if($null -eq $nativeExit){throw 'Native exit unavailable'};$lines=@(Get-Content -LiteralPath "$prefix.stdout")
    [ordered]@{ordinal=$ordinal;row=$row;mode=$mode;attempt=$attempt;exit=$nativeExit;ci_interrupted=$ciInterrupted;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash -LiteralPath "$prefix.stderr").Hash} | ConvertTo-Json | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
    if($ciInterrupted){Write-Output "MODAL_FIELD_AUDIO_INTERRUPTED_CI row=$row mode=$mode attempt=$attempt";exit 3}
    $footer=@($lines | Where-Object {$_ -match "^MODAL_FIELD_AUDIO_FOOTER mode=$mode ordinal=$ordinal row=$row failed=[01] complete=1$"})
    $valid=$footer.Count -eq 1 -and $nativeExit -eq 0 -and $footer[0] -match ' failed=0 '
    [ordered]@{ordinal=$ordinal;row=$row;mode=$mode;attempt=$attempt;exit=$nativeExit;valid=$valid;stdout="$key-attempt-$attempt.stdout";stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;footer=$footer;acceptance='PENDING_READBACK'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $complete -Encoding utf8
    Write-Output "MODAL_FIELD_AUDIO_COMPLETE mode=$mode ordinal=$ordinal row=$row exit=$nativeExit valid=$valid"
    if(!$valid){exit 1}
}
if(!(Test-Path -LiteralPath (Join-Path $evidence 'summary.json'))){[ordered]@{workloads=357;failed=0;acceptance='PENDING_READBACK'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'summary.json') -Encoding utf8}
Write-Output 'MODAL_FIELD_AUDIO_MATRIX_COMPLETE workloads=357 failed=0';exit 0
