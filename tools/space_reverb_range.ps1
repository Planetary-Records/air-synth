param([Parameter(Mandatory=$true)][string]$EvidenceRoot,[switch]$Resume)
$ErrorActionPreference='Stop'
$workspace=(Get-Location).Path
$evidence=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$binary=(Resolve-Path 'build/space-reverb-feasibility/Release/RackSynthSpaceReverbRangeProbe.exe').Path
if(!(Test-Path -LiteralPath $evidence)){
    if($Resume){throw 'Missing evidence'}
    New-Item -ItemType Directory -Path $evidence | Out-Null
    $source=Join-Path $evidence 'source';New-Item -ItemType Directory -Path $source | Out-Null
    $files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-Item src/dsp/space/CMakeLists.txt,probes/space-reverb/SpaceReverbRangeProbe.cpp,probes/space-reverb/ProbeCases.h,probes/space-reverb/ProbePeerGuard.h,probes/space-reverb/CMakeLists.txt,tests/native/SpaceReverbTankTests.cpp,tests/native/SpaceReverbTankGolden.h,CMakeLists.txt)
    $entries=@(foreach($f in $files){$relative=$f.FullName.Substring($workspace.Length+1);$target=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null;Copy-Item -LiteralPath $f.FullName -Destination $target;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $target).Hash}})
    Copy-Item -LiteralPath $binary -Destination (Join-Path $evidence 'RackSynthSpaceReverbRangeFrozen.exe')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1')
    [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');binary_sha256=(Get-FileHash -LiteralPath $binary).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;rows=1470;sources=$entries} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $evidence 'manifest.json')
}elseif(!$Resume){throw 'Evidence exists; explicit Resume required'}
$manifest=Get-Content -Raw (Join-Path $evidence 'manifest.json') | ConvertFrom-Json
$frozen=Join-Path $evidence 'RackSynthSpaceReverbRangeFrozen.exe'
if((Get-FileHash $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash $PSCommandPath).Hash -ne $manifest.controller_sha256){throw 'Frozen identity mismatch'}
foreach($f in $manifest.sources){if((Get-FileHash (Join-Path (Join-Path $evidence 'source') $f.path)).Hash -ne $f.sha256){throw 'Source snapshot changed'}}
$cursor=0;$failed=0
foreach($record in @(Get-ChildItem -LiteralPath $evidence -Filter 'row-*.json' -File | Sort-Object Name)){$v=Get-Content -Raw $record.FullName | ConvertFrom-Json;if($v.row -ne $cursor){throw 'Non-monotonic cursor'};if($v.fields.pass -ne '1'){$failed++};$cursor++}
if($cursor -eq 1470){Write-Output "RANGE_ALREADY_COMPLETE rows=$cursor failed=$failed";exit $(if($failed){1}else{0})}
for($interruption=0;$interruption -lt 4;$interruption++){
    $deadline=[DateTime]::UtcNow.AddMinutes(5)
    while(@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -like 'RackSynth*' -or $_.ProcessName -in @('Runner.Worker','cl','link','pluginval') -or ($_.ProcessName -ne 'Runner.Listener' -and $_.Path -match '[\\/]ai-rack-synth[\\/]')}).Count){if([DateTime]::UtcNow -ge $deadline){exit 3};Start-Sleep -Milliseconds 500}
    $attempt=1;while(Test-Path (Join-Path $evidence "attempt-$attempt.stdout")){$attempt++}
    $prefix=Join-Path $evidence "attempt-$attempt";$start=$cursor;$began=[DateTime]::UtcNow
    $env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
    $proc=Start-Process -FilePath $frozen -ArgumentList @('--range',"$cursor") -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -Wait -PassThru
    $nativeExit=$proc.ExitCode
    foreach($line in @(Get-Content "$prefix.stdout" | Where-Object {$_ -match '^RANGE_ROW '})){
        $fields=[ordered]@{};foreach($pair in [regex]::Matches($line,'(\S+)=(\S+)')){$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}
        if([int]$fields.row -ne $cursor){throw 'Native cursor mismatch'}
        $path=Join-Path $evidence ('row-{0:D4}.json' -f $cursor);if(Test-Path $path){throw 'Existing completed row'}
        [ordered]@{row=$cursor;attempt=$attempt;fields=$fields;stdout_sha256=(Get-FileHash "$prefix.stdout").Hash} | ConvertTo-Json -Depth 5 | Set-Content $path
        if($fields.pass -ne '1'){$failed++};$cursor++
    }
    [ordered]@{started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');start=$start;next=$cursor;exit=$nativeExit;stdout_sha256=(Get-FileHash "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash "$prefix.stderr").Hash} | ConvertTo-Json | Set-Content "$prefix.json"
    if($cursor -eq 1470){if($nativeExit -notin @(0,1) -or !@(Get-Content "$prefix.stdout" | Where-Object {$_ -match '^RANGE_FOOTER .*complete=1$'}).Count){throw 'Missing complete footer'};[ordered]@{rows=$cursor;failed=$failed;acceptance='PENDING_READBACK'} | ConvertTo-Json | Set-Content (Join-Path $evidence 'summary.json');Write-Output "RANGE_COMPLETE rows=$cursor failed=$failed";exit $(if($failed){1}else{0})}
    if($nativeExit -ne 3){throw "Incomplete native run exit=$nativeExit next=$cursor"}
    Write-Output "RANGE_INTERRUPTED next=$cursor"
}
exit 3
