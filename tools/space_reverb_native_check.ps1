param([Parameter(Mandatory=$true)][string]$Prefix,[Parameter(Mandatory=$true)][string]$Target,[string[]]$NativeArguments=@(),[string]$BinaryPath)
$ErrorActionPreference='Stop'
if($env:AIRACK_NATIVE_LEASE -notin @('shared','exclusive')){throw 'Run functional checks through RackSynthNativeRun'}
$NativeArguments=@($NativeArguments | Where-Object {$null -ne $_})
if($Prefix -notmatch '^[a-z0-9-]+$' -or $Target -notmatch '^RackSynth[A-Za-z0-9]+$'){throw 'Invalid evidence/target name'}
$workspace=(Get-Location).Path;$base=(Resolve-Path 'build/space-reverb-product-tank-20261017').Path
$evidence=Join-Path $base $Prefix;if(Test-Path -LiteralPath $evidence){throw 'Evidence exists'}
New-Item -ItemType Directory -Path $evidence | Out-Null;$source=Join-Path $evidence 'source'
$files=@(Get-ChildItem src -Recurse -File)+@(Get-ChildItem probes/space-reverb -File)+@(Get-ChildItem tests/native/SpaceReverb* -File)+@(Get-Item CMakeLists.txt)
$entries=@(foreach($file in $files){$relative=$file.FullName.Substring($workspace.Length+1);$to=Join-Path $source $relative;New-Item -ItemType Directory -Path (Split-Path $to) -Force | Out-Null;Copy-Item -LiteralPath $file.FullName -Destination $to;[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $to).Hash}})
$binary=Join-Path $evidence "$Target.exe"
if(!$BinaryPath){$BinaryPath="build/space-reverb-feasibility/Release/$Target.exe"}
$resolvedBinary=(Resolve-Path -LiteralPath $BinaryPath).Path
if(!$resolvedBinary.StartsWith($workspace+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolvedBinary) -ne "$Target.exe"){throw 'Native binary must be the named target inside this worktree'}
Copy-Item -LiteralPath $resolvedBinary -Destination $binary
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $evidence 'controller.ps1')
[ordered]@{target=$Target;arguments=$NativeArguments;binary_sha256=(Get-FileHash -LiteralPath $binary).Hash;controller_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;sources=$entries;native_resource_lease=$env:AIRACK_NATIVE_LEASE;scope='Coordinated functional native check; captures all product source including promoted runtime; no CPU acceptance';desktop_held=$env:AIRACK_NATIVE_DESKTOP_HELD} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'manifest.json') -Encoding utf8
$env:AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT=Join-Path $evidence 'diagnostics';$began=[DateTime]::UtcNow
$arguments=@{FilePath=$binary;WorkingDirectory=$workspace;WindowStyle='Hidden';RedirectStandardOutput=(Join-Path $evidence 'native.stdout');RedirectStandardError=(Join-Path $evidence 'native.stderr');PassThru=$true}
if($NativeArguments.Count){$arguments.ArgumentList=$NativeArguments}
$proc=Start-Process @arguments;$handle=$proc.Handle;$interrupted=$false
$proc.WaitForExit();$proc.Refresh();if($null -eq $proc.ExitCode){throw 'Native exit unavailable'}
[ordered]@{native_exit=$proc.ExitCode;ci_interrupted=$interrupted;started_utc=$began.ToString('o');ended_utc=[DateTime]::UtcNow.ToString('o');stdout_sha256=(Get-FileHash -LiteralPath (Join-Path $evidence 'native.stdout')).Hash;stderr_sha256=(Get-FileHash -LiteralPath (Join-Path $evidence 'native.stderr')).Hash;acceptance='PENDING_READBACK'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'native.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $evidence 'native.stdout') -Tail 8;Get-Content -LiteralPath (Join-Path $evidence 'native.stderr')
if($interrupted){exit 3};exit $proc.ExitCode
