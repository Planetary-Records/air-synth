param([Parameter(Mandatory=$true)][string]$EvidenceRoot,[switch]$Resume,
    [ValidateSet('modal','impulse','self')][string[]]$Modes=@('modal','impulse'),[int[]]$Ordinals=@(0..23))
$ErrorActionPreference='Stop'
$workspace=(Get-Location).Path
$evidence=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$binary=(Resolve-Path 'build/space-reverb-feasibility/Release/RackSynthSpaceReverbProductWitnessProbe.exe').Path
$rows=@(0,20,42,62,168,188,1365,1372,1385,1412,1414,1415,1418,1421,1424,1427,1428,1448,1449,1452,1455,1461,1462,1469)
if(!$Ordinals.Count -or @($Ordinals | Where-Object {$_ -lt 0 -or $_ -ge $rows.Count}).Count -or @($Ordinals | Select-Object -Unique).Count -ne $Ordinals.Count -or @($Modes | Select-Object -Unique).Count -ne $Modes.Count){throw 'Invalid workload selection'}
$workloads=$Ordinals.Count*$Modes.Count
if('self' -in $Modes -and (($Modes -join ',') -ne 'self' -or ($Ordinals -join ',') -ne '0')){throw 'Self test requires only ordinal0/self'}
if(!(Test-Path -LiteralPath $evidence)){
    if($Resume){throw 'Missing evidence'}
    New-Item -ItemType Directory -Path $evidence | Out-Null
    $source=Join-Path $evidence 'source';New-Item -ItemType Directory -Path $source | Out-Null
    $files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-ChildItem probes/space-reverb -File)+@(Get-Item src/dsp/space/CMakeLists.txt,CMakeLists.txt,tests/native/SpaceReverbTankGolden.h,docs/superpowers/plans/2026-10-18-space-reverb-product-corners.md)
    $entries=@(foreach($f in $files){$relative=$f.FullName.Substring($workspace.Length+1);$target=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null;Copy-Item -LiteralPath $f.FullName -Destination $target;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $target).Hash}})
    Copy-Item -LiteralPath $binary -Destination (Join-Path $evidence 'ProductWitnessFrozen.exe')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1')
    [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');scope='CI-exclusive functional modal/raw-impulse witnesses; no CPU acceptance';binary_sha256=(Get-FileHash -LiteralPath $binary).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;rows=$rows;modes=$Modes;ordinals=$Ordinals;sources=$entries} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'manifest.json') -Encoding utf8
}elseif(!$Resume){throw 'Existing evidence requires Resume'}
$manifest=Get-Content -Raw -LiteralPath (Join-Path $evidence 'manifest.json') | ConvertFrom-Json
$frozen=Join-Path $evidence 'ProductWitnessFrozen.exe'
if((Get-FileHash -LiteralPath $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $manifest.controller_sha256){throw 'Frozen identity mismatch'}
if(($manifest.rows -join ',') -ne ($rows -join ',') -or ($manifest.modes -join ',') -ne ($Modes -join ',') -or ($manifest.ordinals -join ',') -ne ($Ordinals -join ',')){throw 'Workload selection mismatch'}
foreach($f in $manifest.sources){if((Get-FileHash -LiteralPath (Join-Path (Join-Path $evidence 'source') $f.path)).Hash -ne $f.sha256){throw 'Frozen source changed'}}
[ordered]@{controller_pid=$PID;created_utc=[DateTime]::UtcNow.ToString('o');script=$PSCommandPath;binary=$frozen;manifest_sha256=(Get-FileHash -LiteralPath (Join-Path $evidence 'manifest.json')).Hash} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence ('controller-owner-'+$PID+'-'+[DateTime]::UtcNow.Ticks+'.json')) -Encoding utf8
Write-Output "WITNESS_CONTROLLER pid=$PID workloads=$workloads"
$failed=0
foreach($mode in $Modes){foreach($ordinal in $Ordinals){
    $row=$rows[$ordinal];$key='{0}-{1:D2}' -f $mode,$ordinal;$complete=Join-Path $evidence "$key.complete.json"
    if(Test-Path -LiteralPath $complete){$old=Get-Content -Raw -LiteralPath $complete | ConvertFrom-Json;if($old.row -ne $row -or $old.ordinal -ne $ordinal -or $old.mode -ne $mode){throw 'Saved workload mismatch'};if($old.exit -ne 0 -or !$old.valid){$failed++};continue}
    $completed=$false
    for($interruption=0;$interruption -lt 3;$interruption++){
        $deadline=[DateTime]::UtcNow.AddMinutes(5)
        while(@(Get-Process -Name Runner.Worker -ErrorAction SilentlyContinue).Count){if([DateTime]::UtcNow -ge $deadline){exit 3};Start-Sleep -Milliseconds 500}
        $attempt=1;while(Test-Path -LiteralPath (Join-Path $evidence "$key-attempt-$attempt.stdout")){$attempt++}
        $prefix=Join-Path $evidence "$key-attempt-$attempt";$began=[DateTime]::UtcNow;$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
        $nativeArgs=if($mode -eq 'self'){@('--self-test')}else{@("--$mode","$ordinal")}
        $proc=Start-Process -FilePath $frozen -ArgumentList $nativeArgs -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -PassThru
        $handle=$proc.Handle;$ciInterrupted=$false
        try {
            while(!$proc.HasExited){
                if(@(Get-Process -Name Runner.Worker -ErrorAction SilentlyContinue).Count){$owned=Get-Process -Id $proc.Id -ErrorAction SilentlyContinue;if($owned){if($owned.Path -ne $frozen){throw 'Owned witness process mismatch'};Stop-Process -Id $proc.Id;$ciInterrupted=$true};break}
                Start-Sleep -Milliseconds 200;$proc.Refresh()
            }
        } catch {
            $observationError=$_
            try {$proc.Kill();$proc.WaitForExit()} catch {Write-Output 'WITNESS_OWNED_CHILD_CLEANUP_ERROR; verify the retained child handle before any further action'}
            throw $observationError
        }
        $proc.WaitForExit();$proc.Refresh();$nativeExit=$proc.ExitCode;if($null -eq $nativeExit){throw 'Native exit unavailable'};$lines=@(Get-Content -LiteralPath "$prefix.stdout")
        [ordered]@{ordinal=$ordinal;row=$row;mode=$mode;attempt=$attempt;exit=$nativeExit;ci_interrupted=$ciInterrupted;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash -LiteralPath "$prefix.stderr").Hash} | ConvertTo-Json | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
        if($ciInterrupted){
            if(@($lines | Where-Object {$_ -match '^WITNESS_(REGION|IMPULSE|PREPARATION) .*pass=0(?: |$)'}).Count){Write-Output "KNOWN_WITNESS_FAILURE_BEFORE_CI_STOP row=$row mode=$mode; no unchanged replay";exit 1}
            Write-Output "WITNESS_INTERRUPTED_CI row=$row mode=$mode attempt=$attempt";continue
        }
        $footer=@($lines | Where-Object {$_ -match "^WITNESS_FOOTER ordinal=$ordinal row=$row mode=$mode regions=[0-9]+ failed=[0-9]+ complete=1$"})
        if($mode -eq 'self'){$footer=@($lines | Where-Object {$_ -match '^WITNESS_SELF checks=[0-9]+ failures=[0-9]+$'})}
        $valid=$nativeExit -in @(0,1) -and $footer.Count -eq 1
        if($valid){$fields=@{};foreach($pair in [regex]::Matches($footer[0],'(\S+)=(\S+)')){$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value};$failureCount=if($mode -eq 'self'){[int]$fields.failures}else{[int]$fields.failed};$valid=($failureCount -eq 0) -eq ($nativeExit -eq 0)}
        [ordered]@{ordinal=$ordinal;row=$row;mode=$mode;attempt=$attempt;exit=$nativeExit;valid=$valid;stdout="$key-attempt-$attempt.stdout";stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;footer=$footer;acceptance='PENDING_READBACK'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $complete -Encoding utf8
        $completed=$true;if($nativeExit -ne 0 -or !$valid){$failed++};Write-Output "WITNESS_COMPLETE ordinal=$ordinal row=$row mode=$mode exit=$nativeExit valid=$valid";break
    }
    if(!$completed){exit 3}
}}
$summary=Join-Path $evidence 'summary.json';if(!(Test-Path -LiteralPath $summary)){[ordered]@{workloads=$workloads;failed=$failed;acceptance='PENDING_READBACK'} | ConvertTo-Json | Set-Content -LiteralPath $summary -Encoding utf8}
Write-Output "WITNESS_MATRIX_COMPLETE workloads=$workloads failed=$failed";exit $(if($failed){1}else{0})
