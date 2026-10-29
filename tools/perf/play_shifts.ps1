param([string]$Tag = 'bs', [int[]]$Shifts = @(2500, 18000), [int]$Seconds = 150)
# Plays the DOLs that build_shifts.ps1 made, one after the other.  Logs go to <OutDir>\<Tag><shift>.log and the last
# DEMOFPS / SCENE / SPEED lines of each to <OutDir>\runs_<Tag>.txt.  Run it from the PowerShell tool, not with
# `powershell -File` (a comma list of shifts breaks there).
. "$PSScriptRoot\env.ps1"
$out = Join-Path $script:OutDir "runs_$Tag.txt"
Remove-Item $out -ErrorAction SilentlyContinue
foreach ($s in $Shifts) {
    $dol = Join-Path $script:Root "tekken6wii-$Tag$s.dol"
    if (-not (Test-Path $dol)) { Add-Content $out "== shift $s MISSING DOL"; continue }
    $log = Join-Path $script:OutDir "$Tag$s.log"
    & "$PSScriptRoot\run_dol.ps1" -Dol $dol -Seconds $Seconds -Out $log | Out-Null
    Add-Content $out "== shift ${s}ms"
    Select-String -Path $log -Pattern 'DEMOFPS|SCENE|SPEED' | Select-Object -Last 6 | ForEach-Object {
        Add-Content $out ($_.Line -replace '^.*OSREPORT\]: ', '' -replace 'ms/guest-frame', 'ms' -replace 'draw cache hits.*$', '') }
}
Add-Content $out "DONE"
Get-Content $out
