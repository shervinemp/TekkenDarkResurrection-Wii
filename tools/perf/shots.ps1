param(
    [Parameter(Mandatory)][string]$Dol,
    [Parameter(Mandatory)][string]$OutDir,
    [int[]]$At = @(110, 120, 130, 145),
    [int]$Total = 150
)
# Runs a DOL in Dolphin and saves a screenshot of its window (PrintWindow, so the window may sit behind others)
# at each second in -At.  Use it on the production DOL too: a plain `make` build must boot to the attract mode
# (about 100 s in).  The attract/autopilot fight starts around 100-120 s.
$ErrorActionPreference = 'Stop'
$wantOut = $OutDir
. "$PSScriptRoot\env.ps1"   # sets $OutDir too (the default log directory): keep the parameter
$OutDir = $wantOut
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Cap {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Cap]::SetProcessDPIAware() | Out-Null   # without this the captures are cropped
$Dol = (Resolve-Path $Dol).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# only the Dolphin of an earlier run of ours (this user directory): another project's Dolphin is not ours to stop
Get-CimInstance Win32_Process -Filter "Name='Dolphin.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine.Contains($script:UserDir) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800
$p = Start-Process -FilePath $script:Dolphin -ArgumentList @('-u', $script:UserDir, '-b', '-e', $Dol, '-l') -WorkingDirectory (Split-Path $Dol) -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
$k = 0
while ($sw.Elapsed.TotalSeconds -lt $Total -and -not $p.HasExited) {
    Start-Sleep -Seconds 1
    $p.Refresh()
    if ($k -lt $At.Count -and $sw.Elapsed.TotalSeconds -ge $At[$k] -and $p.MainWindowHandle -ne 0) {
        $h = $p.MainWindowHandle
        $r = New-Object Cap+RECT
        [Cap]::GetClientRect($h, [ref]$r) | Out-Null
        $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
        if ($w -gt 0 -and $hh -gt 0) {
            $b = New-Object System.Drawing.Bitmap($w, $hh)
            $g = [System.Drawing.Graphics]::FromImage($b)
            $hdc = $g.GetHdc()
            [Cap]::PrintWindow($h, $hdc, 2) | Out-Null
            $g.ReleaseHdc($hdc)
            $g.Dispose()
            $path = Join-Path $OutDir ("shot_{0}.png" -f $At[$k])
            $b.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
            $b.Dispose()
            Write-Output "saved $path ${w}x${hh}"
        }
        $k++
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
