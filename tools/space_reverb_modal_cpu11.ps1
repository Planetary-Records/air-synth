param([string]$EvidenceRoot,[switch]$Resume,[string]$ResumeAuditFile,[switch]$SelfTest)
$ErrorActionPreference='Stop'
function Resolve-NativeOutcome([string[]]$Raw,[int]$NativeExit,[int]$Row){
 $invalid=[pscustomobject]@{valid=$false;kind='invalid';fields=@{}}
 $results=@($Raw | Where-Object {$_ -match "^SERVICE_CPU10_RESULT row=$Row "});$footers=@($Raw | Where-Object {$_ -match "^SERVICE_CPU10_FOOTER row=$Row "})
 if($results.Count -ne 1 -or $footers.Count -ne 1){return $invalid}
 $fields=@{};$footer=@{}
 foreach($pair in [regex]::Matches($results[0],'(\S+)=(\S+)')){if($fields.ContainsKey($pair.Groups[1].Value)){return $invalid};$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}
 foreach($pair in [regex]::Matches($footers[0],'(\S+)=(\S+)')){if($footer.ContainsKey($pair.Groups[1].Value)){return $invalid};$footer[$pair.Groups[1].Value]=$pair.Groups[2].Value}
 if($fields.phase -notin @('admission','workload') -or $fields.pass -notin @('0','1') -or $fields.known_failure -notin @('0','1') -or $fields.interrupted -notin @('0','1') -or $footer.complete -notin @('0','1') -or $footer.acceptance -ne '0'){return $invalid}
 if($NativeExit -eq 0 -and $fields.pass -eq '1' -and $fields.known_failure -eq '0' -and $fields.interrupted -eq '0' -and $footer.complete -eq '1'){return [pscustomobject]@{valid=$true;kind='passed';fields=$fields}}
 if($fields.pass -eq '0' -and ($NativeExit -eq 1 -or ($NativeExit -eq 3 -and $fields.known_failure -eq '1'))){return [pscustomobject]@{valid=$true;kind='failed';fields=$fields}}
 if($NativeExit -eq 3 -and $fields.pass -eq '0' -and $fields.interrupted -eq '1' -and $footer.complete -eq '0'){return [pscustomobject]@{valid=$true;kind='interrupted';fields=$fields}}
 return $invalid
}
if($SelfTest){
 $checks=0;$failed=0
 function Check([bool]$Value,[string]$Label){$script:checks++;if(!$Value){$script:failed++;Write-Output "CPU10_CONTROLLER_FAIL $Label"}}
 $pass=@('SERVICE_CPU10_RESULT row=0 phase=workload pass=1 known_failure=0 interrupted=0 samples=1 p99_seconds=0.1 kernel_overruns=0','SERVICE_CPU10_FOOTER row=0 complete=1 acceptance=0')
 $out=Resolve-NativeOutcome $pass 0 0;Check ($out.valid -and $out.kind -eq 'passed') 'completed_pass'
 $bad=@('SERVICE_CPU10_RESULT row=0 phase=workload pass=0 known_failure=1 interrupted=0 samples=1 p99_seconds=0.1 kernel_overruns=1','SERVICE_CPU10_FOOTER row=0 complete=1 acceptance=0')
 $out=Resolve-NativeOutcome $bad 1 0;Check ($out.valid -and $out.kind -eq 'failed') 'completed_failure'
 $partial=@('SERVICE_CPU10_RESULT row=0 phase=workload pass=0 known_failure=1 interrupted=1 samples=1 p99_seconds=0.1 kernel_overruns=1','SERVICE_CPU10_FOOTER row=0 complete=0 acceptance=0')
 $out=Resolve-NativeOutcome $partial 3 0;Check ($out.valid -and $out.kind -eq 'failed') 'known_partial_failure'
 $peer=@('SERVICE_CPU10_RESULT row=0 phase=admission pass=0 known_failure=0 interrupted=1 samples=0 interrupt_reason=peer_native_work','SERVICE_CPU10_FOOTER row=0 complete=0 acceptance=0')
 $out=Resolve-NativeOutcome $peer 3 0;Check ($out.valid -and $out.kind -eq 'interrupted') 'unmeasured_interruption_needs_audit'
 $out=Resolve-NativeOutcome ($pass+@($pass[0])) 0 0;Check (!$out.valid) 'duplicate_result'
 $out=Resolve-NativeOutcome @() 2 0;Check (!$out.valid) 'unknown_native_exit'
 $out=Resolve-NativeOutcome $bad 0 0;Check (!$out.valid) 'exit_flag_disagreement'
 $out=Resolve-NativeOutcome $pass 0 1;Check (!$out.valid) 'row_disagreement'
 Write-Output "CPU10_CONTROLLER_MODEL checks=$checks failures=$failed actual_timing=0"
 if($failed){exit 1};exit 0
}

if(!$EvidenceRoot -or [IO.Path]::IsPathRooted($EvidenceRoot) -or $EvidenceRoot -notmatch '^[a-z0-9][a-z0-9/-]*$'){throw 'A task-relative evidence root is required'}
$workspace=(Get-Location).Path;$evidence=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
if(!$evidence.StartsWith($workspace+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Evidence outside workspace'}
$manifestPath=Join-Path $evidence 'manifest.json';$binaryName='RackSynthSpaceReverbModalServiceProfileProbe.exe';$condition='complete-modal1024-periodic-proaudio-processing11'
if(!(Test-Path -LiteralPath $evidence)){
 if($Resume){throw 'Missing frozen evidence'}
 $qualificationPath=Join-Path $workspace 'build/space-reverb-product-tank-20261017/cpu11-tooling-qualification1.json'
 $qualification=Get-Content -Raw -LiteralPath $qualificationPath | ConvertFrom-Json
 $built=Join-Path $workspace 'build/space-reverb-feasibility/Release/RackSynthSpaceReverbModalServiceProfileProbe.exe'
 $readerSource=Join-Path $workspace 'tools/space_reverb_modal_cpu11_readback.py'
 if($qualification.status -ne 'PASS_CPU11_TOOLING' -or (Get-FileHash -LiteralPath $built).Hash -ne $qualification.probe_binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $qualification.controller_sha256 -or (Get-FileHash -LiteralPath $readerSource).Hash -ne $qualification.reader_sha256){throw 'Unqualified or changed timing tools'}
 New-Item -ItemType Directory -Path $evidence | Out-Null;$source=Join-Path $evidence 'source'
 $files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-ChildItem probes/space-reverb -File)+@(Get-Item src/dsp/space/CMakeLists.txt,CMakeLists.txt,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu10.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu11.md)
 $entries=@(foreach($file in $files){$relative=$file.FullName.Substring($workspace.Length+1);$to=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $to) -Force | Out-Null;Copy-Item -LiteralPath $file.FullName -Destination $to;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $to).Hash}})
 Copy-Item -LiteralPath $built -Destination (Join-Path $evidence $binaryName);Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1');Copy-Item -LiteralPath $readerSource -Destination (Join-Path $evidence 'reader.py');Copy-Item -LiteralPath $qualificationPath -Destination (Join-Path $evidence 'qualification.json')
 $cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
 [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');condition=$condition;rows=24;warm_seconds=10;measure_seconds=20;deadline='processing_entry_to_exit';kernel_mean_p99_fraction=.5;copy_p99_fraction=.1;cpu=$cpu;binary_sha256=(Get-FileHash -LiteralPath $built).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;reader_sha256=(Get-FileHash -LiteralPath $readerSource).Hash;qualification_sha256=(Get-FileHash -LiteralPath $qualificationPath).Hash;sources=$entries;acceptance='PENDING_INDEPENDENT_READBACK'} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $manifestPath -Encoding utf8
}elseif(!$Resume){throw 'Evidence exists; do not replay'}
if(Test-Path -LiteralPath (Join-Path $evidence 'admission-stop1.json')){throw 'Retired timing condition; no unchanged replay'}
$manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json;$frozen=Join-Path $evidence $binaryName
if($manifest.condition -ne $condition -or (Get-FileHash -LiteralPath $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $manifest.controller_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'controller.ps1')).Hash -ne $manifest.controller_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'reader.py')).Hash -ne $manifest.reader_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'qualification.json')).Hash -ne $manifest.qualification_sha256){throw 'Frozen identity mismatch'}
foreach($entry in $manifest.sources){if((Get-FileHash -LiteralPath (Join-Path (Join-Path $evidence 'source') $entry.path)).Hash -ne $entry.sha256){throw 'Frozen source mismatch'}}
$accepted=@()
if($Resume){
 if($ResumeAuditFile -notmatch '^resume-audit-[0-9]+\.json$'){throw 'Independent resume audit filename is required'}
 $audit=Get-Content -Raw -LiteralPath (Join-Path $evidence $ResumeAuditFile) | ConvertFrom-Json
 if(!$audit.eligible -or $audit.manifest_sha256 -ne (Get-FileHash -LiteralPath $manifestPath).Hash -or $audit.reader_sha256 -ne $manifest.reader_sha256){throw 'Independent audit rejects resume'}
 $inventory=@(Get-ChildItem -LiteralPath $evidence -File -Filter 'row-*')
 if($inventory.Count -ne @($audit.inventory).Count){throw 'Native inventory changed after resume audit'}
 foreach($file in $inventory){$binding=@($audit.inventory | Where-Object {$_.file -ceq $file.Name});if($binding.Count -ne 1 -or $binding[0].sha256 -ne (Get-FileHash -LiteralPath $file.FullName).Hash){throw 'Unaudited native evidence; do not resume'}}
 $accepted=@($audit.accepted_rows)
}
function Wait-NativeIdle([bool]$RequireQuiet){
 $deadline=[DateTime]::UtcNow.AddMinutes(5);$quiet=$null;$seconds=0;if($RequireQuiet){$seconds=15}
 while([DateTime]::UtcNow -lt $deadline){
  $busy=@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -like 'RackSynth*' -or $_.ProcessName -in @('Runner.Worker','cl','link','pluginval','cmake','ctest') -or ($_.ProcessName -ne 'Runner.Listener' -and $_.Path -match '[\\/]ai-rack-synth[\\/]')}).Count -gt 0
  if($busy){$quiet=$null;$seconds=15}else{if($null -eq $quiet){$quiet=[DateTime]::UtcNow};if(([DateTime]::UtcNow-$quiet).TotalSeconds -ge $seconds){return $true}}
  Start-Sleep -Milliseconds 250
 };return $false
}
Write-Output "SERVICE_CPU11_ARMED controller_pid=$PID condition=$condition rows=24"
$initial=$true
foreach($row in 0..23){
 if($accepted -contains $row){continue}
 $key='row-{0:D2}' -f $row;$complete=Join-Path $evidence "$key.complete.json"
 if(Test-Path -LiteralPath $complete){throw 'Completed marker not independently accepted; do not replay'}
 $attempt=1
 while(Test-Path -LiteralPath (Join-Path $evidence "$key-attempt-$attempt.stdout")){if(!$Resume){throw 'Existing attempt requires independent audit'};$attempt++}
 if(!(Wait-NativeIdle $initial)){Write-Output "SERVICE_CPU11_DEFERRED row=$row reason=peer_busy";exit 3};$initial=$false
 $prefix=Join-Path $evidence "$key-attempt-$attempt";$began=[DateTime]::UtcNow;$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
 $proc=Start-Process -FilePath $frozen -ArgumentList @('--profile',"$row",('"{0}"' -f "$prefix.timings.bin")) -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -PassThru
 $handle=$proc.Handle
 try{$proc.WaitForExit()}catch{$original=$_;try{$proc.Kill();$proc.WaitForExit()}catch{Write-Output 'SERVICE_CPU11_OWNED_CLEANUP_ERROR'};throw $original}
 $proc.Refresh();if($null -eq $proc.ExitCode){throw 'Native exit unavailable'}
 $records=@(foreach($extension in @('.stdout','.stderr','.timings.bin')){if(Test-Path -LiteralPath "$prefix$extension"){[ordered]@{file="$key-attempt-$attempt$extension";sha256=(Get-FileHash -LiteralPath "$prefix$extension").Hash;bytes=(Get-Item -LiteralPath "$prefix$extension").Length}}})
 [ordered]@{row=$row;attempt=$attempt;exit=$proc.ExitCode;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');records=$records} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
 $outcome=Resolve-NativeOutcome @(Get-Content -LiteralPath "$prefix.stdout") $proc.ExitCode $row;$fields=$outcome.fields
 if($outcome.kind -eq 'interrupted'){Write-Output "SERVICE_CPU11_INTERRUPTED row=$row attempt=$attempt reason=$($fields.interrupt_reason) independent_audit_required=1";exit 3}
 [ordered]@{row=$row;attempt=$attempt;exit=$proc.ExitCode;valid=$outcome.valid;kind=$outcome.kind;fields=$fields;acceptance='PENDING_INDEPENDENT_READBACK'} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $complete -Encoding utf8
 Write-Output "SERVICE_CPU11_ROW row=$row exit=$($proc.ExitCode) valid=$($outcome.valid) samples=$($fields.samples) p99=$($fields.p99_seconds) copy_p99=$($fields.copy_p99_seconds) kernel_overruns=$($fields.kernel_overruns) dispatch_misses=$($fields.dispatch_misses)"
 if(!$outcome.valid -or $outcome.kind -ne 'passed'){[ordered]@{status='RETIRED_NATIVE_FAILURE';row=$row;attempt=$attempt;native_exit=$proc.ExitCode;known_failure=$fields.known_failure} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'admission-stop1.json') -Encoding utf8;exit 1}
}
Write-Output 'SERVICE_CPU11_COMPLETE rows=24 independent_readback_pending=1';exit 0

