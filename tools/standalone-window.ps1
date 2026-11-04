<#
.SYNOPSIS
Drives the AI Rack Synth standalone app's window for development review: launches or finds it, sizes it, presses
controls by their accessible names and saves what the window shows as a PNG. Development tooling only.

.DESCRIPTION
Controls are pressed through UI Automation (invoke, toggle or select), so the user's mouse and keyboard are not
touched. Mouse messages posted to the window do not work: JUCE checks the real cursor before a button reacts.
Each call presses, then focuses and sends keys, then captures.

The window raises and takes the focus, so run each call under the desktop slot of RackSynthNativeRun
(docs/development/parallel-native-work.md) and end it with -Close: the launcher ends what its command started.

.EXAMPLE
& $native --desktop --run-seconds 300 -- (Get-Command pwsh).Source -NoProfile -File "$PWD/tools/standalone-window.ps1" -Width 1080 -Height 680 -Press AI -Out build/ai.png -Close
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\windows-debug\RackSynthInstrument_artefacts\Debug\Standalone\AI Rack Synth.exe'),
    # The editor's size in its own pixels; the window adds JUCE's 1 px border and 26 px title bar, at the display's scale.
    [int]$Width = 0,
    [int]$Height = 0,
    # Accessible names of controls to press in turn, such as AI or "Add rack".
    [string[]]$Press = @(),
    # A control to give the keyboard focus, then keys to send it in turn: Up, Down, Left, Right, Enter, Space, Escape, Tab.
    [string]$Focus = '',
    [string[]]$Keys = @(),
    [string]$Out = '',
    [switch]$List,
    [int]$SettleMs = 800,
    [switch]$Close
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StandaloneWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
}
'@
[void][StandaloneWindow]::SetProcessDpiAwarenessContext([IntPtr]-4)

$exePath = (Resolve-Path $Exe).Path
$process = Get-Process | Where-Object { $_.Path -eq $exePath } | Select-Object -First 1
if (-not $process) { $process = Start-Process -FilePath $exePath -PassThru }
$deadline = (Get-Date).AddSeconds(30)
while ($process.MainWindowHandle -eq 0) {
    if ((Get-Date) -gt $deadline) { throw "no window from $exePath" }
    Start-Sleep -Milliseconds 200; $process.Refresh()
}
$hwnd = $process.MainWindowHandle
Start-Sleep -Milliseconds $SettleMs
$window = New-Object StandaloneWindow+RECT

if ($Width -gt 0 -and $Height -gt 0) {
    $scale = [StandaloneWindow]::GetDpiForWindow($hwnd) / 96.0
    [void][StandaloneWindow]::GetWindowRect($hwnd, [ref]$window)
    $w = [int][Math]::Round(($Width + 2) * $scale); $h = [int][Math]::Round(($Height + 28) * $scale)
    # SWP_NOZORDER | SWP_NOACTIVATE: sized where it is, without taking the focus from the user's window.
    [void][StandaloneWindow]::SetWindowPos($hwnd, [IntPtr]::Zero, $window.Left, $window.Top, $w, $h, 0x0014)
    Start-Sleep -Milliseconds $SettleMs
}

$root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
$descendants = [System.Windows.Automation.TreeScope]::Descendants
if ($List) {
    foreach ($e in $root.FindAll($descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
        $c = $e.Current
        if ($c.Name) { "{0,-12} {1}" -f $c.ControlType.ProgrammaticName.Replace('ControlType.', ''), $c.Name }
    }
}
function Find-Control([string]$name) {
    $condition = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::NameProperty), $name
    $element = $root.FindFirst($descendants, $condition)
    if (-not $element) { throw "no control named '$name'" }
    $element
}
foreach ($name in $Press) {
    $element = Find-Control $name
    $pattern = $null
    if ($element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) { $pattern.Invoke() }
    elseif ($element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) { $pattern.Toggle() }
    elseif ($element.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) { $pattern.Select() }
    else { throw "'$name' cannot be pressed" }
    Start-Sleep -Milliseconds $SettleMs
}
if ($Focus) { (Find-Control $Focus).SetFocus(); Start-Sleep -Milliseconds 300 }
# Posted key messages reach the focused component: JUCE reads the key from the message itself.
$codes = @{ Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; Enter = 0x0D; Space = 0x20; Escape = 0x1B; Tab = 0x09 }
foreach ($key in $Keys) {
    if (-not $codes.ContainsKey($key)) { throw "unknown key '$key'" }
    [void][StandaloneWindow]::PostMessage($hwnd, 0x100, [IntPtr]$codes[$key], [IntPtr]1)
    Start-Sleep -Milliseconds 60
    [void][StandaloneWindow]::PostMessage($hwnd, 0x101, [IntPtr]$codes[$key], [IntPtr]0xC0000001)
    Start-Sleep -Milliseconds 250
}
if ($Keys) { Start-Sleep -Milliseconds $SettleMs }

if ($Out) {
    $path = [IO.Path]::GetFullPath($Out, (Get-Location).Path)
    [void](New-Item -ItemType Directory -Force -Path (Split-Path $path))
    [void][StandaloneWindow]::GetWindowRect($hwnd, [ref]$window)
    $bitmap = New-Object System.Drawing.Bitmap ($window.Right - $window.Left), ($window.Bottom - $window.Top)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $g.GetHdc()
    # PW_RENDERFULLCONTENT: the window's content even where it is covered or composed by the GPU.
    [void][StandaloneWindow]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bitmap.Dispose()
    "saved $path ($($window.Right - $window.Left) x $($window.Bottom - $window.Top) px)"
}
if ($Close) {
    # The app saves its session as it quits; a launcher's job would end it at once when this script returns.
    [void]$process.CloseMainWindow()
    if (-not $process.WaitForExit(15000)) { throw "$exePath did not quit" }
}
