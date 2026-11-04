$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Install-NativeResources.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('synth-native-resources-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $first = Join-Path $testRoot 'first.lock'
    $second = Join-Path $testRoot 'second.lock'
    [IO.File]::WriteAllBytes($first, [byte[]]@())
    [IO.File]::WriteAllBytes($second, [byte[]]@())
    $rejected = $false
    try { Assert-CompatibleNativeFiles @($first, $second) } catch { $rejected = $true }
    if (-not $rejected) { throw 'Different existing resource identities were accepted.' }
    $alias = Join-Path $testRoot 'alias.lock'
    New-Item -ItemType HardLink -Path $alias -Target $first | Out-Null
    Assert-CompatibleNativeFiles @($first, $alias)
    if ((Get-NativeFileIdentity $first) -ne (Get-NativeFileIdentity $alias)) { throw 'Hard link identity differs.' }
    [IO.File]::WriteAllBytes($second, [byte[]]@(1))
    $rejected = $false
    try { Assert-CompatibleNativeFiles @($second) } catch { $rejected = $true }
    if (-not $rejected) { throw 'Nonempty resource file was accepted.' }
    $peer = Join-Path $testRoot 'peer'
    New-Item -ItemType Directory -Path $peer | Out-Null
    [IO.File]::WriteAllText((Join-Path $peer 'keep.txt'), 'preserved')
    $junction = Join-Path $testRoot 'redirect'
    New-Item -ItemType Junction -Path $junction -Target $peer | Out-Null
    $rejected = $false
    try { Assert-CompatibleNativeFiles @((Join-Path $junction 'not-created.lock')) } catch { $rejected = $true }
    if (-not $rejected) { throw 'A junction ancestor was accepted.' }
    Remove-Item -LiteralPath $junction
    if ([IO.File]::ReadAllText((Join-Path $peer 'keep.txt')) -ne 'preserved') { throw 'Junction peer changed.' }
    'Native resource setup: identity, mismatch, nonempty and junction checks passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside TEMP.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
