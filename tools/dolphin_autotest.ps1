param(
    [string]$Dol = (Join-Path $PSScriptRoot '..\tekken6wii-debug.dol'),
    [string]$UserDir = 'C:\tkdr_raw_user',
    [string]$OutDir = (Join-Path $env:TEMP 'tekken-auto'),
    [string]$Dolphin = 'C:\Users\sherv\Desktop\Projects\Ballanced\build\tools\dolphin\Dolphin.exe'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Dol = [IO.Path]::GetFullPath($Dol)
$Dolphin = [IO.Path]::GetFullPath($Dolphin)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Auto {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint map);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@

function Capture-Client([IntPtr]$h, [string]$path) {
    $r = New-Object Auto+RECT
    [Auto]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object Auto+POINT
    $pt.X = 0; $pt.Y = 0
    [Auto]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # PrintWindow with PW_RENDERFULLCONTENT captures D3D11 flip-model content.
    $hdc = $g.GetHdc()
    $ok = [Auto]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc)
    if (-not $ok) {
        $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
    }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return "$w x $hh pw=$ok"
}

function Focus-Game([IntPtr]$h) {
    $fg = [Auto]::GetForegroundWindow()
    $ft = [Auto]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    $ct = [Auto]::GetCurrentThreadId()
    [Auto]::AttachThreadInput($ct, $ft, $true) | Out-Null
    [Auto]::ShowWindow($h, 9) | Out-Null
    [Auto]::BringWindowToTop($h) | Out-Null
    [Auto]::SetForegroundWindow($h) | Out-Null
    [Auto]::SetFocus($h) | Out-Null
    [Auto]::AttachThreadInput($ct, $ft, $false) | Out-Null
    Start-Sleep -Milliseconds 300
}

function Press-Key([byte]$vk, [int]$ms = 500) {
    $scan = [byte][Auto]::MapVirtualKey($vk, 0)
    [Auto]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $ms
    [Auto]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
}

$p = Start-Process -FilePath $Dolphin -ArgumentList @('-u',$UserDir,'-b','-e',$Dol,'-l') -WorkingDirectory (Split-Path $Dol) -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 200
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $p.MainWindowHandle; break }
}
if ($hwnd -eq [IntPtr]::Zero) { throw 'Dolphin window not found' }
Focus-Game $hwnd
$fg = [Auto]::GetForegroundWindow()
Write-Output "window=$hwnd foreground=$fg focused=$($fg -eq $hwnd) title=$($p.MainWindowTitle)"

$shots = @()
$shots += @{ t = 5;  name = '01_boot';        key = $null }
$shots += @{ t = 12; name = '02_after_start'; key = 0x0D } # Enter = Start
$shots += @{ t = 20; name = '03_after_cross'; key = 0x5A } # Z = Cross
$shots += @{ t = 28; name = '04_after_down';  key = 0x28 } # Down
$shots += @{ t = 36; name = '05_after_cross2'; key = 0x5A }

$start = Get-Date
foreach ($s in $shots) {
    $target = $s.t
    $elapsed = ((Get-Date) - $start).TotalSeconds
    if ($elapsed -lt $target) { Start-Sleep -Milliseconds ([int](($target - $elapsed) * 1000)) }
    if ($s.key -ne $null) {
        Focus-Game $hwnd
        $fg = [Auto]::GetForegroundWindow()
        Write-Output "input $($s.name) foreground=$fg focused=$($fg -eq $hwnd) key=$($s.key)"
        Press-Key ([byte]$s.key)
    }
    $path = Join-Path $OutDir ($s.name + '.png')
    $size = Capture-Client $hwnd $path
    Write-Output "capture $($s.name) t=$target size=$size"
    $shots[-1].path = $path
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
$log = Join-Path $UserDir 'Logs\dolphin.log'
if (Test-Path $log) { Copy-Item $log (Join-Path $OutDir 'dolphin.log') -Force }
Get-ChildItem $OutDir -File | Select-Object Name,Length,LastWriteTime
