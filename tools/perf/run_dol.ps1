param(
    [Parameter(Mandatory)][string]$Dol,
    [int]$Seconds = 120,
    [string]$Out = 'run.log'
)
# Runs one DOL in Dolphin for a while and copies Dolphin's log (the OSREPORT lines carry DEMOFPS, SPEED, DRAWCATS ...).
# The boot is slow: the autosave dialog takes ~30 s and the logo ~90 s; the autopilot reaches a fight at ~100-120 s.
. "$PSScriptRoot\env.ps1"
$Dol = (Resolve-Path $Dol).Path
$log = Join-Path $script:UserDir 'Logs\dolphin.log'
# only the Dolphin of an earlier run of this script (it uses this user directory): another project's Dolphin is not ours to stop
Get-CimInstance Win32_Process -Filter "Name='Dolphin.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine.Contains($script:UserDir) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800
Remove-Item $log -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $script:Dolphin -ArgumentList @('-u', $script:UserDir, '-b', '-e', $Dol, '-l') -WorkingDirectory (Split-Path $Dol) -PassThru
Write-Output "pid=$($p.Id)"
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) {
    # ask Dolphin to close first: a killed Dolphin can lose the SD image writes it still holds (the -DWII_SDLOG log)
    [void]$p.CloseMainWindow()
    [void]$p.WaitForExit(15000)
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 800
Copy-Item $log $Out -Force
Write-Output "done after $([int]$sw.Elapsed.TotalSeconds)s, log lines: $((Get-Content $Out | Measure-Object -Line).Lines)"
