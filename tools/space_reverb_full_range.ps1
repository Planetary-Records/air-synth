param([Parameter(Mandatory=$true)][string]$EvidenceRoot,[switch]$Resume)
$ErrorActionPreference='Stop'
$workspace=(Get-Location).Path;$evidence=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$rows=@(0..1469)
if(!$rows.Count){throw 'Expected full coverage rows'}
for($i=0;$i -lt $rows.Count;$i++){if($rows[$i] -lt 0 -or $rows[$i] -ge 1470 -or ($i -and $rows[$i] -le $rows[$i-1])){throw 'Invalid row order'}}
$binary=(Resolve-Path 'build/space-reverb-feasibility/Release/RackSynthSpaceReverbRangeProbe.exe').Path
if(!(Test-Path -LiteralPath $evidence)){
    if($Resume){throw 'Missing evidence'}
    New-Item -ItemType Directory -Path $evidence | Out-Null
    $source=Join-Path $evidence 'source';New-Item -ItemType Directory -Path $source | Out-Null
    $files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-Item src/dsp/space/CMakeLists.txt,probes/space-reverb/SpaceReverbRangeProbe.cpp,probes/space-reverb/ProbeCases.h,probes/space-reverb/ProbePeerGuard.h,probes/space-reverb/CMakeLists.txt,tests/native/SpaceReverbEndpointTests.cpp,tests/native/SpaceReverbTankTests.cpp,tests/native/SpaceReverbTankGolden.h,CMakeLists.txt)
    $entries=@(foreach($f in $files){$relative=$f.FullName.Substring($workspace.Length+1);$target=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null;Copy-Item -LiteralPath $f.FullName -Destination $target;[ordered]@{path=$relative;sha256=(Get-FileHash $target).Hash}})
    Copy-Item $binary (Join-Path $evidence 'RackSynthSpaceReverbFullRangeFrozen.exe')
    Copy-Item $PSCommandPath (Join-Path $evidence 'controller.ps1')
    [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');scope='CI exclusive functional preparation/storage only; no CPU/time acceptance';binary_sha256=(Get-FileHash $binary).Hash;controller_sha256=(Get-FileHash $PSCommandPath).Hash;rows=$rows;row_selection='Complete70curves_x7rates_x3sizes';sources=$entries} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $evidence 'manifest.json')
}elseif(!$Resume){throw 'Existing evidence requires Resume'}
$manifest=Get-Content -Raw (Join-Path $evidence 'manifest.json') | ConvertFrom-Json
$frozen=Join-Path $evidence 'RackSynthSpaceReverbFullRangeFrozen.exe'
if((Get-FileHash $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash $PSCommandPath).Hash -ne $manifest.controller_sha256){throw 'Frozen identity mismatch'}
foreach($f in $manifest.sources){if((Get-FileHash (Join-Path (Join-Path $evidence 'source') $f.path)).Hash -ne $f.sha256){throw 'Source snapshot changed'}}
[ordered]@{controller_pid=$PID;created_utc=[DateTime]::UtcNow.ToString('o');script=$PSCommandPath;binary=$frozen;manifest_sha256=(Get-FileHash (Join-Path $evidence 'manifest.json')).Hash;scope='Owned functional controller only; no timing acceptance'} | ConvertTo-Json | Set-Content (Join-Path $evidence ('controller-owner-'+$PID+'-'+[DateTime]::UtcNow.Ticks+'.json'))
Write-Output "MATRIX_CONTROLLER pid=$PID rows=$($rows.Count)"
$failed=0
foreach($row in $rows){
    $key='row-{0:D4}' -f $row;$complete=Join-Path $evidence "$key.complete.json"
    if(Test-Path $complete){$old=Get-Content -Raw $complete | ConvertFrom-Json;if($old.row -ne $row){throw 'Saved row mismatch'};if($old.exit -ne 0){$failed++};continue}
    $completed=$false
    for($interruption=0;$interruption -lt 3;$interruption++){
        $deadline=[DateTime]::UtcNow.AddMinutes(5)
        while(@(Get-Process -Name Runner.Worker -ErrorAction SilentlyContinue).Count){if([DateTime]::UtcNow -ge $deadline){exit 3};Start-Sleep -Milliseconds 500}
        $attempt=1;while(Test-Path (Join-Path $evidence "$key-attempt-$attempt.stdout")){$attempt++}
        $prefix=Join-Path $evidence "$key-attempt-$attempt";$began=[DateTime]::UtcNow;$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
        $proc=Start-Process -FilePath $frozen -ArgumentList @('--row-functional',"$row") -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -PassThru
        $handle=$proc.Handle;$ciInterrupted=$false
        try {
            while(!$proc.HasExited){
                if(@(Get-Process -Name Runner.Worker -ErrorAction SilentlyContinue).Count){$owned=Get-Process -Id $proc.Id -ErrorAction SilentlyContinue;if($owned){if($owned.Path -ne $frozen){throw 'Owned range process mismatch'};Stop-Process -Id $proc.Id;$ciInterrupted=$true};break}
                Start-Sleep -Milliseconds 200;$proc.Refresh()
            }
        } catch {
            $observationError=$_
            try {$proc.Kill();$proc.WaitForExit()} catch {Write-Output 'MATRIX_OWNED_CHILD_CLEANUP_ERROR; verify the retained child handle before any further action'}
            throw $observationError
        }
        $proc.WaitForExit();$proc.Refresh();$nativeExit=$proc.ExitCode;if($null -eq $nativeExit){throw 'Native exit unavailable'};$lines=@(Get-Content "$prefix.stdout")
        [ordered]@{row=$row;attempt=$attempt;exit=$nativeExit;ci_interrupted=$ciInterrupted;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash "$prefix.stderr").Hash} | ConvertTo-Json | Set-Content "$prefix.json"
        if($ciInterrupted){
            if(@($lines | Where-Object {$_ -match "^RANGE_ROW row=$row .*pass=0 "}).Count){Write-Output "KNOWN_FUNCTIONAL_FAILURE_BEFORE_CI_STOP row=$row; no unchanged replay";exit 1}
            Write-Output "MATRIX_INTERRUPTED_CI row=$row attempt=$attempt";continue
        }
        if($nativeExit -eq 3){Write-Output "MATRIX_INTERRUPTED row=$row attempt=$attempt";continue}
        $result=@($lines | Where-Object {$_ -match "^RANGE_ROW row=$row "});$footer=@($lines | Where-Object {$_ -match "^RANGE_FOOTER start=$row completed=1 failed=[01] complete=1$"})
        if($nativeExit -notin @(0,1) -or $result.Count -ne 1 -or $footer.Count -ne 1){throw "Incomplete native record row=$row exit=$nativeExit"}
        $fields=[ordered]@{};foreach($pair in [regex]::Matches($result[0],'(\S+)=(\S+)')){$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}
        if(([int]$fields.pass -eq 1) -ne ($nativeExit -eq 0)){throw 'Pass/exit mismatch'}
        if([int]$fields.curve -ne [Math]::Floor($row/21) -or [double]$fields.rate -ne @(8000,44100,48000,96000,192000,384000,768000)[[Math]::Floor(($row%21)/3)] -or [double]$fields.size -ne @(0,.5,1)[$row%3]){throw 'Workload mismatch'}
        [ordered]@{row=$row;attempt=$attempt;exit=$nativeExit;fields=$fields;stdout="$key-attempt-$attempt.stdout";stdout_sha256=(Get-FileHash "$prefix.stdout").Hash} | ConvertTo-Json -Depth 5 | Set-Content $complete
        $completed=$true;if($nativeExit){$failed++};Write-Output "MATRIX_ROW row=$row pass=$($fields.pass) error=$($fields.contour_error) reason=$($fields.reason)";break
    }
    if(!$completed){exit 3}
}
$summary=Join-Path $evidence 'summary.json';if(!(Test-Path $summary)){[ordered]@{rows=$rows.Count;failed=$failed;acceptance='PENDING_READBACK'} | ConvertTo-Json | Set-Content $summary}
Write-Output "MATRIX_COMPLETE rows=$($rows.Count) failed=$failed";exit $(if($failed){1}else{0})
