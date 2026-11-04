param([string]$EvidenceRoot,[switch]$Resume,[string]$ResumeAuditFile,[switch]$SelfTest)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
function Test-NativeBusyWhileExclusive([string]$Name,[string]$ImagePath){
 $ciCoordinator=$Name -eq 'coordination-launch' -and $ImagePath -match '[\\/]ai-rack-synth[\\/]build[\\/]ci[\\/]coordination-launch\.exe$'
 return (($Name -like 'RackSynth*' -and $Name -ne 'RackSynthNativeRun') -or $Name -in @('cl','link','pluginval','cmake') -or ($Name -notin @('Runner.Listener','Runner.Worker','ctest','RackSynthNativeRun') -and !$ciCoordinator -and $ImagePath -match '[\\/]ai-rack-synth[\\/]'))
}
function Test-PowerCondition([string[]]$Raw,[hashtable]$Fields){
 $lines=@($Raw | Where-Object {$_ -match '^SERVICE_CPU16_POWER '});if($lines.Count -ne 1){return $false}
 $power=@{};foreach($token in ($lines[0] -split '\s+' | Select-Object -Skip 1)){if($token -notmatch '^([^=]+)=([^=]+)$' -or $power.ContainsKey($Matches[1])){return $false};$power[$Matches[1]]=$Matches[2]}
 if($power.Count -ne 16){return $false}
 foreach($key in @('attempted','opened','restored','close1','close2')){if($power[$key] -notin @('0','1')){return $false}}
 if($power.setup_error -ne '0' -or $power.cleanup_error -ne '0' -or $power.close1 -ne '1' -or $power.close2 -ne '1'){return $false}
 $values=@{};try{foreach($prefix in @('before','during','after')){foreach($key in @('version','control','state')){$name=$prefix+'_'+$key;if(!$power.ContainsKey($name) -or $power[$name] -notmatch '^\d+$'){return $false};$values[$name]=[uint32]::Parse($power[$name])}}}catch{return $false}
 if($power.attempted -eq '0'){
  return $Fields.phase -eq 'admission' -and $Fields.interrupt_reason -in @('peer_native_work','process_inventory_unavailable','inventory_monitor_stale') -and $Fields.power_ok -eq '0' -and $power.opened -eq '0' -and $power.restored -eq '0' -and @($values.Values | Where-Object {$_ -ne 0}).Count -eq 0
 }
 return $Fields.power_ok -eq '1' -and $power.opened -eq '1' -and $power.restored -eq '1' -and $values.before_version -eq 1 -and $values.during_version -eq 1 -and $values.after_version -eq 1 -and $values.during_control -eq ($values.before_control -bor 1) -and $values.during_state -eq ($values.before_state -band [uint32]4294967294) -and $values.after_control -eq $values.before_control -and $values.after_state -eq $values.before_state
}
function Resolve-NativeOutcome([string[]]$Raw,[int]$NativeExit,[int]$Row){
 $invalid=[pscustomobject]@{valid=$false;kind='invalid';fields=@{}}
 $results=@($Raw | Where-Object {$_ -match "^SERVICE_CPU10_RESULT row=$Row "});$footers=@($Raw | Where-Object {$_ -match "^SERVICE_CPU10_FOOTER row=$Row "})
 if($results.Count -ne 1 -or $footers.Count -ne 1){return $invalid}
 $fields=@{};$footer=@{}
 foreach($pair in [regex]::Matches($results[0],'(\S+)=(\S+)')){if($fields.ContainsKey($pair.Groups[1].Value)){return $invalid};$fields[$pair.Groups[1].Value]=$pair.Groups[2].Value}
 foreach($pair in [regex]::Matches($footers[0],'(\S+)=(\S+)')){if($footer.ContainsKey($pair.Groups[1].Value)){return $invalid};$footer[$pair.Groups[1].Value]=$pair.Groups[2].Value}
 if(($fields.phase -eq 'workload' -and $fields.modes -ne '128') -or ($fields.phase -eq 'admission' -and $fields.modes -notin @('0','128'))){return $invalid}
 if(($fields.phase -eq 'workload' -and $fields.copy_mode -ne 'prepared') -or ($fields.phase -eq 'admission' -and !(($fields.modes -eq '0' -and $fields.copy_mode -eq 'none') -or ($fields.modes -eq '128' -and $fields.copy_mode -eq 'prepared')))){return $invalid}
 if(($fields.phase -eq 'workload' -and $fields.scatter_mode -ne 'blocked') -or ($fields.phase -eq 'admission' -and !(($fields.modes -eq '0' -and $fields.scatter_mode -eq 'none') -or ($fields.modes -eq '128' -and $fields.scatter_mode -eq 'blocked')))){return $invalid}
 if(($fields.phase -eq 'workload' -and $fields.reduction_mode -ne 'paired_stereo') -or ($fields.phase -eq 'admission' -and !(($fields.modes -eq '0' -and $fields.reduction_mode -eq 'none') -or ($fields.modes -eq '128' -and $fields.reduction_mode -eq 'paired_stereo')))){return $invalid}
 $factor=[string](1+[int][Math]::Floor(($Row%12)/6))
 $active=$fields.converter_mode -eq 'mirrored_avx2' -and $fields.converter_slots -eq '8' -and $fields.converter_factor -eq $factor
 $none=$fields.converter_mode -eq 'none' -and $fields.converter_slots -eq '0' -and $fields.converter_factor -eq '0'
 if(($fields.phase -eq 'workload' -and !$active) -or ($fields.phase -eq 'admission' -and !(($fields.modes -eq '0' -and $none) -or ($fields.modes -eq '128' -and $active)))){return $invalid}
 if(!(Test-PowerCondition $Raw $fields)){return $invalid}
 if($fields.phase -notin @('admission','workload') -or $fields.pass -notin @('0','1') -or $fields.known_failure -notin @('0','1') -or $fields.interrupted -notin @('0','1') -or $footer.complete -notin @('0','1') -or $footer.acceptance -ne '0'){return $invalid}
 if($NativeExit -eq 0 -and $fields.pass -eq '1' -and $fields.known_failure -eq '0' -and $fields.interrupted -eq '0' -and $footer.complete -eq '1'){return [pscustomobject]@{valid=$true;kind='passed';fields=$fields}}
 if($fields.pass -eq '0' -and ($NativeExit -eq 1 -or ($NativeExit -eq 3 -and $fields.known_failure -eq '1'))){return [pscustomobject]@{valid=$true;kind='failed';fields=$fields}}
 if($NativeExit -eq 3 -and $fields.pass -eq '0' -and $fields.interrupted -eq '1' -and $footer.complete -eq '0'){return [pscustomobject]@{valid=$true;kind='interrupted';fields=$fields}}
 return $invalid
}
if($SelfTest){
 $checks=0;$failed=0
 function Check([bool]$Value,[string]$Label){$script:checks++;if(!$Value){$script:failed++;Write-Output "CPU10_CONTROLLER_FAIL $Label"}}
 $power='SERVICE_CPU16_POWER attempted=1 opened=1 restored=1 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=1 before_control=4 before_state=4 during_version=1 during_control=5 during_state=4 after_version=1 after_control=4 after_state=4'
 $untouched='SERVICE_CPU16_POWER attempted=0 opened=0 restored=0 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=0 before_control=0 before_state=0 during_version=0 during_control=0 during_state=0 after_version=0 after_control=0 after_state=0'
 $pass=@($power,'SERVICE_CPU10_RESULT row=0 phase=workload modes=128 power_ok=1 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor=1 pass=1 known_failure=0 interrupted=0 samples=1 p99_seconds=0.1 kernel_overruns=0','SERVICE_CPU10_FOOTER row=0 complete=1 acceptance=0')
 $out=Resolve-NativeOutcome $pass 0 0;Check ($out.valid -and $out.kind -eq 'passed') 'completed_pass'
 $bad=@($power,'SERVICE_CPU10_RESULT row=0 phase=workload modes=128 power_ok=1 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor=1 pass=0 known_failure=1 interrupted=0 samples=1 p99_seconds=0.1 kernel_overruns=1','SERVICE_CPU10_FOOTER row=0 complete=1 acceptance=0')
 $out=Resolve-NativeOutcome $bad 1 0;Check ($out.valid -and $out.kind -eq 'failed') 'completed_failure'
 $partial=@($power,'SERVICE_CPU10_RESULT row=0 phase=workload modes=128 power_ok=1 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor=1 pass=0 known_failure=1 interrupted=1 samples=1 p99_seconds=0.1 kernel_overruns=1','SERVICE_CPU10_FOOTER row=0 complete=0 acceptance=0')
 $out=Resolve-NativeOutcome $partial 3 0;Check ($out.valid -and $out.kind -eq 'failed') 'known_partial_failure'
 $peer=@($untouched,'SERVICE_CPU10_RESULT row=0 phase=admission modes=0 power_ok=0 copy_mode=none scatter_mode=none reduction_mode=none converter_mode=none converter_slots=0 converter_factor=0 pass=0 known_failure=0 interrupted=1 samples=0 interrupt_reason=peer_native_work','SERVICE_CPU10_FOOTER row=0 complete=0 acceptance=0')
 $out=Resolve-NativeOutcome $peer 3 0;Check ($out.valid -and $out.kind -eq 'interrupted') 'unmeasured_interruption_needs_audit'
 $out=Resolve-NativeOutcome ($pass+@($pass[1])) 0 0;Check (!$out.valid) 'duplicate_result'
 $out=Resolve-NativeOutcome @() 2 0;Check (!$out.valid) 'unknown_native_exit'
 $out=Resolve-NativeOutcome $bad 0 0;Check (!$out.valid) 'exit_flag_disagreement'
 $out=Resolve-NativeOutcome $pass 0 1;Check (!$out.valid) 'row_disagreement'
 $wrong=@($pass | ForEach-Object {$_ -replace 'modes=128','modes=512'});$out=Resolve-NativeOutcome $wrong 0 0;Check (!$out.valid) 'density_mismatch'
 $wrongCopy=@($pass | ForEach-Object {$_ -replace 'copy_mode=prepared','copy_mode=validated'});$out=Resolve-NativeOutcome $wrongCopy 0 0;Check (!$out.valid) 'copy_mode_mismatch'
 $wrongScatter=@($pass | ForEach-Object {$_ -replace 'scatter_mode=blocked','scatter_mode=scalar_reference'});$out=Resolve-NativeOutcome $wrongScatter 0 0;Check (!$out.valid) 'scatter_mode_mismatch'
 $wrongReduction=@($pass | ForEach-Object {$_ -replace 'reduction_mode=paired_stereo','reduction_mode=reference'});$out=Resolve-NativeOutcome $wrongReduction 0 0;Check (!$out.valid) 'reduction_mode_mismatch'
 $missingReduction=@($pass | ForEach-Object {$_ -replace ' reduction_mode=paired_stereo',''});$out=Resolve-NativeOutcome $missingReduction 0 0;Check (!$out.valid) 'missing_reduction_mode'
 foreach($mutation in @(@('converter_mode=mirrored_avx2','converter_mode=mirrored_sse2'),@('converter_mode=mirrored_avx2','converter_mode=reference'),@(' converter_mode=mirrored_avx2',''),@('converter_slots=8','converter_slots=1'),@('converter_factor=1','converter_factor=2'))){
  $changed=@($pass | ForEach-Object {$_.Replace($mutation[0],$mutation[1])});$out=Resolve-NativeOutcome $changed 0 0;Check (!$out.valid) 'converter_metadata_mutant'
 }
 $higher=@($pass | ForEach-Object {$_.Replace('row=0','row=10').Replace('converter_factor=1','converter_factor=2')});$out=Resolve-NativeOutcome $higher 0 10;Check ($out.valid -and $out.kind -eq 'passed') 'actual_higher_factor'
 foreach($mutation in @(
  @('during_control=5','during_control=4'),@('during_state=4','during_state=5'),@('after_control=4','after_control=5'),@('after_state=4','after_state=0'),
  @('setup_error=0','setup_error=701'),@('cleanup_error=0','cleanup_error=702'),@('power_ok=1','power_ok=0'),@('power_ok=1','power_ok=2'),
  @('attempted=1','attempted=0'),@('opened=1','opened=0'),@('restored=1','restored=0'),@('close1=1','close1=0'),@('close2=1','close2=0'),
  @('before_version=1','before_version=2'),@('during_version=1','during_version=0'),@('after_version=1','after_version=2'),
  @('before_control=4','before_control=4294967296'),@('during_state=4','during_state=-1'),@('during_state=4','during_state=+4'),
  @(' after_state=4',''),@('attempted=1','attempted=1 attempted=1'),@('attempted=1','attempted=1 extra=1'))){
  $changed=@($pass | ForEach-Object {$_.Replace($mutation[0],$mutation[1])});$out=Resolve-NativeOutcome $changed 0 0;Check (!$out.valid) 'power_metadata_mutant'
 }
 $out=Resolve-NativeOutcome @($pass | Where-Object {$_ -notmatch '^SERVICE_CPU16_POWER '}) 0 0;Check (!$out.valid) 'missing_power'
 $out=Resolve-NativeOutcome ($pass+@($power)) 0 0;Check (!$out.valid) 'duplicate_power'
 foreach($masks in @(@([uint32]0,[uint32]0),@([uint32]1,[uint32]1),@([uint32]4294967294,[uint32]4294967295))){
  $control=$masks[0];$state=$masks[1];$duringControl=$control -bor [uint32]1;$duringState=$state -band [uint32]4294967294
  $replacement=$power.Replace('before_control=4',"before_control=$control").Replace('before_state=4',"before_state=$state").Replace('during_control=5',"during_control=$duringControl").Replace('during_state=4',"during_state=$duringState").Replace('after_control=4',"after_control=$control").Replace('after_state=4',"after_state=$state")
  $out=Resolve-NativeOutcome (@($replacement)+@($pass | Select-Object -Skip 1)) 0 0;Check ($out.valid -and $out.kind -eq 'passed') 'original_mask_restoration'
 }
 foreach($case in @(
  @('coordination-launch','C:/ci/ai-rack-synth/build/ci/coordination-launch.exe',$false),
  @('coordination-launch','C:/work/ai-rack-synth/build/private/coordination-launch.exe',$true),
  @('RackSynthActualPeer','C:/work/ai-rack-synth/build/ci/RackSynthActualPeer.exe',$true),
  @('cl','C:/tools/cl.exe',$true),@('link','C:/tools/link.exe',$true),@('cmake','C:/tools/cmake.exe',$true),@('pluginval','C:/tools/pluginval.exe',$true),
  @('RackSynthNativeRun','C:/work/ai-rack-synth/build/RackSynthNativeRun.exe',$false),@('ctest','C:/tools/ctest.exe',$false),@('Runner.Worker','C:/runner/Runner.Worker.exe',$false))){
  Check ((Test-NativeBusyWhileExclusive $case[0] $case[1]) -eq $case[2]) 'exclusive_native_classifier'
 }
 Write-Output "CPU10_CONTROLLER_MODEL checks=$checks failures=$failed actual_timing=0"
 if($failed){exit 1};exit 0
}

if($env:AIRACK_NATIVE_LEASE -ne 'exclusive'){throw 'Run CPU25 through RackSynthNativeRun --exclusive with bounded wait/run deadlines'}
if(!$EvidenceRoot -or [IO.Path]::IsPathRooted($EvidenceRoot) -or $EvidenceRoot -notmatch '^[a-z0-9][a-z0-9/-]*$'){throw 'A task-relative evidence root is required'}
$nativeLauncher=Join-Path (Get-Location).Path 'build/native-run/Debug/RackSynthNativeRun.exe'
$workspace=(Get-Location).Path;$evidence=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
if(!$evidence.StartsWith($workspace+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Evidence outside workspace'}
$manifestPath=Join-Path $evidence 'manifest.json';$binaryName='RackSynthSpaceReverbModalAvx4HighQosProfileProbe.exe';$condition='complete-modal128-periodic-proaudio-processing25-avx4-explicit-highqos-v1'
if(!(Test-Path -LiteralPath $evidence)){
 if($Resume){throw 'Missing frozen evidence'}
 $qualificationPath=Join-Path $workspace 'build/space-reverb-product-tank-20261017/cpu25-tooling-qualification1.json'
 $qualification=Get-Content -Raw -LiteralPath $qualificationPath | ConvertFrom-Json
 $built=Join-Path $workspace 'build/space-reverb-feasibility/Release/RackSynthSpaceReverbModalAvx4HighQosProfileProbe.exe'
 $readerSource=Join-Path $workspace 'tools/space_reverb_modal_cpu25_readback.py'
 if((Get-FileHash -LiteralPath $nativeLauncher).Hash -ne $qualification.native_launcher_sha256 -or $qualification.status -ne 'PASS_CPU25_TOOLING' -or (Get-FileHash -LiteralPath $built).Hash -ne $qualification.probe_binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $qualification.controller_sha256 -or (Get-FileHash -LiteralPath $readerSource).Hash -ne $qualification.reader_sha256){throw 'Unqualified or changed timing tools'}
 New-Item -ItemType Directory -Path $evidence | Out-Null;$source=Join-Path $evidence 'source'
 $files=@(Get-ChildItem src/dsp/SpaceReverb* -File)+@(Get-ChildItem probes/space-reverb -File)+@(Get-Item src/dsp/space/CMakeLists.txt,CMakeLists.txt,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu10.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu11.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu12.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu13.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu14.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu15.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-cpu16.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-256.md,docs/superpowers/plans/2026-10-18-space-reverb-modal-prepared-adoption.md,docs/superpowers/plans/2026-10-18-space-reverb-blocked-scatter.md,docs/superpowers/plans/2026-10-18-space-reverb-paired-reduction.md)
 $entries=@(foreach($file in $files){$relative=$file.FullName.Substring($workspace.Length+1);$to=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $to) -Force | Out-Null;Copy-Item -LiteralPath $file.FullName -Destination $to;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $to).Hash}})
 Copy-Item -LiteralPath $nativeLauncher -Destination (Join-Path $evidence 'RackSynthNativeRun.exe')
 Copy-Item -LiteralPath $built -Destination (Join-Path $evidence $binaryName);Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1');Copy-Item -LiteralPath $readerSource -Destination (Join-Path $evidence 'reader.py');Copy-Item -LiteralPath $qualificationPath -Destination (Join-Path $evidence 'qualification.json')
 $cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
 [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');condition=$condition;native_resource_lease=$env:AIRACK_NATIVE_LEASE;native_launcher_sha256=(Get-FileHash -LiteralPath $nativeLauncher).Hash;rows=24;warm_seconds=10;measure_seconds=20;deadline='processing_entry_to_exit';kernel_mean_p99_fraction=.5;copy_p99_fraction=.1;cpu=$cpu;binary_sha256=(Get-FileHash -LiteralPath $built).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;reader_sha256=(Get-FileHash -LiteralPath $readerSource).Hash;qualification_sha256=(Get-FileHash -LiteralPath $qualificationPath).Hash;sources=$entries;acceptance='PENDING_INDEPENDENT_READBACK'} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $manifestPath -Encoding utf8
}elseif(!$Resume){throw 'Evidence exists; do not replay'}
if(Test-Path -LiteralPath (Join-Path $evidence 'admission-stop1.json')){throw 'Retired timing condition; no unchanged replay'}
$manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json;$frozen=Join-Path $evidence $binaryName
if($manifest.native_resource_lease -ne 'exclusive' -or (Get-FileHash -LiteralPath $nativeLauncher).Hash -ne $manifest.native_launcher_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'RackSynthNativeRun.exe')).Hash -ne $manifest.native_launcher_sha256 -or $manifest.condition -ne $condition -or (Get-FileHash -LiteralPath $frozen).Hash -ne $manifest.binary_sha256 -or (Get-FileHash -LiteralPath $PSCommandPath).Hash -ne $manifest.controller_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'controller.ps1')).Hash -ne $manifest.controller_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'reader.py')).Hash -ne $manifest.reader_sha256 -or (Get-FileHash -LiteralPath (Join-Path $evidence 'qualification.json')).Hash -ne $manifest.qualification_sha256){throw 'Frozen identity mismatch'}
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
 $deadline=[DateTime]::UtcNow.AddMinutes(5);$quiet=$null;$seconds=0;if($RequireQuiet){$seconds=60}
 while([DateTime]::UtcNow -lt $deadline){
  $busy=@(Get-Process -ErrorAction SilentlyContinue | Where-Object {Test-NativeBusyWhileExclusive $_.ProcessName $_.Path}).Count -gt 0
  if($busy){$quiet=$null;$seconds=$(if($RequireQuiet){60}else{15})}else{if($null -eq $quiet){$quiet=[DateTime]::UtcNow};if(([DateTime]::UtcNow-$quiet).TotalSeconds -ge $seconds){return $true}}
  Start-Sleep -Milliseconds 250
 };return $false
}
Write-Output "SERVICE_CPU25_ARMED controller_pid=$PID condition=$condition rows=24"
$initial=$true
foreach($row in 0..23){
 if($accepted -contains $row){continue}
 $key='row-{0:D2}' -f $row;$complete=Join-Path $evidence "$key.complete.json"
 if(Test-Path -LiteralPath $complete){throw 'Completed marker not independently accepted; do not replay'}
 $attempt=1
 while(Test-Path -LiteralPath (Join-Path $evidence "$key-attempt-$attempt.stdout")){if(!$Resume){throw 'Existing attempt requires independent audit'};$attempt++}
 if(!(Wait-NativeIdle $initial)){Write-Output "SERVICE_CPU25_DEFERRED row=$row reason=peer_busy";exit 3};$initial=$false
 $prefix=Join-Path $evidence "$key-attempt-$attempt";$began=[DateTime]::UtcNow;$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
 $proc=Start-Process -FilePath $frozen -ArgumentList @('--profile',"$row",('"{0}"' -f "$prefix.timings.bin")) -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -PassThru
 $handle=$proc.Handle
 try{$proc.WaitForExit()}catch{$original=$_;try{$proc.Kill();$proc.WaitForExit()}catch{Write-Output 'SERVICE_CPU25_OWNED_CLEANUP_ERROR'};throw $original}
 $proc.Refresh();if($null -eq $proc.ExitCode){throw 'Native exit unavailable'}
 $records=@(foreach($extension in @('.stdout','.stderr','.timings.bin')){if(Test-Path -LiteralPath "$prefix$extension"){[ordered]@{file="$key-attempt-$attempt$extension";sha256=(Get-FileHash -LiteralPath "$prefix$extension").Hash;bytes=(Get-Item -LiteralPath "$prefix$extension").Length}}})
 [ordered]@{row=$row;attempt=$attempt;exit=$proc.ExitCode;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');records=$records} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
 $outcome=Resolve-NativeOutcome @(Get-Content -LiteralPath "$prefix.stdout") $proc.ExitCode $row;$fields=$outcome.fields
 if($outcome.kind -eq 'interrupted'){Write-Output "SERVICE_CPU25_INTERRUPTED row=$row attempt=$attempt reason=$($fields.interrupt_reason) independent_audit_required=1";exit 3}
 [ordered]@{row=$row;attempt=$attempt;exit=$proc.ExitCode;valid=$outcome.valid;kind=$outcome.kind;fields=$fields;acceptance='PENDING_INDEPENDENT_READBACK'} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $complete -Encoding utf8
 Write-Output "SERVICE_CPU25_ROW row=$row exit=$($proc.ExitCode) valid=$($outcome.valid) samples=$($fields.samples) p99=$($fields.p99_seconds) copy_p99=$($fields.copy_p99_seconds) kernel_overruns=$($fields.kernel_overruns) dispatch_misses=$($fields.dispatch_misses)"
 if(!$outcome.valid -or $outcome.kind -ne 'passed'){[ordered]@{status='RETIRED_NATIVE_FAILURE';row=$row;attempt=$attempt;native_exit=$proc.ExitCode;known_failure=$fields.known_failure} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'admission-stop1.json') -Encoding utf8;exit 1}
}
Write-Output 'SERVICE_CPU25_COMPLETE rows=24 independent_readback_pending=1';exit 0

