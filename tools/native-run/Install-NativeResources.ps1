[CmdletBinding(SupportsShouldProcess)]
param([string[]]$Accounts = @('ricar', 'jenkins-agent'))
$ErrorActionPreference = 'Stop'

function Assert-PlainNativePath([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path)) { throw 'Native resource paths must be absolute.' }
    $part = [IO.Path]::GetFullPath($Path)
    while ($part) {
        if ((Test-Path -LiteralPath $part) -and ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Native resource paths must not contain reparse points.'
        }
        $part = [IO.Path]::GetDirectoryName($part)
    }
}

function Get-NativeFileIdentity([string]$Path) {
    if (-not ('RackSynthResourceFiles' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RackSynthResourceFiles {
    [StructLayout(LayoutKind.Sequential)] struct Info {
        public uint Attributes; public System.Runtime.InteropServices.ComTypes.FILETIME Created, Accessed, Written;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle handle, out Info info);
    public static string Identity(string path) {
        using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            Info info;
            if (!GetFileInformationByHandle(file.SafeFileHandle, out info)) throw new IOException("Cannot identify native resource file.");
            return String.Format("{0:x8}:{1:x8}{2:x8}", info.Volume, info.IndexHigh, info.IndexLow);
        }
    }
}
'@
    }
    [RackSynthResourceFiles]::Identity($Path)
}

function Assert-CompatibleNativeFiles([string[]]$Paths) {
    $identity = $null
    foreach ($path in $Paths) {
        Assert-PlainNativePath $path
        if (Test-Path -LiteralPath $path) {
            $file = Get-Item -LiteralPath $path
            if ($file.PSIsContainer -or $file.Length -ne 0) { throw 'A resource path is not an empty lock file.' }
            $next = Get-NativeFileIdentity $path
            if ($identity -and $identity -ne $next) { throw 'Existing native resource files differ; migration requires an idle, reviewed reconciliation.' }
            $identity = $next
        }
    }
}

function Set-NativeResourceAccess([string]$Path, [string[]]$Sids, [switch]$Directory) {
    $acl = if ($Directory) { [Security.AccessControl.DirectorySecurity]::new() } else { [Security.AccessControl.FileSecurity]::new() }
    $acl.SetAccessRuleProtection($true, $false)
    $admins = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $acl.SetOwner($admins)
    $inherit = if ($Directory) { [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit' } else { [Security.AccessControl.InheritanceFlags]::None }
    foreach ($sid in @('S-1-5-32-544', 'S-1-5-18') + $Sids) {
        $rights = if ($sid -in @('S-1-5-32-544', 'S-1-5-18')) { [Security.AccessControl.FileSystemRights]::FullControl }
            elseif ($Directory) { [Security.AccessControl.FileSystemRights]::ReadAndExecute }
            else { [Security.AccessControl.FileSystemRights]'Read,Write,Synchronize' }
        $rule = [Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid), $rights, $inherit,
            [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

if ($MyInvocation.InvocationName -ne '.') {
    $participants = @($Accounts | ForEach-Object {
        $account = Get-LocalUser -Name $_
        $profile = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$($account.SID.Value)"
        [pscustomobject]@{ Sid = $account.SID.Value; Legacy = Join-Path ([Environment]::ExpandEnvironmentVariables($profile.ProfileImagePath)) 'AppData\Local\AI Rack Synth\Development\locks\native-resources-v1.lock' }
    })
    if ($participants.Count -lt 2) { throw 'Name both native development and CI accounts.' }
    $base = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'AI Rack Synth'
    $machine = Join-Path $base 'Development\locks\native-resources-v1.lock'
    $paths = @($machine) + @($participants.Legacy)
    Assert-CompatibleNativeFiles $paths
    foreach ($path in @($base, (Join-Path $base 'Development'), (Join-Path $base 'Development\locks'))) {
        Assert-PlainNativePath $path
        if (Test-Path -LiteralPath $path) {
            $owner = (Get-Acl -LiteralPath $path).GetOwner([Security.Principal.SecurityIdentifier]).Value
            if ($owner -notin @('S-1-5-32-544', 'S-1-5-18')) { throw 'Refusing an existing machine directory not owned by Administrators or SYSTEM.' }
        }
    }
    if ($PSCmdlet.ShouldProcess(($paths -join ', '), 'Provision one shared lock identity, bridge existing account paths, and grant only named accounts read/write')) {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Provisioning requires an administrator; preview with -WhatIf.' }
        foreach ($path in @($base, (Join-Path $base 'Development'), (Join-Path $base 'Development\locks'))) {
            New-Item -ItemType Directory -Path $path -Force | Out-Null
            Set-NativeResourceAccess $path $participants.Sid -Directory
        }
        $anchor = @($paths | Where-Object { Test-Path -LiteralPath $_ }) | Select-Object -First 1
        if (-not $anchor) {
            $stream = [IO.File]::Open($machine, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
            $stream.Dispose()
            $anchor = $machine
        }
        foreach ($path in $paths) {
            if (-not (Test-Path -LiteralPath $path)) {
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
                New-Item -ItemType HardLink -Path $path -Target $anchor | Out-Null
            }
        }
        Set-NativeResourceAccess $machine $participants.Sid
        Assert-CompatibleNativeFiles $paths
        [pscustomobject]@{ Scope = 'machine'; FileIdentity = Get-NativeFileIdentity $machine; Accounts = $Accounts; LegacyAliases = $participants.Count }
    }
}
