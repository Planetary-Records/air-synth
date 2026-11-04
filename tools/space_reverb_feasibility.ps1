param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$SourceBundle,
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [int]$First=0,
    [int]$Last=332,
    [ValidateSet('unitCircle','poleContour')][string]$Domain='unitCircle',
    [ValidateSet('modal','impulse')][string]$Method='modal',
    [switch]$Resume
)
$ErrorActionPreference='Stop'
if($First -lt 0 -or $Last -gt 332 -or $Last -lt $First){throw 'Invalid configuration range'}
if($Method -eq 'impulse' -and $Domain -ne 'poleContour'){throw 'Impulse method requires the selected poleContour domain'}
$workspace=(Get-Location).Path
$binaryPath=(Resolve-Path -LiteralPath $Binary).Path
$sourcePath=(Resolve-Path -LiteralPath $SourceBundle).Path
$evidencePath=[IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
$scriptHash=(Get-FileHash -LiteralPath $PSCommandPath).Hash
$binaryHash=(Get-FileHash -LiteralPath $binaryPath).Hash
$manifestPath=Join-Path $evidencePath 'manifest.json'
if(Test-Path -LiteralPath $evidencePath){
    if(!$Resume){throw 'Evidence directory already exists; explicit Resume is required'}
    $manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if($manifest.binary_sha256 -ne $binaryHash -or $manifest.controller_sha256 -ne $scriptHash -or $manifest.first -ne $First -or $manifest.last -ne $Last -or $manifest.domain -ne $Domain -or $manifest.method -ne $Method){throw 'Resume identity/range mismatch'}
}else{
    if($Resume){throw 'Cannot resume missing evidence'}
    New-Item -ItemType Directory -Path $evidencePath | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $evidencePath 'source') -Recurse
    Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $evidencePath 'RackSynthSpaceReverbFrozen.exe')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidencePath 'controller.ps1')
    $sourceHashes=@(Get-ChildItem -LiteralPath (Join-Path $evidencePath 'source') -Recurse -File | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($evidencePath.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
    [ordered]@{binary_sha256=$binaryHash;controller_sha256=$scriptHash;first=$First;last=$Last;domain=$Domain;method=$Method;created_utc=[DateTime]::UtcNow.ToString('o');source=$sourceHashes;acceptance='DIAGNOSTIC_ONLY'} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    $manifest=Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
}
$frozenBinary=Join-Path $evidencePath 'RackSynthSpaceReverbFrozen.exe'
if((Get-FileHash -LiteralPath $frozenBinary).Hash -ne $binaryHash){throw 'Frozen executable changed'}
foreach($entry in $manifest.source){if((Get-FileHash -LiteralPath (Join-Path $evidencePath $entry.path)).Hash -ne $entry.sha256){throw "Frozen source changed: $($entry.path)"}}
function Wait-NativeIdle {
    $deadline=[DateTime]::UtcNow.AddMinutes(5)
    while(@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -like 'RackSynth*' -or $_.ProcessName -in @('Runner.Worker','cl','link','pluginval') -or ($_.ProcessName -ne 'Runner.Listener' -and $_.Path -match '[\\/]ai-rack-synth[\\/]')}).Count){
        if([DateTime]::UtcNow -ge $deadline){return $false}
        Start-Sleep -Milliseconds 500
    }
    return $true
}
$anyFailure=$false
for($configuration=$First;$configuration -le $Last;$configuration++){
    $key='config-{0:D3}' -f $configuration
    $completePath=Join-Path $evidencePath "$key.complete.json"
    if(Test-Path -LiteralPath $completePath){
        $prior=Get-Content -Raw -LiteralPath $completePath | ConvertFrom-Json
        if($prior.exit_code -ne 0 -or !$prior.valid_complete_native_record){$anyFailure=$true}
        continue
    }
    $nextAttempt=1
    foreach($file in @(Get-ChildItem -LiteralPath $evidencePath -Filter "$key-attempt-*.json" -File)){
        $number=[int]($file.BaseName -replace '^.*-attempt-','');$nextAttempt=[Math]::Max($nextAttempt,$number+1)
    }
    $completed=$false
    for($interruption=0;$interruption -lt 3;$interruption++){
        if(!(Wait-NativeIdle)){Write-Output "MATRIX_DEFERRED config=$configuration reason=peer_busy";exit 3}
        $attempt=$nextAttempt+$interruption
        $prefix=Join-Path $evidencePath "$key-attempt-$attempt"
        foreach($extension in @('.stdout','.stderr','.json')){if(Test-Path -LiteralPath "$prefix$extension"){throw 'Attempt path already exists'}}
        $env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT="$prefix-diagnostics"
        $started=[DateTime]::UtcNow
        $mode=if($Method -eq 'impulse'){'--impulse-pole-row'}elseif($Domain -eq 'poleContour'){'--feasibility-pole-row'}else{'--feasibility-row'}
        $process=Start-Process -FilePath $frozenBinary -ArgumentList @($mode,"$configuration") -WorkingDirectory $workspace -WindowStyle Hidden -RedirectStandardOutput "$prefix.stdout" -RedirectStandardError "$prefix.stderr" -Wait -PassThru
        $nativeExit=$process.ExitCode
        [ordered]@{configuration=$configuration;attempt=$attempt;exit_code=$nativeExit;started_utc=$started.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash -LiteralPath "$prefix.stdout").Hash;stderr_sha256=(Get-FileHash -LiteralPath "$prefix.stderr").Hash} |
            ConvertTo-Json | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
        if($nativeExit -eq 3){Write-Output "MATRIX_INTERRUPTED config=$configuration attempt=$attempt";continue}
        $lines=@(Get-Content -LiteralPath "$prefix.stdout")
        $header=@($lines | Where-Object {$_ -match '^configuration,'})
        $rows=@($lines | Where-Object {$_ -match '^\d+,'})
        $footer=@($lines | Where-Object {$_ -match "^CONFIG_SUMMARY index=$configuration bands=7 failures=\d+ acceptance=0$"})
        $valid=$header.Count -eq 1 -and $rows.Count -eq 7 -and $footer.Count -eq 1 -and $nativeExit -in @(0,1,2)
        $failureCount=7
        if($valid){
            $parsed=@(($header+$rows) | ConvertFrom-Csv)
            $failureCount=@($parsed | Where-Object {$_.pass -ne '1'}).Count
            $reported=[int]([regex]::Match($footer[0],'failures=(\d+)').Groups[1].Value)
            $valid=$reported -eq $failureCount -and (($nativeExit -eq 0) -eq ($failureCount -eq 0))
            ($header+$rows) | Set-Content -LiteralPath "$prefix.rows.csv" -Encoding utf8
        }
        [ordered]@{configuration=$configuration;attempt=$attempt;exit_code=$nativeExit;valid_complete_native_record=$valid;gate_failures=$failureCount;stdout="$key-attempt-$attempt.stdout";rows="$key-attempt-$attempt.rows.csv";acceptance='DIAGNOSTIC_ONLY'} |
            ConvertTo-Json | Set-Content -LiteralPath $completePath -Encoding utf8
        $completed=$true
        if($nativeExit -ne 0 -or !$valid){$anyFailure=$true}
        Write-Output "MATRIX_CONFIG config=$configuration exit=$nativeExit valid=$valid failures=$failureCount"
        break
    }
    if(!$completed){Write-Output "MATRIX_DEFERRED config=$configuration reason=three_interruptions";exit 3}
}
$records=@(Get-ChildItem -LiteralPath $evidencePath -Filter 'config-*.complete.json' -File | Sort-Object Name | ForEach-Object {Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json})
$rows=@()
foreach($record in $records){if($record.valid_complete_native_record){$rows+=@(Import-Csv -LiteralPath (Join-Path $evidencePath $record.rows))}}
$aggregate=Join-Path $evidencePath 'all-rows.csv'
if(!(Test-Path -LiteralPath $aggregate)){$rows | Export-Csv -LiteralPath $aggregate -NoTypeInformation -Encoding utf8}
$summaryPath=Join-Path $evidencePath 'summary.json'
if(!(Test-Path -LiteralPath $summaryPath)){
    [ordered]@{configurations=$records.Count;expected_configurations=$Last-$First+1;native_rows=$rows.Count;expected_rows=7*($Last-$First+1);failed_native_rows=@($rows | Where-Object {$_.pass -ne '1'}).Count;invalid_records=@($records | Where-Object {!$_.valid_complete_native_record}).Count;failed=$anyFailure;acceptance='DIAGNOSTIC_ONLY';binary_sha256=$binaryHash} |
        ConvertTo-Json | Set-Content -LiteralPath $summaryPath -Encoding utf8
}
Write-Output "MATRIX_COMPLETE configurations=$($records.Count) rows=$($rows.Count) failed=$anyFailure acceptance=0"
if($anyFailure){exit 1}
exit 0
