param([Parameter(Mandatory=$true)][string]$EvidenceRoot,[switch]$Resume)
$ErrorActionPreference='Stop';$workspace=(Get-Location).Path
if([IO.Path]::IsPathRooted($EvidenceRoot)){throw 'Relative evidence root required'}
$evidence=Join-Path $workspace $EvidenceRoot;$manifestPath=Join-Path $evidence 'manifest.json'
if(!(Test-Path -LiteralPath $evidence)){
 if($Resume){throw 'Missing evidence'};New-Item -ItemType Directory -Path $evidence | Out-Null
 $source=Join-Path $evidence 'source';$files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-ChildItem probes/space-reverb -File)+@(Get-Item src/dsp/space/CMakeLists.txt,CMakeLists.txt,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu8.md)
 $entries=@(foreach($f in $files){$relative=$f.FullName.Substring($workspace.Length+1);$to=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $to) -Force | Out-Null;Copy-Item -LiteralPath $f.FullName -Destination $to;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $to).Hash}})
 Copy-Item -LiteralPath build/space-reverb-feasibility/Release/RackSynthSpaceReverbDirectModalHandoffProbe.exe -Destination (Join-Path $evidence 'ModalCpuFrozen.exe')
 Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1');Copy-Item -LiteralPath tools/space_reverb_modal_cpu8_readback.py -Destination (Join-Path $evidence 'reader.py')
 [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');condition='direct-modal1024-normal-unpaced8';rows=12;warm_seconds=10;measure_seconds=20;deadline='processing_entry_to_exit';kernel_mean_p99_fraction=.5;copy_p99_fraction=.1;binary_sha256=(Get-FileHash -LiteralPath (Join-Path $evidence 'ModalCpuFrozen.exe')).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;reader_sha256=(Get-FileHash -LiteralPath (Join-Path $evidence 'reader.py')).Hash;sources=$entries;acceptance='PENDING_READBACK'} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $manifestPath -Encoding utf8
}elseif(!$Resume){throw 'Evidence exists'}
if(Test-Path -LiteralPath (Join-Path $evidence 'admission-stop1.json')){throw 'Retired evidence'}
$manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json;$frozen=Join-Path $evidence 'ModalCpuFrozen.exe'
if($manifest.condition -ne 'direct-modal1024-normal-unpaced8' -or (Get-FileHash -LiteralPath $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $manifest.controller_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'reader.py')).Hash -ne $manifest.reader_sha256){throw 'Frozen identities changed'}
foreach($f in $manifest.sources){if((Get-FileHash -LiteralPath (Join-Path (Join-Path $evidence 'source') $f.path)).Hash -ne $f.sha256){throw 'Frozen source changed'}}
function Wait-Idle {
 $deadline=[DateTime]::UtcNow.AddMinutes(5);$quiet=$null
 while([DateTime]::UtcNow -lt $deadline){
  $busy=@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -like 'RackSynth*' -or $_.ProcessName -in @('Runner.Worker','cl','link','pluginval','cmake','ctest') -or ($_.ProcessName -ne 'Runner.Listener' -and $_.Path -match '[\\/]ai-rack-synth[\\/]')}).Count -gt 0
  if($busy){$quiet=$null}else{if($null -eq $quiet){$quiet=[DateTime]::UtcNow};if(([DateTime]::UtcNow-$quiet).TotalSeconds -ge 15){return $true}}
  Start-Sleep -Milliseconds 250
 };return $false
}
Write-Output "MODAL_CPU8_ARMED controller_pid=$PID condition=normal_unpaced count=1024 rows=12"
foreach($row in 0..11){
 $key='row-{0:D2}' -f $row;$complete=Join-Path $evidence "$key.complete.json"
 if(Test-Path -LiteralPath $complete){$old=Get-Content -Raw -LiteralPath $complete | ConvertFrom-Json;if($old.exit -ne 0 -or !$old.valid){throw 'Completed failure; no unchanged replay'};continue}
 $attempt=1
 while(Test-Path -LiteralPath (Join-Path $evidence "$key-attempt-$attempt.stdout")){
  $prior=Join-Path $evidence "$key-attempt-$attempt.stdout";if(Select-String -LiteralPath $prior -Pattern '^MODAL_HANDOFF_DEADLINE_MISS ' -Quiet){throw 'Known partial failure; no unchanged replay'}
  $priorMeta=Join-Path $evidence "$key-attempt-$attempt.json"
  if(!(Test-Path -LiteralPath $priorMeta)){throw 'Prior native termination unclassified; inspect before resume'}
  if((Get-Content -Raw -LiteralPath $priorMeta | ConvertFrom-Json).exit -ne 3){throw 'Prior completed native workload; do not replay'}
  $attempt++
 }
 if(!(Wait-Idle)){Write-Output "MODAL_CPU8_DEFERRED row=$row reason=peer_busy";exit 3}
 $prefix=Join-Path $evidence "$key-attempt-$attempt";$began=[DateTime]::UtcNow;$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
 $proc=Start-Process -FilePath $frozen -ArgumentList @('--profile-monitored-cycles',"$row",('"{0}"' -f "$prefix.timings.bin")) -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -PassThru
 $held=$proc.Handle;try{$proc.WaitForExit()}catch{$original=$_;try{$proc.Kill();$proc.WaitForExit()}catch{Write-Output 'MODAL_CPU8_OWNED_CLEANUP_ERROR'};throw $original};$proc.Refresh();if($null -eq $proc.ExitCode){throw 'Native exit unavailable'}
 $raw=@(Get-Content -LiteralPath "$prefix.stdout");$records=@(foreach($extension in @('.stdout','.stderr','.timings.bin','.timings.bin.cycles.bin')){if(Test-Path -LiteralPath "$prefix$extension"){[ordered]@{file="$key-attempt-$attempt$extension";sha256=(Get-FileHash -LiteralPath "$prefix$extension").Hash;bytes=(Get-Item -LiteralPath "$prefix$extension").Length}}})
 [ordered]@{row=$row;attempt=$attempt;exit=$proc.ExitCode;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');records=$records} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
 $results=@($raw | Where-Object {$_ -match "^MODAL_HANDOFF_CPU_RESULT row=$row "});$footer=@($raw | Where-Object {$_ -eq "MODAL_HANDOFF_CPU_FOOTER row=$row complete=1 acceptance=0"});$fields=@{}
 if($results.Count -eq 1){foreach($pair in [regex]::Matches($results[0],'(\S+)=(\S+)')){$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}}
 if($proc.ExitCode -eq 3){
  $known=@($raw | Where-Object {$_ -match '^MODAL_HANDOFF_DEADLINE_MISS '}).Count -gt 0
  if($fields.ContainsKey('samples') -and [long]$fields.samples -gt 0){$known=$known -or [double]$fields.mean_seconds -gt [double]$fields.limit_seconds -or [double]$fields.p99_seconds -gt [double]$fields.limit_seconds -or [double]$fields.copy_p99_seconds -gt [double]$fields.copy_limit_seconds -or [long]$fields.allocations -ne 0 -or [long]$fields.frees -ne 0}
  if($known){[ordered]@{status='RETIRED_KNOWN_PARTIAL_FAILURE';row=$row;attempt=$attempt} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'admission-stop1.json') -Encoding utf8;Write-Output "MODAL_CPU8_FAILED_PARTIAL row=$row";exit 1}
  Write-Output "MODAL_CPU8_INTERRUPTED row=$row attempt=$attempt";exit 3
 }
 $valid=$proc.ExitCode -in @(0,1) -and $results.Count -eq 1 -and $footer.Count -eq 1 -and $fields.domain -eq 'directModal' -and $fields.interrupted -eq '0' -and (($proc.ExitCode -eq 0) -eq ($fields.pass -eq '1'))
 [ordered]@{row=$row;attempt=$attempt;exit=$proc.ExitCode;valid=$valid;fields=$fields;acceptance='PENDING_READBACK'} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $complete -Encoding utf8
 Write-Output "MODAL_CPU8_ROW row=$row exit=$($proc.ExitCode) valid=$valid samples=$($fields.samples) p99=$($fields.p99_seconds) misses=$($fields.misses)"
 if(!$valid -or $proc.ExitCode -ne 0){[ordered]@{status='RETIRED_COMPLETED_FAILURE';row=$row;attempt=$attempt} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'admission-stop1.json') -Encoding utf8;exit 1}
}
Write-Output 'MODAL_CPU8_COMPLETE rows=12';exit 0
